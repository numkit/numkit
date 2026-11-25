// src/builtin/register/timefun_datetime_class.cpp
// BuiltinClass registration for datetime.

#include "timefun_internal.hpp"

namespace numkit::builtin::detail::timefun {

void register_datetime_class(Engine &engine)
{
    auto *clsAlloc = engine.allocator();

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
                kl.to_ascii_lower_in_place();
                if (kl == "format") {
                    fmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else if (kl == "inputformat") {
                    inFmt = args[nPos - 1].toString();
                    nPos -= 2;
                } else if (kl == "convertfrom") {
                    convFrom = args[nPos - 1].toString();
                    convFrom.to_ascii_lower_in_place();
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
                    sds[i] = p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan();
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
                    throw numkit::Error("numkit:datetime:InvalidConvertFrom: Expected a single numeric input with 'ConvertFrom'.");
                const Value &x = args[0];
                const keel::usize n = x.numel();
                keel::Array<keel::f64> sds(mr);
                sds.resize(n);
                for (keel::usize i = 0; i < n; ++i) {
                    keel::f64 v = x.elemAsDouble(i);
                    if (!keel::is_finite(v)) {
                        sds[i] = keel::Limits<keel::f64>::quiet_nan();
                    } else if (convFrom == "datenum") {
                        sds[i] = v;
                    } else if (convFrom == "posixtime" || convFrom == "posix" || convFrom == "epochtime") {
                        sds[i] = kEpochSerialDay + v / kSecondsPerDay;
                    } else if (convFrom == "juliandate") {
                        sds[i] = v - 1721058.5;
                    } else if (convFrom == "mjuliandate") {
                        sds[i] = v + 678942.0;
                    } else if (convFrom == "excel") {
                        sds[i] = v + 693960.0;
                    } else if (convFrom == "yyyymmdd") {
                        keel::i64 iv = keel::as<keel::i64>(keel::round_i64(v));
                        keel::i64 y = iv / 10000;
                        keel::i64 m = (iv / 100) % 100;
                        keel::i64 d = iv % 100;
                        sds[i] = composeSerialDay(keel::as<keel::f64>(y), keel::as<keel::f64>(m), keel::as<keel::f64>(d));
                    } else {
                        throw numkit::Error(
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
                        if (keel::is_finite(v0) && v0 >= 1.0 && v0 <= 200000.0) {
                            const keel::usize n = args[0].numel();
                            keel::Array<keel::f64> sds(mr);
                            sds.resize(n);
                            for (keel::usize i = 0; i < n; ++i) {
                                keel::f64 v = args[0].elemAsDouble(i);
                                sds[i] = keel::is_finite(v) ? (v + 693960.0)
                                                          : keel::Limits<keel::f64>::quiet_nan();
                            }
                            return makeDatetimeArray(args[0].dims(), sds.as_ref(), fmt.as_ref(), tz.as_ref(), mr);
                        }
                    }
                    throw numkit::Error("numkit:datetime:InvalidDatevecSize: Numeric matrix input to datetime must have 3 or 6 columns.");
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
                        throw numkit::Error("numkit:datetime:NonNumericComponent: Date/time components must be numeric.");
                    if (args[k].numel() != 1) {
                        if (outDims.numel() == 1)
                            outDims = args[k].dims();
                        else if (args[k].dims() != outDims && args[k].numel() != outDims.numel())
                            throw numkit::Error("numkit:datetime:ComponentSizeMismatch: Date/time component arrays must be the same size or scalar.");
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

            throw numkit::Error("numkit:datetime:InvalidSyntax: Unsupported arguments to datetime constructor.");
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
        auto addCompMethod = [&](const char *mname, keel::i32 comp) {
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
                keel::f64 sd = p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan();
                return Value::scalar(keel::is_finite(sd) ? xform(sd) : sd, mr);
            }
            Value out = makeArrayFromDims(self.dims(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                keel::f64 sd = p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan();
                out.doubleDataMut()[i] = keel::is_finite(sd) ? xform(sd) : sd;
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
                CivilDateTime c = decomposeSerialDay(p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan());
                if (c.isNaT) {
                    for (keel::usize k = 0; k < 6; ++k)
                        dst[i + k * n] = keel::Limits<keel::f64>::quiet_nan();
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
                outs[0] = keel::move(dv);
            } else {
                for (keel::usize k = 0; k < keel::min<keel::usize>(nargout, 6); ++k) {
                    if (n == 1) {
                        outs[k] = Value::scalar(dst[k * n], mr);
                    } else {
                        Value col = Value::matrix(n, 1, ValueType::DOUBLE, mr);
                        for (keel::usize i = 0; i < n; ++i)
                            col.doubleDataMut()[i] = dst[i + k * n];
                        outs[k] = keel::move(col);
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
                    formatDateTimeElem(p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan(),
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
                    p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan(), f, anyTime);
                maxW = keel::max(maxW, row.size());
                rows.push_back(keel::move(row));
            }
            Value cm = Value::matrix(n, maxW, ValueType::CHAR, mr);
            char *dst = cm.charDataMut();
            keel::fill(dst, dst + n * maxW, ' ');
            for (keel::usize r = 0; r < n; ++r)
                for (keel::usize c = 0; c < rows[r].size(); ++c)
                    dst[c * n + r] = rows[r][c];
            outs[0] = keel::move(cm);
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
                    i, formatDateTimeElem(p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan(),
                                          f, anyTime));
            }
            outs[0] = keel::move(s);
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
                    formatDateTimeElem(p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan(),
                                       f, anyTime),
                    mr);
            }
            outs[0] = keel::move(c);
        });

        auto dtIsNat = [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                          CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            const keel::usize n = self.objectCount();
            if (n == 1) {
                const auto *p = dtElem(self, 0);
                outs[0] = Value::logicalScalar(!p || !keel::is_finite(p->serialDay), mr);
                return;
            }
            Value out = makeArrayFromDims(self.dims(), ValueType::LOGICAL, mr);
            for (keel::usize i = 0; i < n; ++i) {
                const auto *p = dtElem(self, i);
                out.logicalDataMut()[i] = (!p || !keel::is_finite(p->serialDay)) ? 1 : 0;
            }
            outs[0] = keel::move(out);
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
                throw numkit::Error("numkit:between:InvalidInput: Expected two datetime inputs.");
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
                keel::i64 wholeDays = keel::as<keel::i64>(keel::trunc(remDays));
                keel::f64 remSec = (remDays - keel::as<keel::f64>(wholeDays)) * kSecondsPerDay;
                items[i] = CalDurElemPayload(months, wholeDays, remSec);
            }
            outs[0] = makeCalDurArray(d, items.as_ref(), mr);
        });

        dt.methods.put(keel::String("dateshift", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() < 2)
                throw numkit::Error("numkit:dateshift:NotEnoughInputs: Expected dateshift(t, 'start'|'end', unit).");
            auto *mr = ctx.engine->allocator();
            keel::String rule = args[0].toString();
            keel::String unit = args[1].toString();
            rule.to_ascii_lower_in_place();
            unit.to_ascii_lower_in_place();bool isEnd = (rule == "end");
            const keel::usize n = self.objectCount();
            keel::Array<keel::f64> sds(mr);
            sds.resize(n);
            const auto *p0 = dtElem(self, 0);
            for (keel::usize i = 0; i < n; ++i) {
                CivilDateTime c = decomposeSerialDay(dtElem(self, i)->serialDay);
                if (c.isNaT) {
                    sds[i] = keel::Limits<keel::f64>::quiet_nan();
                    continue;
                }
                if (unit == "year" || unit == "years") {
                    sds[i] = isEnd ? composeSerialDay(c.year, 12, 31) : composeSerialDay(c.year, 1, 1);
                } else if (unit == "quarter" || unit == "quarters") {
                    keel::i32 q = (c.month - 1) / 3 + 1;
                    keel::i32 mStart = (q - 1) * 3 + 1;
                    keel::i32 mEnd = q * 3;
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
                dir.to_ascii_lower_in_place();
                desc = (dir == "descend");
            }
            keel::Array<keel::usize> order(mr);
            order.resize(n);
            for (keel::usize i = 0; i < n; ++i) order[i] = i;
            keel::stable_sort(order.begin(), order.end(), [&](keel::usize a, keel::usize b) {
                keel::f64 sa = dtElem(self, a)->serialDay;
                keel::f64 sb = dtElem(self, b)->serialDay;
                if (keel::is_nan(sa)) return false;
                if (keel::is_nan(sb)) return true;
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
                outs[1] = keel::move(idx);
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
                    if (keel::is_nan(v)) continue;
                    if (keel::is_nan(bestVal) || (isMax ? (v > bestVal) : (v < bestVal))) {
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
                    s.to_ascii_lower_in_place();
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
                keel::stable_sort(order.begin(), order.end(), [&](keel::usize a, keel::usize b) {
                    keel::f64 sa = dtElem(self, a)->serialDay;
                    keel::f64 sb = dtElem(self, b)->serialDay;
                    if (keel::is_nan(sa)) return false;
                    if (keel::is_nan(sb)) return true;
                    return sa < sb - kTol;
                });
                for (keel::usize k = 0; k < n; ++k) {
                    keel::usize idx = order[k];
                    keel::f64 sd = dtElem(self, idx)->serialDay;
                    if (uniqSds.empty() || keel::is_nan(sd) || keel::is_nan(uniqSds.back())
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
                outs[1] = keel::move(iaVal);
            }
            if (nargout >= 3) {
                Value icVal = Value::matrix(n, 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < n; ++i)
                    icVal.doubleDataMut()[i] = keel::as<keel::f64>(ic[i]);
                outs[2] = keel::move(icVal);
            }
        });

        dt.methods.put(keel::String("intersect", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassNameRef() != "datetime")
                throw numkit::Error("numkit:intersect:InvalidInput: Expected two datetime inputs.");
            auto *mr = ctx.engine->allocator();
            const Value &other = args[0];
            bool stable = false;
            for (keel::usize k = 1; k < args.size(); ++k) {
                if (args[k].isChar() || args[k].isString()) {
                    keel::String s = args[k].toString();
                    s.to_ascii_lower_in_place();
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
                if (keel::is_nan(sa)) continue;
                bool dup = false;
                for (const auto &it : items) {
                    if (keel::abs(it.sd - sa) <= kTol) { dup = true; break; }
                }
                if (dup) continue;
                for (keel::usize j = 0; j < nB; ++j) {
                    keel::f64 sb = dtElem(other, j)->serialDay;
                    if (!keel::is_nan(sb) && keel::abs(sa - sb) <= kTol) {
                        items.push_back({sa, i + 1, j + 1});
                        break;
                    }
                }
            }
            if (!stable) {
                keel::sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
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
                outs[1] = keel::move(iaVal);
            }
            if (nargout >= 3) {
                Value ibVal = Value::matrix(items.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < items.size(); ++i)
                    ibVal.doubleDataMut()[i] = keel::as<keel::f64>(items[i].ib);
                outs[2] = keel::move(ibVal);
            }
        });

        dt.methods.put(keel::String("ismember", clsAlloc),
                       [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isObject() || args[0].objectClassNameRef() != "datetime")
                throw numkit::Error("numkit:ismember:InvalidInput: Expected two datetime inputs.");
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
                if (!keel::is_nan(sa)) {
                    for (keel::usize j = 0; j < nB; ++j) {
                        keel::f64 sb = dtElem(other, j)->serialDay;
                        if (!keel::is_nan(sb) && keel::abs(sa - sb) <= kTol) {
                            found = j + 1;
                            break;
                        }
                    }
                }
                lia.logicalDataMut()[i] = found > 0 ? 1 : 0;
                locb.doubleDataMut()[i] = keel::as<keel::f64>(found);
            }
            outs[0] = keel::move(lia);
            if (nargout >= 2)
                outs[1] = keel::move(locb);
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
                throw numkit::Error(
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
                throw numkit::Error("numkit:datetime:InvalidMinus: Cannot subtract a datetime from a non-datetime.");

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
                throw numkit::Error(
                    keel::format("numkit:datetime:InvalidMinusOperand: Cannot subtract value of type '{}' from a datetime.",
                                 mtypeName(rhs.type())).c_str());
            }
            outs[0] = makeDatetimeArray(d, sds.as_ref(),
                                        p0 ? p0->format.as_ref() : keel::StringRef(""),
                                        p0 ? p0->timeZone.as_ref() : keel::StringRef(""), mr);
        });

        auto dtCmp = [](keel::i32 mode) -> ObjectMethod {
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
                    if (keel::is_nan(a) || keel::is_nan(b))
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
                outs[0] = keel::move(out);
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
                throw numkit::Error("numkit:datetime:InvalidColonEndpoints: Colon endpoints must be datetimes.");
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
                    throw numkit::Error("numkit:datetime:ZeroColonStep: Colon step cannot be zero.");
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
                        throw numkit::Error("numkit:datetime:InvalidColonStep: Unsupported colon step type.");
                }
                if (stepDays == 0.0)
                    throw numkit::Error("numkit:datetime:ZeroColonStep: Colon step cannot be zero.");
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
                        p ? p->serialDay : keel::Limits<keel::f64>::quiet_nan(),
                        p ? p->format.as_ref() : keel::StringRef(""), anyTime).as_ref());
                    if (c + 1 < C) os.append("   ");
                }
                os.push_back('\n');
            }
            return os;
        };

        engine.registerClass(keel::move(dt));
    }
}

} // namespace numkit::builtin::detail::timefun
