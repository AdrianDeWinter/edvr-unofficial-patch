// Reading a zip and inflating its entries, with no code but our own.
//
// The launcher unpacks the release's zips into its file cache. The bytes come
// from the network, so every length in the archive is checked against the
// archive's real size before it is used, nothing is ever written to a path
// named inside a zip (a caller asks for an entry by the exact name its signed
// manifest lists and gets the bytes back), and the output of an entry is cut
// off at the size the manifest promised, so a zip bomb cannot fill memory.
//
// Supported: the central directory, stored entries (method 0) and deflate
// (method 8, stored, fixed and dynamic blocks). Refused: zip64, encryption,
// archives spread over several disks, any other method.
#pragma once

#include <string>
#include <vector>

#include <windows.h>

namespace edvr::installer {

// Which kinds of deflate block an inflate run met; the rig uses it to prove
// that its fixtures reached all three decoders.
struct InflateStats {
    int storedBlocks = 0;
    int fixedBlocks = 0;
    int dynamicBlocks = 0;
};

// Inflates the raw RFC 1951 stream `in` into exactly `outSize` bytes at `out`.
// Fails, without writing past `out + outSize`, when the stream is corrupt, ends
// early, produces more or fewer bytes than `outSize`, or refers back past the
// start. `consumed` (may be null) gets the number of input bytes used.
bool inflateRaw(const unsigned char* in, size_t inSize, unsigned char* out, size_t outSize,
                size_t* consumed, InflateStats* stats, std::string* error);

unsigned long crc32Of(const unsigned char* data, size_t size);

class ZipReader {
public:
    ZipReader() = default;
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    // Reads the central directory of a zip held in memory the caller keeps alive.
    bool openMemory(const unsigned char* data, size_t size, std::string* error);
    // Maps a zip file read-only and reads its central directory.
    bool openFile(const std::wstring& path, std::string* error);
    void close();

    // True when exactly one entry has this name (byte for byte).
    bool has(const std::string& entryName) const;
    size_t entryCount() const { return entries_.size(); }

    // The entry's bytes. `expectedSize` must equal the size the archive records,
    // and the output never grows past it. The CRC-32 is checked.
    bool extract(const std::string& entryName, unsigned long long expectedSize,
                 std::vector<unsigned char>* out, std::string* error, InflateStats* stats = nullptr) const;

private:
    struct Entry {
        std::string name;
        unsigned flags = 0;
        unsigned method = 0;
        unsigned long crc = 0;
        unsigned long long compressedSize = 0;
        unsigned long long uncompressedSize = 0;
        unsigned long long localOffset = 0;
    };

    bool parse(std::string* error);

    const unsigned char* data_ = nullptr;
    size_t size_ = 0;
    std::vector<Entry> entries_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const void* view_ = nullptr;
};

}  // namespace edvr::installer
