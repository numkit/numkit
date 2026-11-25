// src/builtin/register/timefun_internal.hpp
// Shared civil-calendar helpers and BuiltinClass payloads for datetime / duration / calendarDuration.
#pragma once

#include <keel/array.h>
#include <keel/algo.h>
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
#include <numkit/value/error.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/object.hpp>
#include <numkit/value/value.hpp>

#include <keel/number.h>


namespace numkit::builtin::detail::timefun {

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
    const keel::u32 yoe = keel::as<keel::u32>(y - era * 400);
    const keel::u32 doy = (153 * (keel::as<keel::u32>(m) + (m > 2 ? -3 : 9)) + 2) / 5 + 0;
    const keel::u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + keel::as<keel::i64>(doe) - 719468 + (d - 1);
}

inline void civilFromDays(keel::i64 z, keel::i64 &y, keel::i64 &m, keel::i64 &d) noexcept
{
    z += 719468;
    const keel::i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const keel::u32 doe = keel::as<keel::u32>(z - era * 146097);
    const keel::u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = keel::as<keel::i64>(yoe) + era * 400;
    const keel::u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const keel::u32 mp = (5 * doy + 2) / 153;
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
    if (keel::is_nan(Y) || keel::is_nan(M) || keel::is_nan(D)
        || keel::is_nan(H) || keel::is_nan(MI) || keel::is_nan(S))
        return keel::Limits<keel::f64>::quiet_nan();
    keel::i64 yi = keel::as<keel::i64>(keel::floor(Y));
    keel::i64 mi = keel::as<keel::i64>(keel::floor(M));
    keel::i64 di = keel::as<keel::i64>(keel::floor(D));
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
    if (!keel::is_finite(serialDay)) {
        out.isNaT = true;
        return out;
    }
    // Round to nearest millisecond to avoid FP drift in H:MI:S extraction
    keel::f64 shifted = serialDay - kEpochSerialDay;
    keel::i64 wholeDays = keel::as<keel::i64>(keel::floor(shifted));
    keel::f64 remSec = (shifted - keel::as<keel::f64>(wholeDays)) * kSecondsPerDay;
    keel::f64 roundedMs = keel::round(remSec * 1000.0);
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
    if (!keel::is_finite(serialDay))
        return keel::Limits<keel::f64>::quiet_nan();
    CivilDateTime c = decomposeSerialDay(serialDay);
    keel::i64 m0 = (c.month - 1) + deltaMonths;
    keel::i64 yNew = c.year + (m0 >= 0 ? m0 / 12 : (m0 - 11) / 12);
    keel::i64 mNew = m0 - (yNew - c.year) * 12 + 1;
    keel::i64 dNew = keel::min(c.day, daysInMonth(yNew, mNew));
    return composeSerialDay(keel::as<keel::f64>(yNew), keel::as<keel::f64>(mNew),
                            keel::as<keel::f64>(dNew + deltaDays),
                            keel::as<keel::f64>(c.hour), keel::as<keel::f64>(c.minute),
                            c.second + deltaSeconds);
}

inline constexpr const char *kMonthShort[12] = {
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
    keel::String y4 = keel::format("{:04}", keel::as<keel::i64>(c.year));
    keel::String m2 = keel::format("{:02}", keel::as<keel::i64>(c.month));
    keel::String d2 = keel::format("{:02}", keel::as<keel::i64>(c.day));
    keel::String h2 = keel::format("{:02}", keel::as<keel::i64>(c.hour));
    keel::String mi2 = keel::format("{:02}", keel::as<keel::i64>(c.minute));
    keel::i32 wholeSec = keel::as<keel::i32>(keel::floor(c.second + 1e-9));
    keel::f64 fracSec = c.second - wholeSec;
    keel::String s2 = keel::format("{:02}", wholeSec);
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
            keel::format_into(res, "{:02}", keel::as<keel::i64>(c.year % 100));
            i += 2;
        } else if (matchAt(i, "QQQ")) {
            keel::i32 q = keel::as<keel::i32>((c.month - 1) / 3 + 1);
            keel::format_into(res, "Q{}", q);
            i += 3;
        } else if (matchAt(i, "QQ")) {
            keel::i32 q = keel::as<keel::i32>((c.month - 1) / 3 + 1);
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
            keel::i32 ms = keel::as<keel::i32>(keel::round(fracSec * 1000.0));
            keel::format_into(res, "{}.{:03}", s2, ms);
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
        return keel::Limits<keel::f64>::quiet_nan();
    keel::String sl = s.clone();
    for (keel::usize i = 0; i < sl.size(); ++i)
        sl[i] = keel::to_ascii_lower(sl[i]);
    if (sl == "now")
        return numkit::builtin::now();
    if (sl == "today")
        return keel::floor(numkit::builtin::now());
    if (sl == "yesterday")
        return keel::floor(numkit::builtin::now()) - 1.0;
    if (sl == "tomorrow")
        return keel::floor(numkit::builtin::now()) + 1.0;

    auto skipWs = [](const char *&p) {
        while (*p && keel::is_ascii_space(*p))
            ++p;
    };
    auto readInt = [&](const char *&p, keel::i32 &out) -> bool {
        skipWs(p);
        const char *q = p;
        if (*q == '+' || *q == '-') ++q;
        const char *dstart = q;
        while (*q && keel::is_ascii_digit(*q)) ++q;
        if (q == dstart) return false;
        auto parsed = keel::parse_i64(keel::StringRef(p, keel::as<keel::usize>(q - p)));
        if (!parsed.is_ok()) return false;
        out = keel::as<keel::i32>(parsed.value());
        p = q;
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
        const char *q = p;
        if (*q == '+' || *q == '-') ++q;
        const char *dstart = q;
        while (*q && keel::is_ascii_digit(*q)) ++q;
        if (*q == '.') {
            ++q;
            while (*q && keel::is_ascii_digit(*q)) ++q;
        }
        if (q == dstart || (q == dstart + 1 && *dstart == '.')) return false;
        if (*q == 'e' || *q == 'E') {
            const char *eq = q + 1;
            if (*eq == '+' || *eq == '-') ++eq;
            const char *ed = eq;
            while (*eq && keel::is_ascii_digit(*eq)) ++eq;
            if (eq > ed) q = eq;
        }
        auto parsed = keel::parse_f64(keel::StringRef(p, keel::as<keel::usize>(q - p)));
        if (!parsed.is_ok()) return false;
        out = parsed.value();
        p = q;
        return true;
    };

    // Try ISO YYYY-MM-DD [HH:MM:SS[.fff]] or YYYY/MM/DD
    keel::i32 y = 0, m = 0, d = 0, hh = 0, mi = 0;
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
        keel::i32 a = 0, b = 0, c = 0;
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
        return keel::Limits<keel::f64>::quiet_nan();
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
    if (!keel::is_finite(sec))
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
    keel::i64 totalMs = keel::as<keel::i64>(keel::round(a * 1000.0));
    keel::i64 wholeSec = totalMs / 1000;
    keel::i64 ms = totalMs % 1000;
    keel::i64 hh = wholeSec / 3600;
    keel::i64 mm = (wholeSec % 3600) / 60;
    keel::i64 ss = wholeSec % 60;
    if (ms == 0) {
        return keel::format("{}{:02}:{:02}:{:02}",
                            neg ? "-" : "", hh, mm, ss);
    }
    return keel::format("{}{:02}:{:02}:{:02}.{:03}",
                        neg ? "-" : "", hh, mm, ss, ms);
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
    throw numkit::Error(
        keel::format("numkit:{}:DimMismatch: Arrays have incompatible sizes for this operation.", fnName).c_str());
}

inline Value extractDatetimeComponent(const Value &self, keel::i32 comp, keel::Allocator *mr)
{
    const keel::usize n = self.objectCount();
    auto compOf = [&](keel::f64 sd) -> keel::f64 {
        CivilDateTime c = decomposeSerialDay(sd);
        if (c.isNaT)
            return keel::Limits<keel::f64>::quiet_nan();
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
        return Value::scalar(compOf(p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan()), mr);
    }
    Value out = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
    keel::f64 *dst = out.doubleDataMut();
    for (keel::usize i = 0; i < n; ++i) {
        const auto *p = dtElem(self, i);
        dst[i] = compOf(p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan());
    }
    return out;
}

inline void mutateDatetimeComponent(Value &self, keel::i32 comp, const Value &val)
{
    const keel::usize n = self.objectCount();
    const keel::usize nv = val.numel();
    if (nv != 1 && nv != n)
        throw numkit::Error("numkit:datetime:SizeMismatch: Assigned value must be a scalar or match the datetime array size.");
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
        throw numkit::Error(
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
        throw numkit::Error(
            keel::format("numkit:{}:InvalidInput: Expected a numeric integer array.", fnName).c_str());
    const keel::usize n = x.numel();
    keel::Array<CalDurElemPayload> items(mr);
    items.resize(n);
    for (keel::usize i = 0; i < n; ++i) {
        keel::f64 v = x.elemAsDouble(i);
        if (!keel::is_finite(v) || keel::floor(v) != v)
            throw numkit::Error(
                keel::format("numkit:{}:MustBeInteger: Calendar duration quantities must be finite integers.", fnName).c_str());
        keel::i64 iv = keel::as<keel::i64>(v);
        items[i] = CalDurElemPayload(iv * monthMult, iv * dayMult, 0.0);
    }
    return makeCalDurArray(x.dims(), items.as_ref(), mr);
}

void register_duration_and_caldur_classes(Engine &engine);
void register_datetime_class(Engine &engine);

} // namespace numkit::builtin::detail::timefun
