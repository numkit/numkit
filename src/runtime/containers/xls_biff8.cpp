// runtime/containers/xls_biff8.cpp
//
// Standalone OLE2/CFBF + BIFF8 (.xls) spreadsheet parser used by xlsread,
// readtable, and detectImportOptions.

#include "table_internal.hpp"

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/string.h>

namespace numkit::runtime::containers_detail {

keel::Array<keel::Array<GridCell>> parseXlsBiff8Grid(
    keel::StringRef bytes, keel::StringRef sheetSpec, keel::i32 sheetNumSpec)
{
    keel::Array<keel::Array<GridCell>> empty;
    if (bytes.size() < 512) return empty;
    const auto *raw = reinterpret_cast<const keel::u8 *>(bytes.data());
    auto rd16 = [](const keel::u8 *p, keel::usize sz, keel::usize off) -> keel::u16 {
        if (off + 2 > sz) return 0;
        return keel::as<keel::u16>(p[off] | (keel::as<keel::u16>(p[off + 1]) << 8u));
    };
    auto rd32 = [](const keel::u8 *p, keel::usize sz, keel::usize off) -> keel::u32 {
        if (off + 4 > sz) return 0;
        return keel::as<keel::u32>(p[off])
             | (keel::as<keel::u32>(p[off + 1]) << 8u)
             | (keel::as<keel::u32>(p[off + 2]) << 16u)
             | (keel::as<keel::u32>(p[off + 3]) << 24u);
    };
    auto rd64f = [](const keel::u8 *p, keel::usize sz, keel::usize off) -> keel::f64 {
        if (off + 8 > sz) return 0.0;
        keel::f64 v = 0.0;
        keel::mem_copy(&v, p + off, 8);
        return v;
    };

    const keel::u16 secShift = rd16(raw, bytes.size(), 30);
    const keel::u16 miniShift = rd16(raw, bytes.size(), 32);
    if (secShift < 9 || secShift > 16) return empty;
    const keel::usize secSz = keel::usize{1} << secShift;
    const keel::usize miniSz = (miniShift >= 4 && miniShift <= 10) ? (keel::usize{1} << miniShift) : 64u;
    const keel::u32 dirStart = rd32(raw, bytes.size(), 48);
    const keel::u32 miniCutoff = rd32(raw, bytes.size(), 56);
    const keel::u32 miniFatStart = rd32(raw, bytes.size(), 60);
    const keel::u32 difatStart = rd32(raw, bytes.size(), 68);
    const keel::u32 difatCount = rd32(raw, bytes.size(), 72);

    static constexpr keel::u32 kEndOfChain = 0xFFFFFFFEu;
    static constexpr keel::u32 kFreeSect   = 0xFFFFFFFFu;
    auto secOff = [&](keel::u32 sec) -> keel::usize {
        return (keel::as<keel::usize>(sec) + 1u) * secSz;
    };

    keel::Array<keel::u32> fatSecs;
    for (keel::usize i = 0; i < 109; ++i) {
        const keel::u32 s = rd32(raw, bytes.size(), 76 + i * 4);
        if (s >= 0xFFFFFFFCu) break;
        fatSecs.push_back(s);
    }
    keel::u32 dSec = difatStart;
    for (keel::u32 di = 0; di < difatCount && dSec < 0xFFFFFFFCu; ++di) {
        const keel::usize off = secOff(dSec);
        if (off + secSz > bytes.size()) break;
        const keel::usize nEnt = secSz / 4 - 1;
        for (keel::usize i = 0; i < nEnt; ++i) {
            const keel::u32 s = rd32(raw, bytes.size(), off + i * 4);
            if (s < 0xFFFFFFFCu) fatSecs.push_back(s);
        }
        dSec = rd32(raw, bytes.size(), off + nEnt * 4);
    }

    keel::Array<keel::u32> fat;
    fat.reserve(fatSecs.size() * (secSz / 4));
    for (keel::usize fi = 0; fi < fatSecs.size(); ++fi) {
        const keel::usize off = secOff(fatSecs[fi]);
        if (off + secSz > bytes.size()) break;
        for (keel::usize i = 0; i < secSz / 4; ++i)
            fat.push_back(rd32(raw, bytes.size(), off + i * 4));
    }

    auto readFatStream = [&](keel::u32 startSec, keel::usize maxLen) -> keel::Array<keel::u8> {
        keel::Array<keel::u8> out;
        keel::u32 cur = startSec;
        keel::usize guard = 0;
        const keel::usize maxHops = fat.size() + 1;
        while (cur < 0xFFFFFFFCu && cur < fat.size() && guard++ < maxHops) {
            const keel::usize off = secOff(cur);
            if (off + secSz > bytes.size()) break;
            const keel::usize oldSz = out.size();
            const keel::usize take = (maxLen > 0 && oldSz + secSz > maxLen) ? (maxLen - oldSz) : secSz;
            out.resize(oldSz + take);
            keel::mem_copy(out.data() + oldSz, raw + off, take);
            if (maxLen > 0 && out.size() >= maxLen) break;
            cur = fat[cur];
        }
        return out;
    };

    keel::Array<keel::u8> dir = readFatStream(dirStart, 0);
    keel::u32 wbStart = kEndOfChain;
    keel::u32 wbSize = 0;
    keel::u32 rootStart = kEndOfChain;
    keel::u32 rootSize = 0;
    for (keel::usize off = 0; off + 128 <= dir.size(); off += 128) {
        const keel::u16 nlen = rd16(dir.data(), dir.size(), off + 64);
        const keel::u8 etype = dir[off + 66];
        if (nlen < 2 || nlen > 64) continue;
        keel::String name;
        for (keel::usize k = 0; k + 2 < nlen; k += 2) {
            const keel::u16 ch = rd16(dir.data(), dir.size(), off + k);
            if (ch == 0) break;
            name.push_back(ch < 128 ? keel::as<char>(ch) : '?');
        }
        const keel::u32 sStart = rd32(dir.data(), dir.size(), off + 116);
        const keel::u32 sSize = rd32(dir.data(), dir.size(), off + 120);
        if (etype == 5) {
            rootStart = sStart;
            rootSize = sSize;
        } else if (etype == 2 && (iequalsTableOpt(name.as_ref(), "Workbook")
                                  || iequalsTableOpt(name.as_ref(), "Book"))) {
            wbStart = sStart;
            wbSize = sSize;
        }
    }
    if (wbStart == kEndOfChain || wbStart == kFreeSect || wbSize == 0) return empty;

    keel::Array<keel::u8> wb;
    if (wbSize < miniCutoff && rootStart < 0xFFFFFFFCu && miniFatStart < 0xFFFFFFFCu) {
        keel::Array<keel::u8> miniFatBytes = readFatStream(miniFatStart, 0);
        keel::Array<keel::u8> miniStream = readFatStream(rootStart, rootSize);
        keel::Array<keel::u32> miniFat;
        for (keel::usize i = 0; i + 4 <= miniFatBytes.size(); i += 4)
            miniFat.push_back(rd32(miniFatBytes.data(), miniFatBytes.size(), i));
        keel::u32 cur = wbStart;
        keel::usize guard = 0;
        while (cur < 0xFFFFFFFCu && cur < miniFat.size() && guard++ < miniFat.size() + 1) {
            const keel::usize off = keel::as<keel::usize>(cur) * miniSz;
            if (off + miniSz > miniStream.size()) break;
            const keel::usize oldSz = wb.size();
            const keel::usize take = (oldSz + miniSz > wbSize) ? (wbSize - oldSz) : miniSz;
            wb.resize(oldSz + take);
            keel::mem_copy(wb.data() + oldSz, miniStream.data() + off, take);
            if (wb.size() >= wbSize) break;
            cur = miniFat[cur];
        }
    } else {
        wb = readFatStream(wbStart, wbSize);
    }
    if (wb.size() < 4) return empty;

    auto decodeRk = [](keel::u32 rk) -> keel::f64 {
        keel::f64 val = 0.0;
        if (rk & 2u) {
            const keel::i32 iv = keel::as<keel::i32>(rk) >> 2;
            val = keel::as<keel::f64>(iv);
        } else {
            const keel::u64 bits = keel::as<keel::u64>(rk & 0xFFFFFFFCu) << 32u;
            keel::mem_copy(&val, &bits, 8);
        }
        if (rk & 1u) val /= 100.0;
        return val;
    };

    auto appendUtf8 = [](keel::String &dst, keel::u16 cp) {
        if (cp < 0x80u) {
            dst.push_back(keel::as<char>(cp));
        } else if (cp < 0x800u) {
            dst.push_back(keel::as<char>(0xC0u | (cp >> 6u)));
            dst.push_back(keel::as<char>(0x80u | (cp & 0x3Fu)));
        } else {
            dst.push_back(keel::as<char>(0xE0u | (cp >> 12u)));
            dst.push_back(keel::as<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
            dst.push_back(keel::as<char>(0x80u | (cp & 0x3Fu)));
        }
    };

    struct SheetInfo {
        keel::u32 bofOff = 0;
        keel::String name;
    };
    keel::Array<SheetInfo> sheets;
    keel::Array<keel::String> sst;

    keel::usize pos = 0;
    while (pos + 4 <= wb.size()) {
        const keel::u16 recId = rd16(wb.data(), wb.size(), pos);
        const keel::u16 recLen = rd16(wb.data(), wb.size(), pos + 2);
        const keel::usize body = pos + 4;
        if (body + recLen > wb.size()) break;
        if (recId == 0x000Au) break;
        if (recId == 0x0085u && recLen >= 8) {
            SheetInfo si;
            si.bofOff = rd32(wb.data(), wb.size(), body);
            const keel::u8 cch = wb[body + 6];
            const keel::u8 flags = wb[body + 7];
            const bool isUtf16 = (flags & 1u) != 0u;
            keel::usize p = body + 8;
            for (keel::usize k = 0; k < cch; ++k) {
                if (isUtf16) {
                    if (p + 2 > body + recLen) break;
                    appendUtf8(si.name, rd16(wb.data(), wb.size(), p));
                    p += 2;
                } else {
                    if (p + 1 > body + recLen) break;
                    appendUtf8(si.name, wb[p++]);
                }
            }
            sheets.push_back(keel::move(si));
        } else if (recId == 0x00FCu && recLen >= 8) {
            struct Chunk { keel::usize off; keel::usize len; };
            keel::Array<Chunk> chunks;
            chunks.push_back(Chunk{body, recLen});
            keel::usize scan = body + recLen;
            while (scan + 4 <= wb.size()) {
                const keel::u16 cid = rd16(wb.data(), wb.size(), scan);
                const keel::u16 clen = rd16(wb.data(), wb.size(), scan + 2);
                if (cid != 0x003Cu || scan + 4 + clen > wb.size()) break;
                chunks.push_back(Chunk{scan + 4, clen});
                scan += 4 + clen;
            }
            const keel::u32 uniqueCount = rd32(wb.data(), wb.size(), body + 4);
            keel::usize ci = 0;
            keel::usize co = 8;
            auto ensureByte = [&]() -> bool {
                while (ci < chunks.size() && co >= chunks[ci].len) {
                    ++ci;
                    co = 0;
                }
                return ci < chunks.size();
            };
            auto getU8 = [&]() -> keel::u8 {
                if (!ensureByte()) return 0;
                return wb[chunks[ci].off + co++];
            };
            auto getU16 = [&]() -> keel::u16 {
                const keel::u8 lo = getU8();
                const keel::u8 hi = getU8();
                return keel::as<keel::u16>(lo | (keel::as<keel::u16>(hi) << 8u));
            };
            auto getU32 = [&]() -> keel::u32 {
                const keel::u32 a = getU8();
                const keel::u32 b = getU8();
                const keel::u32 c = getU8();
                const keel::u32 d = getU8();
                return a | (b << 8u) | (c << 16u) | (d << 24u);
            };
            auto skipBytes = [&](keel::usize n) {
                while (n > 0 && ensureByte()) {
                    const keel::usize avail = chunks[ci].len - co;
                    const keel::usize step = keel::min(avail, n);
                    co += step;
                    n -= step;
                }
            };
            for (keel::u32 sIdx = 0; sIdx < uniqueCount && ensureByte(); ++sIdx) {
                const keel::u16 cch = getU16();
                const keel::u8 flags = getU8();
                bool isUtf16 = (flags & 0x01u) != 0u;
                const bool hasExt = (flags & 0x04u) != 0u;
                const bool hasRich = (flags & 0x08u) != 0u;
                const keel::u16 richRuns = hasRich ? getU16() : 0u;
                const keel::u32 extLen = hasExt ? getU32() : 0u;
                keel::String str;
                keel::usize rem = cch;
                while (rem > 0 && ci < chunks.size()) {
                    if (co >= chunks[ci].len) {
                        ++ci;
                        co = 0;
                        if (ci >= chunks.size()) break;
                        const keel::u8 cflag = wb[chunks[ci].off + co++];
                        isUtf16 = (cflag & 0x01u) != 0u;
                    }
                    if (isUtf16) {
                        if (co + 2 > chunks[ci].len) { co = chunks[ci].len; continue; }
                        appendUtf8(str, rd16(wb.data(), wb.size(), chunks[ci].off + co));
                        co += 2;
                    } else {
                        appendUtf8(str, wb[chunks[ci].off + co]);
                        ++co;
                    }
                    --rem;
                }
                if (richRuns > 0) skipBytes(keel::as<keel::usize>(richRuns) * 4u);
                if (extLen > 0) skipBytes(keel::as<keel::usize>(extLen));
                sst.push_back(keel::move(str));
            }
            pos = scan;
            continue;
        }
        pos = body + recLen;
    }

    keel::usize chosen = 0;
    if (!sheetSpec.empty()) {
        keel::String want = keel::String::from(sheetSpec);
        want.to_ascii_lower_in_place();
        for (keel::usize i = 0; i < sheets.size(); ++i) {
            keel::String cand = sheets[i].name.clone();
            cand.to_ascii_lower_in_place();
            if (cand == want) {
                chosen = i;
                break;
            }
        }
    } else if (sheetNumSpec >= 1 && keel::as<keel::usize>(sheetNumSpec) <= sheets.size()) {
        chosen = keel::as<keel::usize>(sheetNumSpec - 1);
    }
    if (sheets.empty() || sheets[chosen].bofOff >= wb.size()) return empty;

    struct RawCell {
        keel::usize r = 0;
        keel::usize c = 0;
        bool isStr = false;
        keel::String text;
    };
    keel::Array<RawCell> cells;
    keel::usize maxR = 0, maxC = 0;
    auto pushNum = [&](keel::usize r, keel::usize c, keel::f64 v) {
        RawCell rc;
        rc.r = r; rc.c = c; rc.isStr = false;
        rc.text = keel::format("{:.17g}", v);
        cells.push_back(keel::move(rc));
        maxR = keel::max(maxR, r + 1);
        maxC = keel::max(maxC, c + 1);
    };
    auto pushStr = [&](keel::usize r, keel::usize c, keel::StringRef s) {
        RawCell rc;
        rc.r = r; rc.c = c; rc.isStr = true;
        rc.text = keel::String::from(s);
        cells.push_back(keel::move(rc));
        maxR = keel::max(maxR, r + 1);
        maxC = keel::max(maxC, c + 1);
    };

    pos = sheets[chosen].bofOff;
    keel::i32 bofDepth = 0;
    bool pendingFormulaStr = false;
    keel::usize formulaR = 0, formulaC = 0;
    while (pos + 4 <= wb.size()) {
        const keel::u16 recId = rd16(wb.data(), wb.size(), pos);
        const keel::u16 recLen = rd16(wb.data(), wb.size(), pos + 2);
        const keel::usize body = pos + 4;
        if (body + recLen > wb.size()) break;
        if (recId == 0x0809u) {
            ++bofDepth;
        } else if (recId == 0x000Au) {
            --bofDepth;
            if (bofDepth <= 0) break;
        } else if (bofDepth == 1) {
            if (recId == 0x0203u && recLen >= 14) {
                pushNum(rd16(wb.data(), wb.size(), body),
                        rd16(wb.data(), wb.size(), body + 2),
                        rd64f(wb.data(), wb.size(), body + 6));
                pendingFormulaStr = false;
            } else if (recId == 0x027Eu && recLen >= 10) {
                pushNum(rd16(wb.data(), wb.size(), body),
                        rd16(wb.data(), wb.size(), body + 2),
                        decodeRk(rd32(wb.data(), wb.size(), body + 6)));
                pendingFormulaStr = false;
            } else if (recId == 0x00BDu && recLen >= 6) {
                const keel::usize r = rd16(wb.data(), wb.size(), body);
                const keel::usize cFirst = rd16(wb.data(), wb.size(), body + 2);
                const keel::usize nCols = (recLen - 6) / 6;
                for (keel::usize k = 0; k < nCols; ++k) {
                    const keel::u32 rk = rd32(wb.data(), wb.size(), body + 4 + k * 6 + 2);
                    pushNum(r, cFirst + k, decodeRk(rk));
                }
                pendingFormulaStr = false;
            } else if (recId == 0x00FDu && recLen >= 10) {
                const keel::usize r = rd16(wb.data(), wb.size(), body);
                const keel::usize c = rd16(wb.data(), wb.size(), body + 2);
                const keel::u32 idx = rd32(wb.data(), wb.size(), body + 6);
                if (idx < sst.size()) pushStr(r, c, sst[idx].as_ref());
                pendingFormulaStr = false;
            } else if ((recId == 0x0204u || recId == 0x00D6u) && recLen >= 9) {
                const keel::usize r = rd16(wb.data(), wb.size(), body);
                const keel::usize c = rd16(wb.data(), wb.size(), body + 2);
                const keel::u16 cch = rd16(wb.data(), wb.size(), body + 6);
                const keel::u8 flg = wb[body + 8];
                const bool isUtf16 = (flg & 1u) != 0u;
                keel::String s;
                keel::usize p = body + 9;
                for (keel::usize k = 0; k < cch; ++k) {
                    if (isUtf16) {
                        if (p + 2 > body + recLen) break;
                        appendUtf8(s, rd16(wb.data(), wb.size(), p));
                        p += 2;
                    } else {
                        if (p + 1 > body + recLen) break;
                        appendUtf8(s, wb[p++]);
                    }
                }
                pushStr(r, c, s.as_ref());
                pendingFormulaStr = false;
            } else if (recId == 0x0006u && recLen >= 14) {
                const keel::usize r = rd16(wb.data(), wb.size(), body);
                const keel::usize c = rd16(wb.data(), wb.size(), body + 2);
                if (wb[body + 12] == 0xFFu && wb[body + 13] == 0xFFu) {
                    if (wb[body + 6] == 0u) {
                        pendingFormulaStr = true;
                        formulaR = r;
                        formulaC = c;
                    } else if (wb[body + 6] == 1u) {
                        pushNum(r, c, wb[body + 8] != 0u ? 1.0 : 0.0);
                        pendingFormulaStr = false;
                    } else {
                        pendingFormulaStr = false;
                    }
                } else {
                    pushNum(r, c, rd64f(wb.data(), wb.size(), body + 6));
                    pendingFormulaStr = false;
                }
            } else if (recId == 0x0207u && pendingFormulaStr && recLen >= 3) {
                const keel::u16 cch = rd16(wb.data(), wb.size(), body);
                const keel::u8 flg = wb[body + 2];
                const bool isUtf16 = (flg & 1u) != 0u;
                keel::String s;
                keel::usize p = body + 3;
                for (keel::usize k = 0; k < cch; ++k) {
                    if (isUtf16) {
                        if (p + 2 > body + recLen) break;
                        appendUtf8(s, rd16(wb.data(), wb.size(), p));
                        p += 2;
                    } else {
                        if (p + 1 > body + recLen) break;
                        appendUtf8(s, wb[p++]);
                    }
                }
                pushStr(formulaR, formulaC, s.as_ref());
                pendingFormulaStr = false;
            } else if (recId == 0x0205u && recLen >= 8) {
                if (wb[body + 7] == 0u) {
                    pushNum(rd16(wb.data(), wb.size(), body),
                            rd16(wb.data(), wb.size(), body + 2),
                            wb[body + 6] != 0u ? 1.0 : 0.0);
                }
                pendingFormulaStr = false;
            } else {
                pendingFormulaStr = false;
            }
        }
        pos = body + recLen;
    }

    keel::Array<keel::Array<GridCell>> grid;
    grid.resize(maxR);
    for (keel::usize r = 0; r < maxR; ++r) grid[r].resize(maxC);
    for (keel::usize i = 0; i < cells.size(); ++i) {
        auto &dst = grid[cells[i].r][cells[i].c];
        dst.present = true;
        dst.isStr = cells[i].isStr;
        dst.text = keel::move(cells[i].text);
    }
    return grid;
}

} // namespace numkit::runtime::containers_detail
