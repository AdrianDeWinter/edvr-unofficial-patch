// dump_file_test: the eye dump's compressed container, written by the production writer
// (src\common\dump_file.cpp) and read back two ways.
//
//   1. here, by an independent decoder built on miniz's inflate (the container's layout from
//      dump_file.h, parsed by this file, not by the writer);
//   2. by the build gate, which runs `python tools\dump_io.py --check-dir <dir>` on what this
//      rig leaves behind: every NAME.expect holds the bytes the writer was given, and
//      dump_io.read_bytes(NAME) must return them.
//
// Cases: empty, one byte, exactly one chunk, several chunks with a short tail, incompressible
// data, many tiny writes, the one-buffer and streaming forms, the pool on and off, an XOR delta,
// a delta that must be declined (base missing, base another length), and the data-never-lost path:
// a chunk whose compression is made to fail, in a streaming file and in a delta, which must leave
// the file PLAIN under its original name with every byte and no .z beside it.
//
// usage: dump_file_test.exe <output dir>
#include "../../src/common/dump_file.h"
#include "../../src/common/miniz_config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
std::wstring g_dir;

void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}

std::vector<uint8_t> makeData(size_t n, int kind, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        switch (kind) {
        case 0: v[i] = static_cast<uint8_t>(i / 3); break;                  // compressible runs
        case 1: v[i] = static_cast<uint8_t>(x); break;                      // incompressible
        default: v[i] = (i % 64) ? 0 : static_cast<uint8_t>(x); break;      // sparse
        }
    }
    return v;
}

bool readFile(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}

bool writeFilePlain(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) && wrote == data.size());
    CloseHandle(h);
    return ok;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The container, decoded from the layout in dump_file.h alone. Returns false and says why.
bool decodeZ(const std::vector<uint8_t>& blob, const std::wstring& dir, std::vector<uint8_t>& out, std::string& why) {
    out.clear();
    auto u32 = [&](size_t at) { uint32_t v = 0; std::memcpy(&v, blob.data() + at, 4); return v; };
    if (blob.size() < 36 || std::memcmp(blob.data(), "EDVRZ1\0\0", 8) != 0) { why = "magic"; return false; }
    if (u32(8) != 1) { why = "version"; return false; }
    const uint32_t flags = u32(12);
    uint64_t raw = 0;
    std::memcpy(&raw, blob.data() + 16, 8);
    const uint32_t chunkRaw = u32(24), chunkCount = u32(28), nameLen = u32(32);
    if (chunkRaw != dump_file::kChunkBytes) { why = "chunkRawBytes"; return false; }
    if (chunkCount != (raw + chunkRaw - 1) / chunkRaw) { why = "chunkCount does not fit rawBytes"; return false; }
    size_t pos = 36 + nameLen;
    if (pos > blob.size()) { why = "base name overruns"; return false; }
    const std::string baseName(reinterpret_cast<const char*>(blob.data() + 36), nameLen);
    for (uint32_t i = 0; i < chunkCount; ++i) {
        if (pos + 4 > blob.size()) { why = "truncated length"; return false; }
        const uint32_t clen = u32(pos);
        pos += 4;
        if (!clen || pos + clen > blob.size()) { why = "chunk overruns"; return false; }
        const size_t expect = i + 1 < chunkCount ? chunkRaw : static_cast<size_t>(raw - static_cast<uint64_t>(chunkRaw) * (chunkCount - 1));
        std::vector<uint8_t> piece(expect);
        mz_ulong plen = static_cast<mz_ulong>(expect);
        if (mz_uncompress(piece.data(), &plen, blob.data() + pos, clen) != MZ_OK || plen != expect) { why = "inflate"; return false; }
        out.insert(out.end(), piece.begin(), piece.end());
        pos += clen;
    }
    if (pos != blob.size()) { why = "trailing bytes"; return false; }
    if (flags & dump_file::kFlagXor) {
        if (baseName.empty()) { why = "xor without a base"; return false; }
        std::vector<uint8_t> base;
        std::wstring wbase(baseName.begin(), baseName.end());   // ASCII names here
        if (!readFile(dir + L"\\" + wbase, base) || base.size() != out.size()) { why = "base missing or another length"; return false; }
        for (size_t i = 0; i < out.size(); ++i) out[i] ^= base[i];
    }
    return true;
}

// The file the case named, read back whichever form it is in; "z" or "plain" says which.
bool readBack(const std::wstring& name, std::vector<uint8_t>& out, bool& wasZ, std::string& why) {
    const std::wstring path = g_dir + L"\\" + name;
    wasZ = false;
    if (exists(path)) return readFile(path, out);
    std::vector<uint8_t> blob;
    if (!readFile(path + L".z", blob)) { why = "neither the plain nor the .z file exists"; return false; }
    wasZ = true;
    return decodeZ(blob, g_dir, out, why);
}

// A case: the bytes written, the file's name, and what form it must end in.
void verify(const std::wstring& name, const std::vector<uint8_t>& want, bool wantZ, const char* label) {
    std::vector<uint8_t> got;
    bool isZ = false;
    std::string why;
    const bool read = readBack(name, got, isZ, why);
    char msg[200];
    std::snprintf(msg, sizeof(msg), "%s: reads back (%s)", label, why.c_str());
    check(read, msg);
    if (!read) return;
    std::snprintf(msg, sizeof(msg), "%s: bytes equal (%zu vs %zu)", label, got.size(), want.size());
    check(got == want, msg);
    std::snprintf(msg, sizeof(msg), "%s: stored as %s", label, wantZ ? ".z" : "plain");
    check(isZ == wantZ, msg);
    const std::wstring other = g_dir + L"\\" + name + (isZ ? L"" : L".z");
    std::snprintf(msg, sizeof(msg), "%s: never both forms", label);
    check(!exists(other), msg);
    // the cross-language gate reads this
    writeFilePlain(g_dir + L"\\" + name + L".expect", want);
}

void streamed(const wchar_t* name, const std::vector<uint8_t>& data, size_t piece, const char* label) {
    dump_file::Tally t;
    dump_file::TallyScope scope(t);
    dump_file::Writer w;
    const std::wstring path = g_dir + L"\\" + name;
    check(w.open(path.c_str()), label);
    size_t at = 0;
    bool ok = true;
    if (data.empty()) ok = true;
    while (at < data.size()) {
        const size_t n = piece && at + piece < data.size() ? piece : data.size() - at;
        ok = w.write(data.data() + at, n) && ok;
        at += n;
    }
    ok = w.close() && ok;
    check(ok, label);
    check(w.compressed(), "streamed file is a .z");
    check(t.files == 1 && t.plainFiles == 0 && t.rawBytes == data.size(), "tally counts the compressed file");
    verify(name, data, true, label);
}

void whole(const wchar_t* name, const std::vector<uint8_t>& data, const char* label) {
    const std::wstring path = g_dir + L"\\" + name;
    const dump_file::WholeResult r = dump_file::writeWhole(path.c_str(), data.data(), data.size());
    check(r.ok && r.compressed && !r.delta, label);
    verify(name, data, true, label);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: dump_file_test <output dir>\n"); return 2; }
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        g_dir.assign(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &g_dir[0], n);
        g_dir.resize(static_cast<size_t>(n) - 1);
    }
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const size_t kChunk = dump_file::kChunkBytes;

    // Every size of file, both forms, the pool on and off.
    for (int threads = 0; threads < 2; ++threads) {
        dump_file::testNoThreads(threads == 0);
        const wchar_t* tag = threads ? L"mt_" : L"st_";
        auto nm = [&](const wchar_t* base) { return std::wstring(tag) + base; };
        std::wstring n;
        n = nm(L"empty.bin");        streamed(n.c_str(), {}, 0, "empty, streamed");
        n = nm(L"empty_whole.bin");  whole(n.c_str(), {}, "empty, whole");
        n = nm(L"one.bin");          streamed(n.c_str(), makeData(1, 1, 1), 0, "one byte");
        n = nm(L"exact.bin");        streamed(n.c_str(), makeData(kChunk, 0, 2), 0, "exactly one chunk");
        n = nm(L"exact_whole.bin");  whole(n.c_str(), makeData(kChunk, 0, 2), "exactly one chunk, whole");
        n = nm(L"multi.bin");        streamed(n.c_str(), makeData(kChunk * 2 + 12345, 0, 3), 0, "multi-chunk with a short tail");
        n = nm(L"tiny_writes.bin");  streamed(n.c_str(), makeData(kChunk + 70001, 2, 4), 7, "many tiny writes across a chunk edge");
        n = nm(L"random.bin");       streamed(n.c_str(), makeData(300000, 1, 5), 4096, "incompressible");
        n = nm(L"big_random.bin");   streamed(n.c_str(), makeData(kChunk * 3 + 5, 1, 6), 1 << 20, "incompressible, multi-chunk");
        n = nm(L"sparse.bin");       streamed(n.c_str(), makeData(kChunk * 3, 2, 7), 0, "sparse, three chunks");
    }
    dump_file::testNoThreads(false);

    // An XOR delta against a plain base, and the two ways it is declined.
    {
        std::vector<uint8_t> base = makeData(200000, 1, 8);
        std::vector<uint8_t> next = base;
        for (size_t i = 0; i < next.size(); i += 331) next[i] ^= 0x5A;
        writeFilePlain(g_dir + L"\\T00.bmp", base);
        dump_file::Tally t;
        dump_file::TallyScope scope(t);
        const dump_file::WholeResult r = dump_file::writeWhole((g_dir + L"\\P00.bmp").c_str(), next.data(), next.size(), (g_dir + L"\\T00.bmp").c_str());
        check(r.ok && r.compressed && r.delta && !r.deltaDeclined, "delta against a same-length plain base");
        check(t.files == 1 && t.deltaFiles == 1, "the tally counts the delta");
        check(r.writtenBytes < 20000, "a delta of nearly identical files is small");
        verify(L"P00.bmp", next, true, "xor delta");

        const dump_file::WholeResult missing = dump_file::writeWhole((g_dir + L"\\P01.bmp").c_str(), next.data(), next.size(), (g_dir + L"\\NoSuch.bmp").c_str());
        check(missing.ok && !missing.compressed && missing.deltaDeclined && !missing.deltaWhy.empty(), "a missing base declines the delta");
        verify(L"P01.bmp", next, false, "delta declined, base missing");

        std::vector<uint8_t> shorter(base.begin(), base.end() - 1);
        writeFilePlain(g_dir + L"\\T02.bmp", shorter);
        const dump_file::WholeResult sized = dump_file::writeWhole((g_dir + L"\\P02.bmp").c_str(), next.data(), next.size(), (g_dir + L"\\T02.bmp").c_str());
        check(sized.ok && !sized.compressed && sized.deltaDeclined, "a base of another length declines the delta");
        verify(L"P02.bmp", next, false, "delta declined, base another length");
        check(t.files == 1 && t.deltaFiles == 1, "declined deltas are not counted as compressed files");
    }

    // Compression failure: the data must survive, plain, under the original name.
    for (int threads = 0; threads < 2; ++threads) {
        dump_file::testNoThreads(threads == 0);
        const wchar_t* tag = threads ? L"fb_mt_" : L"fb_st_";
        const std::vector<uint8_t> data = makeData(kChunk * 3 + 999, 0, 9);
        for (int failAt = 0; failAt < 4; ++failAt) {
            const std::wstring name = std::wstring(tag) + L"fail" + std::to_wstring(failAt) + L".bin";
            dump_file::testFailChunk(failAt);
            dump_file::Tally t;
            dump_file::TallyScope scope(t);
            dump_file::Writer w;
            check(w.open((g_dir + L"\\" + name).c_str()), "fallback: opens");
            bool ok = true;
            for (size_t at = 0; at < data.size(); at += 1u << 20)
                ok = w.write(data.data() + at, std::min<size_t>(1u << 20, data.size() - at)) && ok;
            ok = w.close() && ok;
            check(ok, "fallback: the writes still succeed");
            check(!w.compressed() && t.plainFiles == 1 && t.files == 0, "fallback: counted as plain, not compressed");
            verify(name, data, false, ("fallback, chunk " + std::to_string(failAt) + " fails").c_str());
        }
    }
    {
        // The failure lands while later chunks are still in flight (more chunks than the pool keeps in
        // flight, at most 16), and writes follow it: the plain file must keep every chunk in order.
        dump_file::testNoThreads(false);
        const std::vector<uint8_t> data = makeData(kChunk * 20 + 77, 0, 11);
        dump_file::testFailChunk(0);
        dump_file::Writer w;
        check(w.open((g_dir + L"\\fb_order.bin").c_str()), "fallback order: opens");
        bool ok = true;
        for (size_t at = 0; at < data.size(); at += 1u << 20)
            ok = w.write(data.data() + at, std::min<size_t>(1u << 20, data.size() - at)) && ok;
        ok = w.close() && ok;
        check(ok && !w.compressed(), "fallback order: written plain");
        verify(L"fb_order.bin", data, false, "fallback with chunks in flight keeps their order");
    }
    {
        // A delta whose compression fails goes out as the ORIGINAL bytes, plain.
        std::vector<uint8_t> base = makeData(kChunk + 4096, 0, 10);
        std::vector<uint8_t> next = base;
        for (size_t i = 0; i < next.size(); i += 4099) next[i] ^= 0x33;
        writeFilePlain(g_dir + L"\\T09.bmp", base);
        for (int failAt = 0; failAt < 2; ++failAt) {
            const std::wstring name = L"P09_" + std::to_wstring(failAt) + L".bmp";
            dump_file::testFailChunk(failAt);
            const dump_file::WholeResult r = dump_file::writeWhole((g_dir + L"\\" + name).c_str(), next.data(), next.size(), (g_dir + L"\\T09.bmp").c_str());
            check(r.ok && !r.compressed && r.deltaDeclined, "a delta whose compression fails falls back");
            verify(name, next, false, "delta fallback keeps the original bytes");
        }
    }
    dump_file::testFailChunk(-1);
    dump_file::testNoThreads(false);

    // An unwritable place: neither form can be made, and the caller is told.
    {
        dump_file::Writer w;
        check(!w.open((g_dir + L"\\no_such_directory\\x.bin").c_str()), "an unwritable path reports failure");
    }
    if (g_fail) { std::printf("dump_file_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("dump_file_test: ok\n");
    return 0;
}
