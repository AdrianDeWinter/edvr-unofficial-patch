// The eye dump's compressed file container (EDVRZ1), and the writer every
// dump file goes through.
//
// A dump file that used to be written as NAME is written as NAME.z: zlib
// streams of 4 MiB chunks behind a small header, compressed at level 1 on a
// few worker threads. tools\dump_io.py reads it (read_bytes / --expand), and
// still reads a plain NAME, so a dump from before this container works too.
//
//   0   char[8]  "EDVRZ1\0\0"
//   8   u32      version = 1
//   12  u32      flags: bit 0 = the decoded bytes are XORed with a base file
//   16  u64      rawBytes: decoded length (with XOR, also the base's length)
//   24  u32      chunkRawBytes = 4 MiB (every chunk but the last decodes to this)
//   28  u32      chunkCount
//   32  u32      baseNameBytes (0 unless XOR), then the base's file name, UTF-8,
//                same directory, no path
//   then chunkCount x { u32 compressedBytes; one RFC 1950 zlib stream }
//   all little-endian
//
// Data is never lost: a file whose compression fails for any reason is written
// plain under its ORIGINAL name, and the log says so. Workers only compress
// memory; nothing here touches D3D, so every call is for the calling thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dump_file {

constexpr uint32_t kChunkBytes = 4u << 20;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kFlagXor = 1u;

// What one scope of writes (an eye run, a pool ledger) did, for the one summary
// line each prints. Counts only what the writers in the scope did.
struct Tally {
    uint32_t files = 0;        // files written compressed (.z), deltas included
    uint32_t deltaFiles = 0;   // of those, stored as an XOR against a base file
    uint32_t plainFiles = 0;   // compression failed: written plain under the original name
    uint64_t rawBytes = 0;     // decoded bytes of the compressed files
    uint64_t writtenBytes = 0; // bytes those files occupy on disk
    uint64_t wallUs = 0;       // calling-thread time inside open..close of every writer
};

// While one is alive on this thread, writers opened on this thread add to
// `tally`. Nesting restores the outer scope on exit; the outer one does not
// receive the inner one's files.
class TallyScope {
public:
    explicit TallyScope(Tally& tally) noexcept;
    ~TallyScope();
    TallyScope(const TallyScope&) = delete;
    TallyScope& operator=(const TallyScope&) = delete;
private:
    Tally* m_prev;
};

// The summary line's common part, "N files compressed (A MB -> B MB), M delta,
// K plain after a failure, T ms". Never empty: zero files says so.
std::string tallyText(const Tally& t);

struct WholeResult;

// A streaming writer: open, write any number of times, close. Header-then-rows
// writers keep their shape. A writer that is destroyed open is closed.
class Writer {
public:
    Writer() noexcept;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // `plainPath` is the file's ORIGINAL name; the bytes land in plainPath + ".z"
    // (or in plainPath itself if compression fails). False if neither opens.
    bool open(const wchar_t* plainPath) noexcept;
    bool write(const void* data, size_t bytes) noexcept;
    // Finishes the file. False if any write or the close failed.
    bool close() noexcept;

    bool isOpen() const noexcept;
    // The path the bytes actually went to (".z" or the plain name); valid after
    // open. Empty before.
    const std::wstring& writtenPath() const noexcept;
    bool compressed() const noexcept;   // the file is (so far) a .z
    uint64_t rawBytes() const noexcept; // bytes given to write() so far

    struct Impl;   // opaque; public only so dump_file.cpp's helpers can name it

private:
    Impl* m_impl;
    friend WholeResult writeWhole(const wchar_t*, const void*, size_t, const wchar_t*) noexcept;
};

struct WholeResult {
    bool ok = false;
    bool compressed = false;
    bool delta = false;           // stored as an XOR against the base
    bool deltaDeclined = false;   // a base was asked for and not used; the file is plain
    std::string deltaWhy;         // why, when deltaDeclined
    std::wstring writtenPath;
    uint64_t rawBytes = 0;
    uint64_t writtenBytes = 0;
};

// One buffer to one file, compressed. With `basePath` the buffer is stored as an
// XOR against that file's bytes (read back from disk) when the base can be read
// and is exactly `bytes` long; otherwise the file is written PLAIN under its
// original name, exactly as it was before this container, and deltaDeclined
// says why. The base is never touched.
WholeResult writeWhole(const wchar_t* plainPath, const void* data, size_t bytes,
                       const wchar_t* basePath = nullptr) noexcept;

// The bytes of a whole plain file, or false. name + ".z" is NOT consulted: a delta's base is always plain.
bool readPlainFile(const wchar_t* path, std::string& out) noexcept;

// The bytes of a dump file under its ORIGINAL name, for C++ code and rigs that read one back: the plain
// file if it exists, else plainPath + ".z" decoded (an XOR delta against its plain base). False, with
// `why` set when given, if neither exists or the container is not whole and valid. tools\dump_io.py is
// the reader for Python and does the same.
bool readDecoded(const wchar_t* plainPath, std::string& out, std::string* why = nullptr) noexcept;

// The path a plain-named dump file really has on disk: NAME.z if that exists, else NAME. For log lines.
std::wstring onDisk(const wchar_t* plainPath);

// Where the writers' own log lines (a fallback to plain, a failure) go. Null until set, so a rig that
// links this file needs no logger; the graphics DLL points it at Log::get().note (dump_file_log.cpp).
typedef void (*LogSink)(const char* line);
void setLogSink(LogSink sink) noexcept;

// Test hooks for tools\dump_file_test: make the Nth chunk compress (0-based,
// counted per process from the call) fail, so the plain fallback runs. -1 = off.
void testFailChunk(int64_t chunkIndex) noexcept;
// Test hook: force the thread pool off (everything compresses on the caller).
void testNoThreads(bool off) noexcept;

}  // namespace dump_file
