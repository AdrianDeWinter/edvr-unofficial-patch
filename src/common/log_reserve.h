// A byte budget for lines the log writes a second time (Log::noteReserved in log.h, and the first line of each kind): the accounting,
// apart from the file, so a rig can hold it.
//
// WHY. The log's head ends at a quarter of log.max_mb and the rest of a session goes to rotating tail segments (log_rotate.h), so a
// line that is the only evidence of a slow event (the memory watch's mem: lines, src\common\mem_watch.h) would be deleted with its
// segment. After the head ends, such a line is also written to the head, from a budget with a fixed limit: a run-away writer cannot
// fill a disk through it. Log holds two, each half of the head's kept-lines room (K = max_mb/8): one for the noteReserved lines and
// one for the first line of each kind. kLogReserveBytes is the limit a budget has until setLimit gives it another.
//
// WHAT. A line is admitted while the bytes already written from the budget plus its own fit the limit. The first line that does not
// fit is refused with Spent, once, so the log can say the budget is gone; every line after that is Skip.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

constexpr uint64_t kLogReserveBytes = 1ull << 20;

// The room one reserved line is formatted in, its stamp and line end included. An ordinary line has 1200 bytes.
constexpr size_t kLogReservedLineBytes = 2800;

enum class ReserveVerdict {
    Write,   // the line is within the reserve
    Spent,   // the reserve cannot take this line: say so, once
    Skip,    // the reserve was spent before: nothing is written
};

class LogReserve {
public:
    void setLimit(uint64_t bytes) { limit_ = bytes; }
    uint64_t limit() const { return limit_; }

    ReserveVerdict admit(uint64_t lineBytes) {
        if (spent_) return ReserveVerdict::Skip;
        if (lineBytes > limit_ - used_) {
            spent_ = true;
            return ReserveVerdict::Spent;
        }
        used_ += lineBytes;
        return ReserveVerdict::Write;
    }

    void reset() {
        used_ = 0;
        spent_ = false;
    }

    uint64_t used() const { return used_; }
    bool spent() const { return spent_; }

private:
    uint64_t limit_ = kLogReserveBytes;
    uint64_t used_ = 0;
    bool spent_ = false;
};

}  // namespace edvr
