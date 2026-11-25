#pragma once
// How much of a card's own memory other programs hold: Windows' "\GPU Adapter Memory(*)\Dedicated Usage" counter (every
// process together, the figure Task Manager shows for the card) less "\GPU Process Memory(pid_<this>*)\Dedicated Usage"
// (this process), both from one collect so they describe the same moment. DXGI does not offer it (its usage is this
// process's alone), and game_vram_hook.cpp needs it once, at start, to know what other programs hold (game_vram.h,
// startBudgetMb).
//
// One query, opened, collected and closed on a pool thread. The read is started when auto is prepared and waited for, a
// bounded time, at Elite's first question about the adapter, so it overlaps the device's creation and Elite's own loading.
// Every card's rows are kept, so a device on another card needs no second read. pdh.dll is loaded at run time, as
// perf_monitor.cpp does. Wine and Windows before 10 1709 have no such counter: the answer is then "unknown", never 0.
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <new>
#include <vector>

namespace edvr {

// "luid_0x<high>_0x<low>_phys_<n>", an instance of Windows' GPU Adapter Memory counters (seen with Get-Counter, 2026-11-24).
inline bool parseAdapterInstance(const wchar_t* name, uint32_t& luidHigh, uint32_t& luidLow) {
    if (!name || std::wcsncmp(name, L"luid_0x", 7) != 0) return false;
    wchar_t* end = nullptr;
    const unsigned long high = std::wcstoul(name + 7, &end, 16);
    if (end == name + 7 || std::wcsncmp(end, L"_0x", 3) != 0) return false;
    const wchar_t* at = end + 3;
    const unsigned long low = std::wcstoul(at, &end, 16);
    if (end == at || std::wcsncmp(end, L"_phys_", 6) != 0) return false;
    luidHigh = static_cast<uint32_t>(high);
    luidLow = static_cast<uint32_t>(low);
    return true;
}

// "pid_<decimal>_luid_0x<high>_0x<low>_phys_<n>", an instance of the GPU Process Memory counters (Get-Counter, 2026-11-27).
inline bool parseProcessInstance(const wchar_t* name, uint32_t& pid, uint32_t& luidHigh, uint32_t& luidLow) {
    if (!name || std::wcsncmp(name, L"pid_", 4) != 0 || !std::iswdigit(name[4])) return false;
    wchar_t* end = nullptr;
    const unsigned long id = std::wcstoul(name + 4, &end, 10);
    if (*end != L'_' || !parseAdapterInstance(end + 1, luidHigh, luidLow)) return false;
    pid = static_cast<uint32_t>(id);
    return true;
}

// One card in a reading: every process's dedicated usage, and this process's.
struct AdapterUsageRow {
    uint32_t luidHigh = 0;
    uint32_t luidLow = 0;
    uint64_t allBytes = 0;
    uint64_t ownBytes = 0;
};

// The row for one card, or false when the reading has none.
inline bool findAdapterRow(const std::vector<AdapterUsageRow>& rows, uint32_t luidHigh, uint32_t luidLow,
                            AdapterUsageRow* found) {
    for (const AdapterUsageRow& row : rows) {
        if (row.luidHigh != luidHigh || row.luidLow != luidLow) continue;
        *found = row;
        return true;
    }
    return false;
}

inline AdapterUsageRow& adapterRow(std::vector<AdapterUsageRow>& rows, uint32_t luidHigh, uint32_t luidLow) {
    for (AdapterUsageRow& row : rows)
        if (row.luidHigh == luidHigh && row.luidLow == luidLow) return row;
    rows.push_back(AdapterUsageRow{luidHigh, luidLow, 0, 0});
    return rows.back();
}

// The formatted values of one counter of a collected query, or false. Rows with no valid data are left out by the caller.
inline bool counterArray(decltype(&::PdhGetFormattedCounterArrayW) array, PDH_HCOUNTER counter,
                         std::vector<std::max_align_t>& buffer, PDH_FMT_COUNTERVALUE_ITEM_W** items, DWORD* count) {
    constexpr DWORD kByteCap = 1u << 20;
    DWORD size = 0;
    *count = 0;
    if (array(counter, PDH_FMT_LARGE, &size, count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA) || size == 0 ||
        size > kByteCap)
        return false;
    buffer.assign((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t), std::max_align_t{});
    *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    return array(counter, PDH_FMT_LARGE, &size, count, *items) == ERROR_SUCCESS;
}

inline bool usableValue(const PDH_FMT_COUNTERVALUE_ITEM_W& item) {
    const DWORD status = item.FmtValue.CStatus;
    return (status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA) && item.FmtValue.largeValue >= 0;
}

// Every card's usage, and process `pid`'s, from one collect. False when it could not be read; `why` then names the step (a
// static string). A card this process has no row for has ownBytes 0.
inline bool adapterUsageRows(uint32_t pid, std::vector<AdapterUsageRow>& rows, const char** why) {
    rows.clear();
    HMODULE lib = LoadLibraryW(L"pdh.dll");   // kept loaded: perf_monitor.cpp uses it too
    if (!lib) {
        *why = "pdh.dll did not load";
        return false;
    }
    const auto open = reinterpret_cast<decltype(&::PdhOpenQueryW)>(GetProcAddress(lib, "PdhOpenQueryW"));
    const auto add = reinterpret_cast<decltype(&::PdhAddEnglishCounterW)>(GetProcAddress(lib, "PdhAddEnglishCounterW"));
    const auto collect = reinterpret_cast<decltype(&::PdhCollectQueryData)>(GetProcAddress(lib, "PdhCollectQueryData"));
    const auto array = reinterpret_cast<decltype(&::PdhGetFormattedCounterArrayW)>(GetProcAddress(lib, "PdhGetFormattedCounterArrayW"));
    const auto close = reinterpret_cast<decltype(&::PdhCloseQuery)>(GetProcAddress(lib, "PdhCloseQuery"));
    if (!open || !add || !collect || !array || !close) {
        *why = "pdh.dll is missing an entry point";
        return false;
    }
    PDH_HQUERY query = nullptr;
    if (open(nullptr, 0, &query) != ERROR_SUCCESS || !query) {
        *why = "PdhOpenQuery refused";
        return false;
    }
    PDH_HCOUNTER all = nullptr;
    PDH_HCOUNTER own = nullptr;
    *why = "no GPU Adapter Memory counter";
    // Gauges, not rates: one collect is enough, and it samples both counters at once.
    if (add(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &all) == ERROR_SUCCESS && all &&
        add(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &own) == ERROR_SUCCESS && own &&
        collect(query) == ERROR_SUCCESS) {
        std::vector<std::max_align_t> buffer;
        PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
        DWORD count = 0;
        if (counterArray(array, all, buffer, &items, &count)) {
            *why = "no card in the counter";
            for (DWORD i = 0; i < count; ++i) {
                uint32_t high = 0;
                uint32_t low = 0;
                if (!usableValue(items[i]) || !parseAdapterInstance(items[i].szName, high, low)) continue;
                adapterRow(rows, high, low).allBytes += static_cast<uint64_t>(items[i].FmtValue.largeValue);   // a linked card has one row a node
            }
            // This process may hold nothing yet, and then has no row: its share is 0.
            if (!rows.empty() && counterArray(array, own, buffer, &items, &count)) {
                for (DWORD i = 0; i < count; ++i) {
                    uint32_t id = 0;
                    uint32_t high = 0;
                    uint32_t low = 0;
                    if (!usableValue(items[i]) || !parseProcessInstance(items[i].szName, id, high, low) || id != pid) continue;
                    adapterRow(rows, high, low).ownBytes += static_cast<uint64_t>(items[i].FmtValue.largeValue);
                }
            }
        }
    }
    close(query);
    if (rows.empty()) return false;
    *why = "";
    return true;
}

// adapterUsageRows on a pool thread. Made once and never freed: a read that is not done by the deadline is left to finish
// on its own, and the owner is told "unknown".
struct AdapterUsageRead {
    HANDLE done = nullptr;   // set when what follows is final, and the last thing the callback touches
    uint32_t pid = 0;
    bool known = false;
    std::vector<AdapterUsageRow> rows;
    const char* why = "";
    double ms = 0.0;         // how long the read took, from submit to done
    LARGE_INTEGER began{};
};

inline VOID CALLBACK adapterUsageCallback(PTP_CALLBACK_INSTANCE instance, PVOID context) {
    CallbackMayRunLong(instance);
    auto* read = static_cast<AdapterUsageRead*>(context);
    try {
        read->known = adapterUsageRows(read->pid, read->rows, &read->why);
    } catch (...) {   // an exception must not leave a pool thread
        read->known = false;
        read->rows.clear();
        read->why = "the read failed";
    }
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    read->ms = 1000.0 * static_cast<double>(now.QuadPart - read->began.QuadPart) / static_cast<double>(frequency.QuadPart);
    SetEvent(read->done);
}

// Starts a read; null when there is no thread to read it on.
inline AdapterUsageRead* adapterUsageStart() {
    auto* read = new (std::nothrow) AdapterUsageRead;
    HANDLE done = read ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    if (read && done) {
        read->done = done;
        read->pid = GetCurrentProcessId();
        QueryPerformanceCounter(&read->began);
    }
    if (!read || !done || !TrySubmitThreadpoolCallback(adapterUsageCallback, read, nullptr)) {
        if (done) CloseHandle(done);
        delete read;
        return nullptr;
    }
    return read;
}

// Waits for `read` until waitMs after it started. True when it finished in time; its fields are then final. A read that
// is not done is not touched.
inline bool adapterUsageFinished(AdapterUsageRead* read, DWORD waitMs, DWORD* waitedMs) {
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double since = 1000.0 * static_cast<double>(now.QuadPart - read->began.QuadPart) / static_cast<double>(frequency.QuadPart);
    const DWORD left = since >= static_cast<double>(waitMs) ? 0 : waitMs - static_cast<DWORD>(since);
    const ULONGLONG before = GetTickCount64();
    const bool finished = WaitForSingleObject(read->done, left) == WAIT_OBJECT_0;
    *waitedMs = static_cast<DWORD>(GetTickCount64() - before);
    return finished;
}

}  // namespace edvr
