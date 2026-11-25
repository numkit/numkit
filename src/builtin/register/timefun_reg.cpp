// src/builtin/register/timefun_reg.cpp
// Scalar and legacy date/time registrations + BuiltinClass dispatch.

#include "timefun_internal.hpp"

namespace numkit::builtin::detail {

namespace {

using namespace timefun;

void clock_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 0, "clock");
    outs[0] = numkit::builtin::clock(ctx.engine->allocator());
}

void date_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
              CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 0, "date");
    outs[0] = Value::fromString(numkit::builtin::date(), ctx.engine->allocator());
}

void pause_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
               CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 1, "pause");
    static bool pauseEnabled = true;
    if (!args.empty() && (args[0].isChar() || args[0].isString())) {
        const char *prev = pauseEnabled ? "on" : "off";
        keel::String mode = args[0].toString();
        mode.to_ascii_lower_in_place();
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
}

void tic_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
             CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 0, "tic");
    const keel::u64 nowNs = keel::monotonic_ns();
    coreEngine(ctx).setTicTimer(nowNs);
    if (nargout > 0) {
        const keel::f64 id = keel::as<keel::f64>(nowNs / 1000ULL);
        outs[0] = Value::scalar(id, ctx.engine->allocator());
    } else {
        outs[0] = Value();
    }
}

void toc_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
             CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 1, "toc");
    const keel::u64 nowNs = keel::monotonic_ns();
    keel::u64 startNs = 0;
    if (!args.empty() && args[0].isScalar()) {
        const keel::f64 v = args[0].toScalar();
        startNs = (v > 0.0) ? keel::as<keel::u64>(v) * 1000ULL : 0ULL;
    } else if (coreEngine(ctx).ticWasCalled()) {
        startNs = coreEngine(ctx).ticTimer();
    } else {
        throw numkit::Error("toc: You must call 'tic' before calling 'toc'.");
    }
    const keel::f64 elapsed = (nowNs >= startNs) ? keel::as<keel::f64>(nowNs - startNs) * 1e-9 : 0.0;
    if (nargout > 0) {
        outs[0] = Value::scalar(elapsed, ctx.engine->allocator());
    } else {
        coreEngine(ctx).outputText(keel::format("Elapsed time is %g seconds.\n", elapsed));
        outs[0] = Value();
    }
}

void cputime_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 0, "cputime");
    outs[0] = Value::scalar(numkit::builtin::cputime(), ctx.engine->allocator());
}

void now_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
             CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 0, "now");
    outs[0] = Value::scalar(numkit::builtin::now(), ctx.engine->allocator());
}

void etime_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx) {
    argcheck::checkNargin(args.size(), 2, 2, "etime");
    outs[0] = numkit::builtin::etime(args[0], args[1], ctx.engine->allocator());
}

void weeknum_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "weeknum");
    outs[0] = numkit::builtin::weeknum(args, ctx.engine->allocator());
}

void addtodate_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                   CallContext &ctx) {
    argcheck::checkNargin(args.size(), 3, 3, "addtodate");
    outs[0] = numkit::builtin::addtodate(args[0], args[1].toScalar(), args[2].toString(),
                                         ctx.engine->allocator());
}

void datenum_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 6, "datenum");
    outs[0] = numkit::builtin::datenum(args, ctx.engine->allocator());
}

void weekday_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "weekday");
    auto *mr = ctx.engine->allocator();
    outs[0] = numkit::builtin::weekday(args[0], mr);
    if (nargout > 1) {
        bool wantLong = false;
        if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
            keel::String fmt = args[1].toString();
            fmt.to_ascii_lower_in_place();
            if (fmt == "long")
                wantLong = true;
            else if (fmt != "short")
                throw numkit::Error("weekday: format must be 'short' or 'long'");
        }
        static const char *kShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *kLong[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        keel::f64 dval = args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(0);
        keel::i64 f = keel::as<keel::i64>(keel::floor(dval)) - 2;
        keel::i64 r = f % 7;
        if (r < 0) r += 7;
        keel::i32 dayIdx = keel::as<keel::i32>(r) + 1;
        outs[1] = Value::fromString(wantLong ? kLong[dayIdx - 1] : kShort[dayIdx - 1], mr);
    }
}

void juliandate_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                    CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 6, "juliandate");
    outs[0] = numkit::builtin::juliandate(args, ctx.engine->allocator());
}

void eomday_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                CallContext &ctx) {
    argcheck::checkNargin(args.size(), 2, 2, "eomday");
    outs[0] = numkit::builtin::eomday(args[0], args[1], ctx.engine->allocator());
}

void calendar_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 2, "calendar");
    outs[0] = numkit::builtin::calendar(args, ctx.engine->allocator());
}

void datestr_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "datestr");
    outs[0] = numkit::builtin::datestr(args, ctx.engine->allocator());
}

void datevec_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "datevec");
    auto *mr = ctx.engine->allocator();
    Value dv = numkit::builtin::datevec(args, mr);
    if (nargout <= 1) {
        outs[0] = keel::move(dv);
    } else {
        const keel::usize N = dv.dims().rows();
        const keel::f64 *o = dv.doubleData();
        for (keel::usize c = 0; c < keel::min(nargout, keel::usize(6)); ++c) {
            if (N == 1) {
                outs[c] = Value::scalar(o[c * N], mr);
            } else {
                auto col = Value::matrix(N, 1, ValueType::DOUBLE, mr);
                keel::f64 *p = col.doubleDataMut();
                for (keel::usize i = 0; i < N; ++i)
                    p[i] = o[i + c * N];
                outs[c] = keel::move(col);
            }
        }
    }
}

void yyyymmdd_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "yyyymmdd");
    outs[0] = numkit::builtin::yyyymmdd(args, ctx.engine->allocator());
}

void mjuliandate_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                     CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 6, "mjuliandate");
    outs[0] = numkit::builtin::mjuliandate(args, ctx.engine->allocator());
}

void seconds_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    outs[0] = constructDurationFromUnit(args, 1.0, "s", "seconds", ctx.engine->allocator());
}

void milliseconds_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                      CallContext &ctx) {
    outs[0] = constructDurationFromUnit(args, 0.001, "s", "milliseconds", ctx.engine->allocator());
}

void minutes_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    outs[0] = constructDurationFromUnit(args, 60.0, "m", "minutes", ctx.engine->allocator());
}

void hours_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx) {
    outs[0] = constructDurationFromUnit(args, 3600.0, "h", "hours", ctx.engine->allocator());
}

void days_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
              CallContext &ctx) {
    outs[0] = constructDurationFromUnit(args, kSecondsPerDay, "d", "days", ctx.engine->allocator());
}

void years_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx) {
    outs[0] = constructDurationFromUnit(args, kSecondsPerYear, "y", "years", ctx.engine->allocator());
}

void caldays_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx) {
    outs[0] = constructCalDurFromUnit(args, 0, 1, "caldays", ctx.engine->allocator());
}

void calweeks_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx) {
    outs[0] = constructCalDurFromUnit(args, 0, 7, "calweeks", ctx.engine->allocator());
}

void calmonths_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                   CallContext &ctx) {
    outs[0] = constructCalDurFromUnit(args, 1, 0, "calmonths", ctx.engine->allocator());
}

void calquarters_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                     CallContext &ctx) {
    outs[0] = constructCalDurFromUnit(args, 3, 0, "calquarters", ctx.engine->allocator());
}

void calyears_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx) {
    outs[0] = constructCalDurFromUnit(args, 12, 0, "calyears", ctx.engine->allocator());
}

void isdatetime_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                    CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "isdatetime");
    outs[0] = Value::logicalScalar(
        args[0].isObject() && args[0].objectClassNameRef() == "datetime",
        ctx.engine->allocator());
}

void isduration_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                    CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "isduration");
    outs[0] = Value::logicalScalar(
        args[0].isObject() && args[0].objectClassNameRef() == "duration",
        ctx.engine->allocator());
}

void iscalendarduration_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                            CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "iscalendarduration");
    outs[0] = Value::logicalScalar(
        args[0].isObject() && args[0].objectClassNameRef() == "calendarDuration",
        ctx.engine->allocator());
}

void isnat_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "isnat");
    auto *mr = ctx.engine->allocator();
    if (!args[0].isObject() || args[0].objectClassNameRef() != "datetime") {
        outs[0] = Value::logicalScalar(false, mr);
        return;
    }
    const keel::usize n = args[0].objectCount();
    if (n == 1) {
        const auto *p = dtElem(args[0], 0);
        outs[0] = Value::logicalScalar(!p || !keel::is_finite(p->serialDay), mr);
        return;
    }
    Value out = makeArrayFromDims(args[0].dims(), ValueType::LOGICAL, mr);
    keel::u8 *dst = out.logicalDataMut();
    for (keel::usize i = 0; i < n; ++i) {
        const auto *p = dtElem(args[0], i);
        dst[i] = (!p || !keel::is_finite(p->serialDay)) ? 1 : 0;
    }
    outs[0] = keel::move(out);
}

void nat_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
             CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 4, "NaT");
    auto *mr = ctx.engine->allocator();
    keel::usize r = 1, c = 1;
    if (args.size() == 1 && args[0].isNumeric()) {
        if (args[0].numel() == 1) {
            r = c = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
        } else if (args[0].numel() >= 2) {
            r = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
            c = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(1)));
        }
    } else if (args.size() >= 2 && args[0].isNumeric() && args[1].isNumeric()) {
        r = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
        c = keel::as<keel::usize>(keel::max(0.0, args[1].elemAsDouble(0)));
    }
    keel::Array<keel::f64> nats(mr);
    nats.assign(r * c, keel::Limits<keel::f64>::quiet_nan());
    outs[0] = makeDatetimeArray(Dims(r, c), nats.as_ref(), "", "", mr);
}

struct TimefunRegEntry {
    const char *name;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
};

constexpr TimefunRegEntry kTimefunFns[] = {
    {"clock",              &clock_reg},
    {"date",               &date_reg},
    {"pause",              &pause_reg},
    {"tic",                &tic_reg},
    {"toc",                &toc_reg},
    {"cputime",            &cputime_reg},
    {"now",                &now_reg},
    {"etime",              &etime_reg},
    {"weeknum",            &weeknum_reg},
    {"addtodate",          &addtodate_reg},
    {"datenum",            &datenum_reg},
    {"weekday",            &weekday_reg},
    {"juliandate",         &juliandate_reg},
    {"eomday",             &eomday_reg},
    {"calendar",           &calendar_reg},
    {"datestr",            &datestr_reg},
    {"datevec",            &datevec_reg},
    {"yyyymmdd",           &yyyymmdd_reg},
    {"mjuliandate",        &mjuliandate_reg},
    {"seconds",            &seconds_reg},
    {"milliseconds",       &milliseconds_reg},
    {"minutes",            &minutes_reg},
    {"hours",              &hours_reg},
    {"days",               &days_reg},
    {"years",              &years_reg},
    {"caldays",            &caldays_reg},
    {"calweeks",           &calweeks_reg},
    {"calmonths",          &calmonths_reg},
    {"calquarters",        &calquarters_reg},
    {"calyears",           &calyears_reg},
    {"isdatetime",         &isdatetime_reg},
    {"isduration",         &isduration_reg},
    {"iscalendarduration", &iscalendarduration_reg},
    {"isnat",              &isnat_reg},
    {"NaT",                &nat_reg},
};

} // namespace

void register_timefun(Engine &engine)
{
    for (const auto &entry : kTimefunFns)
        engine.registerFunction(entry.name, entry.fn);

    timefun::register_duration_and_caldur_classes(engine);
    timefun::register_datetime_class(engine);
}

} // namespace numkit::builtin::detail
