// src/bundle/src/register/builtin/timefun_reg.cpp

#include <numkit/builtin/timefun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/object.hpp>
#include <numkit/value/span.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace numkit::builtin::detail {

namespace {

// ── Proleptic Gregorian civil calendar <-> serial day helpers ───────────────
// In the reference engine's serial date system:
//   0000-01-01 00:00:00 is serial day 1.0
//   1970-01-01 00:00:00 is serial day 719529.0
// Howard Hinnant's civil_from_days / days_from_civil uses 1970-01-01 = 0.
constexpr double kEpochSerialDay = 719529.0;
constexpr double kSecondsPerDay  = 86400.0;
constexpr double kSecondsPerYear = 365.2425 * 86400.0;

inline int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) noexcept
{
    // Normalize month overflow/underflow into year
    int64_t m0 = m - 1;
    int64_t yAdj = y + (m0 >= 0 ? m0 / 12 : (m0 - 11) / 12);
    int64_t mAdj = m0 - (yAdj - y) * 12 + 1;
    y = yAdj;
    m = mAdj;
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (static_cast<unsigned>(m) + (m > 2 ? -3 : 9)) + 2) / 5 + 0;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468 + (d - 1);
}

inline void civilFromDays(int64_t z, int64_t &y, int64_t &m, int64_t &d) noexcept
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int64_t>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int64_t>(mp) + (mp < 10 ? 3 : -9);
    y += (m <= 2) ? 1 : 0;
}

inline bool isLeapYear(int64_t y) noexcept
{
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

inline int64_t daysInMonth(int64_t y, int64_t m) noexcept
{
    static const int64_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int64_t m0 = ((m - 1) % 12 + 12) % 12;
    if (m0 == 1 && isLeapYear(y))
        return 29;
    return kDays[m0];
}

inline double composeSerialDay(double Y, double M, double D,
                               double H = 0.0, double MI = 0.0, double S = 0.0) noexcept
{
    if (std::isnan(Y) || std::isnan(M) || std::isnan(D)
        || std::isnan(H) || std::isnan(MI) || std::isnan(S))
        return std::numeric_limits<double>::quiet_NaN();
    int64_t yi = static_cast<int64_t>(std::floor(Y));
    int64_t mi = static_cast<int64_t>(std::floor(M));
    int64_t di = static_cast<int64_t>(std::floor(D));
    double fracD = (D - static_cast<double>(di))
                   + (H * 3600.0 + MI * 60.0 + S) / kSecondsPerDay;
    int64_t daysSince1970 = daysFromCivil(yi, mi, di);
    return static_cast<double>(daysSince1970) + kEpochSerialDay + fracD;
}

struct CivilDateTime
{
    int64_t year = 0, month = 1, day = 1, hour = 0, minute = 0;
    double second = 0.0;
    bool isNaT = false;
};

inline CivilDateTime decomposeSerialDay(double serialDay) noexcept
{
    CivilDateTime out;
    if (!std::isfinite(serialDay)) {
        out.isNaT = true;
        return out;
    }
    // Round to nearest millisecond to avoid FP drift in H:MI:S extraction
    double shifted = serialDay - kEpochSerialDay;
    int64_t wholeDays = static_cast<int64_t>(std::floor(shifted));
    double remSec = (shifted - static_cast<double>(wholeDays)) * kSecondsPerDay;
    double roundedMs = std::round(remSec * 1000.0);
    if (roundedMs >= 86400000.0) {
        wholeDays += 1;
        roundedMs -= 86400000.0;
    } else if (roundedMs < 0.0) {
        wholeDays -= 1;
        roundedMs += 86400000.0;
    }
    civilFromDays(wholeDays, out.year, out.month, out.day);
    int64_t totalWholeSec = static_cast<int64_t>(roundedMs / 1000.0);
    double msPart = roundedMs - static_cast<double>(totalWholeSec) * 1000.0;
    out.hour = totalWholeSec / 3600;
    out.minute = (totalWholeSec % 3600) / 60;
    out.second = static_cast<double>(totalWholeSec % 60) + msPart / 1000.0;
    return out;
}

inline double addCalendarMonths(double serialDay, int64_t deltaMonths,
                                int64_t deltaDays, double deltaSeconds) noexcept
{
    if (!std::isfinite(serialDay))
        return std::numeric_limits<double>::quiet_NaN();
    CivilDateTime c = decomposeSerialDay(serialDay);
    int64_t m0 = (c.month - 1) + deltaMonths;
    int64_t yNew = c.year + (m0 >= 0 ? m0 / 12 : (m0 - 11) / 12);
    int64_t mNew = m0 - (yNew - c.year) * 12 + 1;
    int64_t dNew = std::min(c.day, daysInMonth(yNew, mNew));
    return composeSerialDay(static_cast<double>(yNew), static_cast<double>(mNew),
                            static_cast<double>(dNew + deltaDays),
                            static_cast<double>(c.hour), static_cast<double>(c.minute),
                            c.second + deltaSeconds);
}

static const char *kMonthShort[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

inline std::string formatDateTimeElem(double serialDay, const std::string &fmt, bool anyTimeInArray)
{
    CivilDateTime c = decomposeSerialDay(serialDay);
    if (c.isNaT)
        return "NaT";
    std::string f = fmt;
    if (f.empty()) {
        f = anyTimeInArray ? "dd-MMM-uuuu HH:mm:ss" : "dd-MMM-uuuu";
    }
    char y4[16], m2[8], d2[8], h2[8], mi2[8], s2[16];
    std::snprintf(y4, sizeof(y4), "%04lld", static_cast<long long>(c.year));
    std::snprintf(m2, sizeof(m2), "%02lld", static_cast<long long>(c.month));
    std::snprintf(d2, sizeof(d2), "%02lld", static_cast<long long>(c.day));
    std::snprintf(h2, sizeof(h2), "%02lld", static_cast<long long>(c.hour));
    std::snprintf(mi2, sizeof(mi2), "%02lld", static_cast<long long>(c.minute));
    int wholeSec = static_cast<int>(std::floor(c.second + 1e-9));
    double fracSec = c.second - wholeSec;
    std::snprintf(s2, sizeof(s2), "%02d", wholeSec);
    const char *mmm = (c.month >= 1 && c.month <= 12) ? kMonthShort[c.month - 1] : "Jan";

    if (f == "dd-MMM-uuuu" || f == "dd-MMM-yyyy")
        return std::string(d2) + "-" + mmm + "-" + y4;
    if (f == "dd-MMM-uuuu HH:mm:ss" || f == "dd-MMM-yyyy HH:mm:ss")
        return std::string(d2) + "-" + mmm + "-" + y4 + " " + h2 + ":" + mi2 + ":" + s2;
    if (f == "yyyy-MM-dd" || f == "uuuu-MM-dd")
        return std::string(y4) + "-" + m2 + "-" + d2;
    if (f == "yyyy-MM-dd HH:mm:ss" || f == "uuuu-MM-dd HH:mm:ss")
        return std::string(y4) + "-" + m2 + "-" + d2 + " " + h2 + ":" + mi2 + ":" + s2;

    // Generic token replacement for custom Format strings
    std::string res;
    for (size_t i = 0; i < f.size();) {
        if (f.compare(i, 4, "uuuu") == 0 || f.compare(i, 4, "yyyy") == 0) {
            res += y4;
            i += 4;
        } else if (f.compare(i, 2, "uu") == 0 || f.compare(i, 2, "yy") == 0) {
            char y2[8];
            std::snprintf(y2, sizeof(y2), "%02lld", static_cast<long long>(c.year % 100));
            res += y2;
            i += 2;
        } else if (f.compare(i, 3, "QQQ") == 0) {
            int q = static_cast<int>((c.month - 1) / 3 + 1);
            res += "Q" + std::to_string(q);
            i += 3;
        } else if (f.compare(i, 2, "QQ") == 0) {
            int q = static_cast<int>((c.month - 1) / 3 + 1);
            res += "Q" + std::to_string(q);
            i += 2;
        } else if (f.compare(i, 3, "MMM") == 0) {
            res += mmm;
            i += 3;
        } else if (f.compare(i, 2, "MM") == 0) {
            res += m2;
            i += 2;
        } else if (f.compare(i, 2, "dd") == 0) {
            res += d2;
            i += 2;
        } else if (f.compare(i, 2, "HH") == 0) {
            res += h2;
            i += 2;
        } else if (f.compare(i, 2, "mm") == 0) {
            res += mi2;
            i += 2;
        } else if (f.compare(i, 6, "ss.SSS") == 0) {
            int ms = static_cast<int>(std::round(fracSec * 1000.0));
            char msBuf[16];
            std::snprintf(msBuf, sizeof(msBuf), "%s.%03d", s2, ms);
            res += msBuf;
            i += 6;
        } else if (f.compare(i, 2, "ss") == 0) {
            res += s2;
            i += 2;
        } else {
            res.push_back(f[i++]);
        }
    }
    return res;
}

inline double parseDateTimeString(const std::string &raw, std::pmr::memory_resource *mr)
{
    std::string s = raw;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    if (s.empty() || s == "NaT")
        return std::numeric_limits<double>::quiet_NaN();
    std::string sl = s;
    for (auto &ch : sl)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (sl == "now")
        return numkit::builtin::now();
    if (sl == "today")
        return std::floor(numkit::builtin::now());
    if (sl == "yesterday")
        return std::floor(numkit::builtin::now()) - 1.0;
    if (sl == "tomorrow")
        return std::floor(numkit::builtin::now()) + 1.0;

    // Try ISO YYYY-MM-DD [HH:MM:SS[.fff]] or YYYY/MM/DD
    int y = 0, m = 0, d = 0, hh = 0, mi = 0;
    double sec = 0.0;
    char sep1 = 0, sep2 = 0;
    {
        std::istringstream iss(s);
        if ((iss >> y >> sep1 >> m >> sep2 >> d)
            && (sep1 == '-' || sep1 == '/') && sep1 == sep2 && y >= 1000) {
            char tsep = 0;
            if (iss >> std::ws && !iss.eof()) {
                if (iss.peek() == 'T' || iss.peek() == 't')
                    iss.get();
                char c1 = 0, c2 = 0;
                if (iss >> hh >> c1 >> mi) {
                    if (iss >> c2 >> sec) {
                        (void)c2;
                    }
                }
            }
            return composeSerialDay(y, m, d, hh, mi, sec);
        }
    }
    // Delegate to existing datenum parser (handles dd-MMM-yyyy, MM/dd/yyyy, etc.)
    {
        std::istringstream iss(s);
        int a = 0, b = 0, c = 0;
        char c1 = 0, c2 = 0;
        if ((iss >> a >> c1 >> b >> c2 >> c) && (c1 == '/' || c1 == '-') && c1 == c2 && c >= 1000 && a >= 1 && a <= 12 && b >= 1 && b <= 31) {
            return composeSerialDay(c, a, b, 0, 0, 0.0);
        }
    }
    try {
        Value sv = Value::fromString(s, mr);
        Value dn = numkit::builtin::datenum(Span<const Value>(&sv, 1), mr);
        return dn.toScalar();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

// ── Payloads ────────────────────────────────────────────────────────────────

struct DatetimeElemPayload : NativePayload
{
    double serialDay = 0.0;
    std::string format;
    std::string timeZone;

    DatetimeElemPayload() = default;
    DatetimeElemPayload(double sd, std::string fmt = "", std::string tz = "")
        : serialDay(sd), format(std::move(fmt)), timeZone(std::move(tz)) {}

    std::shared_ptr<NativePayload> clone() const override
    {
        return std::make_shared<DatetimeElemPayload>(*this);
    }

    double asDouble() const override { return serialDay; }
};

struct DurationElemPayload : NativePayload
{
    double seconds = 0.0;
    std::string format = "hh:mm:ss";

    DurationElemPayload() = default;
    DurationElemPayload(double sec, std::string fmt = "hh:mm:ss")
        : seconds(sec), format(std::move(fmt)) {}

    std::shared_ptr<NativePayload> clone() const override
    {
        return std::make_shared<DurationElemPayload>(*this);
    }

    double asDouble() const override { return seconds; }
};

struct CalDurElemPayload : NativePayload
{
    int64_t months = 0;
    int64_t days = 0;
    double seconds = 0.0;

    CalDurElemPayload() = default;
    CalDurElemPayload(int64_t m, int64_t d, double s = 0.0)
        : months(m), days(d), seconds(s) {}

    std::shared_ptr<NativePayload> clone() const override
    {
        return std::make_shared<CalDurElemPayload>(*this);
    }
};

inline const DatetimeElemPayload *dtElem(const Value &v, size_t i = 0)
{
    const ObjectState *st = v.objectStateAt(i);
    return st ? static_cast<const DatetimeElemPayload *>(st->native.get()) : nullptr;
}

inline DatetimeElemPayload *dtElemMut(Value &v, size_t i = 0)
{
    ObjectState *st = v.objectStateMutAt(i);
    return st ? static_cast<DatetimeElemPayload *>(st->native.get()) : nullptr;
}

inline const DurationElemPayload *durElem(const Value &v, size_t i = 0)
{
    const ObjectState *st = v.objectStateAt(i);
    return st ? static_cast<const DurationElemPayload *>(st->native.get()) : nullptr;
}

inline DurationElemPayload *durElemMut(Value &v, size_t i = 0)
{
    ObjectState *st = v.objectStateMutAt(i);
    return st ? static_cast<DurationElemPayload *>(st->native.get()) : nullptr;
}

inline const CalDurElemPayload *calElem(const Value &v, size_t i = 0)
{
    const ObjectState *st = v.objectStateAt(i);
    return st ? static_cast<const CalDurElemPayload *>(st->native.get()) : nullptr;
}

inline Value makeArrayFromDims(const Dims &d, ValueType t, std::pmr::memory_resource *mr)
{
    return Value::matrix(d.rows(), d.cols(), t, mr);
}

inline Value makeDatetimeArray(const Dims &dims, const std::vector<double> &serialDays,
                               const std::string &fmt, const std::string &tz,
                               std::pmr::memory_resource *mr)
{
    if (!mr)
        mr = std::pmr::get_default_resource();
    std::vector<std::shared_ptr<ObjectState>> states;
    states.reserve(serialDays.size());
    for (double sd : serialDays) {
        auto st = std::make_shared<ObjectState>(mr);
        st->native = std::make_shared<DatetimeElemPayload>(sd, fmt, tz);
        states.push_back(std::move(st));
    }
    if (dims.numel() == 1 && states.size() == 1)
        return Value::object("datetime", std::move(states[0]), /*isHandle=*/false, mr);
    return Value::objectArray("datetime", dims, std::move(states), /*isHandle=*/false, mr);
}

inline Value makeDurationArray(const Dims &dims, const std::vector<double> &secs,
                               const std::string &fmt, std::pmr::memory_resource *mr)
{
    if (!mr)
        mr = std::pmr::get_default_resource();
    std::vector<std::shared_ptr<ObjectState>> states;
    states.reserve(secs.size());
    for (double s : secs) {
        auto st = std::make_shared<ObjectState>(mr);
        st->native = std::make_shared<DurationElemPayload>(s, fmt);
        states.push_back(std::move(st));
    }
    if (dims.numel() == 1 && states.size() == 1)
        return Value::object("duration", std::move(states[0]), /*isHandle=*/false, mr);
    return Value::objectArray("duration", dims, std::move(states), /*isHandle=*/false, mr);
}

inline Value makeCalDurArray(const Dims &dims, const std::vector<CalDurElemPayload> &items,
                             std::pmr::memory_resource *mr)
{
    if (!mr)
        mr = std::pmr::get_default_resource();
    std::vector<std::shared_ptr<ObjectState>> states;
    states.reserve(items.size());
    for (const auto &it : items) {
        auto st = std::make_shared<ObjectState>(mr);
        st->native = std::make_shared<CalDurElemPayload>(it);
        states.push_back(std::move(st));
    }
    if (dims.numel() == 1 && states.size() == 1)
        return Value::object("calendarDuration", std::move(states[0]), /*isHandle=*/false, mr);
    return Value::objectArray("calendarDuration", dims, std::move(states), /*isHandle=*/false, mr);
}

inline std::string formatDurationElem(double sec, const std::string &fmt)
{
    if (!std::isfinite(sec))
        return "NaN";
    std::ostringstream os;
    if (fmt == "s" || fmt == "sec") {
        os << sec << " sec";
        return os.str();
    }
    if (fmt == "m" || fmt == "min") {
        os << (sec / 60.0) << " min";
        return os.str();
    }
    if (fmt == "h" || fmt == "hr") {
        os << (sec / 3600.0) << " hr";
        return os.str();
    }
    if (fmt == "d" || fmt == "day") {
        os << (sec / kSecondsPerDay) << " days";
        return os.str();
    }
    if (fmt == "y" || fmt == "yr") {
        os << (sec / kSecondsPerYear) << " yr";
        return os.str();
    }
    bool neg = sec < 0.0;
    double a = std::abs(sec);
    int64_t totalMs = static_cast<int64_t>(std::round(a * 1000.0));
    int64_t wholeSec = totalMs / 1000;
    int64_t ms = totalMs % 1000;
    int64_t hh = wholeSec / 3600;
    int64_t mm = (wholeSec % 3600) / 60;
    int64_t ss = wholeSec % 60;
    char buf[64];
    if (ms == 0) {
        std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld",
                      neg ? "-" : "",
                      static_cast<long long>(hh),
                      static_cast<long long>(mm),
                      static_cast<long long>(ss));
    } else {
        std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld.%03lld",
                      neg ? "-" : "",
                      static_cast<long long>(hh),
                      static_cast<long long>(mm),
                      static_cast<long long>(ss),
                      static_cast<long long>(ms));
    }
    return buf;
}

inline std::string formatCalDurElem(const CalDurElemPayload &cd)
{
    std::ostringstream os;
    int64_t y = cd.months / 12;
    int64_t m = cd.months % 12;
    bool any = false;
    if (y != 0) {
        os << y << "y";
        any = true;
    }
    if (m != 0) {
        if (any) os << " ";
        os << m << "m";
        any = true;
    }
    if (cd.days != 0 || (!any && cd.seconds == 0.0)) {
        if (any) os << " ";
        os << cd.days << "d";
        any = true;
    }
    if (cd.seconds != 0.0) {
        if (any) os << " ";
        os << formatDurationElem(cd.seconds, "hh:mm:ss");
    }
    return os.str();
}

inline Dims broadcastDims(const Value &a, const Value &b, const char *fnName)
{
    const size_t na = a.isObject() ? a.objectCount() : a.numel();
    const size_t nb = b.isObject() ? b.objectCount() : b.numel();
    if (na == 1)
        return b.dims();
    if (nb == 1)
        return a.dims();
    if (a.dims() == b.dims())
        return a.dims();
    if (na == nb)
        return a.dims();
    throw std::runtime_error(std::string("nk:") + fnName
                             + ":DimMismatch: Arrays have incompatible sizes for this operation.");
}

inline Value extractDatetimeComponent(const Value &self, int comp, std::pmr::memory_resource *mr)
{
    const size_t n = self.objectCount();
    auto compOf = [&](double sd) -> double {
        CivilDateTime c = decomposeSerialDay(sd);
        if (c.isNaT)
            return std::numeric_limits<double>::quiet_NaN();
        switch (comp) {
        case 0: return static_cast<double>(c.year);
        case 1: return static_cast<double>(c.month);
        case 2: return static_cast<double>(c.day);
        case 3: return static_cast<double>(c.hour);
        case 4: return static_cast<double>(c.minute);
        case 5: return c.second;
        case 6: return static_cast<double>((c.month - 1) / 3 + 1); // quarter
        default: return 0.0;
        }
    };
    if (n == 1) {
        const auto *p = dtElem(self, 0);
        return Value::scalar(compOf(p ? p->serialDay : std::numeric_limits<double>::quiet_NaN()), mr);
    }
    Value out = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
    double *dst = out.doubleDataMut();
    for (size_t i = 0; i < n; ++i) {
        const auto *p = dtElem(self, i);
        dst[i] = compOf(p ? p->serialDay : std::numeric_limits<double>::quiet_NaN());
    }
    return out;
}

inline void mutateDatetimeComponent(Value &self, int comp, const Value &val)
{
    const size_t n = self.objectCount();
    const size_t nv = val.numel();
    if (nv != 1 && nv != n)
        throw std::runtime_error("nk:datetime:SizeMismatch: Assigned value must be a scalar or match the datetime array size.");
    for (size_t i = 0; i < n; ++i) {
        auto *p = dtElemMut(self, i);
        if (!p)
            continue;
        double v = (nv == 1) ? val.toScalar() : val.elemAsDouble(i);
        CivilDateTime c = decomposeSerialDay(p->serialDay);
        double y = static_cast<double>(c.year);
        double m = static_cast<double>(c.month);
        double d = static_cast<double>(c.day);
        double h = static_cast<double>(c.hour);
        double mi = static_cast<double>(c.minute);
        double s = c.second;
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

inline Value constructDurationFromUnit(Span<const Value> args, double scaleSeconds,
                                       const char *defaultFmt, const char *fnName,
                                       std::pmr::memory_resource *mr)
{
    argcheck::checkNargin(args.size(), 1, 1, fnName);
    const Value &x = args[0];
    if (x.isObject() && x.objectClassName() == "duration") {
        // When called on a duration, extract numeric units!
        const size_t n = x.objectCount();
        if (n == 1) {
            const auto *p = durElem(x, 0);
            return Value::scalar((p ? p->seconds : 0.0) / scaleSeconds, mr);
        }
        Value out = makeArrayFromDims(x.dims(), ValueType::DOUBLE, mr);
        double *dst = out.doubleDataMut();
        for (size_t i = 0; i < n; ++i) {
            const auto *p = durElem(x, i);
            dst[i] = (p ? p->seconds : 0.0) / scaleSeconds;
        }
        return out;
    }
    if (!x.isNumeric() && !x.isLogical())
        throw std::runtime_error(std::string("nk:") + fnName
                                 + ":InvalidInput: Expected a numeric or duration input.");
    const size_t n = x.numel();
    std::vector<double> secs(n);
    for (size_t i = 0; i < n; ++i)
        secs[i] = x.elemAsDouble(i) * scaleSeconds;
    return makeDurationArray(x.dims(), secs, defaultFmt, mr);
}

inline Value constructCalDurFromUnit(Span<const Value> args, int64_t monthMult, int64_t dayMult,
                                     const char *fnName, std::pmr::memory_resource *mr)
{
    argcheck::checkNargin(args.size(), 1, 1, fnName);
    const Value &x = args[0];
    if (x.isObject() && x.objectClassName() == "calendarDuration") {
        const size_t n = x.objectCount();
        auto extract = [&](const CalDurElemPayload *p) -> double {
            if (!p) return 0.0;
            if (monthMult != 0)
                return static_cast<double>(p->months) / static_cast<double>(monthMult);
            return static_cast<double>(p->days) / static_cast<double>(dayMult);
        };
        if (n == 1)
            return Value::scalar(extract(calElem(x, 0)), mr);
        Value out = makeArrayFromDims(x.dims(), ValueType::DOUBLE, mr);
        double *dst = out.doubleDataMut();
        for (size_t i = 0; i < n; ++i)
            dst[i] = extract(calElem(x, i));
        return out;
    }
    if (!x.isNumeric() && !x.isLogical())
        throw std::runtime_error(std::string("nk:") + fnName
                                 + ":InvalidInput: Expected a numeric integer array.");
    const size_t n = x.numel();
    std::vector<CalDurElemPayload> items(n);
    for (size_t i = 0; i < n; ++i) {
        double v = x.elemAsDouble(i);
        if (!std::isfinite(v) || std::floor(v) != v)
            throw std::runtime_error(std::string("nk:") + fnName
                                     + ":MustBeInteger: Calendar duration quantities must be finite integers.");
        int64_t iv = static_cast<int64_t>(v);
        items[i] = CalDurElemPayload(iv * monthMult, iv * dayMult, 0.0);
    }
    return makeCalDurArray(x.dims(), items, mr);
}

} // namespace

void register_timefun(Engine &engine) {
    engine.registerFunction("clock",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "clock");
            outs[0] = numkit::builtin::clock(ctx.engine->resource());
        });

    engine.registerFunction("date",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "date");
            outs[0] = Value::fromString(numkit::builtin::date(), ctx.engine->resource());
        });

    engine.registerFunction("pause",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &) {
            argcheck::checkNargin(args.size(), 0, 1, "pause");
            double s = args.empty() ? 0.0 : args[0].toScalar();
            numkit::builtin::pause(s);
            outs[0] = Value();
        });

    engine.registerFunction("tic",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "tic");
            auto now = Clock::now();
            ctx.engine->setTicTimer(now);
            if (nargout > 0) {
                double id = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch()).count());
                outs[0] = Value::scalar(id, ctx.engine->resource());
            } else {
                outs[0] = Value();
            }
        });

    engine.registerFunction("toc",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 1, "toc");
            auto now = Clock::now();
            TimePoint start;
            if (!args.empty() && args[0].isScalar()) {
                auto us = static_cast<long long>(args[0].toScalar());
                start = TimePoint(std::chrono::microseconds(us));
            } else if (ctx.engine->ticWasCalled()) {
                start = ctx.engine->ticTimer();
            } else {
                throw std::runtime_error("toc: You must call 'tic' before calling 'toc'.");
            }
            double elapsed = std::chrono::duration<double>(now - start).count();
            if (nargout > 0) {
                outs[0] = Value::scalar(elapsed, ctx.engine->resource());
            } else {
                std::ostringstream os;
                os << "Elapsed time is " << elapsed << " seconds.\n";
                ctx.engine->outputText(os.str());
                outs[0] = Value();
            }
        });

    engine.registerFunction("cputime",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "cputime");
            outs[0] = Value::scalar(numkit::builtin::cputime(), ctx.engine->resource());
        });

    engine.registerFunction("now",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "now");
            outs[0] = Value::scalar(numkit::builtin::now(), ctx.engine->resource());
        });

    engine.registerFunction("etime",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 2, "etime");
            outs[0] = numkit::builtin::etime(args[0], args[1], ctx.engine->resource());
        });

    engine.registerFunction("weeknum",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "weeknum");
            outs[0] = numkit::builtin::weeknum(args, ctx.engine->resource());
        });

    engine.registerFunction("addtodate",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 3, 3, "addtodate");
            outs[0] = numkit::builtin::addtodate(args[0], args[1].toScalar(), args[2].toString(), ctx.engine->resource());
        });

    engine.registerFunction("datenum",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 6, "datenum");
            outs[0] = numkit::builtin::datenum(args, ctx.engine->resource());
        });

    engine.registerFunction("weekday",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "weekday");
            auto *mr = ctx.engine->resource();
            outs[0] = numkit::builtin::weekday(args[0], mr);
            if (nargout > 1) {
                bool wantLong = false;
                if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                    std::string fmt = args[1].toString();
                    for (auto &c : fmt)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (fmt == "long")
                        wantLong = true;
                    else if (fmt != "short")
                        throw std::runtime_error("weekday: format must be 'short' or 'long'");
                }
                static const char *kShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
                static const char *kLong[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
                double dval = args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(0);
                int64_t f = static_cast<int64_t>(std::floor(dval)) - 2;
                int64_t r = f % 7;
                if (r < 0) r += 7;
                int dayIdx = static_cast<int>(r) + 1;
                outs[1] = Value::fromString(wantLong ? kLong[dayIdx - 1] : kShort[dayIdx - 1], mr);
            }
        });

    engine.registerFunction("juliandate",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 6, "juliandate");
            outs[0] = numkit::builtin::juliandate(args, ctx.engine->resource());
        });

    engine.registerFunction("eomday",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 2, "eomday");
            outs[0] = numkit::builtin::eomday(args[0], args[1], ctx.engine->resource());
        });

    engine.registerFunction("calendar",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 2, "calendar");
            outs[0] = numkit::builtin::calendar(args, ctx.engine->resource());
        });

    engine.registerFunction("datestr",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "datestr");
            outs[0] = numkit::builtin::datestr(args, ctx.engine->resource());
        });

    engine.registerFunction("datevec",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "datevec");
            auto *mr = ctx.engine->resource();
            Value dv = numkit::builtin::datevec(args, mr);
            if (nargout <= 1) {
                outs[0] = std::move(dv);
            } else {
                const size_t N = dv.dims().rows();
                const double *o = dv.doubleData();
                for (size_t c = 0; c < std::min(nargout, size_t(6)); ++c) {
                    if (N == 1) {
                        outs[c] = Value::scalar(o[c * N], mr);
                    } else {
                        auto col = Value::matrix(N, 1, ValueType::DOUBLE, mr);
                        double *p = col.doubleDataMut();
                        for (size_t i = 0; i < N; ++i)
                            p[i] = o[i + c * N];
                        outs[c] = std::move(col);
                    }
                }
            }
        });

    engine.registerFunction("yyyymmdd",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "yyyymmdd");
            outs[0] = numkit::builtin::yyyymmdd(args, ctx.engine->resource());
        });

    engine.registerFunction("mjuliandate",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 6, "mjuliandate");
            outs[0] = numkit::builtin::mjuliandate(args, ctx.engine->resource());
        });

    // ── Duration & CalendarDuration unit constructors ──────────────────────
    engine.registerFunction("seconds",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 1.0, "s", "seconds", ctx.engine->resource());
        });
    engine.registerFunction("milliseconds",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 0.001, "s", "milliseconds", ctx.engine->resource());
        });
    engine.registerFunction("minutes",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 60.0, "m", "minutes", ctx.engine->resource());
        });
    engine.registerFunction("hours",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, 3600.0, "h", "hours", ctx.engine->resource());
        });
    engine.registerFunction("days",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, kSecondsPerDay, "d", "days", ctx.engine->resource());
        });
    engine.registerFunction("years",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructDurationFromUnit(args, kSecondsPerYear, "y", "years", ctx.engine->resource());
        });

    engine.registerFunction("caldays",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 0, 1, "caldays", ctx.engine->resource());
        });
    engine.registerFunction("calweeks",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 0, 7, "calweeks", ctx.engine->resource());
        });
    engine.registerFunction("calmonths",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 1, 0, "calmonths", ctx.engine->resource());
        });
    engine.registerFunction("calquarters",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 3, 0, "calquarters", ctx.engine->resource());
        });
    engine.registerFunction("calyears",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = constructCalDurFromUnit(args, 12, 0, "calyears", ctx.engine->resource());
        });

    engine.registerFunction("isdatetime",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "isdatetime");
            outs[0] = Value::logicalScalar(
                args[0].isObject() && args[0].objectClassName() == "datetime",
                ctx.engine->resource());
        });
    engine.registerFunction("isduration",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "isduration");
            outs[0] = Value::logicalScalar(
                args[0].isObject() && args[0].objectClassName() == "duration",
                ctx.engine->resource());
        });
    engine.registerFunction("iscalendarduration",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "iscalendarduration");
            outs[0] = Value::logicalScalar(
                args[0].isObject() && args[0].objectClassName() == "calendarDuration",
                ctx.engine->resource());
        });
    engine.registerFunction("isnat",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "isnat");
            auto *mr = ctx.engine->resource();
            if (!args[0].isObject() || args[0].objectClassName() != "datetime") {
                outs[0] = Value::logicalScalar(false, mr);
                return;
            }
            const size_t n = args[0].objectCount();
            if (n == 1) {
                const auto *p = dtElem(args[0], 0);
                outs[0] = Value::logicalScalar(!p || !std::isfinite(p->serialDay), mr);
                return;
            }
            Value out = makeArrayFromDims(args[0].dims(), ValueType::LOGICAL, mr);
            uint8_t *dst = out.logicalDataMut();
            for (size_t i = 0; i < n; ++i) {
                const auto *p = dtElem(args[0], i);
                dst[i] = (!p || !std::isfinite(p->serialDay)) ? 1 : 0;
            }
            outs[0] = std::move(out);
        });
    engine.registerFunction("NaT",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 4, "NaT");
            auto *mr = ctx.engine->resource();
            size_t r = 1, c = 1;
            if (args.size() == 1 && args[0].isNumeric()) {
                if (args[0].numel() == 1) {
                    r = c = static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(0)));
                } else if (args[0].numel() >= 2) {
                    r = static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(0)));
                    c = static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(1)));
                }
            } else if (args.size() >= 2 && args[0].isNumeric() && args[1].isNumeric()) {
                r = static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(0)));
                c = static_cast<size_t>(std::max(0.0, args[1].elemAsDouble(0)));
            }
            std::vector<double> nats(r * c, std::numeric_limits<double>::quiet_NaN());
            outs[0] = makeDatetimeArray(Dims(r, c), nats, "", "", mr);
        });

    // ── BuiltinClass: duration ─────────────────────────────────────────────
    {
        BuiltinClass dur;
        dur.name = "duration";
        dur.isHandle = false;
        dur.propNames = {"Format"};

        dur.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            std::string fmt = "hh:mm:ss";
            size_t nPos = args.size();
            while (nPos >= 2 && (args[nPos - 2].isChar() || args[nPos - 2].isString())) {
                std::string k = args[nPos - 2].toString();
                for (auto &ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (k == "format") {
                    fmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else {
                    break;
                }
            }
            if (nPos == 0)
                return makeDurationArray(Dims(1, 1), {0.0}, fmt, mr);
            if (nPos == 1 && args[0].isNumeric() && args[0].dims().cols() == 3) {
                const size_t R = args[0].dims().rows();
                std::vector<double> secs(R);
                for (size_t r = 0; r < R; ++r) {
                    double h = args[0].elemAsDouble(r);
                    double m = args[0].elemAsDouble(r + R);
                    double s = args[0].elemAsDouble(r + 2 * R);
                    secs[r] = h * 3600.0 + m * 60.0 + s;
                }
                return makeDurationArray(Dims(R, 1), secs, fmt, mr);
            }
            if (nPos == 3 || nPos == 4) {
                Dims d = broadcastDims(args[0], args[1], "duration");
                d = (d.numel() == 1) ? args[2].dims() : d;
                const size_t n = d.numel();
                std::vector<double> secs(n);
                for (size_t i = 0; i < n; ++i) {
                    double h = args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(i);
                    double m = args[1].numel() == 1 ? args[1].toScalar() : args[1].elemAsDouble(i);
                    double s = args[2].numel() == 1 ? args[2].toScalar() : args[2].elemAsDouble(i);
                    double ms = (nPos == 4)
                                    ? (args[3].numel() == 1 ? args[3].toScalar() : args[3].elemAsDouble(i))
                                    : 0.0;
                    secs[i] = h * 3600.0 + m * 60.0 + s + ms * 0.001;
                }
                return makeDurationArray(d, secs, fmt, mr);
            }
            throw std::runtime_error("nk:duration:InvalidArgs: Expected duration(H, MI, S) or duration([H MI S]).");
        };

        dur.propGet = [](const Value &self, const std::string &name, Value &out,
                         CallContext &ctx) -> bool {
            if (name == "Format") {
                const auto *p = durElem(self, 0);
                out = Value::fromString(p ? p->format : "hh:mm:ss", ctx.engine->resource());
                return true;
            }
            return false;
        };
        dur.propSet = [](Value &self, const std::string &name, const Value &val,
                         CallContext &) -> bool {
            if (name == "Format") {
                std::string f = val.toString();
                for (size_t i = 0; i < self.objectCount(); ++i)
                    if (auto *p = durElemMut(self, i))
                        p->format = f;
                return true;
            }
            return false;
        };

        auto addDurUnitMethod = [&](const char *mname, double scale) {
            dur.methods[mname] = [scale, mname](Value &self, Span<const Value>, size_t,
                                                Span<Value> outs, CallContext &ctx) {
                Value arg[1] = {self};
                outs[0] = constructDurationFromUnit(Span<const Value>(arg, 1), scale, "s",
                                                    mname, ctx.engine->resource());
            };
        };
        addDurUnitMethod("seconds", 1.0);
        addDurUnitMethod("milliseconds", 0.001);
        addDurUnitMethod("minutes", 60.0);
        addDurUnitMethod("hours", 3600.0);
        addDurUnitMethod("days", kSecondsPerDay);
        addDurUnitMethod("years", kSecondsPerYear);

        dur.methods["char"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                 CallContext &ctx) {
            const auto *p = durElem(self, 0);
            outs[0] = Value::fromString(
                p ? formatDurationElem(p->seconds, p->format) : "00:00:00",
                ctx.engine->resource());
        };
        dur.methods["string"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                   CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            Value s = Value::stringArray(self.dims().rows(), self.dims().cols(), mr);
            for (size_t i = 0; i < n; ++i) {
                const auto *p = durElem(self, i);
                s.stringElemSet(i, p ? formatDurationElem(p->seconds, p->format) : "00:00:00");
            }
            outs[0] = std::move(s);
        };
        dur.methods["hms"] = [](Value &self, Span<const Value>, size_t nargout, Span<Value> outs,
                                CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            Value h = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            Value m = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            Value s = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            for (size_t i = 0; i < n; ++i) {
                double sec = durElem(self, i) ? durElem(self, i)->seconds : 0.0;
                double sign = sec < 0.0 ? -1.0 : 1.0;
                double a = std::abs(sec);
                double hh = std::floor(a / 3600.0);
                double mm = std::floor((a - hh * 3600.0) / 60.0);
                double ss = a - hh * 3600.0 - mm * 60.0;
                h.doubleDataMut()[i] = sign * hh;
                m.doubleDataMut()[i] = sign * mm;
                s.doubleDataMut()[i] = sign * ss;
            }
            outs[0] = (n == 1) ? Value::scalar(h.doubleData()[0], mr) : std::move(h);
            if (nargout >= 2)
                outs[1] = (n == 1) ? Value::scalar(m.doubleData()[0], mr) : std::move(m);
            if (nargout >= 3)
                outs[2] = (n == 1) ? Value::scalar(s.doubleData()[0], mr) : std::move(s);
        };

        dur.ops["uminus"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                               CallContext &ctx) {
            const size_t n = self.objectCount();
            std::vector<double> secs(n);
            std::string fmt = durElem(self, 0) ? durElem(self, 0)->format : "hh:mm:ss";
            for (size_t i = 0; i < n; ++i)
                secs[i] = -(durElem(self, i) ? durElem(self, i)->seconds : 0.0);
            outs[0] = makeDurationArray(self.dims(), secs, fmt, ctx.engine->resource());
        };
        dur.ops["uplus"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                              CallContext &) { outs[0] = self; };

        dur.methods["abs"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                CallContext &ctx) {
            const size_t n = self.objectCount();
            std::vector<double> secs(n);
            std::string fmt = durElem(self, 0) ? durElem(self, 0)->format : "hh:mm:ss";
            for (size_t i = 0; i < n; ++i)
                secs[i] = std::abs(durElem(self, i) ? durElem(self, i)->seconds : 0.0);
            outs[0] = makeDurationArray(self.dims(), secs, fmt, ctx.engine->resource());
        };
        auto addDurMinMaxMethod = [&](const char *mname, bool isMax) {
            dur.methods[mname] = [isMax](Value &self, Span<const Value>, size_t nargout,
                                         Span<Value> outs, CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                const size_t n = self.objectCount();
                if (n == 0) {
                    outs[0] = self;
                    return;
                }
                size_t bestIdx = 0;
                double bestVal = durElem(self, 0) ? durElem(self, 0)->seconds : 0.0;
                for (size_t i = 1; i < n; ++i) {
                    double v = durElem(self, i) ? durElem(self, i)->seconds : 0.0;
                    if (std::isnan(v)) continue;
                    if (std::isnan(bestVal) || (isMax ? (v > bestVal) : (v < bestVal))) {
                        bestVal = v;
                        bestIdx = i;
                    }
                }
                std::string fmt = durElem(self, 0) ? durElem(self, 0)->format : "hh:mm:ss";
                outs[0] = makeDurationArray(Dims(1, 1), {bestVal}, fmt, mr);
                if (nargout >= 2)
                    outs[1] = Value::scalar(static_cast<double>(bestIdx + 1), mr);
            };
        };
        addDurMinMaxMethod("min", false);
        addDurMinMaxMethod("max", true);

        auto durAddSub = [](double sign) -> ObjectMethod {
            return [sign](Value &, Span<const Value> args, size_t, Span<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                // duration + datetime
                if (sign > 0.0 && lhs.isObject() && lhs.objectClassName() == "duration"
                    && rhs.isObject() && rhs.objectClassName() == "datetime") {
                    Dims d = broadcastDims(lhs, rhs, "plus");
                    const size_t n = d.numel();
                    std::vector<double> sds(n);
                    const auto *p0 = dtElem(rhs, 0);
                    for (size_t i = 0; i < n; ++i) {
                        double sec = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                        double sd = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                        sds[i] = sd + sec / kSecondsPerDay;
                    }
                    outs[0] = makeDatetimeArray(d, sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
                    return;
                }
                if (!lhs.isObject() || lhs.objectClassName() != "duration"
                    || !rhs.isObject() || rhs.objectClassName() != "duration")
                    throw std::runtime_error("nk:duration:TypeMismatch: Both operands must be durations.");
                Dims d = broadcastDims(lhs, rhs, sign > 0 ? "plus" : "minus");
                const size_t n = d.numel();
                std::vector<double> secs(n);
                std::string fmt = durElem(lhs, 0) ? durElem(lhs, 0)->format : "hh:mm:ss";
                for (size_t i = 0; i < n; ++i) {
                    double s1 = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                    double s2 = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                    secs[i] = s1 + sign * s2;
                }
                outs[0] = makeDurationArray(d, secs, fmt, mr);
            };
        };
        dur.ops["plus"] = durAddSub(+1.0);
        dur.ops["minus"] = durAddSub(-1.0);

        auto durScale = [](bool divide) -> ObjectMethod {
            return [divide](Value &, Span<const Value> args, size_t, Span<Value> outs,
                            CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (divide && lhs.isObject() && rhs.isObject()
                    && lhs.objectClassName() == "duration" && rhs.objectClassName() == "duration") {
                    Dims d = broadcastDims(lhs, rhs, "rdivide");
                    const size_t n = d.numel();
                    if (n == 1) {
                        outs[0] = Value::scalar(durElem(lhs, 0)->seconds / durElem(rhs, 0)->seconds, mr);
                        return;
                    }
                    Value out = makeArrayFromDims(d, ValueType::DOUBLE, mr);
                    for (size_t i = 0; i < n; ++i) {
                        double s1 = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                        double s2 = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                        out.doubleDataMut()[i] = s1 / s2;
                    }
                    outs[0] = std::move(out);
                    return;
                }
                const Value &dVal = lhs.isObject() ? lhs : rhs;
                const Value &nVal = lhs.isObject() ? rhs : lhs;
                Dims d = broadcastDims(dVal, nVal, divide ? "rdivide" : "times");
                const size_t n = d.numel();
                std::vector<double> secs(n);
                std::string fmt = durElem(dVal, 0) ? durElem(dVal, 0)->format : "hh:mm:ss";
                for (size_t i = 0; i < n; ++i) {
                    double s = durElem(dVal, dVal.objectCount() == 1 ? 0 : i)->seconds;
                    double k = nVal.numel() == 1 ? nVal.toScalar() : nVal.elemAsDouble(i);
                    secs[i] = divide ? (s / k) : (s * k);
                }
                outs[0] = makeDurationArray(d, secs, fmt, mr);
            };
        };
        dur.ops["times"] = durScale(false);
        dur.ops["mtimes"] = durScale(false);
        dur.ops["rdivide"] = durScale(true);
        dur.ops["mrdivide"] = durScale(true);

        auto durCmp = [](int mode) -> ObjectMethod {
            return [mode](Value &, Span<const Value> args, size_t, Span<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (!lhs.isObject() || !rhs.isObject()
                    || lhs.objectClassName() != "duration" || rhs.objectClassName() != "duration") {
                    outs[0] = Value::logicalScalar(mode == 1, mr);
                    return;
                }
                Dims d = broadcastDims(lhs, rhs, "relop");
                const size_t n = d.numel();
                auto cmp1 = [mode](double a, double b) -> bool {
                    switch (mode) {
                    case 0: return std::abs(a - b) <= 1e-9;
                    case 1: return !(std::abs(a - b) <= 1e-9);
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
                for (size_t i = 0; i < n; ++i) {
                    double s1 = durElem(lhs, lhs.objectCount() == 1 ? 0 : i)->seconds;
                    double s2 = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                    out.logicalDataMut()[i] = cmp1(s1, s2) ? 1 : 0;
                }
                outs[0] = std::move(out);
            };
        };
        dur.ops["eq"] = durCmp(0);
        dur.ops["ne"] = durCmp(1);
        dur.ops["lt"] = durCmp(2);
        dur.ops["le"] = durCmp(3);
        dur.ops["gt"] = durCmp(4);
        dur.ops["ge"] = durCmp(5);

        dur.dispText = [](const Value &self) -> std::string {
            const size_t R = self.dims().rows(), C = self.dims().cols();
            std::ostringstream os;
            if (R * C == 0) {
                os << "  " << R << "\xC3\x97" << C << " empty duration array\n";
                return os.str();
            }
            if (R * C > 1)
                os << "  " << R << "\xC3\x97" << C << " duration array\n\n";
            else
                os << "  duration\n\n";
            for (size_t r = 0; r < R; ++r) {
                os << "   ";
                for (size_t c = 0; c < C; ++c) {
                    const auto *p = durElem(self, c * R + r);
                    os << " " << (p ? formatDurationElem(p->seconds, p->format) : "00:00:00");
                    if (c + 1 < C) os << "   ";
                }
                os << "\n";
            }
            return os.str();
        };

        engine.registerClass(std::move(dur));
    }

    // ── BuiltinClass: calendarDuration ─────────────────────────────────────
    {
        BuiltinClass cal;
        cal.name = "calendarDuration";
        cal.isHandle = false;
        cal.propNames = {"Format"};

        cal.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            if (args.empty())
                return makeCalDurArray(Dims(1, 1), {CalDurElemPayload(0, 0, 0.0)}, mr);
            if (args.size() == 1 && args[0].isNumeric() && args[0].dims().cols() == 3) {
                const size_t R = args[0].dims().rows();
                std::vector<CalDurElemPayload> items(R);
                for (size_t r = 0; r < R; ++r) {
                    int64_t y = static_cast<int64_t>(std::round(args[0].elemAsDouble(r)));
                    int64_t m = static_cast<int64_t>(std::round(args[0].elemAsDouble(r + R)));
                    int64_t d = static_cast<int64_t>(std::round(args[0].elemAsDouble(r + 2 * R)));
                    items[r] = CalDurElemPayload(y * 12 + m, d, 0.0);
                }
                return makeCalDurArray(Dims(R, 1), items, mr);
            }
            if (args.size() == 3 || args.size() == 6) {
                Dims d = broadcastDims(args[0], args[1], "calendarDuration");
                d = (d.numel() == 1) ? args[2].dims() : d;
                const size_t n = d.numel();
                std::vector<CalDurElemPayload> items(n);
                for (size_t i = 0; i < n; ++i) {
                    int64_t y = static_cast<int64_t>(std::round(args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(i)));
                    int64_t m = static_cast<int64_t>(std::round(args[1].numel() == 1 ? args[1].toScalar() : args[1].elemAsDouble(i)));
                    int64_t dy = static_cast<int64_t>(std::round(args[2].numel() == 1 ? args[2].toScalar() : args[2].elemAsDouble(i)));
                    double sec = 0.0;
                    if (args.size() == 6) {
                        double hh = args[3].numel() == 1 ? args[3].toScalar() : args[3].elemAsDouble(i);
                        double mi = args[4].numel() == 1 ? args[4].toScalar() : args[4].elemAsDouble(i);
                        double ss = args[5].numel() == 1 ? args[5].toScalar() : args[5].elemAsDouble(i);
                        sec = hh * 3600.0 + mi * 60.0 + ss;
                    }
                    items[i] = CalDurElemPayload(y * 12 + m, dy, sec);
                }
                return makeCalDurArray(d, items, mr);
            }
            throw std::runtime_error("nk:calendarDuration:InvalidArgs: Expected calendarDuration(Y, M, D) or calendarDuration(Y, M, D, H, MI, S).");
        };

        cal.methods["calyears"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                     CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(Span<const Value>(arg, 1), 12, 0, "calyears", ctx.engine->resource());
        };
        cal.methods["calquarters"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                        CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(Span<const Value>(arg, 1), 3, 0, "calquarters", ctx.engine->resource());
        };
        cal.methods["calmonths"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                      CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(Span<const Value>(arg, 1), 1, 0, "calmonths", ctx.engine->resource());
        };
        cal.methods["calweeks"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                     CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(Span<const Value>(arg, 1), 0, 7, "calweeks", ctx.engine->resource());
        };
        cal.methods["caldays"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                    CallContext &ctx) {
            Value arg[1] = {self};
            outs[0] = constructCalDurFromUnit(Span<const Value>(arg, 1), 0, 1, "caldays", ctx.engine->resource());
        };
        cal.methods["char"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                 CallContext &ctx) {
            const auto *p = calElem(self, 0);
            outs[0] = Value::fromString(p ? formatCalDurElem(*p) : "0d", ctx.engine->resource());
        };
        cal.methods["string"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                   CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            Value s = Value::stringArray(self.dims().rows(), self.dims().cols(), mr);
            for (size_t i = 0; i < n; ++i) {
                const auto *p = calElem(self, i);
                s.stringElemSet(i, p ? formatCalDurElem(*p) : "0d");
            }
            outs[0] = std::move(s);
        };

        cal.ops["uminus"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                               CallContext &ctx) {
            const size_t n = self.objectCount();
            std::vector<CalDurElemPayload> items(n);
            for (size_t i = 0; i < n; ++i) {
                const auto *p = calElem(self, i);
                if (p)
                    items[i] = CalDurElemPayload(-p->months, -p->days, -p->seconds);
            }
            outs[0] = makeCalDurArray(self.dims(), items, ctx.engine->resource());
        };
        cal.ops["uplus"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                              CallContext &) { outs[0] = self; };

        auto calAddSub = [](int64_t sign) -> ObjectMethod {
            return [sign](Value &, Span<const Value> args, size_t, Span<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (sign > 0 && lhs.isObject() && lhs.objectClassName() == "calendarDuration"
                    && rhs.isObject() && rhs.objectClassName() == "datetime") {
                    Dims d = broadcastDims(lhs, rhs, "plus");
                    const size_t n = d.numel();
                    std::vector<double> sds(n);
                    const auto *p0 = dtElem(rhs, 0);
                    for (size_t i = 0; i < n; ++i) {
                        const auto *cp = calElem(lhs, lhs.objectCount() == 1 ? 0 : i);
                        const auto *dp = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                        sds[i] = addCalendarMonths(dp->serialDay, cp->months, cp->days, cp->seconds);
                    }
                    outs[0] = makeDatetimeArray(d, sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
                    return;
                }
                if (!lhs.isObject() || lhs.objectClassName() != "calendarDuration"
                    || !rhs.isObject() || rhs.objectClassName() != "calendarDuration")
                    throw std::runtime_error("nk:calendarDuration:TypeMismatch: Both operands must be calendarDurations.");
                Dims d = broadcastDims(lhs, rhs, sign > 0 ? "plus" : "minus");
                const size_t n = d.numel();
                std::vector<CalDurElemPayload> items(n);
                for (size_t i = 0; i < n; ++i) {
                    const auto *c1 = calElem(lhs, lhs.objectCount() == 1 ? 0 : i);
                    const auto *c2 = calElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                    items[i] = CalDurElemPayload(
                        c1->months + sign * c2->months,
                        c1->days + sign * c2->days,
                        c1->seconds + static_cast<double>(sign) * c2->seconds);
                }
                outs[0] = makeCalDurArray(d, items, mr);
            };
        };
        cal.ops["plus"] = calAddSub(+1);
        cal.ops["minus"] = calAddSub(-1);

        auto calEq = [](bool neg) -> ObjectMethod {
            return [neg](Value &, Span<const Value> args, size_t, Span<Value> outs,
                         CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                const Value &lhs = args[0];
                const Value &rhs = args[1];
                if (!lhs.isObject() || !rhs.isObject()
                    || lhs.objectClassName() != "calendarDuration"
                    || rhs.objectClassName() != "calendarDuration") {
                    outs[0] = Value::logicalScalar(neg, mr);
                    return;
                }
                Dims d = broadcastDims(lhs, rhs, neg ? "ne" : "eq");
                const size_t n = d.numel();
                auto eq1 = [neg](const CalDurElemPayload *a, const CalDurElemPayload *b) -> bool {
                    bool e = a && b && a->months == b->months && a->days == b->days
                             && std::abs(a->seconds - b->seconds) <= 1e-9;
                    return neg ? !e : e;
                };
                if (n == 1) {
                    outs[0] = Value::logicalScalar(eq1(calElem(lhs, 0), calElem(rhs, 0)), mr);
                    return;
                }
                Value out = makeArrayFromDims(d, ValueType::LOGICAL, mr);
                for (size_t i = 0; i < n; ++i) {
                    const auto *c1 = calElem(lhs, lhs.objectCount() == 1 ? 0 : i);
                    const auto *c2 = calElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                    out.logicalDataMut()[i] = eq1(c1, c2) ? 1 : 0;
                }
                outs[0] = std::move(out);
            };
        };
        cal.ops["eq"] = calEq(false);
        cal.ops["ne"] = calEq(true);

        cal.dispText = [](const Value &self) -> std::string {
            const size_t R = self.dims().rows(), C = self.dims().cols();
            std::ostringstream os;
            if (R * C == 0) {
                os << "  " << R << "\xC3\x97" << C << " empty calendarDuration array\n";
                return os.str();
            }
            if (R * C > 1)
                os << "  " << R << "\xC3\x97" << C << " calendarDuration array\n\n";
            else
                os << "  calendarDuration\n\n";
            for (size_t r = 0; r < R; ++r) {
                os << "   ";
                for (size_t c = 0; c < C; ++c) {
                    const auto *p = calElem(self, c * R + r);
                    os << " " << (p ? formatCalDurElem(*p) : "0d");
                    if (c + 1 < C) os << "   ";
                }
                os << "\n";
            }
            return os.str();
        };

        engine.registerClass(std::move(cal));
    }

    // ── BuiltinClass: datetime ─────────────────────────────────────────────
    {
        BuiltinClass dt;
        dt.name = "datetime";
        dt.isHandle = false;
        dt.propNames = {"Format", "TimeZone", "Year", "Month", "Day", "Hour", "Minute", "Second"};

        dt.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            std::string fmt, inFmt, convFrom, tz;
            size_t nPos = args.size();
            while (nPos >= 2 && (args[nPos - 2].isChar() || args[nPos - 2].isString())) {
                std::string k = args[nPos - 2].toString();
                std::string kl = k;
                for (auto &ch : kl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (kl == "format") {
                    fmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else if (kl == "inputformat") {
                    inFmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else if (kl == "convertfrom") {
                    convFrom = args[nPos - 1].toString();
                    for (auto &ch : convFrom) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
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
                return makeDatetimeArray(Dims(1, 1), {numkit::builtin::now()}, fmt, tz, mr);
            }

            // datetime(dt_obj [, 'Format', fmt, 'TimeZone', tz])
            if (nPos == 1 && args[0].isObject() && args[0].objectClassName() == "datetime") {
                const size_t n = args[0].objectCount();
                std::vector<double> sds(n);
                std::string f0 = fmt;
                std::string tz0 = tz;
                for (size_t i = 0; i < n; ++i) {
                    const auto *p = dtElem(args[0], i);
                    sds[i] = p ? p->serialDay : std::numeric_limits<double>::quiet_NaN();
                    if (i == 0 && p) {
                        if (f0.empty()) f0 = p->format;
                        if (tz0.empty()) tz0 = p->timeZone;
                    }
                }
                return makeDatetimeArray(args[0].dims(), sds, f0, tz0, mr);
            }

            // datetime(x, 'ConvertFrom', type)
            if (!convFrom.empty()) {
                if (nPos != 1 || (!args[0].isNumeric() && !args[0].isLogical()))
                    throw std::runtime_error("nk:datetime:InvalidConvertFrom: Expected a single numeric input with 'ConvertFrom'.");
                const Value &x = args[0];
                const size_t n = x.numel();
                std::vector<double> sds(n);
                for (size_t i = 0; i < n; ++i) {
                    double v = x.elemAsDouble(i);
                    if (!std::isfinite(v)) {
                        sds[i] = std::numeric_limits<double>::quiet_NaN();
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
                        int64_t iv = static_cast<int64_t>(std::llround(v));
                        int64_t y = iv / 10000;
                        int64_t m = (iv / 100) % 100;
                        int64_t d = iv % 100;
                        sds[i] = composeSerialDay(static_cast<double>(y), static_cast<double>(m), static_cast<double>(d));
                    } else {
                        throw std::runtime_error("nk:datetime:UnknownConvertFrom: Unsupported ConvertFrom type '" + convFrom + "'.");
                    }
                }
                return makeDatetimeArray(x.dims(), sds, fmt, tz, mr);
            }

            // datetime(str [, fmt])
            if ((nPos == 1 || nPos == 2)
                && (args[0].isChar() || args[0].isString() || args[0].isCell())) {
                if (nPos == 2 && fmt.empty() && (args[1].isChar() || args[1].isString()))
                    fmt = args[1].toString();
                if (args[0].isChar()) {
                    const size_t R = args[0].dims().rows();
                    const size_t C = args[0].dims().cols();
                    if (R <= 1) {
                        double sd = parseDateTimeString(args[0].toString(), mr);
                        return makeDatetimeArray(Dims(1, 1), {sd}, fmt, tz, mr);
                    }
                    std::vector<double> sds(R);
                    const char *cd = args[0].charData();
                    for (size_t r = 0; r < R; ++r) {
                        std::string rowStr(C, ' ');
                        for (size_t c = 0; c < C; ++c)
                            rowStr[c] = cd[c * R + r];
                        sds[r] = parseDateTimeString(rowStr, mr);
                    }
                    return makeDatetimeArray(Dims(R, 1), sds, fmt, tz, mr);
                }
                if (args[0].isString()) {
                    const size_t n = args[0].numel();
                    std::vector<double> sds(n);
                    for (size_t i = 0; i < n; ++i)
                        sds[i] = parseDateTimeString(args[0].stringElem(i), mr);
                    return makeDatetimeArray(args[0].dims(), sds, fmt, tz, mr);
                }
                if (args[0].isCell()) {
                    const size_t n = args[0].numel();
                    std::vector<double> sds(n);
                    for (size_t i = 0; i < n; ++i)
                        sds[i] = parseDateTimeString(args[0].cellAt(i).toString(), mr);
                    return makeDatetimeArray(args[0].dims(), sds, fmt, tz, mr);
                }
            }

            // datetime(datevecMatrix) where cols == 3 or cols == 6, or Excel serial date column vector
            if (nPos == 1 && args[0].isNumeric()) {
                const size_t R = args[0].dims().rows();
                const size_t C = args[0].dims().cols();
                if (R == 0 && C == 0)
                    return makeDatetimeArray(Dims(0, 0), {}, fmt, tz, mr);
                if (C != 3 && C != 6) {
                    // If a 1D vector of Excel serial dates (e.g. from readtable on .xlsx)
                    // is passed directly to datetime(), convert Excel serial dates when values
                    // lie in [1, 200000].
                    if ((C == 1 || R == 1) && args[0].numel() >= 1) {
                        double v0 = args[0].elemAsDouble(0);
                        if (std::isfinite(v0) && v0 >= 1.0 && v0 <= 200000.0) {
                            const size_t n = args[0].numel();
                            std::vector<double> sds(n);
                            for (size_t i = 0; i < n; ++i) {
                                double v = args[0].elemAsDouble(i);
                                sds[i] = std::isfinite(v) ? (v + 693960.0)
                                                          : std::numeric_limits<double>::quiet_NaN();
                            }
                            return makeDatetimeArray(args[0].dims(), sds, fmt, tz, mr);
                        }
                    }
                    throw std::runtime_error("nk:datetime:InvalidDatevecSize: Numeric matrix input to datetime must have 3 or 6 columns.");
                }
                std::vector<double> sds(R);
                for (size_t r = 0; r < R; ++r) {
                    double y = args[0].elemAsDouble(r);
                    double m = args[0].elemAsDouble(r + R);
                    double d = args[0].elemAsDouble(r + 2 * R);
                    double h = (C == 6) ? args[0].elemAsDouble(r + 3 * R) : 0.0;
                    double mi = (C == 6) ? args[0].elemAsDouble(r + 4 * R) : 0.0;
                    double s = (C == 6) ? args[0].elemAsDouble(r + 5 * R) : 0.0;
                    sds[r] = composeSerialDay(y, m, d, h, mi, s);
                }
                return makeDatetimeArray(Dims(R, 1), sds, fmt, tz, mr);
            }

            // datetime(Y, M, D [, H, MI, S [, MS]])
            if (nPos == 3 || nPos == 6 || nPos == 7) {
                Dims outDims(1, 1);
                for (size_t k = 0; k < nPos; ++k) {
                    if (!args[k].isNumeric() && !args[k].isLogical())
                        throw std::runtime_error("nk:datetime:NonNumericComponent: Date/time components must be numeric.");
                    if (args[k].numel() != 1) {
                        if (outDims.numel() == 1)
                            outDims = args[k].dims();
                        else if (args[k].dims() != outDims && args[k].numel() != outDims.numel())
                            throw std::runtime_error("nk:datetime:ComponentSizeMismatch: Date/time component arrays must be the same size or scalar.");
                    }
                }
                const size_t n = outDims.numel();
                std::vector<double> sds(n);
                auto elem = [&](size_t k, size_t idx) -> double {
                    return args[k].numel() == 1 ? args[k].toScalar() : args[k].elemAsDouble(idx);
                };
                for (size_t i = 0; i < n; ++i) {
                    double y = elem(0, i);
                    double m = elem(1, i);
                    double d = elem(2, i);
                    double h = (nPos >= 6) ? elem(3, i) : 0.0;
                    double mi = (nPos >= 6) ? elem(4, i) : 0.0;
                    double s = (nPos >= 6) ? elem(5, i) : 0.0;
                    double ms = (nPos == 7) ? elem(6, i) : 0.0;
                    sds[i] = composeSerialDay(y, m, d, h, mi, s + ms * 0.001);
                }
                return makeDatetimeArray(outDims, sds, fmt, tz, mr);
            }

            throw std::runtime_error("nk:datetime:InvalidSyntax: Unsupported arguments to datetime constructor.");
        };

        dt.propGet = [](const Value &self, const std::string &name, Value &out,
                        CallContext &ctx) -> bool {
            auto *mr = ctx.engine->resource();
            if (name == "Year")   { out = extractDatetimeComponent(self, 0, mr); return true; }
            if (name == "Month")  { out = extractDatetimeComponent(self, 1, mr); return true; }
            if (name == "Day")    { out = extractDatetimeComponent(self, 2, mr); return true; }
            if (name == "Hour")   { out = extractDatetimeComponent(self, 3, mr); return true; }
            if (name == "Minute") { out = extractDatetimeComponent(self, 4, mr); return true; }
            if (name == "Second") { out = extractDatetimeComponent(self, 5, mr); return true; }
            if (name == "Format") {
                const auto *p = dtElem(self, 0);
                std::string f = (p && !p->format.empty()) ? p->format : "dd-MMM-uuuu";
                out = Value::fromString(f, mr);
                return true;
            }
            if (name == "TimeZone") {
                const auto *p = dtElem(self, 0);
                out = Value::fromString(p ? p->timeZone : "", mr);
                return true;
            }
            return false;
        };

        dt.propSet = [](Value &self, const std::string &name, const Value &val,
                        CallContext &) -> bool {
            if (name == "Year")   { mutateDatetimeComponent(self, 0, val); return true; }
            if (name == "Month")  { mutateDatetimeComponent(self, 1, val); return true; }
            if (name == "Day")    { mutateDatetimeComponent(self, 2, val); return true; }
            if (name == "Hour")   { mutateDatetimeComponent(self, 3, val); return true; }
            if (name == "Minute") { mutateDatetimeComponent(self, 4, val); return true; }
            if (name == "Second") { mutateDatetimeComponent(self, 5, val); return true; }
            if (name == "Format") {
                std::string f = val.toString();
                for (size_t i = 0; i < self.objectCount(); ++i)
                    if (auto *p = dtElemMut(self, i))
                        p->format = f;
                return true;
            }
            if (name == "TimeZone") {
                std::string tz = val.toString();
                for (size_t i = 0; i < self.objectCount(); ++i)
                    if (auto *p = dtElemMut(self, i))
                        p->timeZone = tz;
                return true;
            }
            return false;
        };

        // Component extractor methods: year(d), month(d), day(d), hour(d), minute(d), second(d), quarter(d)
        auto addCompMethod = [&](const char *mname, int comp) {
            dt.methods[mname] = [comp](Value &self, Span<const Value>, size_t,
                                       Span<Value> outs, CallContext &ctx) {
                outs[0] = extractDatetimeComponent(self, comp, ctx.engine->resource());
            };
        };
        addCompMethod("year", 0);
        addCompMethod("month", 1);
        addCompMethod("day", 2);
        addCompMethod("hour", 3);
        addCompMethod("minute", 4);
        addCompMethod("second", 5);
        addCompMethod("quarter", 6);

        dt.methods["ymd"] = [](Value &self, Span<const Value>, size_t nargout, Span<Value> outs,
                               CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            outs[0] = extractDatetimeComponent(self, 0, mr);
            if (nargout >= 2) outs[1] = extractDatetimeComponent(self, 1, mr);
            if (nargout >= 3) outs[2] = extractDatetimeComponent(self, 2, mr);
        };
        dt.methods["hms"] = [](Value &self, Span<const Value>, size_t nargout, Span<Value> outs,
                               CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            outs[0] = extractDatetimeComponent(self, 3, mr);
            if (nargout >= 2) outs[1] = extractDatetimeComponent(self, 4, mr);
            if (nargout >= 3) outs[2] = extractDatetimeComponent(self, 5, mr);
        };

        // Conversions: datenum, posixtime, juliandate, mjuliandate, exceltime, yyyymmdd, datevec, datestr, char, string, cellstr
        auto extractSerialValues = [](const Value &self, auto xform, std::pmr::memory_resource *mr) -> Value {
            const size_t n = self.objectCount();
            if (n == 1) {
                const auto *p = dtElem(self, 0);
                double sd = p ? p->serialDay : std::numeric_limits<double>::quiet_NaN();
                return Value::scalar(std::isfinite(sd) ? xform(sd) : sd, mr);
            }
            Value out = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            for (size_t i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                double sd = p ? p->serialDay : std::numeric_limits<double>::quiet_NaN();
                out.doubleDataMut()[i] = std::isfinite(sd) ? xform(sd) : sd;
            }
            return out;
        };

        dt.methods["datenum"] = [extractSerialValues](Value &self, Span<const Value>, size_t,
                                                      Span<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(self, [](double sd) { return sd; }, ctx.engine->resource());
        };
        dt.methods["posixtime"] = [extractSerialValues](Value &self, Span<const Value>, size_t,
                                                        Span<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](double sd) { return (sd - kEpochSerialDay) * kSecondsPerDay; },
                ctx.engine->resource());
        };
        dt.methods["juliandate"] = [extractSerialValues](Value &self, Span<const Value>, size_t,
                                                         Span<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](double sd) { return sd + 1721058.5; }, ctx.engine->resource());
        };
        dt.methods["mjuliandate"] = [extractSerialValues](Value &self, Span<const Value>, size_t,
                                                          Span<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](double sd) { return sd - 678942.0; }, ctx.engine->resource());
        };
        dt.methods["exceltime"] = [extractSerialValues](Value &self, Span<const Value>, size_t,
                                                        Span<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self, [](double sd) { return sd - 693960.0; }, ctx.engine->resource());
        };
        dt.methods["yyyymmdd"] = [extractSerialValues](Value &self, Span<const Value>, size_t,
                                                       Span<Value> outs, CallContext &ctx) {
            outs[0] = extractSerialValues(
                self,
                [](double sd) {
                    CivilDateTime c = decomposeSerialDay(sd);
                    return static_cast<double>(c.year * 10000 + c.month * 100 + c.day);
                },
                ctx.engine->resource());
        };
        dt.methods["datevec"] = [](Value &self, Span<const Value>, size_t nargout,
                                   Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            Value dv = Value::matrix(n, 6, ValueType::DOUBLE, mr);
            double *dst = dv.doubleDataMut();
            for (size_t i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                CivilDateTime c = decomposeSerialDay(p ? p->serialDay : std::numeric_limits<double>::quiet_NaN());
                if (c.isNaT) {
                    for (size_t k = 0; k < 6; ++k)
                        dst[i + k * n] = std::numeric_limits<double>::quiet_NaN();
                } else {
                    dst[i + 0 * n] = static_cast<double>(c.year);
                    dst[i + 1 * n] = static_cast<double>(c.month);
                    dst[i + 2 * n] = static_cast<double>(c.day);
                    dst[i + 3 * n] = static_cast<double>(c.hour);
                    dst[i + 4 * n] = static_cast<double>(c.minute);
                    dst[i + 5 * n] = c.second;
                }
            }
            if (nargout <= 1) {
                outs[0] = std::move(dv);
            } else {
                for (size_t k = 0; k < std::min<size_t>(nargout, 6); ++k) {
                    if (n == 1) {
                        outs[k] = Value::scalar(dst[k * n], mr);
                    } else {
                        Value col = Value::matrix(n, 1, ValueType::DOUBLE, mr);
                        for (size_t i = 0; i < n; ++i)
                            col.doubleDataMut()[i] = dst[i + k * n];
                        outs[k] = std::move(col);
                    }
                }
            }
        };

        auto hasNonZeroTime = [](const Value &self) -> bool {
            for (size_t i = 0; i < self.objectCount(); ++i) {
                const auto *p = dtElem(self, i);
                if (!p) continue;
                CivilDateTime c = decomposeSerialDay(p->serialDay);
                if (!c.isNaT && (c.hour != 0 || c.minute != 0 || std::abs(c.second) > 1e-6))
                    return true;
            }
            return false;
        };

        dt.methods["char"] = [hasNonZeroTime](Value &self, Span<const Value> args, size_t,
                                              Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            bool anyTime = hasNonZeroTime(self);
            std::string overrideFmt = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                          ? args[0].toString()
                                          : "";
            if (n <= 1) {
                const auto *p = dtElem(self, 0);
                std::string f = !overrideFmt.empty() ? overrideFmt : (p ? p->format : "");
                outs[0] = Value::fromString(
                    formatDateTimeElem(p ? p->serialDay : std::numeric_limits<double>::quiet_NaN(),
                                       f, anyTime),
                    mr);
                return;
            }
            std::vector<std::string> rows(n);
            size_t maxW = 0;
            for (size_t i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                std::string f = !overrideFmt.empty() ? overrideFmt : (p ? p->format : "");
                rows[i] = formatDateTimeElem(p ? p->serialDay : std::numeric_limits<double>::quiet_NaN(),
                                             f, anyTime);
                maxW = std::max(maxW, rows[i].size());
            }
            Value cm = Value::matrix(n, maxW, ValueType::CHAR, mr);
            char *dst = cm.charDataMut();
            std::fill(dst, dst + n * maxW, ' ');
            for (size_t r = 0; r < n; ++r)
                for (size_t c = 0; c < rows[r].size(); ++c)
                    dst[c * n + r] = rows[r][c];
            outs[0] = std::move(cm);
        };
        dt.methods["datestr"] = [hasNonZeroTime](Value &self, Span<const Value> args, size_t nargout,
                                                 Span<Value> outs, CallContext &ctx) {
            std::vector<Value> modArgs(args.begin(), args.end());
            if (!modArgs.empty() && (modArgs[0].isChar() || modArgs[0].isString())) {
                std::string f = modArgs[0].toString();
                for (size_t i = 0; i + 1 < f.size(); ++i) {
                    if (f[i] == 'm' && f[i + 1] == 'm' && (i == 0 || f[i - 1] != ':')) {
                        f[i] = 'M';
                        f[i + 1] = 'M';
                        ++i;
                    } else if (f.compare(i, 3, "mmm") == 0) {
                        f[i] = 'M'; f[i + 1] = 'M'; f[i + 2] = 'M';
                        i += 2;
                    }
                }
                modArgs[0] = Value::fromString(f, ctx.engine->resource());
            }
            const auto *cls = ctx.engine->findClass("datetime");
            if (cls && cls->methods.count("char"))
                cls->methods.at("char")(self, Span<const Value>(modArgs.data(), modArgs.size()),
                                        nargout, outs, ctx);
        };

        dt.methods["string"] = [hasNonZeroTime](Value &self, Span<const Value> args, size_t,
                                                Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            bool anyTime = hasNonZeroTime(self);
            std::string overrideFmt = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                          ? args[0].toString()
                                          : "";
            Value s = Value::stringArray(self.dims().rows(), self.dims().cols(), mr);
            for (size_t i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                std::string f = !overrideFmt.empty() ? overrideFmt : (p ? p->format : "");
                s.stringElemSet(
                    i, formatDateTimeElem(p ? p->serialDay : std::numeric_limits<double>::quiet_NaN(),
                                          f, anyTime));
            }
            outs[0] = std::move(s);
        };
        dt.methods["cellstr"] = [hasNonZeroTime](Value &self, Span<const Value> args, size_t,
                                                 Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            bool anyTime = hasNonZeroTime(self);
            std::string overrideFmt = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                          ? args[0].toString()
                                          : "";
            Value c = Value::cell(self.dims().rows(), self.dims().cols(), mr);
            for (size_t i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                std::string f = !overrideFmt.empty() ? overrideFmt : (p ? p->format : "");
                c.cellAt(i) = Value::fromString(
                    formatDateTimeElem(p ? p->serialDay : std::numeric_limits<double>::quiet_NaN(),
                                       f, anyTime),
                    mr);
            }
            outs[0] = std::move(c);
        };

        dt.methods["isnat"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                 CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            if (n == 1) {
                const auto *p = dtElem(self, 0);
                outs[0] = Value::logicalScalar(!p || !std::isfinite(p->serialDay), mr);
                return;
            }
            Value out = makeArrayFromDims(self.dims(), ValueType::LOGICAL, mr);
            for (size_t i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                out.logicalDataMut()[i] = (!p || !std::isfinite(p->serialDay)) ? 1 : 0;
            }
            outs[0] = std::move(out);
        };
        dt.methods["isnan"] = dt.methods["isnat"];
        dt.methods["ismissing"] = dt.methods["isnat"];

        dt.methods["diff"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs,
                                CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            if (n <= 1) {
                outs[0] = makeDurationArray(Dims(1, 0), {}, "hh:mm:ss", mr);
                return;
            }
            std::vector<double> secs(n - 1);
            for (size_t i = 0; i + 1 < n; ++i) {
                double sd1 = dtElem(self, i)->serialDay;
                double sd2 = dtElem(self, i + 1)->serialDay;
                secs[i] = (sd2 - sd1) * kSecondsPerDay;
            }
            Dims outDims = (self.dims().rows() > 1 && self.dims().cols() == 1)
                               ? Dims(n - 1, 1)
                               : Dims(1, n - 1);
            outs[0] = makeDurationArray(outDims, secs, "hh:mm:ss", mr);
        };

        dt.methods["between"] = [](Value &self, Span<const Value> args, size_t,
                                   Span<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassName() != "datetime")
                throw std::runtime_error("nk:between:InvalidInput: Expected two datetime inputs.");
            auto *mr = ctx.engine->resource();
            const Value &rhs = args[0];
            Dims d = broadcastDims(self, rhs, "between");
            const size_t n = d.numel();
            std::vector<CalDurElemPayload> items(n);
            for (size_t i = 0; i < n; ++i) {
                double sd1 = dtElem(self, self.objectCount() == 1 ? 0 : i)->serialDay;
                double sd2 = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                CivilDateTime c1 = decomposeSerialDay(sd1);
                CivilDateTime c2 = decomposeSerialDay(sd2);
                int64_t months = (c2.year - c1.year) * 12 + (c2.month - c1.month);
                double anchor = addCalendarMonths(sd1, months, 0, 0.0);
                double remDays = sd2 - anchor;
                int64_t wholeDays = static_cast<int64_t>(std::trunc(remDays));
                double remSec = (remDays - static_cast<double>(wholeDays)) * kSecondsPerDay;
                items[i] = CalDurElemPayload(months, wholeDays, remSec);
            }
            outs[0] = makeCalDurArray(d, items, mr);
        };

        dt.methods["dateshift"] = [](Value &self, Span<const Value> args, size_t,
                                     Span<Value> outs, CallContext &ctx) {
            if (args.size() < 2)
                throw std::runtime_error("nk:dateshift:NotEnoughInputs: Expected dateshift(t, 'start'|'end', unit).");
            auto *mr = ctx.engine->resource();
            std::string rule = args[0].toString();
            std::string unit = args[1].toString();
            for (auto &ch : rule) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (auto &ch : unit) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool isEnd = (rule == "end");
            const size_t n = self.objectCount();
            std::vector<double> sds(n);
            const auto *p0 = dtElem(self, 0);
            for (size_t i = 0; i < n; ++i) {
                CivilDateTime c = decomposeSerialDay(dtElem(self, i)->serialDay);
                if (c.isNaT) {
                    sds[i] = std::numeric_limits<double>::quiet_NaN();
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
            outs[0] = makeDatetimeArray(self.dims(), sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
        };

        dt.methods["sort"] = [](Value &self, Span<const Value> args, size_t nargout,
                                Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            bool desc = false;
            if (!args.empty() && (args[0].isChar() || args[0].isString())) {
                std::string dir = args[0].toString();
                for (auto &ch : dir) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                desc = (dir == "descend");
            }
            std::vector<size_t> order(n);
            for (size_t i = 0; i < n; ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                double sa = dtElem(self, a)->serialDay;
                double sb = dtElem(self, b)->serialDay;
                if (std::isnan(sa)) return false;
                if (std::isnan(sb)) return true;
                return desc ? (sa > sb) : (sa < sb);
            });
            std::vector<double> sds(n);
            for (size_t i = 0; i < n; ++i)
                sds[i] = dtElem(self, order[i])->serialDay;
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
            if (nargout >= 2) {
                Value idx = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
                for (size_t i = 0; i < n; ++i)
                    idx.doubleDataMut()[i] = static_cast<double>(order[i] + 1);
                outs[1] = std::move(idx);
            }
        };

        auto addMinMaxMethod = [&](const char *mname, bool isMax) {
            dt.methods[mname] = [isMax](Value &self, Span<const Value>, size_t nargout,
                                        Span<Value> outs, CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                const size_t n = self.objectCount();
                if (n == 0) {
                    outs[0] = self;
                    return;
                }
                size_t bestIdx = 0;
                double bestVal = dtElem(self, 0)->serialDay;
                for (size_t i = 1; i < n; ++i) {
                    double v = dtElem(self, i)->serialDay;
                    if (std::isnan(v)) continue;
                    if (std::isnan(bestVal) || (isMax ? (v > bestVal) : (v < bestVal))) {
                        bestVal = v;
                        bestIdx = i;
                    }
                }
                const auto *p0 = dtElem(self, 0);
                outs[0] = makeDatetimeArray(Dims(1, 1), {bestVal},
                                            p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
                if (nargout >= 2)
                    outs[1] = Value::scalar(static_cast<double>(bestIdx + 1), mr);
            };
        };
        addMinMaxMethod("min", false);
        addMinMaxMethod("max", true);

        dt.methods["flipud"] = [](Value &self, Span<const Value>, size_t,
                                  Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t R = self.dims().rows(), C = self.dims().cols();
            std::vector<double> sds(R * C);
            for (size_t c = 0; c < C; ++c)
                for (size_t r = 0; r < R; ++r)
                    sds[c * R + r] = dtElem(self, c * R + (R - 1 - r))->serialDay;
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
        };
        dt.methods["fliplr"] = [](Value &self, Span<const Value>, size_t,
                                  Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t R = self.dims().rows(), C = self.dims().cols();
            std::vector<double> sds(R * C);
            for (size_t c = 0; c < C; ++c)
                for (size_t r = 0; r < R; ++r)
                    sds[c * R + r] = dtElem(self, (C - 1 - c) * R + r)->serialDay;
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
        };
        dt.methods["flip"] = [](Value &self, Span<const Value>, size_t,
                                Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t R = self.dims().rows(), C = self.dims().cols();
            std::vector<double> sds(R * C);
            if (R > 1) {
                for (size_t c = 0; c < C; ++c)
                    for (size_t r = 0; r < R; ++r)
                        sds[c * R + r] = dtElem(self, c * R + (R - 1 - r))->serialDay;
            } else {
                for (size_t c = 0; c < C; ++c)
                    for (size_t r = 0; r < R; ++r)
                        sds[c * R + r] = dtElem(self, (C - 1 - c) * R + r)->serialDay;
            }
            const auto *p0 = dtElem(self, 0);
            outs[0] = makeDatetimeArray(self.dims(), sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
        };

        dt.methods["unique"] = [](Value &self, Span<const Value> args, size_t nargout,
                                  Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const size_t n = self.objectCount();
            bool stable = false;
            for (const auto &a : args) {
                if (a.isChar() || a.isString()) {
                    std::string s = a.toString();
                    for (auto &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (s == "stable") stable = true;
                }
            }
            constexpr double kTol = 1e-9 / kSecondsPerDay;
            std::vector<double> uniqSds;
            std::vector<size_t> ia;
            std::vector<size_t> ic(n, 0);
            if (stable) {
                for (size_t i = 0; i < n; ++i) {
                    double sd = dtElem(self, i)->serialDay;
                    size_t found = uniqSds.size();
                    for (size_t u = 0; u < uniqSds.size(); ++u) {
                        if (std::abs(uniqSds[u] - sd) <= kTol) { found = u; break; }
                    }
                    if (found == uniqSds.size()) {
                        uniqSds.push_back(sd);
                        ia.push_back(i + 1);
                    }
                    ic[i] = found + 1;
                }
            } else {
                std::vector<size_t> order(n);
                for (size_t i = 0; i < n; ++i) order[i] = i;
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    double sa = dtElem(self, a)->serialDay;
                    double sb = dtElem(self, b)->serialDay;
                    if (std::isnan(sa)) return false;
                    if (std::isnan(sb)) return true;
                    return sa < sb - kTol;
                });
                for (size_t k = 0; k < n; ++k) {
                    size_t idx = order[k];
                    double sd = dtElem(self, idx)->serialDay;
                    if (uniqSds.empty() || std::isnan(sd) || std::isnan(uniqSds.back())
                        || std::abs(sd - uniqSds.back()) > kTol) {
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
            outs[0] = makeDatetimeArray(outDims, uniqSds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
            if (nargout >= 2) {
                Value iaVal = Value::matrix(ia.size(), 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i < ia.size(); ++i)
                    iaVal.doubleDataMut()[i] = static_cast<double>(ia[i]);
                outs[1] = std::move(iaVal);
            }
            if (nargout >= 3) {
                Value icVal = Value::matrix(n, 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i < n; ++i)
                    icVal.doubleDataMut()[i] = static_cast<double>(ic[i]);
                outs[2] = std::move(icVal);
            }
        };

        dt.methods["intersect"] = [](Value &self, Span<const Value> args, size_t nargout,
                                     Span<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassName() != "datetime")
                throw std::runtime_error("nk:intersect:InvalidInput: Expected two datetime inputs.");
            auto *mr = ctx.engine->resource();
            const Value &other = args[0];
            bool stable = false;
            for (size_t k = 1; k < args.size(); ++k) {
                if (args[k].isChar() || args[k].isString()) {
                    std::string s = args[k].toString();
                    for (auto &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (s == "stable") stable = true;
                }
            }
            constexpr double kTol = 1e-9 / kSecondsPerDay;
            const size_t nA = self.objectCount();
            const size_t nB = other.objectCount();
            struct Item { double sd; size_t ia; size_t ib; };
            std::vector<Item> items;
            for (size_t i = 0; i < nA; ++i) {
                double sa = dtElem(self, i)->serialDay;
                if (std::isnan(sa)) continue;
                bool dup = false;
                for (const auto &it : items) {
                    if (std::abs(it.sd - sa) <= kTol) { dup = true; break; }
                }
                if (dup) continue;
                for (size_t j = 0; j < nB; ++j) {
                    double sb = dtElem(other, j)->serialDay;
                    if (!std::isnan(sb) && std::abs(sa - sb) <= kTol) {
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
            std::vector<double> sds(items.size());
            for (size_t i = 0; i < items.size(); ++i) sds[i] = items[i].sd;
            const auto *p0 = dtElem(self, 0);
            Dims outDims = (self.dims().rows() == 1 && self.dims().cols() > 1
                            && other.dims().rows() == 1 && other.dims().cols() > 1)
                               ? Dims(1, sds.size())
                               : Dims(sds.size(), 1);
            outs[0] = makeDatetimeArray(outDims, sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
            if (nargout >= 2) {
                Value iaVal = Value::matrix(items.size(), 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i < items.size(); ++i)
                    iaVal.doubleDataMut()[i] = static_cast<double>(items[i].ia);
                outs[1] = std::move(iaVal);
            }
            if (nargout >= 3) {
                Value ibVal = Value::matrix(items.size(), 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i < items.size(); ++i)
                    ibVal.doubleDataMut()[i] = static_cast<double>(items[i].ib);
                outs[2] = std::move(ibVal);
            }
        };

        dt.methods["ismember"] = [](Value &self, Span<const Value> args, size_t nargout,
                                    Span<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassName() != "datetime")
                throw std::runtime_error("nk:ismember:InvalidInput: Expected two datetime inputs.");
            auto *mr = ctx.engine->resource();
            const Value &other = args[0];
            constexpr double kTol = 1e-9 / kSecondsPerDay;
            const size_t nA = self.objectCount();
            const size_t nB = other.objectCount();
            Value lia = makeArrayFromDims(self.dims(), ValueType::LOGICAL, mr);
            Value locb = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            for (size_t i = 0; i < nA; ++i) {
                double sa = dtElem(self, i)->serialDay;
                size_t found = 0;
                if (!std::isnan(sa)) {
                    for (size_t j = 0; j < nB; ++j) {
                        double sb = dtElem(other, j)->serialDay;
                        if (!std::isnan(sb) && std::abs(sa - sb) <= kTol) {
                            found = j + 1;
                            break;
                        }
                    }
                }
                lia.logicalDataMut()[i] = found > 0 ? 1 : 0;
                locb.doubleDataMut()[i] = static_cast<double>(found);
            }
            outs[0] = std::move(lia);
            if (nargout >= 2)
                outs[1] = std::move(locb);
        };

        // Operators on datetime
        dt.ops["plus"] = [](Value &, Span<const Value> args, size_t, Span<Value> outs,
                            CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const Value &lhs = args[0];
            const Value &rhs = args[1];
            const Value &dtVal = (lhs.isObject() && lhs.objectClassName() == "datetime") ? lhs : rhs;
            const Value &other = (&dtVal == &lhs) ? rhs : lhs;
            const auto *p0 = dtElem(dtVal, 0);
            Dims d = broadcastDims(dtVal, other, "plus");
            const size_t n = d.numel();
            std::vector<double> sds(n);

            if (other.isObject() && other.objectClassName() == "calendarDuration") {
                for (size_t i = 0; i < n; ++i) {
                    double sd = dtElem(dtVal, dtVal.objectCount() == 1 ? 0 : i)->serialDay;
                    const auto *cp = calElem(other, other.objectCount() == 1 ? 0 : i);
                    sds[i] = addCalendarMonths(sd, cp->months, cp->days, cp->seconds);
                }
            } else if (other.isObject() && other.objectClassName() == "duration") {
                for (size_t i = 0; i < n; ++i) {
                    double sd = dtElem(dtVal, dtVal.objectCount() == 1 ? 0 : i)->serialDay;
                    double sec = durElem(other, other.objectCount() == 1 ? 0 : i)->seconds;
                    sds[i] = sd + sec / kSecondsPerDay;
                }
            } else if (other.isNumeric() || other.isLogical()) {
                for (size_t i = 0; i < n; ++i) {
                    double sd = dtElem(dtVal, dtVal.objectCount() == 1 ? 0 : i)->serialDay;
                    double dy = other.numel() == 1 ? other.toScalar() : other.elemAsDouble(i);
                    sds[i] = sd + dy;
                }
            } else {
                throw std::runtime_error("nk:datetime:InvalidPlusOperand: Cannot add value of type '"
                                         + std::string(mtypeName(other.type())) + "' to a datetime.");
            }
            outs[0] = makeDatetimeArray(d, sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
        };

        dt.ops["minus"] = [](Value &, Span<const Value> args, size_t, Span<Value> outs,
                             CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const Value &lhs = args[0];
            const Value &rhs = args[1];
            if (!lhs.isObject() || lhs.objectClassName() != "datetime")
                throw std::runtime_error("nk:datetime:InvalidMinus: Cannot subtract a datetime from a non-datetime.");

            // datetime - datetime -> duration
            if (rhs.isObject() && rhs.objectClassName() == "datetime") {
                Dims d = broadcastDims(lhs, rhs, "minus");
                const size_t n = d.numel();
                std::vector<double> secs(n);
                for (size_t i = 0; i < n; ++i) {
                    double sd1 = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    double sd2 = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                    secs[i] = (sd1 - sd2) * kSecondsPerDay;
                }
                outs[0] = makeDurationArray(d, secs, "hh:mm:ss", mr);
                return;
            }

            const auto *p0 = dtElem(lhs, 0);
            Dims d = broadcastDims(lhs, rhs, "minus");
            const size_t n = d.numel();
            std::vector<double> sds(n);

            if (rhs.isObject() && rhs.objectClassName() == "calendarDuration") {
                for (size_t i = 0; i < n; ++i) {
                    double sd = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    const auto *cp = calElem(rhs, rhs.objectCount() == 1 ? 0 : i);
                    sds[i] = addCalendarMonths(sd, -cp->months, -cp->days, -cp->seconds);
                }
            } else if (rhs.isObject() && rhs.objectClassName() == "duration") {
                for (size_t i = 0; i < n; ++i) {
                    double sd = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    double sec = durElem(rhs, rhs.objectCount() == 1 ? 0 : i)->seconds;
                    sds[i] = sd - sec / kSecondsPerDay;
                }
            } else if (rhs.isNumeric() || rhs.isLogical()) {
                for (size_t i = 0; i < n; ++i) {
                    double sd = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    double dy = rhs.numel() == 1 ? rhs.toScalar() : rhs.elemAsDouble(i);
                    sds[i] = sd - dy;
                }
            } else {
                throw std::runtime_error("nk:datetime:InvalidMinusOperand: Cannot subtract value of type '"
                                         + std::string(mtypeName(rhs.type())) + "' from a datetime.");
            }
            outs[0] = makeDatetimeArray(d, sds, p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
        };

        auto dtCmp = [](int mode) -> ObjectMethod {
            return [mode](Value &, Span<const Value> args, size_t, Span<Value> outs,
                          CallContext &ctx) {
                auto *mr = ctx.engine->resource();
                Value lhs = args[0];
                Value rhs = args[1];
                if (lhs.isChar() || lhs.isString())
                    lhs = makeDatetimeArray(Dims(1, 1), {parseDateTimeString(lhs.toString(), mr)}, "", "", mr);
                if (rhs.isChar() || rhs.isString())
                    rhs = makeDatetimeArray(Dims(1, 1), {parseDateTimeString(rhs.toString(), mr)}, "", "", mr);
                if (!lhs.isObject() || !rhs.isObject()
                    || lhs.objectClassName() != "datetime" || rhs.objectClassName() != "datetime") {
                    outs[0] = Value::logicalScalar(mode == 1, mr);
                    return;
                }
                Dims d = broadcastDims(lhs, rhs, "relop");
                const size_t n = d.numel();
                constexpr double kTol = 1e-9 / kSecondsPerDay;
                auto cmp1 = [mode, kTol](double a, double b) -> bool {
                    if (std::isnan(a) || std::isnan(b))
                        return mode == 1;
                    switch (mode) {
                    case 0: return std::abs(a - b) <= kTol;
                    case 1: return !(std::abs(a - b) <= kTol);
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
                for (size_t i = 0; i < n; ++i) {
                    double s1 = dtElem(lhs, lhs.objectCount() == 1 ? 0 : i)->serialDay;
                    double s2 = dtElem(rhs, rhs.objectCount() == 1 ? 0 : i)->serialDay;
                    out.logicalDataMut()[i] = cmp1(s1, s2) ? 1 : 0;
                }
                outs[0] = std::move(out);
            };
        };
        dt.ops["eq"] = dtCmp(0);
        dt.ops["ne"] = dtCmp(1);
        dt.ops["lt"] = dtCmp(2);
        dt.ops["le"] = dtCmp(3);
        dt.ops["gt"] = dtCmp(4);
        dt.ops["ge"] = dtCmp(5);

        dt.ops["colon"] = [](Value &, Span<const Value> args, size_t, Span<Value> outs,
                             CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const Value &startVal = args[0];
            const Value &stopVal = (args.size() == 3) ? args[2] : args[1];
            if (!startVal.isObject() || startVal.objectClassName() != "datetime"
                || !stopVal.isObject() || stopVal.objectClassName() != "datetime")
                throw std::runtime_error("nk:datetime:InvalidColonEndpoints: Colon endpoints must be datetimes.");
            double startSd = dtElem(startVal, 0)->serialDay;
            double stopSd = dtElem(stopVal, 0)->serialDay;
            const auto *p0 = dtElem(startVal, 0);
            std::vector<double> sds;
            constexpr double kTol = 1e-9 / kSecondsPerDay;

            if (args.size() == 3 && args[1].isObject()
                && args[1].objectClassName() == "calendarDuration") {
                const auto *cp = calElem(args[1], 0);
                double approxStep = static_cast<double>(cp->months) * 30.0
                                    + static_cast<double>(cp->days)
                                    + cp->seconds / kSecondsPerDay;
                if (approxStep == 0.0)
                    throw std::runtime_error("nk:datetime:ZeroColonStep: Colon step cannot be zero.");
                for (int64_t k = 0; k < 100000; ++k) {
                    double cur = addCalendarMonths(startSd, cp->months * k, cp->days * k, cp->seconds * static_cast<double>(k));
                    if ((approxStep > 0.0 && cur > stopSd + kTol)
                        || (approxStep < 0.0 && cur < stopSd - kTol))
                        break;
                    sds.push_back(cur);
                }
            } else {
                double stepDays = 1.0;
                if (args.size() == 3) {
                    if (args[1].isObject() && args[1].objectClassName() == "duration")
                        stepDays = durElem(args[1], 0)->seconds / kSecondsPerDay;
                    else if (args[1].isNumeric())
                        stepDays = args[1].toScalar();
                    else
                        throw std::runtime_error("nk:datetime:InvalidColonStep: Unsupported colon step type.");
                }
                if (stepDays == 0.0)
                    throw std::runtime_error("nk:datetime:ZeroColonStep: Colon step cannot be zero.");
                for (int64_t k = 0; k < 100000; ++k) {
                    double cur = startSd + static_cast<double>(k) * stepDays;
                    if ((stepDays > 0.0 && cur > stopSd + kTol)
                        || (stepDays < 0.0 && cur < stopSd - kTol))
                        break;
                    sds.push_back(cur);
                }
            }
            outs[0] = makeDatetimeArray(Dims(1, sds.size()), sds,
                                        p0 ? p0->format : "", p0 ? p0->timeZone : "", mr);
        };

        dt.dispText = [hasNonZeroTime](const Value &self) -> std::string {
            const size_t R = self.dims().rows(), C = self.dims().cols();
            std::ostringstream os;
            if (R * C == 0) {
                os << "  " << R << "\xC3\x97" << C << " empty datetime array\n";
                return os.str();
            }
            bool anyTime = hasNonZeroTime(self);
            if (R * C > 1)
                os << "  " << R << "\xC3\x97" << C << " datetime array\n\n";
            else
                os << "  datetime\n\n";
            for (size_t r = 0; r < R; ++r) {
                os << "   ";
                for (size_t c = 0; c < C; ++c) {
                    const auto *p = dtElem(self, c * R + r);
                    os << " " << formatDateTimeElem(
                              p ? p->serialDay : std::numeric_limits<double>::quiet_NaN(),
                              p ? p->format : "", anyTime);
                    if (c + 1 < C) os << "   ";
                }
                os << "\n";
            }
            return os.str();
        };

        engine.registerClass(std::move(dt));
    }
}

} // namespace numkit::builtin::detail
