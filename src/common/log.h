// EDVR log.
//
// Plain text, one timestamped line per event. It records which game build is
// running, whether the fix installed, which shader it settled on, and whether
// it engaged -- which is everything needed to make a bug report actionable, and
// nothing else.
//
// Text rather than a binary format on purpose: the support path for this is
// somebody opening the file and pasting it into a forum post, and that should
// not require a decoder.
//
// Writes go to a double-buffered ring drained by a background thread, because
// the alternative is touching the filesystem from the render thread at 90Hz.
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

#include "log_reserve.h"

namespace edvr {

class Log {
public:
    static Log& get();

    // dir is created if needed. Safe to call more than once.
    bool open(const std::wstring& dir, const wchar_t* tag);

    // Normal teardown: stops and joins the flusher, releases everything.
    void close();

    // Teardown for DLL_PROCESS_DETACH during process termination.
    //
    // By then Windows has terminated every other thread, possibly while one
    // held our spinlock or the process heap lock. So this must never join a
    // thread, never take the spinlock, and never free heap. It writes what it
    // can and leaves the rest for the OS to reclaim.
    void detachDuringProcessExit();

    // One line the log writes as the process ends, after everything buffered (detachDuringProcessExit; a FreeLibrary teardown says its own). The game never unloads this
    // DLL, so a summary that is only said at a teardown is never said: this is the way to have one. `fn` runs where nothing may be relied on -- no lock, no heap, no note() --
    // and formats from atomics into `out`, returning the length (0: nothing to say). One slot; the last setter wins.
    using ExitLineFn = int (*)(char* out, size_t cap);
    void setExitLine(ExitLineFn fn) { m_exitLine.store(fn, std::memory_order_release); }

    void note(const char* fmt, ...);

    // A line that is also kept in the log's head once the head has ended (log.max_mb, log_rotate.h). Before that it is exactly
    // note(). After it, the line is written to the tail segment like any other and again to the head, from a budget of half the
    // head's kept-lines room (max_mb/16), in the order it was noted, until the budget is spent; the head then says so once and
    // keeps no more of these either. So the line outlives its segment. For the few lines that are the only record of something
    // slow (the memory watch's mem: lines), not for anything that can be a note(). It has room for a line of
    // kLogReservedLineBytes with its stamp; note() has 1200.
    void noteReserved(const char* fmt, ...);

    bool isOpen() const { return m_open; }
    uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }
    // The size bound in force, in bytes (log.max_mb, read when the log opened); 0 means one file with no bound. For the rigs: the
    // default is pinned by tools\config_test, and the head and the tail segments at a small explicit value with it.
    uint64_t maxBytes() const { return m_maxBytes; }
    const std::wstring& dir() const { return m_dir; }
    // How often the flusher notes the volume line (300000 ms; a rig sets it short). Not a setting.
    void setVolumeIntervalMsForTest(uint32_t ms) { m_volumeIntervalMs.store(ms, std::memory_order_relaxed); }

private:
    Log() = default;
    ~Log();
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void lock();
    void unlock();
    void startFlusherOnce();
    void flusherMain();
    void writeBuffer(int index);
    void writeRotating(std::vector<char>& b, int index);
    void writeHead(const char* p, size_t n);
    void writeTail(const char* p, size_t n, size_t bufferOffset);
    void writeCurrent(const char* p, size_t n);
    void writeSegment(const char* p, size_t n, bool stamped);
    void beginTail();
    bool openSegment();
    void deleteOldSegments(uint32_t opening);
    void writeKept(int index, size_t headEnd);
    uint32_t segmentAt(size_t bufferOffset) const;
    void noteVolumeIfDue();
    void append(const char* text, size_t bytes, bool reserved, uint64_t kind);

    struct Impl;
    Impl* m_impl = nullptr;

    std::wstring m_dir;

    // Atomic, not volatile (2026-09-07). The increment used to happen AFTER
    // append() released the spinlock, so two threads dropping at once lost
    // counts -- and the flusher reads it from a third. This is the same
    // read-modify-write FaultBudget's comment in guard.h already names.
    std::atomic<uint64_t> m_dropped{0};
    std::atomic<ExitLineFn> m_exitLine{nullptr};
    // Written by whichever thread is inside writeBuffer, which is the flusher
    // OR the one calling close() -- never both, because close() joins the
    // flusher first and the join is the happens-before edge. "One writer at a
    // time, ordered", not "one thread": the plain type is safe for that and
    // not for anything looser.
    uint64_t m_droppedReported = 0;
    bool m_open = false;

    // Hard ceiling, so a long session cannot fill a disk: the head, its kept lines and the four tail segments together (log_rotate.h).
    uint64_t m_maxBytes = 0;      // 0 = unlimited, one file
    // The bytes the head file holds. Touched only where the files are written (the flusher, or close() after it is joined).
    uint64_t m_bytesWritten = 0;
    std::atomic<uint32_t> m_volumeIntervalMs{300000};

    // How much unflushed text may queue between two flusher passes. From
    // log.buffer_mb; see the comment on kDefaultBufferMb in log.cpp for why
    // the default is what it is.
    size_t   m_bufferCapBytes = 0;
};

uint64_t fnv1a64(const void* data, size_t bytes);
int64_t  qpcNow();
int64_t  qpcFrequency();

}  // namespace edvr
