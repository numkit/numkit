// src/bundle/src/register/builtin/timefun_reg.cpp

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/time.h>
#include <keel/utf8.h>
#include <numkit/builtin/timefun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/object.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace numkit::builtin::detail {

namespace {

// ── Proleptic Gregorian civil calendar <-> serial day helpers ───────────────
// In the reference engine's serial date system:
//   0000-01-01 00:00:00 is serial day 1.0
//   1970-01-01 00:00:00 is serial day 719529.0
// Howard Hinnant's civil_from_days / days_from_civil uses 1970-01-01 = 0.
constexpr keel::f64 kEpochSerialDay = 719529.0;
constexpr keel::f64 kSecondsPerDay  = 86400.0;
constexpr keel::f64 kSecondsPerYear = 365.2425 * 86400.0;

inline keel::i64 daysFromCivil(keel::i64 y, keel::i64 m, keel::i64 d) noexcept
{
    // Normalize month overflow/underflow into year
    keel::i64 m0 = m - 1;
    keel::i64 yAdj = y + (m0 >= 0 ? m0 / 12 : (m0 - 11) / 12);
    keel::i64 mAdj = m0 - (yAdj - y) * 12 + 1;
    y = yAdj;
    m = mAdj;
    y -= (m <= 2) ? 1 : 0;
    const keel::i64 era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = keel::as<unsigned>(y - era * 400);
    const unsigned doy = (153 * (keel::as<unsigned>(m) + (m > 2 ? -3 : 9)) + 2) / 5 + 0;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + keel::as<keel::i64>(doe) - 719468 + (d - 1);
}

inline void civilFromDays(keel::i64 z, keel::i64 &y, keel::i64 &m, keel::i64 &d) noexcept
{
    z += 719468;
    const keel::i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = keel::as<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = keel::as<keel::i64>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = keel::as<keel::i64>(doy - (153 * mp + 2) / 5 + 1);
    m = keel::as<keel::i64>(mp) + (mp < 10 ? 3 : -9);
    y += (m <= 2) ? 1 : 0;
}

inline bool isLeapYear(keel::i64 y) noexcept
{
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

inline keel::i64 daysInMonth(keel::i64 y, keel::i64 m) noexcept
{
    static const keel::i64 kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    keel::i64 m0 = ((m - 1) % 12 + 12) % 12;
    if (m0 == 1 && isLeapYear(y))
        return 29;
    return kDays[m0];
}

inline keel::f64 composeSerialDay(keel::f64 Y, keel::f64 M, keel::f64 D,
                               keel::f64 H = 0.0, keel::f64 MI = 0.0, keel::f64 S = 0.0) noexcept
{
    if (std::isnan(Y) || std::isnan(M) || std::isnan(D)
        || std::isnan(H) || std::isnan(MI) || std::isnan(S))
        return std::numeric_limits<keel::f64>::quiet_NaN();
    keel::i64 yi = keel::as<keel::i64>(std::floor(Y));
    keel::i64 mi = keel::as<keel::i64>(std::floor(M));
    keel::i64 di = keel::as<keel::i64>(std::floor(D));
    keel::f64 fracD = (D - keel::as<keel::f64>(di))
                   + (H * 3600.0 + MI * 60.0 + S) / kSecondsPerDay;
    keel::i64 daysSince1970 = daysFromCivil(yi, mi, di);
    return keel::as<keel::f64>(daysSince1970) + kEpochSerialDay + fracD;
}

struct CivilDateTime
{
    keel::i64 year = 0, month = 1, day = 1, hour = 0, minute = 0;
    keel::f64 second = 0.0;
    bool isNaT = false;
};

inline CivilDateTime decomposeSerialDay(keel::f64 serialDay) noexcept
{
    CivilDateTime out;
    if (!std::isfinite(serialDay)) {
        out.isNaT = true;
        return out;
    }
    // Round to nearest millisecond to avoid FP drift in H:MI:S extraction
    keel::f64 shifted = serialDay - kEpochSerialDay;
    keel::i64 wholeDays = keel::as<keel::i64>(std::floor(shifted));
    keel::f64 remSec = (shifted - keel::as<keel::f64>(wholeDays)) * kSecondsPerDay;
    keel::f64 roundedMs = std::round(remSec * 1000.0);
    if (roundedMs >= 86400000.0) {
        wholeDays += 1;
        roundedMs -= 86400000.0;
    } else if (roundedMs < 0.0) {
        wholeDays -= 1;
        roundedMs += 86400000.0;
    }
    civilFromDays(wholeDays, out.year, out.month, out.day);
    keel::i64 totalWholeSec = keel::as<keel::i64>(roundedMs / 1000.0);
    keel::f64 msPart = roundedMs - keel::as<keel::f64>(totalWholeSec) * 1000.0;
    out.hour = totalWholeSec / 3600;
    out.minute = (totalWholeSec % 3600) / 60;
    out.second = keel::as<keel::f64>(totalWholeSec % 60) + msPart / 1000.0;
    return out;
}

inline keel::f64 addCalendarMonths(keel::f64 serialDay, keel::i64 deltaMonths,
                                keel::i64 deltaDays, keel::f64 deltaSeconds) noexcept
{
    if (!std::isfinite(serialDay))
        return std::numeric_limits<keel::f64>::quiet_NaN();
    CivilDateTime c = decomposeSerialDay(serialDay);
    keel::i64 m0 = (c.month - 1) + deltaMonths;
    keel::i64 yNew = c.year + (m0 >= 0 ? m0 / 12 : (m0 - 11) / 12);
    keel::i64 mNew = m0 - (yNew - c.year) * 12 + 1;
    keel::i64 dNew = std::min(c.day, daysInMonth(yNew, mNew));
    return composeSerialDay(keel::as<keel::f64>(yNew), keel::as<keel::f64>(mNew),
                            keel::as<keel::f64>(dNew + deltaDays),
                            keel::as<keel::f64>(c.hour), keel::as<keel::f64>(c.minute),
                            c.second + deltaSeconds);
}

static const char *kMonthShort[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

inline keel::String formatDateTimeElem(keel::f64 serialDay, keel::StringRef fmt, bool anyTimeInArray)
{
    CivilDateTime c = decomposeSerialDay(serialDay);
    if (c.isNaT)
        return keel::String::from("NaT");
    keel::StringRef f = fmt;
    if (f.empty()) {
        f = anyTimeInArray ? keel::StringRef("dd-MMM-uuuu HH:mm:ss") : keel::StringRef("dd-MMM-uuuu");
    }
    char y4[16], m2[8], d2[8], h2[8], mi2[8], s2[16];
    std::snprintf(y4, sizeof(y4), "%04lld", keel::as<long long>(c.year));
    std::snprintf(m2, sizeof(m2), "%02lld", keel::as<long long>(c.month));
    std::snprintf(d2, sizeof(d2), "%02lld", keel::as<long long>(c.day));
    std::snprintf(h2, sizeof(h2), "%02lld", keel::as<long long>(c.hour));
    std::snprintf(mi2, sizeof(mi2), "%02lld", keel::as<long long>(c.minute));
    int wholeSec = keel::as<int>(std::floor(c.second + 1e-9));
    keel::f64 fracSec = c.second - wholeSec;
    std::snprintf(s2, sizeof(s2), "%02d", wholeSec);
    const char *mmm = (c.month >= 1 && c.month <= 12) ? kMonthShort[c.month - 1] : "Jan";

    if (f == "dd-MMM-uuuu" || f == "dd-MMM-yyyy")
        return keel::format("{}-{}-{}", d2, mmm, y4);
    if (f == "dd-MMM-uuuu HH:mm:ss" || f == "dd-MMM-yyyy HH:mm:ss")
        return keel::format("{}-{}-{} {}:{}:{}", d2, mmm, y4, h2, mi2, s2);
    if (f == "yyyy-MM-dd" || f == "uuuu-MM-dd")
        return keel::format("{}-{}-{}", y4, m2, d2);
    if (f == "yyyy-MM-dd HH:mm:ss" || f == "uuuu-MM-dd HH:mm:ss")
        return keel::format("{}-{}-{} {}:{}:{}", y4, m2, d2, h2, mi2, s2);

    // Generic token replacement for custom Format strings
    keel::String res;
    auto matchAt = [&](keel::usize pos, keel::StringRef tok) -> bool {
        return pos + tok.size() <= f.size() && f.substr(pos, tok.size()) == tok;
    };
    for (keel::usize i = 0; i < f.size();) {
        if (matchAt(i, "uuuu") || matchAt(i, "yyyy")) {
            res.append(y4);
            i += 4;
        } else if (matchAt(i, "uu") || matchAt(i, "yy")) {
            char y2[8];
            std::snprintf(y2, sizeof(y2), "%02lld", keel::as<long long>(c.year % 100));
            res.append(y2);
            i += 2;
        } else if (matchAt(i, "QQQ")) {
            int q = keel::as<int>((c.month - 1) / 3 + 1);
            keel::format_into(res, "Q{}", q);
            i += 3;
        } else if (matchAt(i, "QQ")) {
            int q = keel::as<int>((c.month - 1) / 3 + 1);
            keel::format_into(res, "Q{}", q);
            i += 2;
        } else if (matchAt(i, "MMM")) {
            res.append(mmm);
            i += 3;
        } else if (matchAt(i, "MM")) {
            res.append(m2);
            i += 2;
        } else if (matchAt(i, "dd")) {
            res.append(d2);
            i += 2;
        } else if (matchAt(i, "HH")) {
            res.append(h2);
            i += 2;
        } else if (matchAt(i, "mm")) {
            res.append(mi2);
            i += 2;
        } else if (matchAt(i, "ss.SSS")) {
            int ms = keel::as<int>(std::round(fracSec * 1000.0));
            char msBuf[16];
            std::snprintf(msBuf, sizeof(msBuf), "%s.%03d", s2, ms);
            res.append(msBuf);
            i += 6;
        } else if (matchAt(i, "ss")) {
            res.append(s2);
            i += 2;
        } else {
            res.push_back(f[i++]);
        }
    }
    return res;
}

inline keel::f64 parseDateTimeString(keel::StringRef raw, keel::Allocator *mr)
{
    keel::usize start = 0;
    keel::usize end = raw.size();
    while (start < end && keel::is_ascii_space(raw[start]))
        ++start;
    while (end > start && keel::is_ascii_space(raw[end - 1]))
        --end;
    keel::String s = keel::String::from(raw.substr(start, end - start), mr);
    if (s.empty() || s == "NaT")
        return std::numeric_limits<keel::f64>::quiet_NaN();
    keel::String sl = s.clone();
    for (keel::usize i = 0; i < sl.size(); ++i)
        sl[i] = keel::to_ascii_lower(sl[i]);
    if (sl == "now")
        return numkit::builtin::now();
    if (sl == "today")
        return std::floor(numkit::builtin::now());
    if (sl == "yesterday")
        return std::floor(numkit::builtin::now()) - 1.0;
    if (sl == "tomorrow")
        return std::floor(numkit::builtin::now()) + 1.0;

    auto skipWs = [](const char *&p) {
        while (*p && keel::is_ascii_space(*p))
            ++p;
    };
    auto readInt = [&](const char *&p, int &out) -> bool {
        skipWs(p);
        char *ep = nullptr;
        long val = std::strtol(p, &ep, 10);
        if (ep == p)
            return false;
        out = keel::as<int>(val);
        p = ep;
        return true;
    };
    auto readChar = [&](const char *&p, char &out) -> bool {
        skipWs(p);
        if (!*p)
            return false;
        out = *p++;
        return true;
    };
    auto readDouble = [&](const char *&p, keel::f64 &out) -> bool {
        skipWs(p);
        char *ep = nullptr;
        keel::f64 val = std::strtod(p, &ep);
        if (ep == p)
            return false;
        out = val;
        p = ep;
        return true;
    };

    // Try ISO YYYY-MM-DD [HH:MM:SS[.fff]] or YYYY/MM/DD
    int y = 0, m = 0, d = 0, hh = 0, mi = 0;
    keel::f64 sec = 0.0;
    char sep1 = 0, sep2 = 0;
    {
        const char *p = s.c_str();
        if (readInt(p, y) && readChar(p, sep1) && readInt(p, m) && readChar(p, sep2) && readInt(p, d)
            && (sep1 == '-' || sep1 == '/') && sep1 == sep2 && y >= 1000) {
            skipWs(p);
            if (*p != '\0') {
                if (*p == 'T' || *p == 't')
                    ++p;
                char c1 = 0, c2 = 0;
                if (readInt(p, hh) && readChar(p, c1) && readInt(p, mi)) {
                    if (readChar(p, c2) && readDouble(p, sec)) {
                        (void)c2;
                    }
                }
            }
            return composeSerialDay(y, m, d, hh, mi, sec);
        }
    }
    // Delegate to existing datenum parser (handles dd-MMM-yyyy, MM/dd/yyyy, etc.)
    {
        const char *p = s.c_str();
        int a = 0, b = 0, c = 0;
        char c1 = 0, c2 = 0;
        if (readInt(p, a) && readChar(p, c1) && readInt(p, b) && readChar(p, c2) && readInt(p, c)
            && (c1 == '/' || c1 == '-') && c1 == c2 && c >= 1000 && a >= 1 && a <= 12 && b >= 1 && b <= 31) {
            return composeSerialDay(c, a, b, 0, 0, 0.0);
        }
    }
    try {
        Value sv = Value::fromString(s, mr);
        Value dn = numkit::builtin::datenum(keel::ArrayRef<Value>(&sv, 1), mr);
        return dn.toScalar();
    } catch (...) {
        return std::numeric_limits<keel::f64>::quiet_NaN();
    }
}

// ── Payloads ────────────────────────────────────────────────────────────────

struct DatetimeElemPayload : NativePayload
{
    keel::f64 serialDay = 0.0;
    keel::String format;
    keel::String timeZone;

    DatetimeElemPayload() = default;
    DatetimeElemPayload(keel::f64 sd, keel::StringRef fmt = "", keel::StringRef tz = "",
                        keel::Allocator *alloc = nullptr)
        : serialDay(sd)
        , format(keel::String::from(fmt, alloc ? alloc : keel::default_allocator()))
        , timeZone(keel::String::from(tz, alloc ? alloc : keel::default_allocator())) {}

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        return keel::make_arc_in<DatetimeElemPayload>(alloc, serialDay, format.as_ref(), timeZone.as_ref(), alloc);
    }

    keel::f64 asDouble() const override { return serialDay; }

    keel::Arc<NativePayload> fromScalarDouble(keel::f64 v, keel::Allocator *alloc = nullptr) const override
    {
        return keel::make_arc_in<DatetimeElemPayload>(alloc, v, format.as_ref(), timeZone.as_ref(), alloc);
    }
};

struct DurationElemPayload : NativePayload
{
    keel::f64 seconds = 0.0;
    keel::String format = keel::String::from("hh:mm:ss");

    DurationElemPayload() = default;
    DurationElemPayload(keel::f64 sec, keel::StringRef fmt = "hh:mm:ss",
                        keel::Allocator *alloc = nullptr)
        : seconds(sec)
        , format(keel::String::from(fmt, alloc ? alloc : keel::default_allocator())) {}

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        return keel::make_arc_in<DurationElemPayload>(alloc, seconds, format.as_ref(), alloc);
    }

    keel::f64 asDouble() const override { return seconds; }

    keel::Arc<NativePayload> fromScalarDouble(keel::f64 v, keel::Allocator *alloc = nullptr) const override
    {
        return keel::make_arc_in<DurationElemPayload>(alloc, v, format.as_ref(), alloc);
    }
};

struct CalDurElemPayload : NativePayload
{
    keel::i64 months = 0;
    keel::i64 days = 0;
    keel::f64 seconds = 0.0;

    CalDurElemPayload() = default;
    CalDurElemPayload(keel::i64 m, keel::i64 d, keel::f64 s = 0.0)
        : months(m), days(d), seconds(s) {}

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        return keel::make_arc_in<CalDurElemPayload>(alloc, *this);
    }
};

inline const DatetimeElemPayload *dtElem(const Value &v, keel::usize i = 0)
{
    const ObjectState *st = v.objectStateAt(i);
    return st ? static_cast<const DatetimeElemPayload *>(st->native.get()) : nullptr;
}

inline DatetimeElemPayload *dtElemMut(Value &v, keel::usize i = 0)
{
    ObjectState *st = v.objectStateMutAt(i);
    return st ? static_cast<DatetimeElemPayload *>(st->native.get()) : nullptr;
}

inline const DurationElemPayload *durElem(const Value &v, keel::usize i = 0)
{
    const ObjectState *st = v.objectStateAt(i);
    return st ? static_cast<const DurationElemPayload *>(st->native.get()) : nullptr;
}

inline DurationElemPayload *durElemMut(Value &v, keel::usize i = 0)
{
    ObjectState *st = v.objectStateMutAt(i);
    return st ? static_cast<DurationElemPayload *>(st->native.get()) : nullptr;
}

inline const CalDurElemPayload *calElem(const Value &v, keel::usize i = 0)
{
    const ObjectState *st = v.objectStateAt(i);
    return st ? static_cast<const CalDurElemPayload *>(st->native.get()) : nullptr;
}

inline Value makeArrayFromDims(const Dims &d, ValueType t, keel::Allocator *mr)
{
    return Value::matrix(d.rows(), d.cols(), t, mr);
}

inline Value makeDatetimeArray(const Dims &dims, keel::ArrayRef<keel::f64> serialDays,
                               keel::StringRef fmt, keel::StringRef tz,
                               keel::Allocator *mr)
{
    if (!mr)
        mr = keel::default_allocator();
    keel::Array<keel::Arc<ObjectState>> states(mr);
    states.reserve(serialDays.size());
    for (keel::f64 sd : serialDays) {
        auto st = keel::make_arc_in<ObjectState>(mr, mr);
        st->native = keel::make_arc_in<DatetimeElemPayload>(mr, sd, fmt, tz, mr);
        states.push_back(keel::move(st));
    }
    if (dims.numel() == 1 && states.size() == 1)
        return Value::object("datetime", keel::move(states[0]), /*isHandle=*/false, mr);
    return Value::objectArray("datetime", dims, keel::move(states), /*isHandle=*/false, mr);
}

inline Value makeDatetimeScalar(keel::f64 sd, keel::StringRef fmt, keel::StringRef tz,
                                keel::Allocator *mr)
{
    return makeDatetimeArray(Dims(1, 1), keel::ArrayRef<keel::f64>(&sd, 1), fmt, tz, mr);
}

inline Value makeDurationArray(const Dims &dims, keel::ArrayRef<keel::f64> secs,
                               keel::StringRef fmt, keel::Allocator *mr)
{
    if (!mr)
        mr = keel::default_allocator();
    keel::Array<keel::Arc<ObjectState>> states(mr);
    states.reserve(secs.size());
    for (keel::f64 s : secs) {
        auto st = keel::make_arc_in<ObjectState>(mr, mr);
        st->native = keel::make_arc_in<DurationElemPayload>(mr, s, fmt, mr);
        states.push_back(keel::move(st));
    }
    if (dims.numel() == 1 && states.size() == 1)
        return Value::object("duration", keel::move(states[0]), /*isHandle=*/false, mr);
    return Value::objectArray("duration", dims, keel::move(states), /*isHandle=*/false, mr);
}

inline Value makeDurationScalar(keel::f64 sec, keel::StringRef fmt, keel::Allocator *mr)
{
    return makeDurationArray(Dims(1, 1), keel::ArrayRef<keel::f64>(&sec, 1), fmt, mr);
}

inline Value makeCalDurArray(const Dims &dims, keel::ArrayRef<CalDurElemPayload> items,
                             keel::Allocator *mr)
{
    if (!mr)
        mr = keel::default_allocator();
    keel::Array<keel::Arc<ObjectState>> states(mr);
    states.reserve(items.size());
    for (const auto &it : items) {
        auto st = keel::make_arc_in<ObjectState>(mr, mr);
        st->native = keel::make_arc_in<CalDurElemPayload>(mr, it);
        states.push_back(keel::move(st));
    }
    if (dims.numel() == 1 && states.size() == 1)
        return Value::object("calendarDuration", keel::move(states[0]), /*isHandle=*/false, mr);
    return Value::objectArray("calendarDuration", dims, keel::move(states), /*isHandle=*/false, mr);
}

inline keel::String formatDurationElem(keel::f64 sec, keel::StringRef fmt)
{
    if (!std::isfinite(sec))
        return keel::String::from("NaN");
    if (fmt == "s" || fmt == "sec")
        return keel::format("%g sec", sec);
    if (fmt == "m" || fmt == "min")
        return keel::format("%g min", sec / 60.0);
    if (fmt == "h" || fmt == "hr")
        return keel::format("%g hr", sec / 3600.0);
    if (fmt == "d" || fmt == "day")
        return keel::format("%g days", sec / kSecondsPerDay);
    if (fmt == "y" || fmt == "yr")
        return keel::format("%g yr", sec / kSecondsPerYear);
    bool neg = sec < 0.0;
    keel::f64 a = keel::abs(sec);
    keel::i64 totalMs = keel::as<keel::i64>(std::round(a * 1000.0));
    keel::i64 wholeSec = totalMs / 1000;
    keel::i64 ms = totalMs % 1000;
    keel::i64 hh = wholeSec / 3600;
    keel::i64 mm = (wholeSec % 3600) / 60;
    keel::i64 ss = wholeSec % 60;
    char buf[64];
    if (ms == 0) {
        std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld",
                      neg ? "-" : "",
                      keel::as<long long>(hh),
                      keel::as<long long>(mm),
                      keel::as<long long>(ss));
    } else {
        std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld.%03lld",
                      neg ? "-" : "",
                      keel::as<long long>(hh),
                      keel::as<long long>(mm),
                      keel::as<long long>(ss),
                      keel::as<long long>(ms));
    }
    return keel::String::from(buf);
}

inline keel::String formatCalDurElem(const CalDurElemPayload &cd)
{
    keel::String os;
    keel::i64 y = cd.months / 12;
    keel::i64 m = cd.months % 12;
    bool any = false;
    if (y != 0) {
        keel::format_into(os, "{}y", y);
        any = true;
    }
    if (m != 0) {
        if (any) os.push_back(' ');
        keel::format_into(os, "{}m", m);
        any = true;
    }
    if (cd.days != 0 || (!any && cd.seconds == 0.0)) {
        if (any) os.push_back(' ');
        keel::format_into(os, "{}d", cd.days);
        any = true;
    }
    if (cd.seconds != 0.0) {
        if (any) os.push_back(' ');
        os.append(formatDurationElem(cd.seconds, "hh:mm:ss").as_ref());
    }
    return os;
}

inline Dims broadcastDims(const Value &a, const Value &b, const char *fnName)
{
    const keel::usize na = a.isObject() ? a.objectCount() : a.numel();
    const keel::usize nb = b.isObject() ? b.objectCount() : b.numel();
    if (na == 1)
        return b.dims();
    if (nb == 1)
        return a.dims();
    if (a.dims() == b.dims())
        return a.dims();
    if (na == nb)
        return a.dims();
    throw std::runtime_error(
        keel::format("numkit:{}:DimMismatch: Arrays have incompatible sizes for this operation.", fnName).c_str());
}

inline Value extractDatetimeComponent(const Value &self, int comp, keel::Allocator *mr)
{
    const keel::usize n = self.objectCount();
    auto compOf = [&](keel::f64 sd) -> keel::f64 {
        CivilDateTime c = decomposeSerialDay(sd);
        if (c.isNaT)
            return std::numeric_limits<keel::f64>::quiet_NaN();
        switch (comp) {
        case 0: return keel::as<keel::f64>(c.year);
        case 1: return keel::as<keel::f64>(c.month);
        case 2: return keel::as<keel::f64>(c.day);
        case 3: return keel::as<keel::f64>(c.hour);
        case 4: return keel::as<keel::f64>(c.minute);
        case 5: return c.second;
        case 6: return keel::as<keel::f64>((c.month - 1) / 3 + 1); // quarter
        default: return 0.0;
        }
    };
    if (n == 1) {
        const auto *p = dtElem(self, 0);
        return Value::scalar(compOf(p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN()), mr);
    }
    Value out = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
    keel::f64 *dst = out.doubleDataMut();
    for (keel::usize i = 0; i < n; ++i) {
        const auto *p = dtElem(self, i);
        dst[i] = compOf(p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN());
    }
    return out;
}

inline void mutateDatetimeComponent(Value &self, int comp, const Value &val)
{
    const keel::usize n = self.objectCount();
    const keel::usize nv = val.numel();
    if (nv != 1 && nv != n)
        throw std::runtime_error("numkit:datetime:SizeMismatch: Assigned value must be a scalar or match the datetime array size.");
    for (keel::usize i = 0; i < n; ++i) {
        auto *p = dtElemMut(self, i);
        if (!p)
            continue;
        keel::f64 v = (nv == 1) ? val.toScalar() : val.elemAsDouble(i);
        CivilDateTime c = decomposeSerialDay(p->serialDay);
        keel::f64 y = keel::as<keel::f64>(c.year);
        keel::f64 m = keel::as<keel::f64>(c.month);
        keel::f64 d = keel::as<keel::f64>(c.day);
        keel::f64 h = keel::as<keel::f64>(c.hour);
        keel::f64 mi = keel::as<keel::f64>(c.minute);
        keel::f64 s = c.second;
        switch (comp) {
        case 0: y = v; break;
        case 1: m = v; break;
        case 2: d = v; break;
        case 3: h = v; break;
        case 4: mi = v; break;
        case 5: s = v; break;
        default: break;
        }
        p->serialDay = composeSerialDay(y, m, d, h, mi, s);
    }
}

inline Value constructDurationFromUnit(keel::ArrayRef<Value> args, keel::f64 scaleSeconds,
                                       const char *defaultFmt, const char *fnName,
                                       keel::Allocator *mr)
{
    argcheck::checkNargin(args.size(), 1, 1, fnName);
    const Value &x = args[0];
    if (x.isObject() && x.objectClassNameRef() == "duration") {
        // When called on a duration, extract numeric units!
        const keel::usize n = x.objectCount();
        if (n == 1) {
            const auto *p = durElem(x, 0);
            return Value::scalar((p ? p->seconds : 0.0) / scaleSeconds, mr);
        }
        Value out = makeArrayFromDims(x.dims(), ValueType::DOUBLE, mr);
        keel::f64 *dst = out.doubleDataMut();
        for (keel::usize i = 0; i < n; ++i) {
            const auto *p = durElem(x, i);
            dst[i] = (p ? p->seconds : 0.0) / scaleSeconds;
        }
        return out;
    }
    if (!x.isNumeric() && !x.isLogical())
        throw std::runtime_error(
            keel::format("numkit:{}:InvalidInput: Expected a numeric or duration input.", fnName).c_str());
    const keel::usize n = x.numel();
    keel::Array<keel::f64> secs(mr);
    secs.resize(n);
    for (keel::usize i = 0; i < n; ++i)
        secs[i] = x.elemAsDouble(i) * scaleSeconds;
    return makeDurationArray(x.dims(), secs.as_ref(), defaultFmt, mr);
}

inline Value constructCalDurFromUnit(keel::ArrayRef<Value> args, keel::i64 monthMult, keel::i64 dayMult,
                                     const char *fnName, keel::Allocator *mr)
{
    argcheck::checkNargin(args.size(), 1, 1, fnName);
    const Value &x = args[0];
    if (x.isObject() && x.objectClassNameRef() == "calendarDuration") {
        const keel::usize n = x.objectCount();
        auto extract = [&](const CalDurElemPayload *p) -> keel::f64 {
            if (!p) return 0.0;
            if (monthMult != 0)
                return keel::as<keel::f64>(p->months) / keel::as<keel::f64>(monthMult);
            return keel::as<keel::f64>(p->days) / keel::as<keel::f64>(dayMult);
        };
        if (n == 1)
            return Value::scalar(extract(calElem(x, 0)), mr);
        Value out = makeArrayFromDims(x.dims(), ValueType::DOUBLE, mr);
        keel::f64 *dst = out.doubleDataMut();
        for (keel::usize i = 0; i < n; ++i)
            dst[i] = extract(calElem(x, i));
        return out;
    }
    if (!x.isNumeric() && !x.isLogical())
        throw std::runtime_error(
            keel::format("numkit:{}:InvalidInput: Expected a numeric integer array.", fnName).c_str());
    const keel::usize n = x.numel();
    keel::Array<CalDurElemPayload> items(mr);
    items.resize(n);
    for (keel::usize i = 0; i < n; ++i) {
        keel::f64 v = x.elemAsDouble(i);
        if (!std::isfinite(v) || std::floor(v) != v)
            throw std::runtime_error(
                keel::format("numkit:{}:MustBeInteger: Calendar duration quantities must be finite integers.", fnName).c_str());
        keel::i64 iv = keel::as<keel::i64>(v);
        items[i] = CalDurElemPayload(iv * monthMult, iv * dayMult, 0.0);
    }
    return makeCalDurArray(x.dims(), items.as_ref(), mr);
}

} // namespace

void register_timefun(Engine &engine) {
    engine.registerFunction("clock",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "clock");
            outs[0] = numkit::builtin::clock(ctx.engine->allocator());
        });

    engine.registerFunction("date",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "date");
            outs[0] = Value::fromString(numkit::builtin::date(), ctx.engine->allocator());
        });

    engine.registerFunction("pause",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 1, "pause");
            static bool pauseEnabled = true;
            if (!args.empty() && (args[0].isChar() || args[0].isString())) {
                const char *prev = pauseEnabled ? "on" : "off";
                keel::String mode = args[0].toString();
                for (char &c : mode) c = keel::to_ascii_lower(c);
                if (mode == "off") pauseEnabled = false;
                else if (mode == "on") pauseEnabled = true;
                if (nargout > 0 || mode == "query") {
                    outs[0] = Value::fromString(prev, ctx.engine->allocator());
                } else {
                    outs[0] = Value();
                }
                return;
            }
            keel::f64 s = args.empty() ? 0.0 : args[0].toScalar();
            if (pauseEnabled && s > 0.0) {
                numkit::builtin::pause(s);
            }
            outs[0] = Value();
        });

    engine.registerFunction("tic",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "tic");
            const keel::u64 nowNs = keel::monotonic_ns();
            ctx.engine->setTicTimer(nowNs);
            if (nargout > 0) {
                const keel::f64 id = keel::as<keel::f64>(nowNs / 1000ULL);
                outs[0] = Value::scalar(id, ctx.engine->allocator());
            } else {
                outs[0] = Value();
            }
        });

    engine.registerFunction("toc",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 1, "toc");
            const keel::u64 nowNs = keel::monotonic_ns();
            keel::u64 startNs = 0;
            if (!args.empty() && args[0].isScalar()) {
                const keel::f64 v = args[0].toScalar();
                startNs = (v > 0.0) ? keel::as<keel::u64>(v) * 1000ULL : 0ULL;
            } else if (ctx.engine->ticWasCalled()) {
                startNs = ctx.engine->ticTimer();
            } else {
                throw std::runtime_error("toc: You must call 'tic' before calling 'toc'.");
            }
            const keel::f64 elapsed = (nowNs >= startNs) ? keel::as<keel::f64>(nowNs - startNs) * 1e-9 : 0.0;
            if (nargout > 0) {
                outs[0] = Value::scalar(elapsed, ctx.engine->allocator());
            } else {
                ctx.engine->outputText(keel::format("Elapsed time is %g seconds.\n", elapsed));
                outs[0] = Value();
            }
        });

    engine.registerFunction("cputime",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "cputime");
            outs[0] = Value::scalar(numkit::builtin::cputime(), ctx.engine->allocator());
        });

    engine.registerFunction("now",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "now");
            outs[0] = Value::scalar(numkit::builtin::now(), ctx.engine->allocator());
        });

    engine.registerFunction("etime",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 2, "etime");
            outs[0] = numkit::builtin::etime(args[0], args[1], ctx.engine->allocator());
        });

    engine.registerFunction("weeknum",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "weeknum");
            outs[0] = numkit::builtin::weeknum(args, ctx.engine->allocator());
        });

    engine.registerFunction("addtodate",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 3, 3, "addtodate");
            outs[0] = numkit::builtin::addtodate(args[0], args[1].toScalar(), args[2].toString(), ctx.engine->allocator());
        });

    engine.registerFunction("datenum",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 6, "datenum");
            outs[0] = numkit::builtin::datenum(args, ctx.engine->allocator());
        });

    engine.registerFunction("weekday",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "weekday");
            auto *mr = ctx.engine->allocator();
            outs[0] = numkit::builtin::weekday(args[0], mr);
            if (nargout > 1) {
                bool wantLong = false;
                if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                    keel::String fmt = args[1].toString();
                    for (auto &c : fmt)
                        c = keel::to_ascii_lower(c);
                    if (fmt == "long")
                        wantLong = true;
                    else if (fmt != "short")
                        throw std::runtime_error("weekday: format must be 'short' or 'long'");
                }
                static const char *kShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
                static const char *kLong[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
                keel::f64 dval = args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(0);
                keel::i64 f = keel::as<keel::i64>(std::floor(dval)) - 2;
                keel::i64 r = f % 7;
                if (r < 0) r += 7;
                int dayIdx = keel::as<int>(r) + 1;
                outs[1] = Value::fromString(wantLong ? kLong[dayIdx - 1] : kShort[dayIdx - 1], mr);
            }
        });

    engine.registerFunction("juliandate",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 6, "juliandate");
            outs[0] = numkit::builtin::juliandate(args, ctx.engine->allocator());
        });

    engine.registerFunction("eomday",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 2, "eomday");
            outs[0] = numkit::builtin::eomday(args[0], args[1], ctx.engine->allocator());
        });

    engine.registerFunction("calendar",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 2, "calendar");
            outs[0] = numkit::builtin::calendar(args, ctx.engine->allocator());
        });

    engine.registerFunction("datestr",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "datestr");
            outs[0] = numkit::builtin::datestr(args, ctx.engine->allocator());
        });

    engine.registerFunction("datevec",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "datevec");
            auto *mr = ctx.engine->allocator();
            Value dv = numkit::builtin::datevec(args, mr);
            if (nargout <= 1) {
                outs[0] = std::move(dv);
            } else {
                const keel::usize N = dv.dims().rows();
                const keel::f64 *o = dv.doubleData();
                for (keel::usize c = 0; c < std::min(nargout, keel::usize(6)); ++c) {
                    if (N == 1) {
                        outs[c] = Value::scalar(o[c * N], mr);
                    } else {
                        auto col = Value::matrix(N, 1, ValueType::DOUBLE, mr);
                        keel::f64 *p = col.doubleDataMut();
                        for (keel::usize i = 0; i < N; ++i)
                            p[i] = o[i + c * N];
                        outs[c] = std::move(col);
                    }
                }
            }
        });

    engine.registerFunction("yyyymmdd",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "yyyymmdd");
            outs[0] = numkit::builtin::yyyymmdd(args, ctx.engine->allocator());
        });

    engine.registerFunction("mjuliandate",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 6, "mjuliandate");
            outs[0] = numkit::builtin::mjuliandate(args, ctx.engine->allocator());
        });

    // ── Duration & CalendarDuration unit constructors ──────────────────────
    engine.registerFunction("seconds",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 1.0, "s", "seconds", ctx.engine->allocator());
        });
    engine.registerFunction("milliseconds",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 0.001, "s", "milliseconds", ctx.engine->allocator());
        });
    engine.registerFunction("minutes",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 60.0, "m", "minutes", ctx.engine->allocator());
        });
    engine.registerFunction("hours",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 3600.0, "h", "hours", ctx.engine->allocator());
        });
    engine.registerFunction("days",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, kSecondsPerDay, "d", "days", ctx.engine->allocator());
        });
    engine.registerFunction("years",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, kSecondsPerYear, "y", "years", ctx.engine->allocator());
        });

    engine.registerFunction("caldays",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 0, 1, "caldays", ctx.engine->allocator());
        });
    engine.registerFunction("calweeks",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 0, 7, "calweeks", ctx.engine->allocator());
        });
    engine.registerFunction("calmonths",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 1, 0, "calmonths", ctx.engine->allocator());
        });
    engine.registerFunction("calquarters",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 3, 0, "calquarters", ctx.engine->allocator());
        });
    engine.registerFunction("calyears",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 12, 0, "calyears", ctx.engine->allocator());
        });

    engine.registerFunction("isdatetime",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "isdatetime");
            outs[0] = Value::logicalScalar(
                args[0].isObject() && args[0].objectClassNameRef() == "datetime",
                ctx.engine->allocator());
        });
    engine.registerFunction("isduration",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "isduration");
            outs[0] = Value::logicalScalar(
                args[0].isObject() && args[0].objectClassNameRef() == "duration",
                ctx.engine->allocator());
        });
    engine.registerFunction("iscalendarduration",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "iscalendarduration");
            outs[0] = Value::logicalScalar(
                args[0].isObject() && args[0].objectClassNameRef() == "calendarDuration",
                ctx.engine->allocator());
        });
    engine.registerFunction("isnat",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "isnat");
            auto *mr = ctx.engine->allocator();
            if (!args[0].isObject() || args[0].objectClassNameRef() != "datetime") {
                outs[0] = Value::logicalScalar(false, mr);
                return;
            }
            const keel::usize n = args[0].objectCount();
            if (n == 1) {
                const auto *p = dtElem(args[0], 0);
                outs[0] = Value::logicalScalar(!p || !std::isfinite(p->serialDay), mr);
                return;
            }
            Value out = makeArrayFromDims(args[0].dims(), ValueType::LOGICAL, mr);
            keel::u8 *dst = out.logicalDataMut();
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(args[0], i);
                dst[i] = (!p || !std::isfinite(p->serialDay)) ? 1 : 0;
            }
            outs[0] = std::move(out);
        });
    engine.registerFunction("NaT",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 4, "NaT");
            auto *mr = ctx.engine->allocator();
            keel::usize r = 1, c = 1;
            if (args.size() == 1 && args[0].isNumeric()) {
                if (args[0].numel() == 1) {
                    r = c = keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(0)));
                } else if (args[0].numel() >= 2) {
                    r = keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(0)));
                    c = keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(1)));
                }
            } else if (args.size() >= 2 && args[0].isNumeric() && args[1].isNumeric()) {
                r = keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(0)));
                c = keel::as<keel::usize>(std::max(0.0, args[1].elemAsDouble(0)));
            }
            keel::Array<keel::f64> nats(mr);
            nats.assign(r * c, std::numeric_limits<keel::f64>::quiet_NaN());
            outs[0] = makeDatetimeArray(Dims(r, c), nats.as_ref(), "", "", mr);
        });

    auto *clsAlloc = engine.allocator();

    // ── BuiltinClass: duration ─────────────────────────────────────────────
    {
        BuiltinClass dur;
        dur.name = keel::String("duration", clsAlloc);
        dur.isHandle = false;
        dur.propNames.push_back(keel::String("Format", clsAlloc));

        dur.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            keel::String fmt = keel::String::from("hh:mm:ss", mr);
            keel::usize nPos = args.size();
            while (nPos >= 2 && (args[nPos - 2].isChar() || args[nPos - 2].isString())) {
                keel::String k = args[nPos - 2].toString();
                for (auto &ch : k) ch = keel::to_ascii_lower(ch);
                if (k == "format") {
                    fmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else {
                    break;
                }
            }
            if (nPos == 0)
                return makeDurationScalar(0.0, fmt.as_ref(), mr);
            if (nPos == 1 && args[0].isNumeric() && args[0].dims().cols() == 3) {
                const keel::usize R = args[0].dims().rows();
                keel::Array<keel::f64> secs(mr);
                secs.resize(R);
                for (keel::usize r = 0; r < R; ++r) {
                    keel::f64 h = args[0].elemAsDouble(r);
                    keel::f64 m = args[0].elemAsDouble(r + R);
                    keel::f64 s = args[0].elemAsDouble(r + 2 * R);
                    secs[r] = h * 3600.0 + m * 60.0 + s;
                }
                return makeDurationArray(Dims(R, 1), secs.as_ref(), fmt.as_ref(), mr);
            }
            if (nPos == 3 || nPos == 4) {
                Dims d = broadcastDims(args[0], args[1], "duration");
                d = (d.numel() == 1) ? args[2].dims() : d;
                const keel::usize n = d.numel();
                keel::Array<keel::f64> secs(mr);
                secs.resize(n);
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 h = args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(i);
                    keel::f64 m = args[1].numel() == 1 ? args[1].toScalar() : args[1].elemAsDouble(i);
                    keel::f64 s = args[2].numel() == 1 ? args[2].toScalar() : args[2].elemAsDouble(i);
                    keel::f64 ms = (nPos == 4)
                                    ? (args[3].numel() == 1 ? args[3].toScalar() : args[3].elemAsDouble(i))
                                    : 0.0;
                    secs[i] = h * 3600.0 + m * 60.0 + s + ms * 0.001;
                }
                return makeDurationArray(d, secs.as_ref(), fmt.as_ref(), mr);
            }
            throw std::runtime_error("numkit:duration:InvalidArgs: Expected duration(H, MI, S) or duration([H MI S]).");
        };

        dur.propGet = [](const Value &self, keel::StringRef name, Value &out,
                         CallContext &ctx) -> bool {
            if (name == "Format") {
                const auto *p = durElem(self, 0);
                out = Value::fromString(p ? p->format.as_ref() : keel::StringRef("hh:mm:ss"), ctx.engine->allocator());
                return true;
            }
            return false;
        };
        dur.propSet = [](Value &self, keel::StringRef name, const Value &val,
                         CallContext &) -> bool {
            if (name == "Format") {
                keel::String f = val.toString();
                for (keel::usize i = 0; i < self.objectCount(); ++i)
                    if (auto *p = durElemMut(self, i))
                        p->format.assign(f.as_ref());
                return true;
            }
            return false;
        };

        auto addDurUnitMethod = [&](const char *mname, keel::f64 scale) {
            dur.methods.put(keel::String(mname, clsAlloc), [scale, mname](Value &self, keel::ArrayRef<Value>, keel::usize,
                                                keel::MutArrayRef<Value> outs, CallContext &ctx) {
                Value arg[1] = {self};
                outs[0] = constructDurationFromUnit(keel::ArrayRef<Value>(arg, 1), scale, "s",
                                                    mname, ctx.engine->allocator());
            });
        };
        addDurUnitMethod("seconds", 1.0);
        addDurUnitMethod("milliseconds", 0.001);
        addDurUnitMethod("minutes", 60.0);
        addDurUnitMethod("hours", 3600.0);
        addDurUnitMethod("days", kSecondsPerDay);
        addDurUnitMethod("years", kSecondsPerYear);

        dur.methods.put(keel::String("char", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                 CallContext &ctx) {
            const auto *p = durElem(self, 0);
            outs[0] = Value::fromString(
                p ? formatDurationElem(p->seconds, p->format) : keel::String::from("00:00:00"),
                ctx.engine->allocator());
        });
        dur.methods.put(keel::String("string", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                   CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            Value s = Value::stringArray(self.dims().rows(), self.dims().cols(), mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = durElem(self, i);
                s.stringElemSet(i, p ? formatDurationElem(p->seconds, p->format)
                                     : keel::String::from("00:00:00"));
            }
            outs[0] = std::move(s);
        });
        dur.methods.put(keel::String("hms", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs,
                                CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            Value h = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            Value m = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            Value s = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < n; ++i) {
                keel::f64 sec = durElem(self, i) ? durElem(self, i)->seconds : 0.0;
                keel::f64 sign = sec < 0.0 ? -1.0 : 1.0;
                keel::f64 a = keel::abs(sec);
                keel::f64 hh = std::floor(a / 3600.0);
                keel::f64 mm = std::floor((a - hh * 3600.0) / 60.0);
                keel::f64 ss = a - hh * 3600.0 - mm * 60.0;
                h.doubleDataMut()[i] = sign * hh;
                m.doubleDataMut()[i] = sign * mm;
                s.doubleDataMut()[i] = sign * ss;
            }
            outs[0] = (n == 1) ? Value::scalar(h.doubleData()[0], mr) : std::move(h);
            if (nargout >= 2)
                outs[1] = (n == 1) ? Value::scalar(m.doubleData()[0], mr) : std::move(m);
            if (nargout >= 3)
                outs[2] = (n == 1) ? Value::scalar(s.doubleData()[0], mr) : std::move(s);
        });

        dur.ops.put(keel::String("uminus", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                               CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            keel::Array<keel::f64> secs(mr);
            secs.resize(n);
            keel::StringRef fmt = durElem(self, 0) ? durElem(self, 0)->format.as_ref() : keel::StringRef("hh:mm:ss");
            for (keel::usize i = 0; i < n; ++i)
                secs[i] = -(durElem(self, i) ? durElem(self, i)->seconds : 0.0);
            outs[0] = makeDurationArray(self.dims(), secs.as_ref(), fmt, mr);
        });
        dur.ops.put(keel::String("uplus", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                              CallContext &) { outs[0] = self; });

        dur.methods.put(keel::String("abs", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            keel::Array<keel::f64> secs(mr);
            secs.resize(n);
            keel::StringRef fmt = durElem(self, 0) ? durElem(self, 0)->format.as_ref() : keel::StringRef("hh:mm:ss");
            for (keel::usize i = 0; i < n; ++i)
                secs[i] = keel::abs(durElem(self, i) ? durElem(self, i)->seconds : 0.0);
            outs[0] = makeDurationArray(self.dims(), secs.as_ref(), fmt, mr);
        });
        auto addDurMinMaxMethod = [&](const char *mname, bool isMax) {
            dur.methods.put(keel::String(mname, clsAlloc), [isMax](Value &self, keel::ArrayRef<Value>, keel::usize nargout,
                                         keel::MutArrayRef<Value> outs, CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                const keel::usize n = self.objectCount();
                if (n == 0) {
                    outs[0] = self;
                    return;
                }
                keel::usize bestIdx = 0;
                keel::f64 bestVal = durElem(self, 0) ? durElem(self, 0)->seconds : 0.0;
                for (keel::usize i = 1; i < n; ++i) {
                    keel::f64 v = durElem(self, i) ? durElem(self, i)->seconds : 0.0;
                    if (std::isnan(v)) continue;
                    if (std::isnan(bestVal) || (isMax ? (v > bestVal) : (v < bestVal))) {
                        bestVal = v;
                        bestIdx = i;
                    }
                }
                keel::StringRef fmt = durElem(self, 0) ? durElem(self, 0)->format.as_ref() : keel::StringRef("hh:mm:ss");
                outs[0] = makeDurationScalar(bestVal, fmt, mr);
                if (nargout >= 2)
                    outs[1] = Value::scalar(keel::as<keel::f64>(bestIdx + 1), mr);
            });
        };
        addDurMinMaxMethod("min", false);
        addDurMinMaxMethod("max", true);

        auto durAddSub = [](keel::f64 sign) -> ObjectMethod {
            return [sign](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                // duration + datetime
                if (sign > 0.0 && lhs.isObject() && lhs.objectClassNameRef() == "duration"
                    && rhs.isObject() && rhs.objectClassNameRef() == "datetime") {
                    Dims d = broadcastDims(lhs, rhs, "plus");
                    const keel::usize n = d.numel();
                    keel::Array<keel::f64> sds(mr);
                    sds.resize(n);
                    const auto *p0 = dtElem(rhs, 0);
                    for (keel::usize i = 0; i < n; ++i) {
                        keel::f64 sec = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                        keel::f64 sd = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                        sds[i] = sd + sec / kSecondsPerDay;
                    }
                    outs[0] = makeDatetimeArray(d, sds.as_ref(),
                                                p0 ? p0->format.as_ref() : keel::StringRef(""),
                                                p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
                    return;
                }
                if (!lhs.isObject() || lhs.objectClassNameRef() != "duration"
                    || !rhs.isObject() || rhs.objectClassNameRef() != "duration")
                    throw std::runtime_error("numkit:duration:TypeMismatch: Both operands must be durations.");
                Dims d = broadcastDims(lhs, rhs, sign > 0 ? "plus" : "minus");
                const keel::usize n = d.numel();
                keel::Array<keel::f64> secs(mr);
                secs.resize(n);
                keel::StringRef fmt = durElem(lhs, 0) ? durElem(lhs, 0)->format.as_ref() : keel::StringRef("hh:mm:ss");
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 s1 = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                    keel::f64 s2 = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                    secs[i] = s1 + sign * s2;
                }
                outs[0] = makeDurationArray(d, secs.as_ref(), fmt, mr);
            };
        };
        dur.ops.put(keel::String("plus", clsAlloc), durAddSub(+1.0));
        dur.ops.put(keel::String("minus", clsAlloc), durAddSub(-1.0));

        auto durScale = [](bool divide) -> ObjectMethod {
            return [divide](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                            CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (divide && lhs.isObject() && rhs.isObject()
                    && lhs.objectClassNameRef() == "duration" && rhs.objectClassNameRef() == "duration") {
                    Dims d = broadcastDims(lhs, rhs, "rdivide");
                    const keel::usize n = d.numel();
                    if (n == 1) {
                        outs[0] = Value::scalar(durElem(lhs, 0)->seconds / durElem(rhs, 0)->seconds, mr);
                        return;
                    }
                    Value out = makeArrayFromDims(d, ValueType::DOUBLE, mr);
                    for (keel::usize i = 0; i < n; ++i) {
                        keel::f64 s1 = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                        keel::f64 s2 = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                        out.doubleDataMut()[i] = s1 / s2;
                    }
                    outs[0] = std::move(out);
                    return;
                }
                const Value &dVal = lhs.isObject() ? lhs : rhs;
                const Value &nVal = lhs.isObject() ? rhs : lhs;
                Dims d = broadcastDims(dVal, nVal, divide ? "rdivide" : "times");
                const keel::usize n = d.numel();
                keel::Array<keel::f64> secs(mr);
                secs.resize(n);
                keel::StringRef fmt = durElem(dVal, 0) ? durElem(dVal, 0)->format.as_ref() : keel::StringRef("hh:mm:ss");
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 s = durElem(dVal, dVal.objectCount() == 1 ? 0 : i)->seconds;
                    keel::f64 k = nVal.numel() == 1 ? nVal.toScalar() : nVal.elemAsDouble(i);
                    secs[i] = divide ? (s / k) : (s * k);
                }
                outs[0] = makeDurationArray(d, secs.as_ref(), fmt, mr);
            };
        };
        dur.ops.put(keel::String("times", clsAlloc), durScale(false));
        dur.ops.put(keel::String("mtimes", clsAlloc), durScale(false));
        dur.ops.put(keel::String("rdivide", clsAlloc), durScale(true));
        dur.ops.put(keel::String("mrdivide", clsAlloc), durScale(true));

        auto durCmp = [](int mode) -> ObjectMethod {
            return [mode](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (!lhs.isObject() || !rhs.isObject()
                    || lhs.objectClassNameRef() != "duration" || rhs.objectClassNameRef() != "duration") {
                    outs[0] = Value::logicalScalar(mode == 1, mr);
                    return;
                }
                Dims d = broadcastDims(lhs, rhs, "relop");
                const keel::usize n = d.numel();
                auto cmp1 = [mode](keel::f64 a, keel::f64 b) -> bool {
                    switch (mode) {
                    case 0: return keel::abs(a - b) <= 1e-9;
                    case 1: return !(keel::abs(a - b) <= 1e-9);
                    case 2: return a < b - 1e-9;
                    case 3: return a <= b + 1e-9;
                    case 4: return a > b + 1e-9;
                    case 5: return a >= b - 1e-9;
                    default: return false;
                    }
                };
                if (n == 1) {
                    outs[0] = Value::logicalScalar(
                        cmp1(durElem(lhs, 0)->seconds, durElem(rhs, 0)->seconds), mr);
                    return;
                }
                Value out = makeArrayFromDims(d, ValueType::LOGICAL, mr);
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 s1 = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                    keel::f64 s2 = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                    out.logicalDataMut()[i] = cmp1(s1, s2) ? 1 : 0;
                }
                outs[0] = std::move(out);
            };
        };
        dur.ops.put(keel::String("eq", clsAlloc), durCmp(0));
        dur.ops.put(keel::String("ne", clsAlloc), durCmp(1));
        dur.ops.put(keel::String("lt", clsAlloc), durCmp(2));
        dur.ops.put(keel::String("le", clsAlloc), durCmp(3));
        dur.ops.put(keel::String("gt", clsAlloc), durCmp(4));
        dur.ops.put(keel::String("ge", clsAlloc), durCmp(5));

        dur.dispText = [](const Value &self) -> keel::String {
            const keel::usize R = self.dims().rows(), C = self.dims().cols();
            keel::String os;
            if (R * C == 0) {
                keel::format_into(os, "  {}\xC3\x97{} empty duration array\n", R, C);
                return os;
            }
            if (R * C > 1)
                keel::format_into(os, "  {}\xC3\x97{} duration array\n\n", R, C);
            else
                os.append("  duration\n\n");
            for (keel::usize r = 0; r < R; ++r) {
                os.append("   ");
                for (keel::usize c = 0; c < C; ++c) {
                    const auto *p = durElem(self, c * R + r);
                    os.push_back(' ');
                    if (p)
                        os.append(formatDurationElem(p->seconds, p->format).as_ref());
                    else
                        os.append("00:00:00");
                    if (c + 1 < C) os.append("   ");
                }
                os.push_back('\n');
            }
            return os;
        };

        engine.registerClass(std::move(dur));
    }

    // ── BuiltinClass: calendarDuration ─────────────────────────────────────
    {
        BuiltinClass cal;
        cal.name = keel::String("calendarDuration", clsAlloc);
        cal.isHandle = false;
        cal.propNames.push_back(keel::String("Format", clsAlloc));

        cal.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            if (args.empty()) {
                CalDurElemPayload zero(0, 0, 0.0);
                return makeCalDurArray(Dims(1, 1), keel::ArrayRef<CalDurElemPayload>(&zero, 1), mr);
            }
            if (args.size() == 1 && args[0].isNumeric() && args[0].dims().cols() == 3) {
                const keel::usize R = args[0].dims().rows();
                keel::Array<CalDurElemPayload> items(mr);
                items.resize(R);
                for (keel::usize r = 0; r < R; ++r) {
                    keel::i64 y = keel::as<keel::i64>(std::round(args[0].elemAsDouble(r)));
                    keel::i64 m = keel::as<keel::i64>(std::round(args[0].elemAsDouble(r + R)));
                    keel::i64 d = keel::as<keel::i64>(std::round(args[0].elemAsDouble(r + 2 * R)));
                    items[r] = CalDurElemPayload(y * 12 + m, d, 0.0);
                }
                return makeCalDurArray(Dims(R, 1), items.as_ref(), mr);
            }
            if (args.size() == 3 || args.size() == 6) {
                Dims d = broadcastDims(args[0], args[1], "calendarDuration");
                d = (d.numel() == 1) ? args[2].dims() : d;
                const keel::usize n = d.numel();
                keel::Array<CalDurElemPayload> items(mr);
                items.resize(n);
                for (keel::usize i = 0; i < n; ++i) {
                    keel::i64 y = keel::as<keel::i64>(std::round(args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(i)));
                    keel::i64 m = keel::as<keel::i64>(std::round(args[1].numel() == 1 ? args[1].toScalar() : args[1].elemAsDouble(i)));
                    keel::i64 dy = keel::as<keel::i64>(std::round(args[2].numel() == 1 ? args[2].toScalar() : args[2].elemAsDouble(i)));
                    keel::f64 sec = 0.0;
                    if (args.size() == 6) {
                        keel::f64 hh = args[3].numel() == 1 ? args[3].toScalar() : args[3].elemAsDouble(i);
                        keel::f64 mi = args[4].numel() == 1 ? args[4].toScalar() : args[4].elemAsDouble(i);
                        keel::f64 ss = args[5].numel() == 1 ? args[5].toScalar() : args[5].elemAsDouble(i);
                        sec = hh * 3600.0 + mi * 60.0 + ss;
                    }
                    items[i] = CalDurElemPayload(y * 12 + m, dy, sec);
                }
                return makeCalDurArray(d, items.as_ref(), mr);
            }
            throw std::runtime_error("numkit:calendarDuration:InvalidArgs: Expected calendarDuration(Y, M, D) or calendarDuration(Y, M, D, H, MI, S).");
        };

        cal.methods.put(keel::String("calyears", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                     CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(keel::ArrayRef<Value>(arg, 1), 12, 0, "calyears", ctx.engine->allocator());
        });
        cal.methods.put(keel::String("calquarters", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                        CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(keel::ArrayRef<Value>(arg, 1), 3, 0, "calquarters", ctx.engine->allocator());
        });
        cal.methods.put(keel::String("calmonths", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                      CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(keel::ArrayRef<Value>(arg, 1), 1, 0, "calmonths", ctx.engine->allocator());
        });
        cal.methods.put(keel::String("calweeks", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                     CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(keel::ArrayRef<Value>(arg, 1), 0, 7, "calweeks", ctx.engine->allocator());
        });
        cal.methods.put(keel::String("caldays", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                    CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(keel::ArrayRef<Value>(arg, 1), 0, 1, "caldays", ctx.engine->allocator());
        });
        cal.methods.put(keel::String("char", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                 CallContext &ctx) {
            const auto *p = calElem(self, 0);
            outs[0] = Value::fromString(p ? formatCalDurElem(*p) : keel::String::from("0d"), ctx.engine->allocator());
        });
        cal.methods.put(keel::String("string", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                   CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            Value s = Value::stringArray(self.dims().rows(), self.dims().cols(), mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = calElem(self, i);
                s.stringElemSet(i, p ? formatCalDurElem(*p) : keel::String::from("0d"));
            }
            outs[0] = std::move(s);
        });

        cal.ops.put(keel::String("uminus", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                               CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            keel::Array<CalDurElemPayload> items(mr);
            items.resize(n);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = calElem(self, i);
                if (p)
                    items[i] = CalDurElemPayload(-p->months, -p->days, -p->seconds);
            }
            outs[0] = makeCalDurArray(self.dims(), items.as_ref(), mr);
        });
        cal.ops.put(keel::String("uplus", clsAlloc), [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                              CallContext &) { outs[0] = self; });

        auto calAddSub = [](keel::i64 sign) -> ObjectMethod {
            return [sign](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (sign > 0 && lhs.isObject() && lhs.objectClassNameRef() == "calendarDuration"
                    && rhs.isObject() && rhs.objectClassNameRef() == "datetime") {
                    Dims d = broadcastDims(lhs, rhs, "plus");
                    const keel::usize n = d.numel();
                    keel::Array<keel::f64> sds(mr);
                    sds.resize(n);
                    const auto *p0 = dtElem(rhs, 0);
                    for (keel::usize i = 0; i < n; ++i) {
                        const auto *cp = calElem(lhs, lhs.objectCount() == 1 ? 0 : i);
                        const auto *dp = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                        sds[i] = addCalendarMonths(dp->serialDay, cp->months, cp->days, cp->seconds);
                    }
                    outs[0] = makeDatetimeArray(d, sds.as_ref(),
                                                p0 ? p0->format.as_ref() : keel::StringRef(""),
                                                p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
                    return;
                }
                if (!lhs.isObject() || lhs.objectClassNameRef() != "calendarDuration"
                    || !rhs.isObject() || rhs.objectClassNameRef() != "calendarDuration")
                    throw std::runtime_error("numkit:calendarDuration:TypeMismatch: Both operands must be calendarDurations.");
                Dims d = broadcastDims(lhs, rhs, sign > 0 ? "plus" : "minus");
                const keel::usize n = d.numel();
                keel::Array<CalDurElemPayload> items(mr);
                items.resize(n);
                for (keel::usize i = 0; i < n; ++i) {
                    const auto *c1 = calElem(lhs, lhs.objectCount() == 1 ? 0 : i);
                    const auto *c2 = calElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                    items[i] = CalDurElemPayload(
                        c1->months + sign * c2->months,
                        c1->days + sign * c2->days,
                        c1->seconds + keel::as<keel::f64>(sign) * c2->seconds);
                }
                outs[0] = makeCalDurArray(d, items.as_ref(), mr);
            };
        };
        cal.ops.put(keel::String("plus", clsAlloc), calAddSub(+1));
        cal.ops.put(keel::String("minus", clsAlloc), calAddSub(-1));

        auto calEq = [](bool neg) -> ObjectMethod {
            return [neg](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                         CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (!lhs.isObject() || !rhs.isObject()
                    || lhs.objectClassNameRef() != "calendarDuration"
                    || rhs.objectClassNameRef() != "calendarDuration") {
                    outs[0] = Value::logicalScalar(neg, mr);
                    return;
                }
                Dims d = broadcastDims(lhs, rhs, neg ? "ne" : "eq");
                const keel::usize n = d.numel();
                auto eq1 = [neg](const CalDurElemPayload *a, const CalDurElemPayload *b) -> bool {
                    bool e = a && b && a->months == b->months && a->days == b->days
                             && keel::abs(a->seconds - b->seconds) <= 1e-9;
                    return neg ? !e : e;
                };
                if (n == 1) {
                    outs[0] = Value::logicalScalar(eq1(calElem(lhs, 0), calElem(rhs, 0)), mr);
                    return;
                }
                Value out = makeArrayFromDims(d, ValueType::LOGICAL, mr);
                for (keel::usize i = 0; i < n; ++i) {
                    const auto *c1 = calElem(lhs, lhs.objectCount() == 1 ? 0 : i);
                    const auto *c2 = calElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                    out.logicalDataMut()[i] = eq1(c1, c2) ? 1 : 0;
                }
                outs[0] = std::move(out);
            };
        };
        cal.ops.put(keel::String("eq", clsAlloc), calEq(false));
        cal.ops.put(keel::String("ne", clsAlloc), calEq(true));

        cal.dispText = [](const Value &self) -> keel::String {
            const keel::usize R = self.dims().rows(), C = self.dims().cols();
            keel::String os;
            if (R * C == 0) {
                keel::format_into(os, "  {}\xC3\x97{} empty calendarDuration array\n", R, C);
                return os;
            }
            if (R * C > 1)
                keel::format_into(os, "  {}\xC3\x97{} calendarDuration array\n\n", R, C);
            else
                os.append("  calendarDuration\n\n");
            for (keel::usize r = 0; r < R; ++r) {
                os.append("   ");
                for (keel::usize c = 0; c < C; ++c) {
                    const auto *p = calElem(self, c * R + r);
                    os.push_back(' ');
                    if (p)
                        os.append(formatCalDurElem(*p).as_ref());
                    else
                        os.append("0d");
                    if (c + 1 < C) os.append("   ");
                }
                os.push_back('\n');
            }
            return os;
        };

        engine.registerClass(std::move(cal));
    }

    // ── BuiltinClass: datetime ─────────────────────────────────────────────
    {
        BuiltinClass dt;
        dt.name = keel::String("datetime", clsAlloc);
        dt.isHandle = false;
        for (const char *pn : {"Format", "TimeZone", "Year", "Month", "Day", "Hour", "Minute", "Second"})
            dt.propNames.push_back(keel::String(pn, clsAlloc));

        dt.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            keel::String fmt(mr), inFmt(mr), convFrom(mr), tz(mr);
            keel::usize nPos = args.size();
            while (nPos >= 2 && (args[nPos - 2].isChar() || args[nPos - 2].isString())) {
                keel::String kl = args[nPos - 2].toString();
                for (auto &ch : kl) ch = keel::to_ascii_lower(ch);
                if (kl == "format") {
                    fmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else if (kl == "inputformat") {
                    inFmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else if (kl == "convertfrom") {
                    convFrom = args[nPos - 1].toString();
                    for (auto &ch : convFrom) ch = keel::to_ascii_lower(ch);
                    nPos -= 2;
                } else if (kl == "timezone") {
                    tz = args[nPos - 1].toString();
                    nPos -= 2;
                } else {
                    break;
                }
            }
            (void)inFmt;

            // datetime() -> current instant
            if (nPos == 0) {
                return makeDatetimeScalar(numkit::builtin::now(), fmt.as_ref(), tz.as_ref(), mr);
            }

            // datetime(dt_obj [, 'Format', fmt, 'TimeZone', tz])
            if (nPos == 1 && args[0].isObject() && args[0].objectClassNameRef() == "datetime") {
                const keel::usize n = args[0].objectCount();
                keel::Array<keel::f64> sds(mr);
                sds.resize(n);
                keel::String f0 = fmt.clone();
                keel::String tz0 = tz.clone();
                for (keel::usize i = 0; i < n; ++i) {
                    const auto *p = dtElem(args[0], i);
                    sds[i] = p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN();
                    if (i == 0 && p) {
                        if (f0.empty()) f0.assign(p->format.as_ref());
                        if (tz0.empty()) tz0.assign(p->timeZone.as_ref());
                    }
                }
                return makeDatetimeArray(args[0].dims(), sds.as_ref(), f0.as_ref(), tz0.as_ref(), mr);
            }

            // datetime(x, 'ConvertFrom', type)
            if (!convFrom.empty()) {
                if (nPos != 1 || (!args[0].isNumeric() && !args[0].isLogical()))
                    throw std::runtime_error("numkit:datetime:InvalidConvertFrom: Expected a single numeric input with 'ConvertFrom'.");
                const Value &x = args[0];
                const keel::usize n = x.numel();
                keel::Array<keel::f64> sds(mr);
                sds.resize(n);
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 v = x.elemAsDouble(i);
                    if (!std::isfinite(v)) {
                        sds[i] = std::numeric_limits<keel::f64>::quiet_NaN();
                    } else if (convFrom == "datenum") {
                        sds[i] = v;
                    } else if (convFrom == "posixtime" || convFrom == "epochtime") {
                        sds[i] = kEpochSerialDay + v / kSecondsPerDay;
                    } else if (convFrom == "juliandate") {
                        sds[i] = v - 1721058.5;
                    } else if (convFrom == "mjuliandate") {
                        sds[i] = v + 678942.0;
                    } else if (convFrom == "excel") {
                        sds[i] = v + 693960.0;
                    } else if (convFrom == "yyyymmdd") {
                        keel::i64 iv = keel::as<keel::i64>(std::llround(v));
                        keel::i64 y = iv / 10000;
                        keel::i64 m = (iv / 100) % 100;
                        keel::i64 d = iv % 100;
                        sds[i] = composeSerialDay(keel::as<keel::f64>(y), keel::as<keel::f64>(m), keel::as<keel::f64>(d));
                    } else {
                        throw std::runtime_error(
                            keel::format("numkit:datetime:UnknownConvertFrom: Unsupported ConvertFrom type '{}'.", convFrom).c_str());
                    }
                }
                return makeDatetimeArray(x.dims(), sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
            }

            // datetime(str [, fmt])
            if ((nPos == 1 || nPos == 2)
                && (args[0].isChar() || args[0].isString() || args[0].isCell())) {
                if (nPos == 2 && fmt.empty() && (args[1].isChar() || args[1].isString()))
                    fmt = args[1].toString();
                if (args[0].isChar()) {
                    const keel::usize R = args[0].dims().rows();
                    const keel::usize C = args[0].dims().cols();
                    if (R <= 1) {
                        keel::f64 sd = parseDateTimeString(args[0].toString(), mr);
                        return makeDatetimeScalar(sd, fmt.as_ref(), tz.as_ref(), mr);
                    }
                    keel::Array<keel::f64> sds(mr);
                    sds.resize(R);
                    const char *cd = args[0].charData();
                    for (keel::usize r = 0; r < R; ++r) {
                        keel::String rowStr(mr);
                        rowStr.resize(C, ' ');
                        for (keel::usize c = 0; c < C; ++c)
                            rowStr[c] = cd[c * R + r];
                        sds[r] = parseDateTimeString(rowStr, mr);
                    }
                    return makeDatetimeArray(Dims(R, 1), sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
                }
                if (args[0].isString()) {
                    const keel::usize n = args[0].numel();
                    keel::Array<keel::f64> sds(mr);
                    sds.resize(n);
                    for (keel::usize i = 0; i < n; ++i)
                        sds[i] = parseDateTimeString(args[0].stringElem(i), mr);
                    return makeDatetimeArray(args[0].dims(), sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
                }
                if (args[0].isCell()) {
                    const keel::usize n = args[0].numel();
                    keel::Array<keel::f64> sds(mr);
                    sds.resize(n);
                    for (keel::usize i = 0; i < n; ++i)
                        sds[i] = parseDateTimeString(args[0].cellAt(i).toString(), mr);
                    return makeDatetimeArray(args[0].dims(), sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
                }
            }

            // datetime(datevecMatrix) where cols == 3 or cols == 6, or Excel serial date column vector
            if (nPos == 1 && args[0].isNumeric()) {
                const keel::usize R = args[0].dims().rows();
                const keel::usize C = args[0].dims().cols();
                if (R == 0 && C == 0)
                    return makeDatetimeArray(Dims(0, 0), keel::ArrayRef<keel::f64>(), fmt.as_ref(), tz.as_ref(), mr);
                if (C != 3 && C != 6) {
                    // If a 1D vector of Excel serial dates (e.g. from readtable on .xlsx)
                    // is passed directly to datetime(), convert Excel serial dates when values
                    // lie in [1, 200000].
                    if ((C == 1 || R == 1) && args[0].numel() >= 1) {
                        keel::f64 v0 = args[0].elemAsDouble(0);
                        if (std::isfinite(v0) && v0 >= 1.0 && v0 <= 200000.0) {
                            const keel::usize n = args[0].numel();
                            keel::Array<keel::f64> sds(mr);
                            sds.resize(n);
                            for (keel::usize i = 0; i < n; ++i) {
                                keel::f64 v = args[0].elemAsDouble(i);
                                sds[i] = std::isfinite(v) ? (v + 693960.0)
                                                          : std::numeric_limits<keel::f64>::quiet_NaN();
                            }
                            return makeDatetimeArray(args[0].dims(), sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
                        }
                    }
                    throw std::runtime_error("numkit:datetime:InvalidDatevecSize: Numeric matrix input to datetime must have 3 or 6 columns.");
                }
                keel::Array<keel::f64> sds(mr);
                sds.resize(R);
                for (keel::usize r = 0; r < R; ++r) {
                    keel::f64 y = args[0].elemAsDouble(r);
                    keel::f64 m = args[0].elemAsDouble(r + R);
                    keel::f64 d = args[0].elemAsDouble(r + 2 * R);
                    keel::f64 h = (C == 6) ? args[0].elemAsDouble(r + 3 * R) : 0.0;
                    keel::f64 mi = (C == 6) ? args[0].elemAsDouble(r + 4 * R) : 0.0;
                    keel::f64 s = (C == 6) ? args[0].elemAsDouble(r + 5 * R) : 0.0;
                    sds[r] = composeSerialDay(y, m, d, h, mi, s);
                }
                return makeDatetimeArray(Dims(R, 1), sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
            }

            // datetime(Y, M, D [, H, MI, S [, MS]])
            if (nPos == 3 || nPos == 6 || nPos == 7) {
                Dims outDims(1, 1);
                for (keel::usize k = 0; k < nPos; ++k) {
                    if (!args[k].isNumeric() && !args[k].isLogical())
                        throw std::runtime_error("numkit:datetime:NonNumericComponent: Date/time components must be numeric.");
                    if (args[k].numel() != 1) {
                        if (outDims.numel() == 1)
                            outDims = args[k].dims();
                        else if (args[k].dims() != outDims && args[k].numel() != outDims.numel())
                            throw std::runtime_error("numkit:datetime:ComponentSizeMismatch: Date/time component arrays must be the same size or scalar.");
                    }
                }
                const keel::usize n = outDims.numel();
                keel::Array<keel::f64> sds(mr);
                sds.resize(n);
                auto elem = [&](keel::usize k, keel::usize idx) -> keel::f64 {
                    return args[k].numel() == 1 ? args[k].toScalar() : args[k].elemAsDouble(idx);
                };
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 y = elem(0, i);
                    keel::f64 m = elem(1, i);
                    keel::f64 d = elem(2, i);
                    keel::f64 h = (nPos >= 6) ? elem(3, i) : 0.0;
                    keel::f64 mi = (nPos >= 6) ? elem(4, i) : 0.0;
                    keel::f64 s = (nPos >= 6) ? elem(5, i) : 0.0;
                    keel::f64 ms = (nPos == 7) ? elem(6, i) : 0.0;
                    sds[i] = composeSerialDay(y, m, d, h, mi, s + ms * 0.001);
                }
                return makeDatetimeArray(outDims, sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
            }

            throw std::runtime_error("numkit:datetime:InvalidSyntax: Unsupported arguments to datetime constructor.");
        };

        dt.propGet = [](const Value &self, keel::StringRef name, Value &out,
                        CallContext &ctx) -> bool {
            auto *mr = ctx.engine->allocator();
            if (name == "Year")   { out = extractDatetimeComponent(self, 0, mr); return true; }
            if (name == "Month")  { out = extractDatetimeComponent(self, 1, mr); return true; }
            if (name == "Day")    { out = extractDatetimeComponent(self, 2, mr); return true; }
            if (name == "Hour")   { out = extractDatetimeComponent(self, 3, mr); return true; }
            if (name == "Minute") { out = extractDatetimeComponent(self, 4, mr); return true; }
            if (name == "Second") { out = extractDatetimeComponent(self, 5, mr); return true; }
            if (name == "Format") {
                const auto *p = dtElem(self, 0);
                keel::StringRef f = (p && !p->format.empty()) ? p->format.as_ref() : keel::StringRef("dd-MMM-uuuu");
                out = Value::fromString(f, mr);
                return true;
            }
            if (name == "TimeZone") {
                const auto *p = dtElem(self, 0);
                out = Value::fromString(p ? p->timeZone.as_ref() : keel::StringRef(""), mr);
                return true;
            }
            return false;
        };

        dt.propSet = [](Value &self, keel::StringRef name, const Value &val,
                        CallContext &) -> bool {
            if (name == "Year")   { mutateDatetimeComponent(self, 0, val); return true; }
            if (name == "Month")  { mutateDatetimeComponent(self, 1, val); return true; }
            if (name == "Day")    { mutateDatetimeComponent(self, 2, val); return true; }
            if (name == "Hour")   { mutateDatetimeComponent(self, 3, val); return true; }
            if (name == "Minute") { mutateDatetimeComponent(self, 4, val); return true; }
            if (name == "Second") { mutateDatetimeComponent(self, 5, val); return true; }
            if (name == "Format") {
                keel::String f = val.toString();
                for (keel::usize i = 0; i < self.objectCount(); ++i)
                    if (auto *p = dtElemMut(self, i))
                        p->format.assign(f.as_ref());
                return true;
            }
            if (name == "TimeZone") {
                keel::String tz = val.toString();
                for (keel::usize i = 0; i < self.objectCount(); ++i)
                    if (auto *p = dtElemMut(self, i))
                        p->timeZone.assign(tz.as_ref());
                return true;
            }
            return false;
        };

        // Component extractor methods: year(d), month(d), day(d), hour(d), minute(d), second(d), quarter(d)
        auto addCompMethod = [&](const char *mname, int comp) {
            dt.methods.put(keel::String(mname, clsAlloc),
                           [comp](Value &self, keel::ArrayRef<Value>, keel::usize,
                                  keel::MutArrayRef<Value> outs, CallContext &ctx) {
                               outs[0] = extractDatetimeComponent(self, comp, ctx.engine->allocator());
                           });
        };
        addCompMethod("year", 0);
        addCompMethod("month", 1);
        addCompMethod("day", 2);
        addCompMethod("hour", 3);
        addCompMethod("minute", 4);
        addCompMethod("second", 5);
        addCompMethod("quarter", 6);

        dt.methods.put(keel::String("ymd", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            outs[0] = extractDatetimeComponent(self, 0, mr);
            if (nargout >= 2) outs[1] = extractDatetimeComponent(self, 1, mr);
            if (nargout >= 3) outs[2] = extractDatetimeComponent(self, 2, mr);
        });
        dt.methods.put(keel::String("hms", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            outs[0] = extractDatetimeComponent(self, 3, mr);
            if (nargout >= 2) outs[1] = extractDatetimeComponent(self, 4, mr);
            if (nargout >= 3) outs[2] = extractDatetimeComponent(self, 5, mr);
        });

        // Conversions: datenum, posixtime, juliandate, mjuliandate, exceltime, yyyymmdd, datevec, datestr, char, string, cellstr
        auto extractSerialValues = [](const Value &self, auto xform, keel::Allocator *mr) -> Value {
            const keel::usize n = self.objectCount();
            if (n == 1) {
                const auto *p = dtElem(self, 0);
                keel::f64 sd = p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN();
                return Value::scalar(std::isfinite(sd) ? xform(sd) : sd, mr);
            }
            Value out = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                keel::f64 sd = p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN();
                out.doubleDataMut()[i] = std::isfinite(sd) ? xform(sd) : sd;
            }
            return out;
        };

        dt.methods.put(keel::String("datenum", clsAlloc),
                       [extractSerialValues](Value &self, keel::ArrayRef<Value>, keel::usize,
                                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(self, [](keel::f64 sd) { return sd; }, ctx.engine->allocator());
        });
        dt.methods.put(keel::String("posixtime", clsAlloc),
                       [extractSerialValues](Value &self, keel::ArrayRef<Value>, keel::usize,
                                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](keel::f64 sd) { return (sd - kEpochSerialDay) * kSecondsPerDay; },
                ctx.engine->allocator());
        });
        dt.methods.put(keel::String("juliandate", clsAlloc),
                       [extractSerialValues](Value &self, keel::ArrayRef<Value>, keel::usize,
                                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](keel::f64 sd) { return sd + 1721058.5; }, ctx.engine->allocator());
        });
        dt.methods.put(keel::String("mjuliandate", clsAlloc),
                       [extractSerialValues](Value &self, keel::ArrayRef<Value>, keel::usize,
                                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](keel::f64 sd) { return sd - 678942.0; }, ctx.engine->allocator());
        });
        dt.methods.put(keel::String("exceltime", clsAlloc),
                       [extractSerialValues](Value &self, keel::ArrayRef<Value>, keel::usize,
                                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](keel::f64 sd) { return sd - 693960.0; }, ctx.engine->allocator());
        });
        dt.methods.put(keel::String("yyyymmdd", clsAlloc),
                       [extractSerialValues](Value &self, keel::ArrayRef<Value>, keel::usize,
                                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self,
                [](keel::f64 sd) {
                    CivilDateTime c = decomposeSerialDay(sd);
                    return keel::as<keel::f64>(c.year * 10000 + c.month * 100 + c.day);
                },
                ctx.engine->allocator());
        });
        dt.methods.put(keel::String("datevec", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value>, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            Value dv = Value::matrix(n, 6, ValueType::DOUBLE, mr);
            keel::f64 *dst = dv.doubleDataMut();
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                CivilDateTime c = decomposeSerialDay(p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN());
                if (c.isNaT) {
                    for (keel::usize k = 0; k < 6; ++k)
                        dst[i + k * n] = std::numeric_limits<keel::f64>::quiet_NaN();
                } else {
                    dst[i + 0 * n] = keel::as<keel::f64>(c.year);
                    dst[i + 1 * n] = keel::as<keel::f64>(c.month);
                    dst[i + 2 * n] = keel::as<keel::f64>(c.day);
                    dst[i + 3 * n] = keel::as<keel::f64>(c.hour);
                    dst[i + 4 * n] = keel::as<keel::f64>(c.minute);
                    dst[i + 5 * n] = c.second;
                }
            }
            if (nargout <= 1) {
                outs[0] = std::move(dv);
            } else {
                for (keel::usize k = 0; k < std::min<keel::usize>(nargout, 6); ++k) {
                    if (n == 1) {
                        outs[k] = Value::scalar(dst[k * n], mr);
                    } else {
                        Value col = Value::matrix(n, 1, ValueType::DOUBLE, mr);
                        for (keel::usize i = 0; i < n; ++i)
                            col.doubleDataMut()[i] = dst[i + k * n];
                        outs[k] = std::move(col);
                    }
                }
            }
        });

        auto hasNonZeroTime = [](const Value &self) -> bool {
            for (keel::usize i = 0; i < self.objectCount(); ++i) {
                const auto *p = dtElem(self, i);
                if (!p) continue;
                CivilDateTime c = decomposeSerialDay(p->serialDay);
                if (!c.isNaT && (c.hour != 0 || c.minute != 0 || keel::abs(c.second) > 1e-6))
                    return true;
            }
            return false;
        };

        dt.methods.put(keel::String("char", clsAlloc),
                       [hasNonZeroTime](Value &self, keel::ArrayRef<Value> args, keel::usize,
                                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            bool anyTime = hasNonZeroTime(self);
            keel::String overrideFmt = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                           ? args[0].toString()
                                           : keel::String(mr);
            if (n <= 1) {
                const auto *p = dtElem(self, 0);
                keel::StringRef f = !overrideFmt.empty() ? overrideFmt.as_ref()
                                                         : (p ? p->format.as_ref() : keel::StringRef(""));
                outs[0] = Value::fromString(
                    formatDateTimeElem(p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN(),
                                       f, anyTime),
                    mr);
                return;
            }
            keel::Array<keel::String> rows(mr);
            rows.reserve(n);
            keel::usize maxW = 0;
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                keel::StringRef f = !overrideFmt.empty() ? overrideFmt.as_ref()
                                                         : (p ? p->format.as_ref() : keel::StringRef(""));
                keel::String row = formatDateTimeElem(
                    p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN(), f, anyTime);
                maxW = std::max(maxW, row.size());
                rows.push_back(std::move(row));
            }
            Value cm = Value::matrix(n, maxW, ValueType::CHAR, mr);
            char *dst = cm.charDataMut();
            std::fill(dst, dst + n * maxW, ' ');
            for (keel::usize r = 0; r < n; ++r)
                for (keel::usize c = 0; c < rows[r].size(); ++c)
                    dst[c * n + r] = rows[r][c];
            outs[0] = std::move(cm);
        });
        dt.methods.put(keel::String("datestr", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            keel::Array<Value> modArgs(mr);
            modArgs.reserve(args.size());
            for (const auto &a : args)
                modArgs.push_back(a);
            if (!modArgs.empty() && (modArgs[0].isChar() || modArgs[0].isString())) {
                keel::String f = modArgs[0].toString();
                for (keel::usize i = 0; i + 1 < f.size(); ++i) {
                    if (f[i] == 'm' && f[i + 1] == 'm' && (i == 0 || f[i - 1] != ':')) {
                        f[i] = 'M';
                        f[i + 1] = 'M';
                        ++i;
                    } else if (i + 3 <= f.size() && f.as_ref().substr(i, 3) == "mmm") {
                        f[i] = 'M'; f[i + 1] = 'M'; f[i + 2] = 'M';
                        i += 2;
                    }
                }
                modArgs[0] = Value::fromString(f, mr);
            }
            const auto *cls = ctx.engine->findClass("datetime");
            if (cls) {
                if (const ObjectMethod *charFn = cls->methods.get("char"))
                    (*charFn)(self, keel::ArrayRef<Value>(modArgs.data(), modArgs.size()),
                              nargout, outs, ctx);
            }
        });

        dt.methods.put(keel::String("string", clsAlloc),
                       [hasNonZeroTime](Value &self, keel::ArrayRef<Value> args, keel::usize,
                                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            bool anyTime = hasNonZeroTime(self);
            keel::String overrideFmt = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                           ? args[0].toString()
                                           : keel::String(mr);
            Value s = Value::stringArray(self.dims().rows(), self.dims().cols(), mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                keel::StringRef f = !overrideFmt.empty() ? overrideFmt.as_ref()
                                                         : (p ? p->format.as_ref() : keel::StringRef(""));
                s.stringElemSet(
                    i, formatDateTimeElem(p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN(),
                                          f, anyTime));
            }
            outs[0] = std::move(s);
        });
        dt.methods.put(keel::String("cellstr", clsAlloc),
                       [hasNonZeroTime](Value &self, keel::ArrayRef<Value> args, keel::usize,
                                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            bool anyTime = hasNonZeroTime(self);
            keel::String overrideFmt = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                           ? args[0].toString()
                                           : keel::String(mr);
            Value c = Value::cell(self.dims().rows(), self.dims().cols(), mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                keel::StringRef f = !overrideFmt.empty() ? overrideFmt.as_ref()
                                                         : (p ? p->format.as_ref() : keel::StringRef(""));
                c.cellAt(i) = Value::fromString(
                    formatDateTimeElem(p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN(),
                                       f, anyTime),
                    mr);
            }
            outs[0] = std::move(c);
        });

        auto dtIsNat = [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            if (n == 1) {
                const auto *p = dtElem(self, 0);
                outs[0] = Value::logicalScalar(!p || !std::isfinite(p->serialDay), mr);
                return;
            }
            Value out = makeArrayFromDims(self.dims(), ValueType::LOGICAL, mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                out.logicalDataMut()[i] = (!p || !std::isfinite(p->serialDay)) ? 1 : 0;
            }
            outs[0] = std::move(out);
        };
        dt.methods.put(keel::String("isnat", clsAlloc), dtIsNat);
        dt.methods.put(keel::String("isnan", clsAlloc), dtIsNat);
        dt.methods.put(keel::String("ismissing", clsAlloc), dtIsNat);

        dt.methods.put(keel::String("diff", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            if (n <= 1) {
                outs[0] = makeDurationArray(Dims(1, 0), keel::ArrayRef<keel::f64>(), "hh:mm:ss", mr);
                return;
            }
            keel::Array<keel::f64> secs(mr);
            secs.resize(n - 1);
            for (keel::usize i = 0; i + 1 < n; ++i) {
                keel::f64 sd1 = dtElem(self, i)->serialDay;
                keel::f64 sd2 = dtElem(self, i + 1)->serialDay;
                secs[i] = (sd2 - sd1) * kSecondsPerDay;
            }
            Dims outDims = (self.dims().rows() > 1 && self.dims().cols() == 1)
                               ? Dims(n - 1, 1)
                               : Dims(1, n - 1);
            outs[0] = makeDurationArray(outDims, secs.as_ref(), "hh:mm:ss", mr);
        });

        dt.methods.put(keel::String("between", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassNameRef() != "datetime")
                throw std::runtime_error("numkit:between:InvalidInput: Expected two datetime inputs.");
            auto *mr = ctx.engine->allocator();
            const Value &rhs = args[0];
            Dims d = broadcastDims(self, rhs, "between");
            const keel::usize n = d.numel();
            keel::Array<CalDurElemPayload> items(mr);
            items.resize(n);
            for (keel::usize i = 0; i < n; ++i) {
                keel::f64 sd1 = dtElem(self, self.objectCount() == 1 ? 0 : i)->serialDay;
                keel::f64 sd2 = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                CivilDateTime c1 = decomposeSerialDay(sd1);
                CivilDateTime c2 = decomposeSerialDay(sd2);
                keel::i64 months = (c2.year - c1.year) * 12 + (c2.month - c1.month);
                keel::f64 anchor = addCalendarMonths(sd1, months, 0, 0.0);
                keel::f64 remDays = sd2 - anchor;
                keel::i64 wholeDays = keel::as<keel::i64>(std::trunc(remDays));
                keel::f64 remSec = (remDays - keel::as<keel::f64>(wholeDays)) * kSecondsPerDay;
                items[i] = CalDurElemPayload(months, wholeDays, remSec);
            }
            outs[0] = makeCalDurArray(d, items.as_ref(), mr);
        });

        dt.methods.put(keel::String("dateshift", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() < 2)
                throw std::runtime_error("numkit:dateshift:NotEnoughInputs: Expected dateshift(t, 'start'|'end', unit).");
            auto *mr = ctx.engine->allocator();
            keel::String rule = args[0].toString();
            keel::String unit = args[1].toString();
            for (auto &ch : rule) ch = keel::to_ascii_lower(ch);
            for (auto &ch : unit) ch = keel::to_ascii_lower(ch);
            bool isEnd = (rule == "end");
            const keel::usize n = self.objectCount();
            keel::Array<keel::f64> sds(mr);
            sds.resize(n);
            const auto *p0 = dtElem(self, 0);
            for (keel::usize i = 0; i < n; ++i) {
                CivilDateTime c = decomposeSerialDay(dtElem(self, i)->serialDay);
                if (c.isNaT) {
                    sds[i] = std::numeric_limits<keel::f64>::quiet_NaN();
                    continue;
                }
                if (unit == "year" || unit == "years") {
                    sds[i] = isEnd ? composeSerialDay(c.year, 12, 31) : composeSerialDay(c.year, 1, 1);
                } else if (unit == "quarter" || unit == "quarters") {
                    int q = (c.month - 1) / 3 + 1;
                    int mStart = (q - 1) * 3 + 1;
                    int mEnd = q * 3;
                    sds[i] = isEnd ? composeSerialDay(c.year, mEnd, daysInMonth(c.year, mEnd))
                                   : composeSerialDay(c.year, mStart, 1);
                } else if (unit == "month" || unit == "months") {
                    sds[i] = isEnd ? composeSerialDay(c.year, c.month, daysInMonth(c.year, c.month))
                                   : composeSerialDay(c.year, c.month, 1);
                } else if (unit == "day" || unit == "days") {
                    sds[i] = isEnd ? composeSerialDay(c.year + 0.0, c.month + 0.0, c.day + 1.0)
                                   : composeSerialDay(c.year + 0.0, c.month + 0.0, c.day + 0.0);
                } else if (unit == "hour" || unit == "hours") {
                    sds[i] = composeSerialDay(c.year, c.month, c.day, c.hour + (isEnd ? 1 : 0), 0, 0);
                } else if (unit == "minute" || unit == "minutes") {
                    sds[i] = composeSerialDay(c.year, c.month, c.day, c.hour, c.minute + (isEnd ? 1 : 0), 0);
                } else {
                    sds[i] = composeSerialDay(c.year, c.month, c.day);
                }
            }
            outs[0] = makeDatetimeArray(self.dims(), sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });

        dt.methods.put(keel::String("sort", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            bool desc = false;
            if (!args.empty() && (args[0].isChar() || args[0].isString())) {
                keel::String dir = args[0].toString();
                for (auto &ch : dir) ch = keel::to_ascii_lower(ch);
                desc = (dir == "descend");
            }
            keel::Array<keel::usize> order(mr);
            order.resize(n);
            for (keel::usize i = 0; i < n; ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](keel::usize a, keel::usize b) {
                keel::f64 sa = dtElem(self, a)->serialDay;
                keel::f64 sb = dtElem(self, b)->serialDay;
                if (std::isnan(sa)) return false;
                if (std::isnan(sb)) return true;
                return desc ? (sa > sb) : (sa < sb);
            });
            keel::Array<keel::f64> sds(mr);
            sds.resize(n);
            for (keel::usize i = 0; i < n; ++i)
                sds[i] = dtElem(self, order[i])->serialDay;
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
            if (nargout >= 2) {
                Value idx = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < n; ++i)
                    idx.doubleDataMut()[i] = keel::as<keel::f64>(order[i] + 1);
                outs[1] = std::move(idx);
            }
        });

        auto addMinMaxMethod = [&](const char *mname, bool isMax) {
            dt.methods.put(keel::String(mname, clsAlloc),
                           [isMax](Value &self, keel::ArrayRef<Value>, keel::usize nargout,
                                   keel::MutArrayRef<Value> outs, CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                const keel::usize n = self.objectCount();
                if (n == 0) {
                    outs[0] = self;
                    return;
                }
                keel::usize bestIdx = 0;
                keel::f64 bestVal = dtElem(self, 0)->serialDay;
                for (keel::usize i = 1; i < n; ++i) {
                    keel::f64 v = dtElem(self, i)->serialDay;
                    if (std::isnan(v)) continue;
                    if (std::isnan(bestVal) || (isMax ? (v > bestVal) : (v < bestVal))) {
                        bestVal = v;
                        bestIdx = i;
                    }
                }
                const auto *p0 = dtElem(self, 0);
                outs[0] = makeDatetimeScalar(bestVal,
                                             p0 ? p0->format.as_ref() : keel::StringRef(""),
                                             p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
                if (nargout >= 2)
                    outs[1] = Value::scalar(keel::as<keel::f64>(bestIdx + 1), mr);
            });
        };
        addMinMaxMethod("min", false);
        addMinMaxMethod("max", true);

        dt.methods.put(keel::String("flipud", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value>, keel::usize,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize R = self.dims().rows(), C = self.dims().cols();
            keel::Array<keel::f64> sds(mr);
            sds.resize(R * C);
            for (keel::usize c = 0; c < C; ++c)
                for (keel::usize r = 0; r < R; ++r)
                    sds[c * R + r] = dtElem(self, c * R + (R - 1 - r))->serialDay;
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });
        dt.methods.put(keel::String("fliplr", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value>, keel::usize,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize R = self.dims().rows(), C = self.dims().cols();
            keel::Array<keel::f64> sds(mr);
            sds.resize(R * C);
            for (keel::usize c = 0; c < C; ++c)
                for (keel::usize r = 0; r < R; ++r)
                    sds[c * R + r] = dtElem(self, (C - 1 - c) * R + r)->serialDay;
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });
        dt.methods.put(keel::String("flip", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value>, keel::usize,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize R = self.dims().rows(), C = self.dims().cols();
            keel::Array<keel::f64> sds(mr);
            sds.resize(R * C);
            if (R > 1) {
                for (keel::usize c = 0; c < C; ++c)
                    for (keel::usize r = 0; r < R; ++r)
                        sds[c * R + r] = dtElem(self, c * R + (R - 1 - r))->serialDay;
            } else {
                for (keel::usize c = 0; c < C; ++c)
                    for (keel::usize r = 0; r < R; ++r)
                        sds[c * R + r] = dtElem(self, (C - 1 - c) * R + r)->serialDay;
            }
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });

        dt.methods.put(keel::String("unique", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            bool stable = false;
            for (const auto &a : args) {
                if (a.isChar() || a.isString()) {
                    keel::String s = a.toString();
                    for (auto &ch : s) ch = keel::to_ascii_lower(ch);
                    if (s == "stable") stable = true;
                }
            }
            constexpr keel::f64 kTol = 1e-9 / kSecondsPerDay;
            keel::Array<keel::f64> uniqSds(mr);
            keel::Array<keel::usize> ia(mr);
            keel::Array<keel::usize> ic(mr);
            ic.assign(n, 0);
            if (stable) {
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd = dtElem(self, i)->serialDay;
                    keel::usize found = uniqSds.size();
                    for (keel::usize u = 0; u < uniqSds.size(); ++u) {
                        if (keel::abs(uniqSds[u] - sd) <= kTol) { found = u; break; }
                    }
                    if (found == uniqSds.size()) {
                        uniqSds.push_back(sd);
                        ia.push_back(i + 1);
                    }
                    ic[i] = found + 1;
                }
            } else {
                keel::Array<keel::usize> order(mr);
                order.resize(n);
                for (keel::usize i = 0; i < n; ++i) order[i] = i;
                std::stable_sort(order.begin(), order.end(), [&](keel::usize a, keel::usize b) {
                    keel::f64 sa = dtElem(self, a)->serialDay;
                    keel::f64 sb = dtElem(self, b)->serialDay;
                    if (std::isnan(sa)) return false;
                    if (std::isnan(sb)) return true;
                    return sa < sb - kTol;
                });
                for (keel::usize k = 0; k < n; ++k) {
                    keel::usize idx = order[k];
                    keel::f64 sd = dtElem(self, idx)->serialDay;
                    if (uniqSds.empty() || std::isnan(sd) || std::isnan(uniqSds.back())
                        || keel::abs(sd - uniqSds.back()) > kTol) {
                        uniqSds.push_back(sd);
                        ia.push_back(idx + 1);
                    }
                    ic[idx] = uniqSds.size();
                }
            }
            const auto *p0 = dtElem(self, 0);
            Dims outDims = (self.dims().rows() == 1 && self.dims().cols() > 1)
                               ? Dims(1, uniqSds.size())
                               : Dims(uniqSds.size(), 1);
            outs[0] = makeDatetimeArray(outDims, uniqSds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
            if (nargout >= 2) {
                Value iaVal = Value::matrix(ia.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < ia.size(); ++i)
                    iaVal.doubleDataMut()[i] = keel::as<keel::f64>(ia[i]);
                outs[1] = std::move(iaVal);
            }
            if (nargout >= 3) {
                Value icVal = Value::matrix(n, 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < n; ++i)
                    icVal.doubleDataMut()[i] = keel::as<keel::f64>(ic[i]);
                outs[2] = std::move(icVal);
            }
        });

        dt.methods.put(keel::String("intersect", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassNameRef() != "datetime")
                throw std::runtime_error("numkit:intersect:InvalidInput: Expected two datetime inputs.");
            auto *mr = ctx.engine->allocator();
            const Value &other = args[0];
            bool stable = false;
            for (keel::usize k = 1; k < args.size(); ++k) {
                if (args[k].isChar() || args[k].isString()) {
                    keel::String s = args[k].toString();
                    for (auto &ch : s) ch = keel::to_ascii_lower(ch);
                    if (s == "stable") stable = true;
                }
            }
            constexpr keel::f64 kTol = 1e-9 / kSecondsPerDay;
            const keel::usize nA = self.objectCount();
            const keel::usize nB = other.objectCount();
            struct Item { keel::f64 sd; keel::usize ia; keel::usize ib; };
            keel::Array<Item> items(mr);
            for (keel::usize i = 0; i < nA; ++i) {
                keel::f64 sa = dtElem(self, i)->serialDay;
                if (std::isnan(sa)) continue;
                bool dup = false;
                for (const auto &it : items) {
                    if (keel::abs(it.sd - sa) <= kTol) { dup = true; break; }
                }
                if (dup) continue;
                for (keel::usize j = 0; j < nB; ++j) {
                    keel::f64 sb = dtElem(other, j)->serialDay;
                    if (!std::isnan(sb) && keel::abs(sa - sb) <= kTol) {
                        items.push_back({sa, i + 1, j + 1});
                        break;
                    }
                }
            }
            if (!stable) {
                std::sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
                    return a.sd < b.sd - kTol;
                });
            }
            keel::Array<keel::f64> sds(mr);
            sds.resize(items.size());
            for (keel::usize i = 0; i < items.size(); ++i) sds[i] = items[i].sd;
            const auto *p0 = dtElem(self, 0);
            Dims outDims = (self.dims().rows() == 1 && self.dims().cols() > 1
                            && other.dims().rows() == 1 && other.dims().cols() > 1)
                               ? Dims(1, sds.size())
                               : Dims(sds.size(), 1);
            outs[0] = makeDatetimeArray(outDims, sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
            if (nargout >= 2) {
                Value iaVal = Value::matrix(items.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < items.size(); ++i)
                    iaVal.doubleDataMut()[i] = keel::as<keel::f64>(items[i].ia);
                outs[1] = std::move(iaVal);
            }
            if (nargout >= 3) {
                Value ibVal = Value::matrix(items.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < items.size(); ++i)
                    ibVal.doubleDataMut()[i] = keel::as<keel::f64>(items[i].ib);
                outs[2] = std::move(ibVal);
            }
        });

        dt.methods.put(keel::String("ismember", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassNameRef() != "datetime")
                throw std::runtime_error("numkit:ismember:InvalidInput: Expected two datetime inputs.");
            auto *mr = ctx.engine->allocator();
            const Value &other = args[0];
            constexpr keel::f64 kTol = 1e-9 / kSecondsPerDay;
            const keel::usize nA = self.objectCount();
            const keel::usize nB = other.objectCount();
            Value lia = makeArrayFromDims(self.dims(), ValueType::LOGICAL, mr);
            Value locb = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < nA; ++i) {
                keel::f64 sa = dtElem(self, i)->serialDay;
                keel::usize found = 0;
                if (!std::isnan(sa)) {
                    for (keel::usize j = 0; j < nB; ++j) {
                        keel::f64 sb = dtElem(other, j)->serialDay;
                        if (!std::isnan(sb) && keel::abs(sa - sb) <= kTol) {
                            found = j + 1;
                            break;
                        }
                    }
                }
                lia.logicalDataMut()[i] = found > 0 ? 1 : 0;
                locb.doubleDataMut()[i] = keel::as<keel::f64>(found);
            }
            outs[0] = std::move(lia);
            if (nargout >= 2)
                outs[1] = std::move(locb);
        });

        // Operators on datetime
        dt.ops.put(keel::String("plus", clsAlloc),
                   [](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                      CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const Value &lhs = args[0];
            const Value &rhs = args[1];
            const Value &dtVal = (lhs.isObject() && lhs.objectClassNameRef() == "datetime") ? lhs : rhs;
            const Value &other = (&dtVal == &lhs) ? rhs : lhs;
            const auto *p0 = dtElem(dtVal, 0);
            Dims d = broadcastDims(dtVal, other, "plus");
            const keel::usize n = d.numel();
            keel::Array<keel::f64> sds(mr);
            sds.resize(n);

            if (other.isObject() && other.objectClassNameRef() == "calendarDuration") {
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd = dtElem(dtVal, dtVal.objectCount() == 1 ? 0 : i)->serialDay;
                    const auto *cp = calElem(other, other.objectCount() == 1 ? 0 : i);
                    sds[i] = addCalendarMonths(sd, cp->months, cp->days, cp->seconds);
                }
            } else if (other.isObject() && other.objectClassNameRef() == "duration") {
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd = dtElem(dtVal, dtVal.objectCount() == 1 ? 0 : i)->serialDay;
                    keel::f64 sec = durElem(other, other.objectCount() == 1 ? 0 : i)->seconds;
                    sds[i] = sd + sec / kSecondsPerDay;
                }
            } else if (other.isNumeric() || other.isLogical()) {
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd = dtElem(dtVal, dtVal.objectCount() == 1 ? 0 : i)->serialDay;
                    keel::f64 dy = other.numel() == 1 ? other.toScalar() : other.elemAsDouble(i);
                    sds[i] = sd + dy;
                }
            } else {
                throw std::runtime_error(
                    keel::format("numkit:datetime:InvalidPlusOperand: Cannot add value of type '{}' to a datetime.",
                                 mtypeName(other.type())).c_str());
            }
            outs[0] = makeDatetimeArray(d, sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });

        dt.ops.put(keel::String("minus", clsAlloc),
                   [](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                      CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const Value &lhs = args[0];
            const Value &rhs = args[1];
            if (!lhs.isObject() || lhs.objectClassNameRef() != "datetime")
                throw std::runtime_error("numkit:datetime:InvalidMinus: Cannot subtract a datetime from a non-datetime.");

            // datetime - datetime -> duration
            if (rhs.isObject() && rhs.objectClassNameRef() == "datetime") {
                Dims d = broadcastDims(lhs, rhs, "minus");
                const keel::usize n = d.numel();
                keel::Array<keel::f64> secs(mr);
                secs.resize(n);
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd1 = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    keel::f64 sd2 = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                    secs[i] = (sd1 - sd2) * kSecondsPerDay;
                }
                outs[0] = makeDurationArray(d, secs.as_ref(), "hh:mm:ss", mr);
                return;
            }

            const auto *p0 = dtElem(lhs, 0);
            Dims d = broadcastDims(lhs, rhs, "minus");
            const keel::usize n = d.numel();
            keel::Array<keel::f64> sds(mr);
            sds.resize(n);

            if (rhs.isObject() && rhs.objectClassNameRef() == "calendarDuration") {
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    const auto *cp = calElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                    sds[i] = addCalendarMonths(sd, -cp->months, -cp->days, -cp->seconds);
                }
            } else if (rhs.isObject() && rhs.objectClassNameRef() == "duration") {
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    keel::f64 sec = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                    sds[i] = sd - sec / kSecondsPerDay;
                }
            } else if (rhs.isNumeric() || rhs.isLogical()) {
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 sd = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    keel::f64 dy = rhs.numel() == 1 ? rhs.toScalar() : rhs.elemAsDouble(i);
                    sds[i] = sd - dy;
                }
            } else {
                throw std::runtime_error(
                    keel::format("numkit:datetime:InvalidMinusOperand: Cannot subtract value of type '{}' from a datetime.",
                                 mtypeName(rhs.type())).c_str());
            }
            outs[0] = makeDatetimeArray(d, sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });

        auto dtCmp = [](int mode) -> ObjectMethod {
            return [mode](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->allocator();
                Value lhs = args[0];
                Value rhs = args[1];
                if (lhs.isChar() || lhs.isString())
                    lhs = makeDatetimeScalar(parseDateTimeString(lhs.toString(), mr), "", "", mr);
                if (rhs.isChar() || rhs.isString())
                    rhs = makeDatetimeScalar(parseDateTimeString(rhs.toString(), mr), "", "", mr);
                if (!lhs.isObject() || !rhs.isObject()
                    || lhs.objectClassNameRef() != "datetime" || rhs.objectClassNameRef() != "datetime") {
                    outs[0] = Value::logicalScalar(mode == 1, mr);
                    return;
                }
                Dims d = broadcastDims(lhs, rhs, "relop");
                const keel::usize n = d.numel();
                constexpr keel::f64 kTol = 1e-9 / kSecondsPerDay;
                auto cmp1 = [mode](keel::f64 a, keel::f64 b) -> bool {
                    if (std::isnan(a) || std::isnan(b))
                        return mode == 1;
                    switch (mode) {
                    case 0: return keel::abs(a - b) <= kTol;
                    case 1: return !(keel::abs(a - b) <= kTol);
                    case 2: return a < b - kTol;
                    case 3: return a <= b + kTol;
                    case 4: return a > b + kTol;
                    case 5: return a >= b - kTol;
                    default: return false;
                    }
                };
                if (n == 1) {
                    outs[0] = Value::logicalScalar(
                        cmp1(dtElem(lhs, 0)->serialDay, dtElem(rhs, 0)->serialDay), mr);
                    return;
                }
                Value out = makeArrayFromDims(d, ValueType::LOGICAL, mr);
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 s1 = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    keel::f64 s2 = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                    out.logicalDataMut()[i] = cmp1(s1, s2) ? 1 : 0;
                }
                outs[0] = std::move(out);
            };
        };
        dt.ops.put(keel::String("eq", clsAlloc), dtCmp(0));
        dt.ops.put(keel::String("ne", clsAlloc), dtCmp(1));
        dt.ops.put(keel::String("lt", clsAlloc), dtCmp(2));
        dt.ops.put(keel::String("le", clsAlloc), dtCmp(3));
        dt.ops.put(keel::String("gt", clsAlloc), dtCmp(4));
        dt.ops.put(keel::String("ge", clsAlloc), dtCmp(5));

        dt.ops.put(keel::String("colon", clsAlloc),
                   [](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                      CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const Value &startVal = args[0];
            const Value &stopVal = (args.size() == 3) ? args[2] : args[1];
            if (!startVal.isObject() || startVal.objectClassNameRef() != "datetime"
                || !stopVal.isObject() || stopVal.objectClassNameRef() != "datetime")
                throw std::runtime_error("numkit:datetime:InvalidColonEndpoints: Colon endpoints must be datetimes.");
            keel::f64 startSd = dtElem(startVal, 0)->serialDay;
            keel::f64 stopSd = dtElem(stopVal, 0)->serialDay;
            const auto *p0 = dtElem(startVal, 0);
            keel::Array<keel::f64> sds(mr);
            constexpr keel::f64 kTol = 1e-9 / kSecondsPerDay;

            if (args.size() == 3 && args[1].isObject()
                && args[1].objectClassNameRef() == "calendarDuration") {
                const auto *cp = calElem(args[1], 0);
                keel::f64 approxStep = keel::as<keel::f64>(cp->months) * 30.0
                                    + keel::as<keel::f64>(cp->days)
                                    + cp->seconds / kSecondsPerDay;
                if (approxStep == 0.0)
                    throw std::runtime_error("numkit:datetime:ZeroColonStep: Colon step cannot be zero.");
                for (keel::i64 k = 0; k < 100000; ++k) {
                    keel::f64 cur = addCalendarMonths(startSd, cp->months * k, cp->days * k, cp->seconds * keel::as<keel::f64>(k));
                    if ((approxStep > 0.0 && cur > stopSd + kTol)
                        || (approxStep < 0.0 && cur < stopSd - kTol))
                        break;
                    sds.push_back(cur);
                }
            } else {
                keel::f64 stepDays = 1.0;
                if (args.size() == 3) {
                    if (args[1].isObject() && args[1].objectClassNameRef() == "duration")
                        stepDays = durElem(args[1], 0)->seconds / kSecondsPerDay;
                    else if (args[1].isNumeric())
                        stepDays = args[1].toScalar();
                    else
                        throw std::runtime_error("numkit:datetime:InvalidColonStep: Unsupported colon step type.");
                }
                if (stepDays == 0.0)
                    throw std::runtime_error("numkit:datetime:ZeroColonStep: Colon step cannot be zero.");
                for (keel::i64 k = 0; k < 100000; ++k) {
                    keel::f64 cur = startSd + keel::as<keel::f64>(k) * stepDays;
                    if ((stepDays > 0.0 && cur > stopSd + kTol)
                        || (stepDays < 0.0 && cur < stopSd - kTol))
                        break;
                    sds.push_back(cur);
                }
            }
            outs[0] = makeDatetimeArray(Dims(1, sds.size()), sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });

        dt.dispText = [hasNonZeroTime](const Value &self) -> keel::String {
            const keel::usize R = self.dims().rows(), C = self.dims().cols();
            keel::String os;
            if (R * C == 0) {
                keel::format_into(os, "  {}\xC3\x97{} empty datetime array\n", R, C);
                return os;
            }
            bool anyTime = hasNonZeroTime(self);
            if (R * C > 1)
                keel::format_into(os, "  {}\xC3\x97{} datetime array\n\n", R, C);
            else
                os.append("  datetime\n\n");
            for (keel::usize r = 0; r < R; ++r) {
                os.append("   ");
                for (keel::usize c = 0; c < C; ++c) {
                    const auto *p = dtElem(self, c * R + r);
                    os.push_back(' ');
                    os.append(formatDateTimeElem(
                        p ? p->serialDay : std::numeric_limits<keel::f64>::quiet_NaN(),
                        p ? p->format.as_ref() : keel::StringRef(""), anyTime).as_ref());
                    if (c + 1 < C) os.append("   ");
                }
                os.push_back('\n');
            }
            return os;
        };

        engine.registerClass(std::move(dt));
    }
}

} // namespace numkit::builtin::detail
