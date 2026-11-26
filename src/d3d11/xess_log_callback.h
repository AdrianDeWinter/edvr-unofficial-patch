// Thread-safe, bounded formatter for XeSS SDK logging callbacks (issue #256).
//
// XeSS may invoke its logging callback from worker threads with an ephemeral
// UTF-8 pointer valid only for the call's duration (xess.h). This helper copies
// and sanitizes each line into a fixed buffer and enforces an atomic cap so
// runtime chatter cannot flood the log.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <xess/xess.h>

#include "../common/log.h"

namespace edvr {

struct XessLogFormatResult {
    enum class Action { Emit, EmitSuppressedNotice, Drop };
    Action action = Action::Drop;
    char line[320] = {};
};

class XessLogLimiter {
  public:
    // Separate budgets for DEBUG/INFO vs WARNING/ERROR so verbose startup INFO
    // chatter can never starve runtime warnings or errors.
    explicit constexpr XessLogLimiter(uint32_t warnErrCap = 40, uint32_t infoCap = 24)
        : warnErrCap_(warnErrCap), infoCap_(infoCap) {}

    void reset() {
        warnErrCount_.store(0, std::memory_order_relaxed);
        infoCount_.store(0, std::memory_order_relaxed);
    }
    uint32_t count() const {
        return warnErrCount_.load(std::memory_order_relaxed) +
               infoCount_.load(std::memory_order_relaxed);
    }
    uint32_t warnErrCount() const { return warnErrCount_.load(std::memory_order_relaxed); }
    uint32_t infoCount() const { return infoCount_.load(std::memory_order_relaxed); }
    uint32_t cap() const { return warnErrCap_; }
    uint32_t infoCap() const { return infoCap_; }

    XessLogFormatResult format(const char* message, int loggingLevel) {
        XessLogFormatResult res;
        const bool isInfo = (loggingLevel == 0 || loggingLevel == 1);
        std::atomic<uint32_t>& bucket = isInfo ? infoCount_ : warnErrCount_;
        const uint32_t limit = isInfo ? infoCap_ : warnErrCap_;
        const uint32_t n = bucket.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n > limit) {
            if (n == limit + 1) {
                res.action = XessLogFormatResult::Action::EmitSuppressedNotice;
                std::snprintf(res.line, sizeof(res.line),
                              "xess: further runtime %s messages suppressed (cap=%u)",
                              isInfo ? "info/debug" : "warning/error", limit);
            }
            return res;
        }

        char levelBuf[24]{};
        const char* levelName = nullptr;
        switch (loggingLevel) {
            case 0: levelName = "debug"; break;
            case 1: levelName = "info"; break;
            case 2: levelName = "warning"; break;
            case 3: levelName = "error"; break;
            default:
                std::snprintf(levelBuf, sizeof(levelBuf), "level=%d", loggingLevel);
                levelName = levelBuf;
                break;
        }

        // Copy ephemeral message into a bounded single-line buffer.
        constexpr size_t kMaxMsg = 256;
        char sanitized[kMaxMsg + 1]{};
        bool truncated = false;
        if (!message) {
            std::snprintf(sanitized, sizeof(sanitized), "(null)");
        } else {
            size_t outLen = 0;
            for (const char* p = message; *p; ++p) {
                if (outLen >= kMaxMsg) {
                    truncated = true;
                    break;
                }
                const unsigned char ch = static_cast<unsigned char>(*p);
                sanitized[outLen++] = (ch == '\r' || ch == '\n' || ch == '\t' || ch < 0x20) ? ' ' : *p;
            }
            while (outLen > 0 && sanitized[outLen - 1] == ' ') --outLen;
            if (truncated && outLen >= 3) {
                sanitized[outLen - 3] = '.';
                sanitized[outLen - 2] = '.';
                sanitized[outLen - 1] = '.';
            }
            sanitized[outLen] = '\0';
        }

        res.action = XessLogFormatResult::Action::Emit;
        std::snprintf(res.line, sizeof(res.line), "xess: runtime %s: %s", levelName, sanitized);
        return res;
    }

  private:
    uint32_t warnErrCap_;
    uint32_t infoCap_;
    std::atomic<uint32_t> warnErrCount_{0};
    std::atomic<uint32_t> infoCount_{0};
};

// The SDK callback carries no context handle or user data. Give each slot its
// own entry point and reset that slot's budget before registering for a new context.
template <std::size_t SlotCount>
class XessLogCallbacks {
  public:
    static xess_app_log_callback_t beginInit(unsigned slot) {
        static constexpr auto callbacks = makeCallbacks(std::make_index_sequence<SlotCount>{});
        limiters_[slot].reset();
        return callbacks[slot];
    }

  private:
    template <std::size_t Slot>
    static void callback(const char* message, xess_logging_level_t level) {
        const XessLogFormatResult result = limiters_[Slot].format(message, static_cast<int>(level));
        if (result.action != XessLogFormatResult::Action::Drop)
            Log::get().note("%s", result.line);
    }

    template <std::size_t... Slots>
    static constexpr auto makeCallbacks(std::index_sequence<Slots...>) {
        return std::array<xess_app_log_callback_t, SlotCount>{&callback<Slots>...};
    }

    inline static std::array<XessLogLimiter, SlotCount> limiters_;
};

}  // namespace edvr
