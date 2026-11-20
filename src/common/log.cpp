#include "log.h"

// Set by build.bat from `git describe` for the two shipped DLLs. The test
// binaries that link this file compile without the define, and the fallback
// says what it is rather than impersonating a release.
#ifdef EDVR_HAVE_VERSION_HEADER
#include "edvr_version.h"
#endif
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "log_rotate.h"

namespace edvr {
namespace {

// Past this we drop lines and count them rather than growing without bound.
//
// WHY 16 AND NOT 1 (2026-09-07, and it cost six censuses to find).
//
// The flusher wakes every 250 ms, so this bounds how much text a burst may
// queue BETWEEN two passes -- not how much a session may write, which is
// log.max_mb. A draw census is exactly such a burst: it emits one line per
// draw for a few whole frames, which at 90 Hz is over in about 20 ms, so the
// entire capture lands in one buffer and is written by a single pass.
//
// Measured on the 2026-09-07 flights: one frame of a busy scene is about
// 3,900 census lines and 1.05 MB. At the old 1 MB cap every census on both
// flights stopped within 900 bytes of the cap -- six for six -- losing the
// tail of frame 0 and, with it, finish()'s intern table and end line. The
// symptom was a census whose @N tokens resolved to nothing, and it read as a
// bug in draw_census.cpp, which it was not. advanced.census_lines could not
// help: this cap bites first.
//
// 16 MB covers a three-frame census of the densest scene flown with room to
// spare, and it is a CEILING on what may queue, not a standing allocation:
// the buffers start at kReserveBytes and grow only as far as a burst takes
// them.
//
// But growth is not free afterwards. std::vector::clear() keeps capacity, so
// without the reclaim in writeBuffer() one census would leave a buffer
// holding its peak for the rest of the session -- and both of the pair can
// peak, so a 16 MB ceiling would mean up to 32 MB resident in a DLL sitting
// inside somebody's VR session. Hence kReclaimAboveBytes: past that, the
// buffer is swapped for a fresh one at the reserve size. Below it the
// capacity is left alone, so the steady state never allocates at all.
constexpr int    kDefaultBufferMb   = 16;
constexpr size_t kReserveBytes      = 64u * 1024u;
constexpr size_t kReclaimAboveBytes = 1u * 1024u * 1024u;
constexpr DWORD  kFlushIntervalMs   = 250;

// The link time of the module this code is compiled into, read from its own PE
// header. This is the identity a binary carries wherever it is copied, and the
// log states it so "which build produced this log" is never again a diagnosis
// (6ao: the anchor hunt was debugged three times in source that had never
// flown). Zero when the header is not as expected -- "no stamp", not an error.
// Kept textually identical in both repos' log.cpp; cross-diff when touching.
uint32_t moduleLinkStamp() {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleLinkStamp), &mod) ||
        !mod) {
        return 0;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(mod);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

}  // namespace

int64_t qpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t qpcFrequency() {
    static int64_t freq = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return freq;
}

uint64_t fnv1a64(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Where in a buffer a line sits that the head keeps once it has ended: the first line of a kind, or a noteReserved line.
struct KeptSpan {
    size_t offset;
    size_t bytes;
    bool reserved;   // a noteReserved line (the reserved budget); otherwise the first line of its kind (the kinds' budget)
};

// One tail segment: its number, its size and the first and last stamp of the lines written to it ("hh:mm:ss.mmm", "" when it has none).
struct SegInfo {
    uint32_t no = 0;
    uint64_t bytes = 0;
    char first[13] = {};
    char last[13] = {};
    bool onDisk = true;
};

// Which tail segment the buffer's bytes from `offset` on went to, for the kept lines of that buffer.
struct ChunkMark {
    size_t offset;
    uint32_t segment;
};

struct Log::Impl {
    std::vector<char> buf[2];
    std::vector<KeptSpan> spans[2];   // the kept-line candidates in buf[i], in order; guarded with it
    std::atomic<int>  active{0};
    std::atomic_flag  spin = ATOMIC_FLAG_INIT;
    std::thread       flusher;
    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    HANDLE            file = INVALID_HANDLE_VALUE;   // the head
    HANDLE            seg = INVALID_HANDLE_VALUE;    // the tail segment being written, once the head has ended

    // Below: only where the files are written (the flusher, or close() once it is joined), except kinds, which is guarded by spin.
    bool              rotating = false;   // log.max_mb is not 0
    bool              cut = false;        // the head has ended
    LogPlan           plan;
    std::wstring      headPath;
    std::string       headLeaf;
    LogKindTable      kinds;
    LogReserve        keepReserved;       // the noteReserved lines kept in the head: K/2
    LogReserve        keepKinds;          // the first line of each kind kept in the head: K/2
    std::vector<SegInfo> segs;            // every segment opened, in order
    std::vector<SegInfo> deleted;         // the ones deleted, in the order they were
    std::vector<ChunkMark> chunks;        // for the buffer being written
    uint32_t          segNo = 0;          // the newest segment opened (0: none)
    uint64_t          segBytes = 0;       // what the segment being written holds
    bool              segFresh = true;    // the segment being written holds its top marker and no line yet
    uint32_t          keptSeg = 0;        // the segment the last kept line in the head was marked as belonging to
    bool              keptSpentNoted = false;
    bool              tailErrorNoted = false;
    ULONGLONG         volumeStart = 0;
};

namespace {

// printf into out[off..cap), moving off; a line that does not fit is cut, never overrun.
void appendf(char* out, size_t cap, size_t& off, const char* fmt, ...) {
    if (off >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(out + off, cap - off, _TRUNCATE, fmt, ap);
    va_end(ap);
    off += n > 0 ? static_cast<size_t>(n) : std::strlen(out + off);
}

std::wstring segmentPath(const std::wstring& headPath, uint32_t no) {
    std::wstring base = headPath;
    if (base.size() > 4) base.resize(base.size() - 4);   // ".log"
    wchar_t suffix[32];
    _snwprintf_s(suffix, _TRUNCATE, L".tail-%03u.log", no);
    return base + suffix;
}

std::string leafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    char narrow[MAX_PATH * 3] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), static_cast<int>(leaf.size()), narrow, static_cast<int>(sizeof(narrow)) - 1, nullptr, nullptr);
    return std::string(narrow, n > 0 ? static_cast<size_t>(n) : 0);
}

// The first and last stamp of the lines in p[0..n) into the segment's record (the first only when it has none yet).
void noteSegmentStamps(SegInfo& info, const char* p, size_t n) {
    if (n == 0) return;
    if (!info.first[0]) {
        size_t at = 0;
        for (int tries = 0; tries < 4 && at < n; ++tries) {
            if (logStampAt(p + at, n - at, info.first)) break;
            at += logFirstLineBytes(p + at, n - at);
        }
    }
    size_t last = n >= 2 ? n - 2 : 0;
    while (last > 0 && p[last - 1] != '\n') --last;
    logStampAt(p + last, n - last, info.last);
}

}  // namespace

Log& Log::get() {
    static Log instance;
    return instance;
}

Log::~Log() { close(); }

void Log::lock() {
    while (m_impl->spin.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
}

void Log::unlock() { m_impl->spin.clear(std::memory_order_release); }

bool Log::open(const std::wstring& dir, const wchar_t* tag) {
    if (m_open) return true;
    if (!Config::get().getBool("log.enabled", true)) return false;

    // The session's size cap, in megabytes: 16 since 2026-10-01 (it was 4). The issue 63 reporter's graphics log reached 3.9 MB, so a
    // longer session would have gone silent at the cap and lost the freeze lines (docs/freeze-diagnostics-2026-10-01.md). 0 is no cap.
    // One class and one default for both profiles: the flat profile allows the "log." keys and reads this one from edvr-flat.ini
    // first, then edvr.ini. An ini that says max_mb = 4 keeps 4; the shipped edvr.ini only documents the default, commented out.
    // The bound is the head (a quarter of it), the kept lines (an eighth) and four rotating tail segments (the rest), log_rotate.h; 0 keeps one file with no bound.
    // F125: the detailed on-foot log (log.on_foot_detail = on) writes about 4 MB a minute on foot, which reaches 16 MB some four minutes after going on foot.
    // With that key on when the log opens, a cap nobody set is 64 MB. "Nobody set" is exactly when getInt takes its default: no max_mb line, one with no
    // value, or one that is not a number. Any max_mb written as a number wins, 16 included. Read once, here, as max_mb is: turning the key on later in the session (it is live for the
    // on-foot lines) leaves the cap the log opened with, and the key's announce line says which that is.
    const bool onFootDetail = Config::get().getBool("log.on_foot_detail", false);
    const int maxMb = Config::get().getInt("log.max_mb", onFootDetail ? 64 : 16);
    m_maxBytes = maxMb > 0 ? static_cast<uint64_t>(maxMb) * 1024ull * 1024ull : 0ull;
    // The counts are per session: a reopened log counts from zero. (Only the rigs reopen the singleton; a process opens its log once. Without
    // this, the second open in one process inherited the first one's byte count, and a cap smaller than it stopped the new file at
    // its first flush.) The rest of the per-session state is in Impl, which is new at each open.
    m_bytesWritten = 0;

    // Clamped rather than trusted: 0 here would drop every line silently, and
    // the value is read once at open so a typo cannot be walked back mid
    // session.
    //
    // The raw text is kept because getIntInRange reports a clamp or a parse
    // failure through note() -- and this is the first call to it that happens
    // BEFORE the log is open, so that report goes nowhere (note() returns on
    // !m_open). A buffer_mb of 0, 999 or "sixteen" would otherwise be
    // corrected in silence, which is the one failure this whole change exists
    // to end. Said below, once the log can carry it.
    const std::string rawBufMb = Config::get().getString("log.buffer_mb", "");
    const int bufMb = Config::get().getIntInRange("log.buffer_mb",
                                                  kDefaultBufferMb, 1, 256);
    m_bufferCapBytes = static_cast<size_t>(bufMb) * 1024u * 1024u;

    m_impl = new Impl();
    if (m_maxBytes) {
        m_impl->rotating = true;
        m_impl->plan = LogPlan::make(m_maxBytes);
        m_impl->kinds.init();
        m_impl->keepReserved.setLimit(m_impl->plan.kept / 2);
        m_impl->keepKinds.setLimit(m_impl->plan.kept / 2);
        m_impl->volumeStart = GetTickCount64();
    }
    m_impl->buf[0].reserve(kReserveBytes);
    m_impl->buf[1].reserve(kReserveBytes);
    m_impl->spans[0].reserve(16);
    m_impl->spans[1].reserve(16);
    m_dir = dir;

    // Created here rather than at config time, so logging turned off leaves no
    // empty directory behind.
    ensureDirectory(dir);

    // Named to the second, and never over an existing file: two processes
    // opening the same tag in one second -- the test rigs do, run
    // concurrently by build.bat -- used to leave one log truncated by the
    // other's CREATE_ALWAYS. The second arrival takes a name with the
    // millisecond and its pid appended, the form the native trace has always
    // used (native_trace.h), which every reader of these names accepts
    // (tools\edvr_log.py, the installer's log bundle).
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\edvr_%s_%04u%02u%02u_%02u%02u%02u.log",
                 dir.c_str(), tag, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
    m_impl->file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_impl->file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_SHARING_VIOLATION) {
            _snwprintf_s(path, _TRUNCATE, L"%s\\edvr_%s_%04u%02u%02u_%02u%02u%02u_%03u_%lu.log",
                         dir.c_str(), tag, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                         st.wSecond, st.wMilliseconds, GetCurrentProcessId());
            m_impl->file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (m_impl->file == INVALID_HANDLE_VALUE) {
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    m_impl->headPath = path;
    m_impl->headLeaf = leafName(path);
    m_open = true;
    note("ED-UP log -- unofficial VR and flat fixes for Elite Dangerous: Odyssey");
    // The version names the git tag the DLL was built from, straight from
    // `git describe` at build time (see build.bat). It exists because the link
    // stamp alone cannot answer "which release is this" without correlating
    // hex against tag dates by hand -- which is what triaging a field log used
    // to start with. A -N-g or -dirty suffix means a dev build and says so.
    // TimeDateStamp is seconds since 1970 UTC unless a reproducible-build flag
    // replaced it with a hash; the hex is the exact identity either way, and
    // it stays printed because two builds of one tag are still two builds.
    const uint32_t stamp = moduleLinkStamp();
    if (stamp != 0) {
        ULARGE_INTEGER t;
        t.QuadPart = 116444736000000000ull +
                     static_cast<uint64_t>(stamp) * 10000000ull;
        FILETIME ft;
        ft.dwLowDateTime  = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        SYSTEMTIME bs{};
        if (FileTimeToSystemTime(&ft, &bs)) {
            note("version %s (build %08X) -- this DLL was linked %04u-%02u-%02u "
                 "%02u:%02u:%02u UTC",
                 EDVR_VERSION_STRING, stamp, bs.wYear, bs.wMonth, bs.wDay,
                 bs.wHour, bs.wMinute, bs.wSecond);
        } else {
            note("version %s (build %08X)", EDVR_VERSION_STRING, stamp);
        }
    } else {
        note("version %s", EDVR_VERSION_STRING);
    }
    note("If you are reporting a problem, paste this whole file and the .tail-NNN.log files beside it.");

    // The buffer in force, and whether the file said something else. Printed
    // here rather than at the read above because note() needs m_open. The
    // size is worth a line on its own: the drop notice tells a reader to
    // raise log.buffer_mb, and that advice is only actionable next to the
    // number actually running.
    if (!rawBufMb.empty() && rawBufMb != std::to_string(bufMb)) {
        note("log.buffer_mb = \"%s\" is not a value this build can use; "
             "running at %d MB instead (1..256).",
             rawBufMb.c_str(), bufMb);
    } else if (bufMb != kDefaultBufferMb) {
        note("log buffer %d MB (log.buffer_mb; the default is %d).", bufMb,
             kDefaultBufferMb);
    }
    return true;
}

void Log::startFlusherOnce() {
    // Never called from DllMain: the first line is written from an export call,
    // well after loader lock.
    bool expected = false;
    if (!m_impl->started.compare_exchange_strong(expected, true)) return;
    m_impl->flusher = std::thread([this] { flusherMain(); });
}

void Log::flusherMain() {
    SetThreadDescription(GetCurrentThread(), L"edvr-log");
    while (!m_impl->stop.load(std::memory_order_relaxed)) {
        Sleep(kFlushIntervalMs);
        lock();
        const int full = m_impl->active.load(std::memory_order_relaxed);
        m_impl->active.store(full ^ 1, std::memory_order_relaxed);
        unlock();
        writeBuffer(full);
        noteVolumeIfDue();
    }
}

void Log::writeBuffer(int index) {
    std::vector<char>& b = m_impl->buf[index];
    // NOT "if (b.empty()) return;" any more: a pass with nothing to write may
    // still owe the drop notice below, and a window in which EVERY line was
    // dropped is exactly the one a reader most needs told about.
    if (b.empty() && m_dropped.load(std::memory_order_relaxed) == m_droppedReported) {
        return;
    }

    // Empty it AND give the memory back, on every path that empties it.
    // clear() keeps capacity, so a burst that grew this buffer to the ceiling
    // would hold that memory until the process exits; a swap rather than
    // shrink_to_fit(), which is only a request. Below the threshold the
    // capacity is left alone, so the steady state never reallocates.
    auto release = [&] {
        b.clear();
        m_impl->spans[index].clear();
        if (b.capacity() > kReclaimAboveBytes) {
            std::vector<char>().swap(b);
            b.reserve(kReserveBytes);
        }
    };

    if (!b.empty()) {
        if (m_impl->rotating) {
            writeRotating(b, index);
        } else {
            writeHead(b.data(), b.size());
        }
        release();
    }

    // Say so when lines were lost (2026-09-07).
    //
    // The counter has existed since the buffer did, and nothing ever printed
    // it, so an overrun looked exactly like a quiet moment. Six draw censuses
    // were read as complete when each had lost its tail; the diagnosis went to
    // draw_census.cpp twice before it came here.
    //
    // Written straight to the file rather than through note(): note() calls
    // append(), which is where the drop happened, and a full buffer would eat
    // the notice too. It lands right at the gap it describes: in the file being
    // written, which is the head until it ends and a tail segment after.
    //
    // Reported as a running total from this thread only, so no lock is needed
    // for m_droppedReported and the line cannot itself be lost.
    const uint64_t lost = m_dropped.load(std::memory_order_relaxed);
    if (lost > m_droppedReported) {
        char msg[288];
        // The interval and the size come from the values in force, not from
        // literals: the advice to raise log.buffer_mb is only actionable
        // beside the number the reader is actually running, and a hard-coded
        // "250 ms" would drift the day kFlushIntervalMs changes.
        const int n = _snprintf_s(
            msg, sizeof(msg), _TRUNCATE,
            "[edvr] %llu log line(s) were DROPPED here -- more text arrived "
            "between two writes to disk (at least %u ms apart) than the %zu MB "
            "buffer holds, so what follows is not continuous. %llu lost this "
            "session. Raise log.buffer_mb if this is a capture you need "
            "whole.\r\n",
            static_cast<unsigned long long>(lost - m_droppedReported),
            static_cast<unsigned>(kFlushIntervalMs),
            m_bufferCapBytes / (1024u * 1024u),
            static_cast<unsigned long long>(lost));
        if (n > 0) writeCurrent(msg, static_cast<size_t>(n));
        m_droppedReported = lost;
    }
}

void Log::writeHead(const char* p, size_t n) {
    if (!n) return;
    DWORD w = 0;
    WriteFile(m_impl->file, p, static_cast<DWORD>(n), &w, nullptr);
    m_bytesWritten += w;
}

// The file being written: the head until it has ended, then the tail segment.
void Log::writeCurrent(const char* p, size_t n) {
    if (m_impl->cut && m_impl->seg != INVALID_HANDLE_VALUE) {
        writeSegment(p, n, false);
    } else {
        writeHead(p, n);
    }
}

void Log::writeSegment(const char* p, size_t n, bool stamped) {
    Impl& I = *m_impl;
    if (!n || I.seg == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(I.seg, p, static_cast<DWORD>(n), &w, nullptr);
    I.segBytes += w;
    SegInfo& info = I.segs.back();
    info.bytes = I.segBytes;
    if (stamped) noteSegmentStamps(info, p, n);
}

// A buffer, with log.max_mb in force: into the head until the next line would pass its share, then into tail segments, then the kept lines
// of the buffer into the head.
void Log::writeRotating(std::vector<char>& b, int index) {
    Impl& I = *m_impl;
    const char* data = b.data();
    const size_t n = b.size();
    I.chunks.clear();
    size_t headEnd = 0;   // the bytes of this buffer that went to the head
    if (!I.cut) {
        const uint64_t room = I.plan.head > m_bytesWritten ? I.plan.head - m_bytesWritten : 0;
        if (n <= room) {
            writeHead(data, n);
            return;
        }
        headEnd = logCutAtLine(data, n, static_cast<size_t>(room));
        writeHead(data, headEnd);
        beginTail();
    }
    writeTail(data + headEnd, n - headEnd, headEnd);
    writeKept(index, headEnd);
}

// The head ends: tail segment 1 opens, and the head says where it ends and where the log goes on.
void Log::beginTail() {
    Impl& I = *m_impl;
    I.cut = true;
    openSegment();   // a failure is noted by it, and tried again with the next lines
    static_assert(kLogTailSegments == 4, "the head's marker says 4");
    const std::string first = leafName(segmentPath(I.headPath, 1));
    char marker[768];
    size_t off = 0;
    appendf(marker, sizeof(marker), off,
            "[edvr] log head ends here at %llu KB (a quarter of log.max_mb = %llu MB); the log continues in %s, only the newest 4 tail "
            "segments are kept, and what follows below is the first line of each kind written after this point and the memory watch's "
            "lines, up to %llu KB.\r\n",
            static_cast<unsigned long long>(I.plan.head / 1024), static_cast<unsigned long long>(m_maxBytes / (1024ull * 1024ull)),
            first.c_str(), static_cast<unsigned long long>(I.plan.kept / 1024));
    writeHead(marker, off);
}

// Delete the segments older than the newest 4 there will be once `opening` is open. One that cannot be deleted (someone has it open
// without sharing delete) stays, and is tried again at the next rotation; it never holds up the log.
void Log::deleteOldSegments(uint32_t opening) {
    Impl& I = *m_impl;
    for (SegInfo& s : I.segs) {
        if (!s.onDisk || s.no + kLogTailSegments > opening) continue;
        if (DeleteFileW(segmentPath(I.headPath, s.no).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            s.onDisk = false;
            I.deleted.push_back(s);
        }
    }
    for (size_t i = I.segs.size(); i > 0; --i) {
        if (!I.segs[i - 1].onDisk) I.segs.erase(I.segs.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }
}

// Open the next tail segment (the one after the newest opened) and write its top marker; false when it cannot be created.
bool Log::openSegment() {
    Impl& I = *m_impl;
    const uint32_t no = I.segNo + 1;
    deleteOldSegments(no);
    HANDLE h = CreateFileW(segmentPath(I.headPath, no).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (!I.tailErrorNoted) {
            I.tailErrorNoted = true;
            char msg[200];
            const int n = _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                                      "[edvr] could not open tail segment %u (error %lu); log lines are discarded until it can be opened.\r\n", no, GetLastError());
            if (n > 0) writeHead(msg, static_cast<size_t>(n));
        }
        return false;
    }
    I.tailErrorNoted = false;
    I.seg = h;
    I.segNo = no;
    I.segBytes = 0;
    I.segFresh = true;
    SegInfo info;
    info.no = no;
    I.segs.push_back(info);

    char marker[1536];
    size_t off = 0;
    appendf(marker, sizeof(marker), off, "[edvr] tail segment %u of %s.", no, I.headLeaf.c_str());
    if (!I.deleted.empty()) {
        appendf(marker, sizeof(marker), off, " Deleted so far:");
        const size_t shown = I.deleted.size() < 6 ? I.deleted.size() : 6;
        const size_t start = I.deleted.size() - shown;
        const char* sep = " ";
        if (start > 0) {
            appendf(marker, sizeof(marker), off, " %zu older segments not listed", start);
            sep = "; ";
        }
        for (size_t i = start; i < I.deleted.size(); ++i) {
            const SegInfo& d = I.deleted[i];
            appendf(marker, sizeof(marker), off, "%ssegment %u (%llu bytes, %s to %s)", sep, d.no, static_cast<unsigned long long>(d.bytes),
                    d.first[0] ? d.first : "?", d.last[0] ? d.last : "?");
            sep = "; ";
        }
        appendf(marker, sizeof(marker), off, ".");
    }
    appendf(marker, sizeof(marker), off, "\r\n");
    writeSegment(marker, off, false);
    return true;
}

// p[0..n) of a buffer, which starts `bufferOffset` bytes into it, into tail segments, cut between lines, the next opening as one fills.
void Log::writeTail(const char* p, size_t n, size_t bufferOffset) {
    Impl& I = *m_impl;
    while (n > 0) {
        if (I.seg == INVALID_HANDLE_VALUE && !openSegment()) return;   // the lines are discarded; the head said so
        const uint64_t room = I.segBytes < I.plan.segment ? I.plan.segment - I.segBytes : 0;
        size_t take = n <= room ? n : logCutAtLine(p, n, static_cast<size_t>(room));
        if (take == 0) {
            if (!I.segFresh) {
                CloseHandle(I.seg);   // full: the next opens at the top of the loop
                I.seg = INVALID_HANDLE_VALUE;
                continue;
            }
            take = logFirstLineBytes(p, n);   // a line wider than a segment still goes in, whole
        }
        I.chunks.push_back(ChunkMark{bufferOffset, I.segNo});
        writeSegment(p, take, true);
        I.segFresh = false;
        p += take;
        n -= take;
        bufferOffset += take;
        if (n > 0) {
            CloseHandle(I.seg);
            I.seg = INVALID_HANDLE_VALUE;
        }
    }
}

uint32_t Log::segmentAt(size_t bufferOffset) const {
    const std::vector<ChunkMark>& c = m_impl->chunks;
    for (size_t i = c.size(); i > 0; --i) {
        if (c[i - 1].offset <= bufferOffset) return c[i - 1].segment;
    }
    return m_impl->segNo;
}

// The kept lines of a buffer the head has ended before (or in): the first line of each kind and the noteReserved lines, in the order
// they were noted, into the head, under a marker that says which tail segment each belongs to. A line that went to the head itself
// (offset < headEnd) is not written again. Each of the two budgets says once that it is spent, and the head says so once in all.
void Log::writeKept(int index, size_t headEnd) {
    Impl& I = *m_impl;
    if (I.chunks.empty()) return;
    const std::vector<char>& b = I.buf[index];
    for (const KeptSpan& span : I.spans[index]) {
        if (span.offset < headEnd) continue;
        if (span.offset > b.size() || span.bytes > b.size() - span.offset) break;
        const ReserveVerdict verdict = (span.reserved ? I.keepReserved : I.keepKinds).admit(span.bytes);
        if (verdict == ReserveVerdict::Skip) continue;
        if (verdict == ReserveVerdict::Spent) {
            if (!I.keptSpentNoted) {
                I.keptSpentNoted = true;
                const char msg[] = "[edvr] the kept-lines budget is spent (the first lines of new kinds, or the memory watch's lines, no longer fit here); "
                                   "later ones are only in the tail segments still on disk.\r\n";
                writeHead(msg, sizeof(msg) - 1);
            }
            continue;
        }
        const uint32_t segment = segmentAt(span.offset);
        if (segment != I.keptSeg) {
            char marker[96];
            const int n = _snprintf_s(marker, sizeof(marker), _TRUNCATE, "[edvr] kept lines from tail segment %u\r\n", segment);
            if (n > 0) writeHead(marker, static_cast<size_t>(n));
            I.keptSeg = segment;
        }
        writeHead(b.data() + span.offset, span.bytes);
    }
}

// Every 5 minutes (m_volumeIntervalMs): one line with what the log took in the window and which kinds took most of it. Nothing for an empty window.
void Log::noteVolumeIfDue() {
    Impl& I = *m_impl;
    if (!I.rotating) return;
    const uint32_t interval = m_volumeIntervalMs.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    if (interval == 0 || now - I.volumeStart < interval) return;
    I.volumeStart = now;

    LogKindTable::Top top[6];
    uint64_t totalBytes = 0, totalLines = 0, untracked = 0;
    lock();
    const size_t have = I.kinds.snapshot(top, 6, &totalBytes, &totalLines);
    untracked = I.kinds.overflow();
    unlock();
    if (totalLines == 0) return;

    char text[1000];
    size_t off = 0;
    if (interval % 60000 == 0) {
        appendf(text, sizeof(text), off, "log volume (last %u minutes): ", interval / 60000);
    } else {
        appendf(text, sizeof(text), off, "log volume (last %u ms): ", interval);
    }
    appendf(text, sizeof(text), off, "%.2f MB in %llu lines. Top kinds by size:", static_cast<double>(totalBytes) / 1048576.0,
            static_cast<unsigned long long>(totalLines));
    for (size_t i = 0; i < have; ++i) {
        for (char* c = top[i].text; *c; ++c) {
            if (static_cast<unsigned char>(*c) < 0x20 || static_cast<unsigned char>(*c) > 0x7e) *c = '?';
        }
        appendf(text, sizeof(text), off, "%s \"%s\" %.2f MB %u lines", i ? ";" : "", top[i].text, static_cast<double>(top[i].bytes) / 1048576.0, top[i].lines);
    }
    if (untracked) {
        appendf(text, sizeof(text), off, ". %llu lines so far were of kinds the full kind table could not track", static_cast<unsigned long long>(untracked));
    }
    note("%s", text);
}

void Log::append(const char* text, size_t bytes, bool reserved, uint64_t kind) {
    lock();
    const int active = m_impl->active.load(std::memory_order_relaxed);
    std::vector<char>& b = m_impl->buf[active];
    if (b.size() + bytes > m_bufferCapBytes) {
        // Counted BEFORE the unlock and through an atomic, so a burst on two
        // threads cannot lose counts. The number is the whole point: an
        // uncounted drop is a hole in the log that reads like a quiet frame.
        // (And a dropped line is not a kind seen: the table is touched below.)
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        unlock();
        return;
    }
    bool keep = reserved;
    if (kind && m_impl->kinds.ready() && bytes >= kLogStampBytes + 2) {
        if (m_impl->kinds.touch(kind, text + kLogStampBytes, bytes - kLogStampBytes - 2, bytes) == LogKindTable::Touch::New) keep = true;
    }
    if (keep && m_impl->rotating) m_impl->spans[active].push_back(KeptSpan{b.size(), bytes, reserved});
    b.insert(b.end(), text, text + bytes);
    unlock();
}

// One timestamped line, "[hh:mm:ss.mmm] text\r\n", in line[0..cap). Returns its length, or 0 when nothing can be written.
static int formatStamped(char* line, size_t cap, const char* fmt, va_list ap) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    int n = _snprintf_s(line, cap, _TRUNCATE, "[%02u:%02u:%02u.%03u] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0) return 0;

    // Room for the marker is reserved up front, not found afterwards.
    //
    // Formatting into the whole buffer and then looking for space left exactly
    // one byte, so the "...[truncated]" marker could never render -- a single
    // space was appended instead, and a truncated line looked like an ordinary
    // one. Messages lose the marker's width of capacity; a line long enough to
    // notice that is a line being truncated anyway.
    static const char kMark[] = " ...[truncated]";
    const int kMarkLen = static_cast<int>(sizeof(kMark)) - 1;
    const int m = _vsnprintf_s(line + n, cap - n - 3 - kMarkLen, _TRUNCATE,
                               fmt, ap);
    if (m > 0) {
        n += m;
    } else if (m < 0) {
        // Truncated, not failed.
        //
        // _vsnprintf_s with _TRUNCATE returns -1 when the text did not fit,
        // having already written as much as did and NUL-terminated it. Testing
        // `m > 0` therefore threw the whole message away and left a bare
        // timestamp -- and the messages long enough to hit this are the ones
        // carrying paths and diagnostics somebody is reading off a support log.
        n += static_cast<int>(strlen(line + n));
        const int room = static_cast<int>(cap) - n - 3;
        const int take = kMarkLen < room ? kMarkLen : room;
        if (take > 0) {
            memcpy(line + n, kMark, static_cast<size_t>(take));
            n += take;
        }
    }

    line[n++] = '\r';
    line[n++] = '\n';
    return n;
}

void Log::note(const char* fmt, ...) {
    if (!m_open) return;
    startFlusherOnce();

    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    const int n = formatStamped(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) append(line, static_cast<size_t>(n), false, m_maxBytes ? logKindHash(fmt, line, static_cast<size_t>(n)) : 0);
}

void Log::noteReserved(const char* fmt, ...) {
    if (!m_open) return;
    startFlusherOnce();

    char line[kLogReservedLineBytes];
    va_list ap;
    va_start(ap, fmt);
    const int n = formatStamped(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) append(line, static_cast<size_t>(n), true, m_maxBytes ? logKindHash(fmt, line, static_cast<size_t>(n)) : 0);
}

void Log::detachDuringProcessExit() {
    if (!m_open || !m_impl) return;

    // Cleared first so every later note() is a no-op, including any the guard
    // filter would emit, and so close() at CRT teardown returns immediately.
    m_open = false;
    m_impl->stop.store(true);

    // Unlocked and best effort: a terminated flusher may still hold the spinlock,
    // and blocking here would hang the process rather than lose a few lines.
    // What is written goes to the file being written: the head until it has ended, then the tail segment. The kept lines are not written here.
    const HANDLE out = m_impl->seg != INVALID_HANDLE_VALUE ? m_impl->seg : m_impl->file;
    __try {
        for (int i = 0; i < 2; ++i) {
            std::vector<char>& b = m_impl->buf[i];
            if (b.empty()) continue;
            DWORD written = 0;
            WriteFile(out, b.data(), static_cast<DWORD>(b.size()), &written,
                      nullptr);
        }
        // And the drop notice, which writeBuffer would have written had a
        // flush pass got there first. Without this, lines lost in the last
        // window before the process dies are still silent -- the same silence
        // this change exists to end, narrowed to the final 250 ms, and a
        // crash is exactly when a reader most needs to know the tail is
        // incomplete. Stack buffer and a direct WriteFile: no lock, no heap,
        // no note(), which is what this function is allowed to use.
        const uint64_t lost = m_dropped.load(std::memory_order_relaxed);
        if (lost > m_droppedReported) {
            char msg[200];
            const int n = _snprintf_s(
                msg, sizeof(msg), _TRUNCATE,
                "[edvr] %llu log line(s) were DROPPED before this point and "
                "the process is exiting; the tail above is not continuous.\r\n",
                static_cast<unsigned long long>(lost));
            if (n > 0) {
                DWORD w = 0;
                WriteFile(out, msg, static_cast<DWORD>(n), &w, nullptr);
            }
        }
        // The one line a module asked to have said at the end (setExitLine), stamped like note()'s: stack buffer, direct WriteFile, nothing that locks or allocates.
        if (const ExitLineFn exitLine = m_exitLine.load(std::memory_order_acquire)) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char msg[400];
            int n = _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            if (n > 0) {
                const int m = exitLine(msg + n, sizeof(msg) - static_cast<size_t>(n) - 3);
                if (m > 0 && n + m + 2 <= static_cast<int>(sizeof(msg))) {
                    n += m;
                    msg[n++] = '\r';
                    msg[n++] = '\n';
                    DWORD w = 0;
                    WriteFile(out, msg, static_cast<DWORD>(n), &w, nullptr);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Buffers were mid-update when their writer was killed. Nothing to
        // recover; everything flushed before this is already on disk.
    }

    if (m_impl->seg != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_impl->seg);
        CloseHandle(m_impl->seg);
        m_impl->seg = INVALID_HANDLE_VALUE;
    }
    if (m_impl->file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_impl->file);
        CloseHandle(m_impl->file);
        m_impl->file = INVALID_HANDLE_VALUE;
    }

    // m_impl is deliberately leaked: freeing it would run a std::thread
    // destructor on a dead thread and call the allocator, either of which can
    // fault if the terminated flusher held the heap lock.
}

void Log::close() {
    if (!m_open || !m_impl) return;
    m_open = false;

    if (m_impl->started.load()) {
        m_impl->stop.store(true);
        if (m_impl->flusher.joinable()) m_impl->flusher.join();
    }
    writeBuffer(0);
    writeBuffer(1);
    if (m_impl->seg != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_impl->seg);
        CloseHandle(m_impl->seg);
        m_impl->seg = INVALID_HANDLE_VALUE;
    }
    if (m_impl->file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_impl->file);
        CloseHandle(m_impl->file);
        m_impl->file = INVALID_HANDLE_VALUE;
    }
    delete m_impl;
    m_impl = nullptr;
}

}  // namespace edvr
