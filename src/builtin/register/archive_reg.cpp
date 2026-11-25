// builtin/register/archive_reg.cpp
//
// Archive and compression builtins (zip, unzip, gzip, gunzip) backed by
// keel::compress (ZipWriter, ZipReader, gzip_compress, gzip_decompress).

#include <keel/array.h>
#include <keel/compress.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/fs.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/sys/fs_context.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::builtin::detail {

namespace {

static keel::Array<keel::String> extractPathList(const Value &v, keel::StringRef fnName)
{
    keel::Array<keel::String> out;
    if (v.isChar()) {
        out.push_back(v.toString());
        return out;
    }
    if (v.isString()) {
        for (keel::usize i = 0; i < v.numel(); ++i)
            out.push_back(keel::String::from(v.stringElem(i)));
        return out;
    }
    if (v.isCell()) {
        for (keel::usize i = 0; i < v.numel(); ++i) {
            const Value &el = v.cellAt(i);
            if (!el.isChar() && !el.isString()) {
                throw Error(keel::format("{}: file list elements must be character vectors or strings", fnName),
                            0, 0, fnName, "", keel::format("numkit:{}:invalidFileList", fnName));
            }
            out.push_back(el.toString());
        }
        return out;
    }
    throw Error(keel::format("{}: files argument must be a character vector, string array, or cell array of character vectors", fnName),
                0, 0, fnName, "", keel::format("numkit:{}:invalidInput", fnName));
}

static Value makeCellStrList(const keel::Array<keel::String> &items, keel::Allocator *mr)
{
    Value cell = Value::cell(1, items.size(), mr);
    for (keel::usize i = 0; i < items.size(); ++i)
        cell.cellAt(i) = Value::fromString(items[i].as_ref(), mr);
    return cell;
}

static bool hasWildcard(keel::StringRef s) noexcept
{
    for (keel::usize i = 0; i < s.size(); ++i) {
        if (s[i] == '*' || s[i] == '?') return true;
    }
    return false;
}

static bool matchWildcard(keel::StringRef pat, keel::StringRef text) noexcept
{
    keel::usize pi = 0, ti = 0;
    keel::usize starPi = keel::kInvalidIndex, starTi = 0;
    while (ti < text.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == text[ti])) {
            ++pi;
            ++ti;
        } else if (pi < pat.size() && pat[pi] == '*') {
            starPi = pi++;
            starTi = ti;
        } else if (starPi != keel::kInvalidIndex) {
            pi = starPi + 1;
            ti = ++starTi;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

static keel::Array<keel::String> expandPathItem(numkit::sys::FsContext &fsc,
                                                keel::StringRef item,
                                                keel::StringRef rootFolder)
{
    keel::Array<keel::String> out;
    if (!hasWildcard(item)) {
        out.push_back(keel::String::from(item));
        return out;
    }
    keel::String norm = keel::path_normalize(item);
    keel::StringRef dirPart = keel::path_dirname(norm.as_ref());
    keel::StringRef patPart = keel::path_basename(norm.as_ref());

    keel::String searchDir;
    if (rootFolder.empty()) {
        searchDir = dirPart.empty() ? keel::String::from(".") : keel::String::from(dirPart);
    } else {
        searchDir = dirPart.empty()
            ? keel::String::from(rootFolder)
            : keel::path_join(rootFolder, dirPart);
    }

    auto res = fsc.resolvePath(searchDir.as_ref());
    if (!res.fs) return out;
    auto st = res.fs->stat(res.path.as_ref());
    if (!st.has_value() || st->kind != numkit::sys::FileStat::Kind::Directory) return out;

    auto entries = res.fs->listDir(res.path.as_ref());
    for (const auto &e : entries) {
        if (e.name == "." || e.name == "..") continue;
        if (matchWildcard(patPart, e.name.as_ref())) {
            keel::String matched = (dirPart.empty() || dirPart == ".")
                ? e.name.clone()
                : keel::path_join(dirPart, e.name.as_ref());
            out.push_back(keel::move(matched));
        }
    }
    return out;
}

static bool hasSuffixIgnoreCase(keel::StringRef s, keel::StringRef suffix) noexcept
{
    if (s.size() < suffix.size()) return false;
    const keel::usize off = s.size() - suffix.size();
    for (keel::usize i = 0; i < suffix.size(); ++i) {
        keel::u8 a = keel::as<keel::u8>(s[off + i]);
        keel::u8 b = keel::as<keel::u8>(suffix[i]);
        if (a >= 'A' && a <= 'Z') a = keel::as<keel::u8>(a + 32u);
        if (b >= 'A' && b <= 'Z') b = keel::as<keel::u8>(b + 32u);
        if (a != b) return false;
    }
    return true;
}

static void ensureParentDirs(numkit::sys::VirtualFS &fs, keel::StringRef filePath)
{
    keel::String norm = keel::path_normalize(filePath);
    keel::StringRef dir = keel::path_dirname(norm.as_ref());
    if (dir.empty() || dir == "." || dir == "/") return;
    if (fs.exists(dir)) return;

    // Create intermediate directories incrementally
    keel::String cur;
    keel::usize pos = 0;
    if (!dir.empty() && dir[0] == '/') {
        cur.push_back('/');
        pos = 1;
    } else if (dir.size() >= 2 && dir[1] == ':') {
        cur.append(dir.substr(0, 2));
        pos = 2;
        if (pos < dir.size() && dir[pos] == '/') {
            cur.push_back('/');
            ++pos;
        }
    }
    while (pos < dir.size()) {
        keel::usize slash = dir.find('/', pos);
        keel::StringRef seg = (slash == keel::kInvalidIndex) ? dir.substr(pos) : dir.substr(pos, slash - pos);
        if (!seg.empty()) {
            if (!cur.empty() && cur[cur.size() - 1] != '/') cur.push_back('/');
            cur.append(seg);
            if (!fs.exists(cur.as_ref())) {
                try { fs.mkdir(cur.as_ref()); } catch (...) {}
            }
        }
        if (slash == keel::kInvalidIndex) break;
        pos = slash + 1;
    }
}

static void addPathToZip(numkit::sys::VirtualFS &fs,
                         keel::StringRef fsPath,
                         keel::StringRef archiveRelPath,
                         keel::compress::ZipWriter &zw,
                         keel::Array<keel::String> &addedEntries)
{
    auto st = fs.stat(fsPath);
    if (!st.has_value()) {
        throw Error(keel::format("zip: file or folder '{}' does not exist", fsPath),
                    0, 0, "zip", "", "numkit:zip:fileNotFound");
    }
    if (st->kind == numkit::sys::FileStat::Kind::Directory) {
        auto children = fs.listDir(fsPath);
        if (children.empty()) {
            keel::String dirEntry = keel::String::from(archiveRelPath);
            if (!dirEntry.empty() && dirEntry[dirEntry.size() - 1] != '/') dirEntry.push_back('/');
            if (!dirEntry.empty()) {
                (void)zw.add_directory(dirEntry.as_ref());
                addedEntries.push_back(keel::move(dirEntry));
            }
            return;
        }
        for (const auto &ch : children) {
            if (ch.name == "." || ch.name == "..") continue;
            keel::String childFs = keel::path_join(fsPath, ch.name.as_ref());
            keel::String childRel = archiveRelPath.empty()
                ? ch.name.clone()
                : keel::path_join(archiveRelPath, ch.name.as_ref());
            addPathToZip(fs, childFs.as_ref(), childRel.as_ref(), zw, addedEntries);
        }
        return;
    }

    keel::String bytes = fs.readFileBytes(fsPath);
    keel::ArrayRef<keel::u8> raw(reinterpret_cast<const keel::u8 *>(bytes.data()), bytes.size());
    const keel::ErrorCode ec = zw.add_file(archiveRelPath, raw, 6);
    if (ec != keel::ErrorCode::Ok) {
        throw Error(keel::format("zip: failed to add '{}'", archiveRelPath),
                    0, 0, "zip", "", "numkit:zip:writeFailed");
    }
    addedEntries.push_back(keel::String::from(archiveRelPath));
}

// ── zip(zipfilename, filenames [, rootfolder]) ───────────────────────────
static void zip_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "zip");
    if (!args[0].isChar() && !args[0].isString()) {
        throw Error("zip: archive filename must be a character vector or string",
                    0, 0, "zip", "", "numkit:zip:invalidFilename");
    }

    auto *mr = ctx.engine->allocator();
    auto &fsc = static_cast<Engine *>(ctx.engine)->fsContext();

    keel::String zipName = args[0].toString();
    if (!hasSuffixIgnoreCase(zipName.as_ref(), ".zip")) {
        zipName.append(".zip");
    }
    keel::Array<keel::String> fileList = extractPathList(args[1], "zip");

    keel::String rootFolder;
    if (args.size() >= 3) {
        if (!args[2].isChar() && !args[2].isString()) {
            throw Error("zip: rootfolder must be a character vector or string",
                        0, 0, "zip", "", "numkit:zip:invalidRootFolder");
        }
        rootFolder = args[2].toString();
    }

    keel::compress::ZipWriter zw;
    keel::Array<keel::String> addedEntries;

    for (const auto &rawItem : fileList) {
        keel::Array<keel::String> expanded = expandPathItem(fsc, rawItem.as_ref(), rootFolder.as_ref());
        if (expanded.empty()) {
            throw Error(keel::format("zip: no files matching '{}'", rawItem),
                        0, 0, "zip", "", "numkit:zip:fileNotFound");
        }
        for (const auto &item : expanded) {
            keel::String fullInput = rootFolder.empty()
                ? item.clone()
                : keel::path_join(rootFolder.as_ref(), item.as_ref());
            auto resolved = fsc.resolvePath(fullInput.as_ref());
            if (!resolved.fs) {
                throw Error(keel::format("zip: cannot resolve path '{}'", fullInput),
                            0, 0, "zip", "", "numkit:zip:fileNotFound");
            }

            // Compute clean relative archive path
            keel::String relPath = keel::path_normalize(item.as_ref());
            if (keel::path_is_absolute(relPath.as_ref()) || !keel::compress::is_safe_zip_path(relPath.as_ref())) {
                relPath = keel::String::from(keel::path_basename(relPath.as_ref()));
            }
            addPathToZip(*resolved.fs, resolved.path.as_ref(), relPath.as_ref(), zw, addedEntries);
        }
    }

    keel::Array<keel::u8> archive = zw.finish();
    auto outRes = fsc.resolvePath(zipName.as_ref());
    if (!outRes.fs) {
        throw Error(keel::format("zip: cannot resolve output archive '{}'", zipName),
                    0, 0, "zip", "", "numkit:zip:writeFailed");
    }
    ensureParentDirs(*outRes.fs, outRes.path.as_ref());
    keel::StringRef outView(reinterpret_cast<const char *>(archive.data()), archive.size());
    try {
        outRes.fs->writeFileBytes(outRes.path.as_ref(), outView);
    } catch (const std::exception &e) {
        throw Error(keel::format("zip: failed to write '{}': {}", zipName, e.what()),
                    0, 0, "zip", "", "numkit:zip:writeFailed");
    }

    if (nargout >= 1 && outs.size() >= 1) {
        outs[0] = makeCellStrList(addedEntries, mr);
    }
}

// ── unzip(zipfilename [, outputfolder]) ──────────────────────────────────
static void unzip_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "unzip");
    if (!args[0].isChar() && !args[0].isString()) {
        throw Error("unzip: archive filename must be a character vector or string",
                    0, 0, "unzip", "", "numkit:unzip:invalidFilename");
    }

    auto *mr = ctx.engine->allocator();
    auto &fsc = static_cast<Engine *>(ctx.engine)->fsContext();

    keel::String zipName = args[0].toString();
    auto inRes = fsc.resolvePath(zipName.as_ref());
    if ((!inRes.fs || !inRes.fs->exists(inRes.path.as_ref())) && !hasSuffixIgnoreCase(zipName.as_ref(), ".zip")) {
        keel::String withExt = zipName.clone();
        withExt.append(".zip");
        auto alt = fsc.resolvePath(withExt.as_ref());
        if (alt.fs && alt.fs->exists(alt.path.as_ref())) {
            inRes = keel::move(alt);
        }
    }
    if (!inRes.fs || !inRes.fs->exists(inRes.path.as_ref())) {
        throw Error(keel::format("unzip: archive '{}' not found", zipName),
                    0, 0, "unzip", "", "numkit:unzip:fileNotFound");
    }

    keel::String outFolder = keel::String::from(".");
    if (args.size() >= 2) {
        if (!args[1].isChar() && !args[1].isString()) {
            throw Error("unzip: outputfolder must be a character vector or string",
                        0, 0, "unzip", "", "numkit:unzip:invalidOutputFolder");
        }
        outFolder = args[1].toString();
    }
    auto outBase = fsc.resolvePath(outFolder.as_ref());
    if (!outBase.fs) {
        throw Error(keel::format("unzip: cannot resolve output folder '{}'", outFolder),
                    0, 0, "unzip", "", "numkit:unzip:invalidOutputFolder");
    }
    if (!outBase.fs->exists(outBase.path.as_ref())) {
        ensureParentDirs(*outBase.fs, keel::path_join(outBase.path.as_ref(), "_").as_ref());
    }

    keel::String archiveBytes = inRes.fs->readFileBytes(inRes.path.as_ref());
    keel::ArrayRef<keel::u8> raw(reinterpret_cast<const keel::u8 *>(archiveBytes.data()), archiveBytes.size());
    auto zrRes = keel::compress::ZipReader::open(raw);
    if (zrRes.is_err()) {
        throw Error(keel::format("unzip: invalid or corrupted ZIP archive '{}'", zipName),
                    0, 0, "unzip", "", "numkit:unzip:invalidArchive");
    }
    const auto &zr = zrRes.value();

    keel::Array<keel::String> extractedFiles;
    for (keel::usize i = 0; i < zr.entry_count(); ++i) {
        const auto &e = zr.entry(i);
        if (!keel::compress::is_safe_zip_path(e.name.as_ref())) {
            throw Error(keel::format("unzip: unsafe archive entry path '{}'", e.name),
                        0, 0, "unzip", "", "numkit:unzip:unsafePath");
        }
        keel::String destPath = keel::path_join(outBase.path.as_ref(), e.name.as_ref());
        if (e.is_directory()) {
            ensureParentDirs(*outBase.fs, keel::path_join(destPath.as_ref(), "_").as_ref());
            continue;
        }
        auto dataRes = zr.extract(i);
        if (dataRes.is_err()) {
            throw Error(keel::format("unzip: failed to extract entry '{}' from '{}'", e.name, zipName),
                        0, 0, "unzip", "", "numkit:unzip:corruptEntry");
        }
        ensureParentDirs(*outBase.fs, destPath.as_ref());
        keel::StringRef payload(reinterpret_cast<const char *>(dataRes->data()), dataRes->size());
        try {
            outBase.fs->writeFileBytes(destPath.as_ref(), payload);
        } catch (const std::exception &ex) {
            throw Error(keel::format("unzip: failed to write '{}': {}", destPath, ex.what()),
                        0, 0, "unzip", "", "numkit:unzip:writeFailed");
        }
        extractedFiles.push_back(keel::move(destPath));
    }

    if (nargout >= 1 && outs.size() >= 1) {
        outs[0] = makeCellStrList(extractedFiles, mr);
    }
}

// ── gzip(files [, outputfolder]) ─────────────────────────────────────────
static void gzip_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "gzip");
    auto *mr = ctx.engine->allocator();
    auto &fsc = static_cast<Engine *>(ctx.engine)->fsContext();

    keel::Array<keel::String> fileList = extractPathList(args[0], "gzip");
    bool hasOutFolder = false;
    keel::String outFolderPath;
    numkit::sys::VirtualFS *outFs = nullptr;
    if (args.size() >= 2) {
        if (!args[1].isChar() && !args[1].isString()) {
            throw Error("gzip: outputfolder must be a character vector or string",
                        0, 0, "gzip", "", "numkit:gzip:invalidOutputFolder");
        }
        auto outRes = fsc.resolvePath(args[1].toString().as_ref());
        if (!outRes.fs) {
            throw Error("gzip: cannot resolve output folder",
                        0, 0, "gzip", "", "numkit:gzip:invalidOutputFolder");
        }
        outFolderPath = keel::move(outRes.path);
        outFs = outRes.fs;
        hasOutFolder = true;
    }

    keel::Array<keel::String> createdFiles;
    auto compressOne = [&](numkit::sys::VirtualFS &inFs, keel::StringRef inPath) {
        keel::String bytes = inFs.readFileBytes(inPath);
        keel::ArrayRef<keel::u8> raw(reinterpret_cast<const keel::u8 *>(bytes.data()), bytes.size());
        keel::compress::GzipOptions gopts{};
        gopts.level = 6;
        gopts.filename = keel::path_basename(inPath);
        keel::Array<keel::u8> gz = keel::compress::gzip_compress(raw, gopts);

        keel::String destPath;
        numkit::sys::VirtualFS *dstFs = hasOutFolder ? outFs : &inFs;
        if (hasOutFolder) {
            keel::String base = keel::String::from(keel::path_basename(inPath));
            base.append(".gz");
            destPath = keel::path_join(outFolderPath.as_ref(), base.as_ref());
        } else {
            destPath = keel::String::from(inPath);
            destPath.append(".gz");
        }
        ensureParentDirs(*dstFs, destPath.as_ref());
        keel::StringRef payload(reinterpret_cast<const char *>(gz.data()), gz.size());
        try {
            dstFs->writeFileBytes(destPath.as_ref(), payload);
        } catch (const std::exception &e) {
            throw Error(keel::format("gzip: failed to write '{}': {}", destPath, e.what()),
                        0, 0, "gzip", "", "numkit:gzip:writeFailed");
        }
        createdFiles.push_back(keel::move(destPath));
    };

    for (const auto &rawItem : fileList) {
        keel::Array<keel::String> expanded = expandPathItem(fsc, rawItem.as_ref(), "");
        if (expanded.empty()) {
            throw Error(keel::format("gzip: file '{}' not found", rawItem),
                        0, 0, "gzip", "", "numkit:gzip:fileNotFound");
        }
        for (const auto &item : expanded) {
            auto res = fsc.resolvePath(item.as_ref());
            if (!res.fs) {
                throw Error(keel::format("gzip: file '{}' not found", item),
                            0, 0, "gzip", "", "numkit:gzip:fileNotFound");
            }
            auto st = res.fs->stat(res.path.as_ref());
            if (!st.has_value()) {
                throw Error(keel::format("gzip: file '{}' not found", item),
                            0, 0, "gzip", "", "numkit:gzip:fileNotFound");
            }
            if (st->kind == numkit::sys::FileStat::Kind::Directory) {
                auto children = res.fs->listDir(res.path.as_ref());
                for (const auto &ch : children) {
                    if (ch.isDirectory || ch.name == "." || ch.name == "..") continue;
                    keel::String childPath = keel::path_join(res.path.as_ref(), ch.name.as_ref());
                    compressOne(*res.fs, childPath.as_ref());
                }
            } else {
                compressOne(*res.fs, res.path.as_ref());
            }
        }
    }

    if (nargout >= 1 && outs.size() >= 1) {
        outs[0] = makeCellStrList(createdFiles, mr);
    }
}

// ── gunzip(files [, outputfolder]) ───────────────────────────────────────
static void gunzip_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "gunzip");
    auto *mr = ctx.engine->allocator();
    auto &fsc = static_cast<Engine *>(ctx.engine)->fsContext();

    keel::Array<keel::String> fileList = extractPathList(args[0], "gunzip");
    bool hasOutFolder = false;
    keel::String outFolderPath;
    numkit::sys::VirtualFS *outFs = nullptr;
    if (args.size() >= 2) {
        if (!args[1].isChar() && !args[1].isString()) {
            throw Error("gunzip: outputfolder must be a character vector or string",
                        0, 0, "gunzip", "", "numkit:gunzip:invalidOutputFolder");
        }
        auto outRes = fsc.resolvePath(args[1].toString().as_ref());
        if (!outRes.fs) {
            throw Error("gunzip: cannot resolve output folder",
                        0, 0, "gunzip", "", "numkit:gunzip:invalidOutputFolder");
        }
        outFolderPath = keel::move(outRes.path);
        outFs = outRes.fs;
        hasOutFolder = true;
    }

    keel::Array<keel::String> extractedFiles;
    auto decompressOne = [&](numkit::sys::VirtualFS &inFs, keel::StringRef inPath, keel::StringRef origLabel) {
        keel::String gzBytes = inFs.readFileBytes(inPath);
        keel::ArrayRef<keel::u8> raw(reinterpret_cast<const keel::u8 *>(gzBytes.data()), gzBytes.size());
        auto decRes = keel::compress::gzip_decompress(raw);
        if (decRes.is_err()) {
            throw Error(keel::format("gunzip: invalid or corrupted GZIP file '{}'", origLabel),
                        0, 0, "gunzip", "", "numkit:gunzip:invalidArchive");
        }

        keel::StringRef base = keel::path_basename(inPath);
        keel::String outBaseName = hasSuffixIgnoreCase(base, ".gz")
            ? keel::String::from(base.substr(0, base.size() - 3))
            : keel::String::from(base);
        if (outBaseName.empty()) outBaseName = keel::String::from("gunzip_output");

        numkit::sys::VirtualFS *dstFs = hasOutFolder ? outFs : &inFs;
        keel::String destPath;
        if (hasOutFolder) {
            destPath = keel::path_join(outFolderPath.as_ref(), outBaseName.as_ref());
        } else {
            keel::StringRef dir = keel::path_dirname(inPath);
            destPath = dir.empty() ? outBaseName.clone() : keel::path_join(dir, outBaseName.as_ref());
        }
        ensureParentDirs(*dstFs, destPath.as_ref());
        keel::StringRef payload(reinterpret_cast<const char *>(decRes->data()), decRes->size());
        try {
            dstFs->writeFileBytes(destPath.as_ref(), payload);
        } catch (const std::exception &e) {
            throw Error(keel::format("gunzip: failed to write '{}': {}", destPath, e.what()),
                        0, 0, "gunzip", "", "numkit:gunzip:writeFailed");
        }
        extractedFiles.push_back(keel::move(destPath));
    };

    for (const auto &rawItem : fileList) {
        keel::Array<keel::String> expanded = expandPathItem(fsc, rawItem.as_ref(), "");
        if (expanded.empty()) {
            throw Error(keel::format("gunzip: file '{}' not found", rawItem),
                        0, 0, "gunzip", "", "numkit:gunzip:fileNotFound");
        }
        for (const auto &item : expanded) {
            auto res = fsc.resolvePath(item.as_ref());
            if ((!res.fs || !res.fs->exists(res.path.as_ref())) && !hasSuffixIgnoreCase(item.as_ref(), ".gz")) {
                keel::String withExt = item.clone();
                withExt.append(".gz");
                auto alt = fsc.resolvePath(withExt.as_ref());
                if (alt.fs && alt.fs->exists(alt.path.as_ref())) {
                    res = keel::move(alt);
                }
            }
            if (!res.fs || !res.fs->exists(res.path.as_ref())) {
                throw Error(keel::format("gunzip: file '{}' not found", item),
                            0, 0, "gunzip", "", "numkit:gunzip:fileNotFound");
            }
            auto st = res.fs->stat(res.path.as_ref());
            if (st.has_value() && st->kind == numkit::sys::FileStat::Kind::Directory) {
                auto children = res.fs->listDir(res.path.as_ref());
                for (const auto &ch : children) {
                    if (ch.isDirectory || ch.name == "." || ch.name == "..") continue;
                    if (!hasSuffixIgnoreCase(ch.name.as_ref(), ".gz")) continue;
                    keel::String childPath = keel::path_join(res.path.as_ref(), ch.name.as_ref());
                    decompressOne(*res.fs, childPath.as_ref(), ch.name.as_ref());
                }
            } else {
                decompressOne(*res.fs, res.path.as_ref(), item.as_ref());
            }
        }
    }

    if (nargout >= 1 && outs.size() >= 1) {
        outs[0] = makeCellStrList(extractedFiles, mr);
    }
}

} // namespace

void register_archive_io(Engine &engine)
{
    engine.registerFunction("zip",    &zip_reg);
    engine.registerFunction("unzip",  &unzip_reg);
    engine.registerFunction("gzip",   &gzip_reg);
    engine.registerFunction("gunzip", &gunzip_reg);
}

} // namespace numkit::builtin::detail
