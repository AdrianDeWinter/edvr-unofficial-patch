// The log's size plan and its bookkeeping, apart from the files (Log in log.h): the arithmetic of the head and the tail segments, where a
// cut falls, and the table of line kinds. Header-only so the pieces are small and plain; the files and the flusher are in log.cpp.
//
// WHY. The log used to stop at log.max_mb and say so, which lost everything after the first ten minutes of an on-foot session: the crash
// or the hang that was being chased. Now a session keeps its start in full and always has its newest lines, within the same bound:
//
//   max_mb M  (0: one unbounded file, as before)
//   head      H = M/4      the session's own file; the opening lines, in full, until the next line would pass H
//   kept      K = M/8      also in the head, after the cut: the first line of each kind written after it, and the memory watch's lines
//   tail      T = M - H - K   split in 4 segments of S = T/4, named <head>.tail-001.log ..., the oldest deleted as a fifth opens
//
// so the head (with K), plus 4 segments, is at most M. The head says where it ends and where the log goes on, each segment says which
// it is and what was deleted before it, and tools\edvr_log.py reads the three as one session.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "log_reserve.h"

namespace edvr {

constexpr int kLogTailSegments = 4;

struct LogPlan {
    uint64_t head = 0;      // H
    uint64_t kept = 0;      // K
    uint64_t tail = 0;      // T
    uint64_t segment = 0;   // S

    static LogPlan make(uint64_t maxBytes) {
        LogPlan p;
        p.head = maxBytes / 4;
        p.kept = maxBytes / 8;
        p.tail = maxBytes - p.head - p.kept;
        p.segment = p.tail / kLogTailSegments;
        return p;
    }
};

// A line ends "\r\n". Returns the largest c <= limit with p[0..c) made of whole lines, or n when all n bytes fit; 0 when not even the first
// line fits under limit.
inline size_t logCutAtLine(const char* p, size_t n, size_t limit) {
    if (limit >= n) return n;
    size_t i = limit;
    while (i >= 2 && !(p[i - 1] == '\n' && p[i - 2] == '\r')) --i;
    return i >= 2 ? i : 0;
}

// The length of the first line of p[0..n), its end included (n when it has none).
inline size_t logFirstLineBytes(const char* p, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        if (p[i] == '\n' && p[i - 1] == '\r') return i + 1;
    }
    return n;
}

// "[hh:mm:ss.mmm] " in front of every line Log::note writes.
constexpr size_t kLogStampBytes = 15;

// The "hh:mm:ss.mmm" of a stamped line at p, copied to out (13 bytes with the NUL); false when the line carries no stamp.
inline bool logStampAt(const char* p, size_t n, char* out) {
    if (n < kLogStampBytes || p[0] != '[' || p[3] != ':' || p[6] != ':' || p[9] != '.' || p[13] != ']' || p[14] != ' ') return false;
    static const int kDigits[] = {1, 2, 4, 5, 7, 8, 10, 11, 12};
    for (int d : kDigits) {
        if (p[d] < '0' || p[d] > '9') return false;
    }
    std::memcpy(out, p + 1, 12);
    out[12] = 0;
    return true;
}

// A line's kind: the format string's address, then the message text (after the stamp) up to its first digit or 64 bytes. The address alone
// is not enough (about 300 call sites say note("%s", ...)), the text alone would split one site's lines at every number.
inline uint64_t logKindHash(const void* fmt, const char* line, size_t lineBytes) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* f = reinterpret_cast<const uint8_t*>(&fmt);
    for (size_t i = 0; i < sizeof(fmt); ++i) {
        h ^= f[i];
        h *= 1099511628211ull;
    }
    const size_t end = lineBytes >= 2 ? lineBytes - 2 : lineBytes;
    for (size_t i = kLogStampBytes; i < end && i < kLogStampBytes + 64; ++i) {
        const char c = line[i];
        if (c >= '0' && c <= '9') break;
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h ? h : 1;   // 0 marks an empty slot
}

// Every kind seen in the session, in a fixed open-addressing table allocated at open, plus what each kind wrote in the current volume
// window. Touched under Log's spinlock only.
class LogKindTable {
public:
    static constexpr size_t kSlots = 16384;                 // a power of two
    static constexpr size_t kMaxUsed = kSlots - kSlots / 8; // past this the table is full: no new kind is tracked
    static constexpr size_t kTextBytes = 40;

    enum class Touch { Known, New, Untracked };

    struct Top {
        char text[kTextBytes + 1];
        uint32_t bytes;
        uint32_t lines;
    };

    void init() {
        slots_.assign(kSlots, Slot{});
        touched_.clear();
        touched_.reserve(kSlots);
        used_ = 0;
        overflow_ = 0;
        winBytes_ = 0;
        winLines_ = 0;
    }
    bool ready() const { return !slots_.empty(); }
    size_t used() const { return used_; }
    uint64_t overflow() const { return overflow_; }

    // One line of `lineBytes`, its message text at text[0..textLen). New: the first line of a kind the session has not seen.
    Touch touch(uint64_t hash, const char* text, size_t textLen, size_t lineBytes) {
        winBytes_ += lineBytes;
        ++winLines_;
        size_t i = static_cast<size_t>(hash) & (kSlots - 1);
        for (size_t probe = 0; probe < kSlots; ++probe, i = (i + 1) & (kSlots - 1)) {
            Slot& s = slots_[i];
            if (s.hash == hash) {
                count(s, i, lineBytes);
                return Touch::Known;
            }
            if (s.hash == 0) {
                if (used_ >= kMaxUsed) break;
                s.hash = hash;
                s.textLen = static_cast<uint8_t>(textLen < kTextBytes ? textLen : kTextBytes);
                std::memcpy(s.text, text, s.textLen);
                ++used_;
                count(s, i, lineBytes);
                return Touch::New;
            }
        }
        ++overflow_;
        return Touch::Untracked;
    }

    // The window's totals and its `cap` biggest kinds by bytes (out[0] the biggest), then a new window. Returns how many kinds are in out.
    size_t snapshot(Top* out, size_t cap, uint64_t* totalBytes, uint64_t* totalLines) {
        *totalBytes = winBytes_;
        *totalLines = winLines_;
        size_t have = 0;
        for (uint32_t idx : touched_) {
            const Slot& s = slots_[idx];
            if (have == cap && s.bytes <= out[have - 1].bytes) continue;
            size_t at = have < cap ? have++ : cap - 1;
            while (at > 0 && out[at - 1].bytes < s.bytes) {
                out[at] = out[at - 1];
                --at;
            }
            std::memcpy(out[at].text, s.text, s.textLen);
            out[at].text[s.textLen] = 0;
            out[at].bytes = s.bytes;
            out[at].lines = s.lines;
        }
        for (uint32_t idx : touched_) {
            slots_[idx].bytes = 0;
            slots_[idx].lines = 0;
        }
        touched_.clear();
        winBytes_ = 0;
        winLines_ = 0;
        return have;
    }

private:
    struct Slot {
        uint64_t hash = 0;
        uint32_t bytes = 0;   // in the current window
        uint32_t lines = 0;   // in the current window
        uint8_t textLen = 0;
        char text[kTextBytes] = {};
    };

    void count(Slot& s, size_t index, size_t lineBytes) {
        if (s.lines == 0) touched_.push_back(static_cast<uint32_t>(index));
        ++s.lines;
        s.bytes += static_cast<uint32_t>(lineBytes);
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> touched_;
    size_t used_ = 0;
    uint64_t overflow_ = 0;
    uint64_t winBytes_ = 0;
    uint64_t winLines_ = 0;
};

}  // namespace edvr
