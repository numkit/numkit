// src/builtin/register/timefun_duration_class.cpp
// BuiltinClass registrations for duration and calendarDuration.

#include "timefun_internal.hpp"

namespace numkit::builtin::detail::timefun {

void register_duration_and_caldur_classes(Engine &engine)
{
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
                k.to_ascii_lower_in_place();
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
            throw numkit::Error("numkit:duration:InvalidArgs: Expected duration(H, MI, S) or duration([H MI S]).");
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
            outs[0] = keel::move(s);
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
                keel::f64 hh = keel::floor(a / 3600.0);
                keel::f64 mm = keel::floor((a - hh * 3600.0) / 60.0);
                keel::f64 ss = a - hh * 3600.0 - mm * 60.0;
                h.doubleDataMut()[i] = sign * hh;
                m.doubleDataMut()[i] = sign * mm;
                s.doubleDataMut()[i] = sign * ss;
            }
            outs[0] = (n == 1) ? Value::scalar(h.doubleData()[0], mr) : keel::move(h);
            if (nargout >= 2)
                outs[1] = (n == 1) ? Value::scalar(m.doubleData()[0], mr) : keel::move(m);
            if (nargout >= 3)
                outs[2] = (n == 1) ? Value::scalar(s.doubleData()[0], mr) : keel::move(s);
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
                    if (keel::is_nan(v)) continue;
                    if (keel::is_nan(bestVal) || (isMax ? (v > bestVal) : (v < bestVal))) {
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
                    throw numkit::Error("numkit:duration:TypeMismatch: Both operands must be durations.");
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
                    outs[0] = keel::move(out);
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

        auto durCmp = [](keel::i32 mode) -> ObjectMethod {
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
                outs[0] = keel::move(out);
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

        engine.registerClass(keel::move(dur));
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
                    keel::i64 y = keel::as<keel::i64>(keel::round(args[0].elemAsDouble(r)));
                    keel::i64 m = keel::as<keel::i64>(keel::round(args[0].elemAsDouble(r + R)));
                    keel::i64 d = keel::as<keel::i64>(keel::round(args[0].elemAsDouble(r + 2 * R)));
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
                    keel::i64 y = keel::as<keel::i64>(keel::round(args[0].numel() == 1 ? args[0].toScalar() : args[0].elemAsDouble(i)));
                    keel::i64 m = keel::as<keel::i64>(keel::round(args[1].numel() == 1 ? args[1].toScalar() : args[1].elemAsDouble(i)));
                    keel::i64 dy = keel::as<keel::i64>(keel::round(args[2].numel() == 1 ? args[2].toScalar() : args[2].elemAsDouble(i)));
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
            throw numkit::Error("numkit:calendarDuration:InvalidArgs: Expected calendarDuration(Y, M, D) or calendarDuration(Y, M, D, H, MI, S).");
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
            outs[0] = keel::move(s);
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
                    throw numkit::Error("numkit:calendarDuration:TypeMismatch: Both operands must be calendarDurations.");
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
                outs[0] = keel::move(out);
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

        engine.registerClass(keel::move(cal));
    }
}

} // namespace numkit::builtin::detail::timefun
