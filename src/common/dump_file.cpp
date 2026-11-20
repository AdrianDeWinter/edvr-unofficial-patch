#include "dump_file.h"

#include "miniz_config.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dump_file {
namespace {

thread_local Tally* t_scope = nullptr;
std::atomic<int64_t> g_failChunk{-1};
std::atomic<int64_t> g_chunkSeq{0};
std::atomic<bool> g_noThreads{false};
LogSink g_sink = nullptr;   // constant-initialised: safe to read before any static initialiser has run

void logLine(const char* fmt, ...) noexcept {
    const LogSink sink = g_sink;
    if (!sink) return;
    char buf[1200];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    sink(buf);
}

uint64_t qpcNow() noexcept {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

uint64_t qpcToUs(uint64_t ticks) noexcept {
    static const uint64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? static_cast<uint64_t>(f.QuadPart) : 1ull;
    }();
    return ticks * 1000000ull / freq;
}

bool writeAll(HANDLE h, const void* data, size_t bytes) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (bytes) {
        const DWORD n = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(h, p, n, &wrote, nullptr) || wrote == 0) return false;
        p += wrote;
        bytes -= wrote;
    }
    return true;
}

bool readAll(HANDLE h, void* data, size_t bytes) noexcept {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (bytes) {
        const DWORD n = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, nullptr) || got == 0) return false;
        p += got;
        bytes -= got;
    }
    return true;
}

std::string narrowUtf8(const wchar_t* s) {
    if (!s || !*s) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n) - 1);
    return out;
}

const wchar_t* leafOf(const wchar_t* path) noexcept {
    const wchar_t* leaf = path;
    for (const wchar_t* c = path; *c; ++c)
        if (*c == L'\\' || *c == L'/') leaf = c + 1;
    return leaf;
}

struct Job {
    std::vector<uint8_t> raw;    // XORed already when the file is a delta
    std::vector<uint8_t> comp;
    uint64_t rawStart = 0;
    bool injectFail = false;
    int state = 0;               // 0 queued or running, 1 compressed, 2 failed; under Impl::mu
};

bool compressChunk(Job& j) noexcept {
    try {
        if (j.injectFail) return false;
        const mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(j.raw.size()));
        j.comp.resize(bound);
        mz_ulong outLen = bound;
        const int rc = mz_compress2(j.comp.data(), &outLen, j.raw.data(),
                                    static_cast<mz_ulong>(j.raw.size()), 1);
        if (rc != MZ_OK) return false;
        j.comp.resize(outLen);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

TallyScope::TallyScope(Tally& tally) noexcept : m_prev(t_scope) { t_scope = &tally; }
TallyScope::~TallyScope() { t_scope = m_prev; }

std::string tallyText(const Tally& t) {
    char buf[320];
    if (!t.files && !t.plainFiles) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "no file compressed (0 files, %.1f ms)", t.wallUs / 1000.0);
        return buf;
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "%u files compressed, %.1f MB raw -> %.1f MB written, %u of them stored as a delta, "
                "%u written plain after a compression failure, %.1f ms compressing and writing",
                t.files, t.rawBytes / 1048576.0, t.writtenBytes / 1048576.0, t.deltaFiles, t.plainFiles,
                t.wallUs / 1000.0);
    return buf;
}

void setLogSink(LogSink sink) noexcept { g_sink = sink; }

void testFailChunk(int64_t chunkIndex) noexcept {
    g_chunkSeq.store(0);
    g_failChunk.store(chunkIndex);
}
void testNoThreads(bool off) noexcept { g_noThreads.store(off); }

std::wstring onDisk(const wchar_t* plainPath) {
    std::wstring z = std::wstring(plainPath ? plainPath : L"") + L".z";
    return GetFileAttributesW(z.c_str()) != INVALID_FILE_ATTRIBUTES ? z : std::wstring(plainPath ? plainPath : L"");
}

bool readPlainFile(const wchar_t* path, std::string& out) noexcept {
    try {
        out.clear();
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0;
        if (ok) {
            out.resize(static_cast<size_t>(size.QuadPart));
            ok = out.empty() || readAll(h, &out[0], out.size());
        }
        CloseHandle(h);
        if (!ok) out.clear();
        return ok;
    } catch (...) {
        out.clear();
        return false;
    }
}

bool readDecoded(const wchar_t* plainPath, std::string& out, std::string* why) noexcept {
    std::string whyLocal;
    std::string& reason = why ? *why : whyLocal;
    reason.clear();
    out.clear();
    try {
        if (readPlainFile(plainPath, out)) return true;
        const std::wstring zPath = std::wstring(plainPath) + L".z";
        std::string blob;
        if (!readPlainFile(zPath.c_str(), blob)) {
            reason = "neither the plain file nor the .z exists";
            return false;
        }
        auto u32 = [&](size_t at) { uint32_t v = 0; std::memcpy(&v, blob.data() + at, 4); return v; };
        if (blob.size() < 36 || std::memcmp(blob.data(), "EDVRZ1\0\0", 8) != 0) { reason = "not an EDVRZ1 file"; return false; }
        if (u32(8) != kVersion) { reason = "unknown version"; return false; }
        const uint32_t flags = u32(12);
        uint64_t raw = 0;
        std::memcpy(&raw, blob.data() + 16, 8);
        const uint32_t chunkRaw = u32(24), chunkCount = u32(28), nameLen = u32(32);
        if (!chunkRaw || (flags & ~kFlagXor) || chunkCount != (raw + chunkRaw - 1) / chunkRaw || 36 + static_cast<size_t>(nameLen) > blob.size()) {
            reason = "bad header";
            return false;
        }
        size_t pos = 36 + nameLen;
        const std::string baseName(blob.data() + 36, nameLen);
        out.resize(static_cast<size_t>(raw));
        size_t at = 0;
        for (uint32_t i = 0; i < chunkCount; ++i) {
            if (pos + 4 > blob.size()) { reason = "truncated"; return false; }
            const uint32_t clen = u32(pos);
            pos += 4;
            const size_t expect = i + 1 < chunkCount ? chunkRaw : static_cast<size_t>(raw - static_cast<uint64_t>(chunkRaw) * (chunkCount - 1));
            mz_ulong got = static_cast<mz_ulong>(expect);
            if (!clen || pos + clen > blob.size() ||
                mz_uncompress(reinterpret_cast<unsigned char*>(&out[at]), &got, reinterpret_cast<const unsigned char*>(blob.data() + pos), clen) != MZ_OK ||
                got != expect) {
                reason = "a chunk is corrupt or truncated";
                return false;
            }
            at += expect;
            pos += clen;
        }
        if (pos != blob.size()) { reason = "trailing bytes"; return false; }
        if (flags & kFlagXor) {
            const std::wstring dir = std::wstring(plainPath, leafOf(plainPath));
            std::wstring wbase(nameLen, L'\0');
            if (nameLen && MultiByteToWideChar(CP_UTF8, 0, baseName.data(), static_cast<int>(nameLen), &wbase[0], static_cast<int>(nameLen)) <= 0) { reason = "bad base name"; return false; }
            std::string base;
            if (!nameLen || !readPlainFile((dir + wbase).c_str(), base) || base.size() != out.size()) { reason = "the delta's base is missing or another length"; return false; }
            for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<char>(out[i] ^ base[i]);
        }
        return true;
    } catch (...) {
        out.clear();
        reason = "out of memory";
        return false;
    }
}

struct Writer::Impl {
    std::wstring plainPath, zPath, written;
    HANDLE h = INVALID_HANDLE_VALUE;
    bool opened = false;
    bool zMode = false;          // the file is a .z
    bool failed = false;         // an I/O or state failure: the file is not trustworthy
    uint32_t flags = 0;
    std::string baseName;
    std::string xorBase;         // the base's bytes when kFlagXor
    std::vector<uint8_t> cur;    // bytes given to write(), not yet a chunk
    uint64_t curStart = 0;       // the decoded offset of cur[0]
    uint64_t rawTotal = 0;
    uint32_t chunkCount = 0;
    uint64_t fileBytes = 0;      // bytes on disk so far, container included
    uint64_t dataStart = 0;      // offset of the first chunk
    std::deque<std::unique_ptr<Job>> inflight;
    std::deque<Job*> queue;
    std::mutex mu;
    std::condition_variable cvWork, cvDone;
    std::vector<std::thread> workers;
    bool threadsTried = false;
    bool stop = false;
    size_t maxInflight = 2;
    Tally* tally = nullptr;
    uint64_t t0 = 0;
    std::string fallbackWhy;

    void applyXor(std::vector<uint8_t>& raw, uint64_t start) const noexcept {
        if (!(flags & kFlagXor)) return;
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] = static_cast<uint8_t>(raw[i] ^ static_cast<uint8_t>(xorBase[static_cast<size_t>(start) + i]));
    }

    void workerMain() noexcept {
        for (;;) {
            Job* job = nullptr;
            {
                std::unique_lock<std::mutex> lk(mu);
                cvWork.wait(lk, [&] { return stop || !queue.empty(); });
                if (queue.empty()) return;
                job = queue.front();
                queue.pop_front();
            }
            const bool ok = compressChunk(*job);
            {
                std::lock_guard<std::mutex> lk(mu);
                job->state = ok ? 1 : 2;
            }
            cvDone.notify_all();
        }
    }

    void startWorkers() noexcept {
        threadsTried = true;
        if (g_noThreads.load()) return;
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        const unsigned want = std::min(8u, std::max(1u, hw > 2 ? hw - 2 : 1u));
        for (unsigned i = 0; i < want; ++i) {
            try {
                workers.emplace_back(&Impl::workerMain, this);
            } catch (...) {
                break;
            }
        }
        maxInflight = std::max<size_t>(2, workers.size() * 2);
    }

    void stopWorkers() noexcept {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cvWork.notify_all();
        for (std::thread& t : workers) {
            try { if (t.joinable()) t.join(); } catch (...) {}
        }
        workers.clear();
    }

    void markFailed(const char* why) noexcept {
        failed = true;
        if (fallbackWhy.empty()) fallbackWhy = why;
    }

    // The compression of a chunk failed: turn the .z written so far into the plain
    // file under the original name, and carry on plain.
    bool fallbackToPlain(const char* why) noexcept {
        if (!zMode) return true;
        fallbackWhy = why;
        bool ok = false;
        HANDLE plain = CreateFileW(plainPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (plain != INVALID_HANDLE_VALUE) {
            ok = true;
            LARGE_INTEGER at{};
            at.QuadPart = static_cast<LONGLONG>(dataStart);
            ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) != 0;
            uint64_t offset = 0;
            try {
                std::vector<uint8_t> comp, raw;
                for (uint32_t i = 0; ok && i < chunkCount; ++i) {
                    uint32_t len = 0;
                    ok = readAll(h, &len, sizeof(len)) && len > 0 && len <= mz_compressBound(kChunkBytes);
                    if (!ok) break;
                    comp.resize(len);
                    ok = readAll(h, comp.data(), len);
                    if (!ok) break;
                    raw.resize(kChunkBytes);
                    mz_ulong rawLen = kChunkBytes;
                    ok = mz_uncompress(raw.data(), &rawLen, comp.data(), len) == MZ_OK;
                    if (!ok) break;
                    raw.resize(rawLen);
                    applyXor(raw, offset);
                    offset += rawLen;
                    ok = writeAll(plain, raw.data(), raw.size());
                }
            } catch (...) {
                ok = false;
            }
            if (!ok) {
                CloseHandle(plain);
                DeleteFileW(plainPath.c_str());
            }
        }
        if (!ok) {
            failed = true;
            return false;
        }
        CloseHandle(h);
        DeleteFileW(zPath.c_str());
        h = plain;
        zMode = false;
        written = plainPath;
        fileBytes = 0;
        return true;
    }

    // One finished chunk to disk, in order. A failed chunk turns the file plain.
    void landJob(Job& job) noexcept {
        if (failed) return;
        if (zMode && job.state == 1) {
            const uint32_t len = static_cast<uint32_t>(job.comp.size());
            if (!writeAll(h, &len, sizeof(len)) || !writeAll(h, job.comp.data(), job.comp.size())) {
                markFailed("a write to the .z file failed");
                return;
            }
            ++chunkCount;
            fileBytes += sizeof(len) + len;
            return;
        }
        if (zMode && !fallbackToPlain("a chunk failed to compress")) {
            markFailed("compression failed and the plain file could not be written");
            return;
        }
        applyXor(job.raw, job.rawStart);   // the plain file holds the original bytes
        if (!writeAll(h, job.raw.data(), job.raw.size())) markFailed("a write to the plain file failed");
    }

    void drainFront() noexcept {
        Job* job = inflight.front().get();
        {
            std::unique_lock<std::mutex> lk(mu);
            cvDone.wait(lk, [&] { return job->state != 0; });
        }
        landJob(*job);
        inflight.pop_front();
    }

    void submit(bool final) noexcept {
        try {
            std::unique_ptr<Job> job(new Job());
            job->raw.swap(cur);
            job->rawStart = curStart;
            curStart += job->raw.size();
            cur.clear();
            if (g_failChunk.load() >= 0) job->injectFail = g_chunkSeq.fetch_add(1) == g_failChunk.load();
            applyXor(job->raw, job->rawStart);
            if (!threadsTried && !final) startWorkers();
            if (workers.empty()) {
                // The calling thread: a file under one chunk, a failed pool, or the test hook.
                job->state = compressChunk(*job) ? 1 : 2;
                landJob(*job);
                return;
            }
            Job* raw = job.get();
            inflight.push_back(std::move(job));
            {
                std::lock_guard<std::mutex> lk(mu);
                queue.push_back(raw);
            }
            cvWork.notify_one();
            while (inflight.size() > maxInflight) drainFront();
        } catch (...) {
            markFailed("out of memory");
        }
    }

    void drainAll() noexcept {
        while (!inflight.empty()) drainFront();
    }

    void flushPlainChunk() noexcept {
        // A fallback to plain can leave later chunks still in flight; they precede cur in the file.
        drainAll();
        if (!failed && !writeAll(h, cur.data(), cur.size())) markFailed("a write to the plain file failed");
        curStart += cur.size();
        cur.clear();
    }
};

Writer::Writer() noexcept : m_impl(nullptr) {}

Writer::~Writer() {
    if (m_impl) {
        if (m_impl->opened) close();
        delete m_impl;
    }
}

bool Writer::isOpen() const noexcept { return m_impl && m_impl->opened; }
const std::wstring& Writer::writtenPath() const noexcept {
    static const std::wstring none;
    return m_impl ? m_impl->written : none;
}
bool Writer::compressed() const noexcept { return m_impl && m_impl->zMode; }
uint64_t Writer::rawBytes() const noexcept { return m_impl ? m_impl->rawTotal : 0; }

bool Writer::open(const wchar_t* plainPath) noexcept {
    try {
        if (m_impl && m_impl->opened) close();
        delete m_impl;
        m_impl = new Impl();
        Impl& s = *m_impl;
        s.t0 = qpcNow();
        s.tally = t_scope;
        s.plainPath = plainPath ? plainPath : L"";
        s.zPath = s.plainPath + L".z";
        if (s.plainPath.empty()) return false;
        s.h = CreateFileW(s.zPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s.h != INVALID_HANDLE_VALUE) {
            s.zMode = true;
            s.written = s.zPath;
        } else {
            // No .z to be had: the original name, as before this container.
            s.h = CreateFileW(s.plainPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (s.h == INVALID_HANDLE_VALUE) return false;
            s.written = s.plainPath;
            s.fallbackWhy = "the .z file could not be created";
        }
        s.opened = true;
        return true;
    } catch (...) {
        return false;
    }
}

// The header goes down when the first byte of the file's contents is known to be
// wanted: a streaming writer must be able to say its XOR base before open, so the
// header is written by writeHeader() at open for the plain streaming case and by
// writeWhole() for a delta.
static bool writeHeader(Writer::Impl* s) noexcept;

bool Writer::write(const void* data, size_t bytes) noexcept {
    if (!m_impl || !m_impl->opened) return false;
    Impl& s = *m_impl;
    if (s.failed) return false;
    try {
        if (s.zMode && s.chunkCount == 0 && s.fileBytes == 0 && !writeHeader(&s)) {
            s.markFailed("the .z header could not be written");
            return false;
        }
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t left = bytes;
        while (left && !s.failed) {
            if (s.cur.capacity() == 0) s.cur.reserve(left < kChunkBytes ? std::max<size_t>(left, 4096) : kChunkBytes);
            const size_t room = kChunkBytes - s.cur.size();
            const size_t n = std::min(room, left);
            s.cur.insert(s.cur.end(), p, p + n);
            p += n;
            left -= n;
            s.rawTotal += n;
            if (s.cur.size() == kChunkBytes) {
                if (s.zMode) s.submit(false);
                else s.flushPlainChunk();
            }
        }
        return !s.failed;
    } catch (...) {
        s.markFailed("out of memory");
        return false;
    }
}

static bool writeHeader(Writer::Impl* sp) noexcept {
    Writer::Impl& s = *sp;
    try {
        std::vector<uint8_t> hdr(36 + s.baseName.size(), 0);
        std::memcpy(hdr.data(), "EDVRZ1\0\0", 8);
        const uint32_t version = kVersion, flags = s.flags, chunkRaw = kChunkBytes, nameLen = static_cast<uint32_t>(s.baseName.size());
        const uint64_t raw = 0;
        const uint32_t count = 0;
        std::memcpy(hdr.data() + 8, &version, 4);
        std::memcpy(hdr.data() + 12, &flags, 4);
        std::memcpy(hdr.data() + 16, &raw, 8);
        std::memcpy(hdr.data() + 24, &chunkRaw, 4);
        std::memcpy(hdr.data() + 28, &count, 4);
        std::memcpy(hdr.data() + 32, &nameLen, 4);
        if (nameLen) std::memcpy(hdr.data() + 36, s.baseName.data(), nameLen);
        if (!writeAll(s.h, hdr.data(), hdr.size())) return false;
        s.dataStart = hdr.size();
        s.fileBytes = hdr.size();
        return true;
    } catch (...) {
        return false;
    }
}

bool Writer::close() noexcept {
    if (!m_impl || !m_impl->opened) return false;
    Impl& s = *m_impl;
    s.opened = false;
    bool ok = !s.failed;
    try {
        if (s.zMode && s.fileBytes == 0 && !s.failed && !writeHeader(&s)) s.markFailed("the .z header could not be written");
        if (!s.failed && !s.cur.empty()) {
            if (s.zMode) s.submit(true);
            else s.flushPlainChunk();
        }
        s.drainAll();
        s.stopWorkers();
        if (s.zMode && !s.failed) {
            if ((s.flags & kFlagXor) && s.rawTotal != s.xorBase.size()) {
                s.markFailed("the delta is not the base's length");
            } else {
                LARGE_INTEGER at{};
                at.QuadPart = 16;
                bool patched = SetFilePointerEx(s.h, at, nullptr, FILE_BEGIN) != 0 && writeAll(s.h, &s.rawTotal, 8);
                at.QuadPart = 28;
                patched = patched && SetFilePointerEx(s.h, at, nullptr, FILE_BEGIN) != 0 &&
                          writeAll(s.h, &s.chunkCount, 4);
                if (!patched) s.markFailed("the .z header could not be patched");
            }
        }
        if (s.h != INVALID_HANDLE_VALUE && !CloseHandle(s.h)) s.failed = true;
        s.h = INVALID_HANDLE_VALUE;
    } catch (...) {
        s.failed = true;
    }
    ok = !s.failed;
    const bool compressedOk = ok && s.zMode;
    if (compressedOk) {
        DeleteFileW(s.plainPath.c_str());   // never both
    } else if (!s.zMode) {
        DeleteFileW(s.zPath.c_str());
    }
    if (s.tally) {
        if (compressedOk) {
            ++s.tally->files;
            if (s.flags & kFlagXor) ++s.tally->deltaFiles;
            s.tally->rawBytes += s.rawTotal;
            s.tally->writtenBytes += s.fileBytes;
        } else if (ok) {
            ++s.tally->plainFiles;
        }
        s.tally->wallUs += qpcToUs(qpcNow() - s.t0);
    }
    if (ok && !s.zMode) {
        logLine("dump file: %ls written PLAIN, not compressed (%s).", s.plainPath.c_str(),
                        s.fallbackWhy.empty() ? "compression was not used" : s.fallbackWhy.c_str());
    } else if (!ok) {
        logLine("dump file: %ls FAILED (%s).", s.written.c_str(),
                        s.fallbackWhy.empty() ? "a write failed" : s.fallbackWhy.c_str());
    }
    return ok;
}

WholeResult writeWhole(const wchar_t* plainPath, const void* data, size_t bytes,
                       const wchar_t* basePath) noexcept {
    WholeResult r;
    const uint64_t t0 = qpcNow();
    try {
        Writer w;
        std::string base;
        std::string declined;
        if (basePath && *basePath) {
            if (!readPlainFile(basePath, base)) declined = "the base file could not be read";
            else if (base.size() != bytes) {
                char why[96];
                _snprintf_s(why, sizeof(why), _TRUNCATE, "the base is %llu bytes, this file is %llu",
                            static_cast<unsigned long long>(base.size()), static_cast<unsigned long long>(bytes));
                declined = why;
            }
            if (!declined.empty()) {
                // As before this container: the original name, plain.
                r.deltaDeclined = true;
                r.deltaWhy = declined;
                HANDLE h = CreateFileW(plainPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                bool ok = h != INVALID_HANDLE_VALUE && writeAll(h, data, bytes);
                if (h != INVALID_HANDLE_VALUE && !CloseHandle(h)) ok = false;
                r.ok = ok;
                r.writtenPath = plainPath;
                r.rawBytes = bytes;
                r.writtenBytes = bytes;
                return r;
            }
        }
        if (!w.open(plainPath)) return r;
        Writer::Impl& s = *w.m_impl;
        s.t0 = t0;
        if (!base.empty() || (basePath && *basePath)) {
            s.flags = kFlagXor;
            s.baseName = narrowUtf8(leafOf(basePath));
            s.xorBase = std::move(base);
        }
        const bool wrote = w.write(data, bytes);
        const bool closed = w.close();
        r.ok = wrote && closed;
        r.compressed = r.ok && s.zMode;
        r.delta = r.compressed && (s.flags & kFlagXor) != 0;
        r.deltaDeclined = (s.flags & kFlagXor) != 0 && !r.delta;
        if (r.deltaDeclined) r.deltaWhy = s.fallbackWhy.empty() ? "compression failed" : s.fallbackWhy;
        r.writtenPath = s.written;
        r.rawBytes = bytes;
        r.writtenBytes = r.compressed ? s.fileBytes : bytes;
    } catch (...) {
        r.ok = false;
    }
    return r;
}

}  // namespace dump_file
