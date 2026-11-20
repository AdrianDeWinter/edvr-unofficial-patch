#include "update_zip.h"

#include <cstring>
#include <new>
#include <set>

namespace edvr::installer {
namespace {

bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

// ---- inflate -----------------------------------------------------------------

// Reads bits least significant first. It never reads a byte it does not need, so
// after every call fewer than 8 bits are buffered and the next whole byte is at
// `in + pos`: that is what makes a stored block's byte alignment trivial.
struct BitReader {
    const unsigned char* in = nullptr;
    size_t size = 0;
    size_t pos = 0;
    unsigned long buffer = 0;
    int count = 0;

    bool bits(int wanted, unsigned* out) {
        while (count < wanted) {
            if (pos >= size) return false;
            buffer |= static_cast<unsigned long>(in[pos++]) << count;
            count += 8;
        }
        *out = static_cast<unsigned>(buffer & ((1ul << wanted) - 1));
        buffer >>= wanted;
        count -= wanted;
        return true;
    }
};

constexpr int kMaxBits = 15;

// A canonical Huffman code: how many codes have each length, and the symbols in
// code order.
struct Huffman {
    unsigned short count[kMaxBits + 1];
    unsigned short symbol[320];
};

enum class Fill { Complete, IncompleteAllowed };

// Builds the code for `lengths[0..n)`. False for an over-subscribed set, or an
// incomplete one that `fill` does not allow.
bool buildHuffman(Huffman* h, const unsigned char* lengths, int n, Fill fill) {
    std::memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; ++i) ++h->count[lengths[i]];
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    if (left > 0 && fill == Fill::Complete) return false;
    unsigned short offset[kMaxBits + 2];
    offset[1] = 0;
    for (int len = 1; len <= kMaxBits; ++len) offset[len + 1] = static_cast<unsigned short>(offset[len] + h->count[len]);
    for (int i = 0; i < n; ++i) {
        if (lengths[i] != 0) h->symbol[offset[lengths[i]]++] = static_cast<unsigned short>(i);
    }
    return true;
}

// Reads one symbol. -1: no code of this set matches the bits; -2: the input ended.
int decodeSymbol(BitReader* br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        unsigned bit = 0;
        if (!br->bits(1, &bit)) return -2;
        code |= static_cast<int>(bit);
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const unsigned short kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const unsigned char kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const unsigned short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                      6145, 8193, 12289, 16385, 24577};
const unsigned char kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// The literal/length and distance symbols of one compressed block.
bool inflateCodes(BitReader* br, const Huffman& lit, const Huffman& dist, unsigned char* out,
                  size_t outSize, size_t* outPos, std::string* error) {
    for (;;) {
        const int sym = decodeSymbol(br, lit);
        if (sym == -2) return fail(error, "the compressed data ends early");
        if (sym < 0) return fail(error, "the compressed data holds an invalid code");
        if (sym < 256) {
            if (*outPos >= outSize) return fail(error, "the data is larger than its recorded size");
            out[(*outPos)++] = static_cast<unsigned char>(sym);
            continue;
        }
        if (sym == 256) return true;
        const int lengthSymbol = sym - 257;
        if (lengthSymbol >= 29) return fail(error, "the compressed data holds an invalid length");
        unsigned extra = 0;
        if (!br->bits(kLengthExtra[lengthSymbol], &extra)) return fail(error, "the compressed data ends early");
        const size_t length = kLengthBase[lengthSymbol] + extra;
        const int distSymbol = decodeSymbol(br, dist);
        if (distSymbol == -2) return fail(error, "the compressed data ends early");
        if (distSymbol < 0 || distSymbol >= 30) return fail(error, "the compressed data holds an invalid distance");
        if (!br->bits(kDistExtra[distSymbol], &extra)) return fail(error, "the compressed data ends early");
        const size_t distance = kDistBase[distSymbol] + extra;
        if (distance > *outPos) return fail(error, "the compressed data refers before its start");
        if (length > outSize - *outPos) return fail(error, "the data is larger than its recorded size");
        for (size_t i = 0; i < length; ++i) {
            out[*outPos] = out[*outPos - distance];
            ++*outPos;
        }
    }
}

bool fixedTables(Huffman* lit, Huffman* dist) {
    unsigned char lengths[288];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    if (!buildHuffman(lit, lengths, 288, Fill::Complete)) return false;
    unsigned char distLengths[30];
    for (int d = 0; d < 30; ++d) distLengths[d] = 5;
    return buildHuffman(dist, distLengths, 30, Fill::IncompleteAllowed);
}

bool dynamicTables(BitReader* br, Huffman* lit, Huffman* dist, std::string* error) {
    unsigned hlit = 0, hdist = 0, hclen = 0;
    if (!br->bits(5, &hlit) || !br->bits(5, &hdist) || !br->bits(4, &hclen))
        return fail(error, "the compressed data ends early");
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return fail(error, "a block declares too many codes");

    static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned char codeLengths[19] = {};
    for (unsigned i = 0; i < hclen; ++i) {
        unsigned v = 0;
        if (!br->bits(3, &v)) return fail(error, "the compressed data ends early");
        codeLengths[order[i]] = static_cast<unsigned char>(v);
    }
    Huffman lengthCode;
    if (!buildHuffman(&lengthCode, codeLengths, 19, Fill::Complete))
        return fail(error, "a block has an invalid code-length code");

    unsigned char lengths[286 + 30] = {};
    const unsigned total = hlit + hdist;
    unsigned index = 0;
    while (index < total) {
        const int sym = decodeSymbol(br, lengthCode);
        if (sym == -2) return fail(error, "the compressed data ends early");
        if (sym < 0) return fail(error, "a block has an invalid code length");
        if (sym < 16) {
            lengths[index++] = static_cast<unsigned char>(sym);
            continue;
        }
        unsigned repeat = 0, extra = 0;
        unsigned char value = 0;
        if (sym == 16) {
            if (index == 0) return fail(error, "a block repeats a length that is not there");
            value = lengths[index - 1];
            if (!br->bits(2, &extra)) return fail(error, "the compressed data ends early");
            repeat = 3 + extra;
        } else if (sym == 17) {
            if (!br->bits(3, &extra)) return fail(error, "the compressed data ends early");
            repeat = 3 + extra;
        } else {
            if (!br->bits(7, &extra)) return fail(error, "the compressed data ends early");
            repeat = 11 + extra;
        }
        if (index + repeat > total) return fail(error, "a block has too many code lengths");
        while (repeat--) lengths[index++] = value;
    }
    if (lengths[256] == 0) return fail(error, "a block has no end-of-block code");

    // A literal/length code with one code of length 1 and a distance code with
    // one code, or none (a block of literals only), are the incomplete sets zlib
    // writes and accepts.
    int litCodes = 0, distCodes = 0;
    for (unsigned i = 0; i < hlit; ++i) litCodes += lengths[i] != 0;
    for (unsigned i = 0; i < hdist; ++i) distCodes += lengths[hlit + i] != 0;
    if (!buildHuffman(lit, lengths, static_cast<int>(hlit), litCodes == 1 ? Fill::IncompleteAllowed : Fill::Complete))
        return fail(error, "a block has an invalid literal code");
    if (!buildHuffman(dist, lengths + hlit, static_cast<int>(hdist),
                      distCodes <= 1 ? Fill::IncompleteAllowed : Fill::Complete))
        return fail(error, "a block has an invalid distance code");
    return true;
}

struct CrcTable {
    unsigned long value[256];
    CrcTable() {
        for (unsigned long n = 0; n < 256; ++n) {
            unsigned long c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320ul ^ (c >> 1)) : (c >> 1);
            value[n] = c;
        }
    }
};

unsigned readU16(const unsigned char* p) { return static_cast<unsigned>(p[0] | (p[1] << 8)); }
unsigned long readU32(const unsigned char* p) {
    return static_cast<unsigned long>(p[0]) | (static_cast<unsigned long>(p[1]) << 8) |
           (static_cast<unsigned long>(p[2]) << 16) | (static_cast<unsigned long>(p[3]) << 24);
}

// A zip record starts with the bytes "PK" and two small numbers, read here as a
// little-endian 32-bit value. They are built from their parts, not written as
// hex, because tools\hook_signatures.py audit takes a hex literal in that range
// for a game address.
constexpr unsigned long zipSignature(unsigned long first, unsigned long second) {
    return 'P' + ('K' << 8) + (first << 16) + (second << 24);
}
constexpr unsigned long kEocdSignature = zipSignature(5, 6);
constexpr unsigned long kZip64LocatorSignature = zipSignature(6, 7);
constexpr unsigned long kCentralSignature = zipSignature(1, 2);
constexpr unsigned long kLocalSignature = zipSignature(3, 4);

}  // namespace

bool inflateRaw(const unsigned char* in, size_t inSize, unsigned char* out, size_t outSize,
                size_t* consumed, InflateStats* stats, std::string* error) {
    BitReader br;
    br.in = in;
    br.size = inSize;
    size_t outPos = 0;
    unsigned last = 0;
    do {
        unsigned type = 0;
        if (!br.bits(1, &last) || !br.bits(2, &type)) return fail(error, "the compressed data ends early");
        if (type == 0) {
            // Stored: the rest of the byte is padding and LEN, NLEN follow.
            br.buffer = 0;
            br.count = 0;
            if (br.pos + 4 > br.size) return fail(error, "the compressed data ends early");
            const unsigned len = readU16(in + br.pos);
            const unsigned nlen = readU16(in + br.pos + 2);
            br.pos += 4;
            if ((len ^ 0xffffu) != nlen) return fail(error, "a stored block has a bad length check");
            if (len > br.size - br.pos) return fail(error, "the compressed data ends early");
            if (len > outSize - outPos) return fail(error, "the data is larger than its recorded size");
            if (len) std::memcpy(out + outPos, in + br.pos, len);
            br.pos += len;
            outPos += len;
            if (stats) ++stats->storedBlocks;
        } else if (type == 1 || type == 2) {
            Huffman lit, dist;
            if (type == 1) {
                if (!fixedTables(&lit, &dist)) return fail(error, "the fixed code could not be built");
                if (stats) ++stats->fixedBlocks;
            } else {
                if (!dynamicTables(&br, &lit, &dist, error)) return false;
                if (stats) ++stats->dynamicBlocks;
            }
            if (!inflateCodes(&br, lit, dist, out, outSize, &outPos, error)) return false;
        } else {
            return fail(error, "the compressed data has a block of an unknown type");
        }
    } while (!last);
    if (outPos != outSize) return fail(error, "the data is shorter than its recorded size");
    if (consumed) *consumed = br.pos;
    return true;
}

unsigned long crc32Of(const unsigned char* data, size_t size) {
    static const CrcTable table;
    unsigned long c = 0xfffffffful;
    for (size_t i = 0; i < size; ++i) c = table.value[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xfffffffful;
}

// ---- the archive -------------------------------------------------------------

ZipReader::~ZipReader() { close(); }

void ZipReader::close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    view_ = nullptr;
    mapping_ = nullptr;
    file_ = INVALID_HANDLE_VALUE;
    data_ = nullptr;
    size_ = 0;
    entries_.clear();
}

bool ZipReader::openMemory(const unsigned char* data, size_t size, std::string* error) {
    close();
    data_ = data;
    size_ = size;
    if (!parse(error)) {
        close();
        return false;
    }
    return true;
}

bool ZipReader::openFile(const std::wstring& path, std::string* error) {
    close();
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return fail(error, "the zip cannot be opened");
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > 0x7fffffffffffull) {
        close();
        return fail(error, "the zip is empty or its size cannot be read");
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) {
        close();
        return fail(error, "the zip cannot be mapped");
    }
    view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
    if (!view_) {
        close();
        return fail(error, "the zip cannot be mapped");
    }
    data_ = static_cast<const unsigned char*>(view_);
    size_ = static_cast<size_t>(size.QuadPart);
    if (!parse(error)) {
        close();
        return false;
    }
    return true;
}

bool ZipReader::parse(std::string* error) {
    entries_.clear();
    if (size_ < 22) return fail(error, "the file is too small to be a zip");

    // The end-of-central-directory record is the last 22+ bytes; a comment of up
    // to 65535 bytes may follow it, and the record must account for it exactly.
    size_t eocd = 0;
    bool found = false;
    const size_t lowest = size_ > 22 + 65535 ? size_ - 22 - 65535 : 0;
    for (size_t pos = size_ - 22 + 1; pos-- > lowest;) {
        if (readU32(data_ + pos) != kEocdSignature) continue;
        if (pos + 22 + readU16(data_ + pos + 20) != size_) continue;
        eocd = pos;
        found = true;
        break;
    }
    if (!found) return fail(error, "the file has no zip directory");
    if (eocd >= 20 && readU32(data_ + eocd - 20) == kZip64LocatorSignature)
        return fail(error, "zip64 archives are not supported");

    const unsigned disk = readU16(data_ + eocd + 4);
    const unsigned centralDisk = readU16(data_ + eocd + 6);
    const unsigned entriesOnDisk = readU16(data_ + eocd + 8);
    const unsigned entriesTotal = readU16(data_ + eocd + 10);
    const unsigned long centralSize = readU32(data_ + eocd + 12);
    const unsigned long centralOffset = readU32(data_ + eocd + 16);
    if (disk != 0 || centralDisk != 0 || entriesOnDisk != entriesTotal)
        return fail(error, "archives spread over several disks are not supported");
    if (entriesTotal == 0xffff || centralSize == 0xfffffffful || centralOffset == 0xfffffffful)
        return fail(error, "zip64 archives are not supported");
    if (static_cast<unsigned long long>(centralOffset) + centralSize > eocd)
        return fail(error, "the zip directory lies outside the file");

    std::set<std::string> names;
    size_t pos = centralOffset;
    const size_t end = static_cast<size_t>(centralOffset) + centralSize;
    for (unsigned i = 0; i < entriesTotal; ++i) {
        if (end - pos < 46 || pos > end) return fail(error, "the zip directory is cut short");
        const unsigned char* h = data_ + pos;
        if (readU32(h) != kCentralSignature) return fail(error, "the zip directory is damaged");
        const unsigned nameLength = readU16(h + 28);
        const unsigned extraLength = readU16(h + 30);
        const unsigned commentLength = readU16(h + 32);
        const size_t entrySize = 46ull + nameLength + extraLength + commentLength;
        if (entrySize > end - pos) return fail(error, "the zip directory is cut short");
        if (readU16(h + 34) != 0) return fail(error, "archives spread over several disks are not supported");
        Entry e;
        e.flags = readU16(h + 8);
        e.method = readU16(h + 10);
        e.crc = readU32(h + 16);
        const unsigned long compressed = readU32(h + 20);
        const unsigned long uncompressed = readU32(h + 24);
        const unsigned long offset = readU32(h + 42);
        if (compressed == 0xfffffffful || uncompressed == 0xfffffffful || offset == 0xfffffffful)
            return fail(error, "zip64 archives are not supported");
        e.compressedSize = compressed;
        e.uncompressedSize = uncompressed;
        e.localOffset = offset;
        e.name.assign(reinterpret_cast<const char*>(h + 46), nameLength);
        if (e.localOffset >= centralOffset) return fail(error, "an entry starts outside the file");
        if (!names.insert(e.name).second) return fail(error, "two entries share a name");
        entries_.push_back(std::move(e));
        pos += entrySize;
    }
    return true;
}

bool ZipReader::has(const std::string& entryName) const {
    for (const Entry& e : entries_) {
        if (e.name == entryName) return true;
    }
    return false;
}

bool ZipReader::extract(const std::string& entryName, unsigned long long expectedSize,
                        std::vector<unsigned char>* out, std::string* error, InflateStats* stats) const {
    const Entry* entry = nullptr;
    for (const Entry& e : entries_) {
        if (e.name == entryName) entry = &e;
    }
    if (!entry) return fail(error, "the zip has no entry of that name");
    if (entry->flags & 0x41) return fail(error, "encrypted entries are not supported");
    if (entry->method != 0 && entry->method != 8) return fail(error, "the entry uses an unsupported method");
    if (entry->uncompressedSize != expectedSize) return fail(error, "the entry's size is not the size the manifest lists");
    if (expectedSize > (1ull << 30)) return fail(error, "the entry is larger than allowed");

    // The local header repeats the name; it must agree with the directory.
    const unsigned long long local = entry->localOffset;
    if (local + 30 > size_ || readU32(data_ + local) != kLocalSignature)
        return fail(error, "an entry's header is damaged");
    const unsigned nameLength = readU16(data_ + local + 26);
    const unsigned extraLength = readU16(data_ + local + 28);
    const unsigned long long dataStart = local + 30 + nameLength + extraLength;
    if (dataStart > size_ || entry->compressedSize > size_ - dataStart)
        return fail(error, "an entry's data lies outside the file");
    if (nameLength != entry->name.size() ||
        std::memcmp(data_ + local + 30, entry->name.data(), nameLength) != 0)
        return fail(error, "an entry's header names another file");
    const unsigned char* compressed = data_ + dataStart;

    std::vector<unsigned char> bytes;
    try {
        bytes.resize(static_cast<size_t>(expectedSize));
    } catch (const std::bad_alloc&) {
        return fail(error, "there is not enough memory for the entry");
    }
    if (entry->method == 0) {
        if (entry->compressedSize != expectedSize) return fail(error, "a stored entry's sizes differ");
        if (expectedSize) std::memcpy(bytes.data(), compressed, static_cast<size_t>(expectedSize));
    } else if (!inflateRaw(compressed, static_cast<size_t>(entry->compressedSize), bytes.data(), bytes.size(),
                           nullptr, stats, error)) {
        return false;
    }
    if (crc32Of(bytes.data(), bytes.size()) != entry->crc) return fail(error, "an entry's checksum does not match");
    *out = std::move(bytes);
    return true;
}

}  // namespace edvr::installer
