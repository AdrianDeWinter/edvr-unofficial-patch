#include "update_manifest.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include "update_json.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

// ---- the signature -----------------------------------------------------------

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decodeHex(const char* text, size_t digits, unsigned char* out) {
    for (size_t i = 0; i + 1 < digits; i += 2) {
        const int hi = hexDigit(text[i]);
        const int lo = hexDigit(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i / 2] = static_cast<unsigned char>(hi * 16 + lo);
    }
    return true;
}

// The file's 128 characters, or false for anything else: wrong length, an upper
// case or non-hex character, whitespace in front, a second line ending.
bool parseSignatureText(const std::string& text, unsigned char sig[64]) {
    size_t length = text.size();
    if (length == 129 && text[128] == '\n') length = 128;
    else if (length == 130 && text[128] == '\r' && text[129] == '\n') length = 128;
    if (length != 128) return false;
    for (size_t i = 0; i < 128; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return decodeHex(text.data(), 128, sig);
}

bool sha256Raw(const std::string& bytes, unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    const NTSTATUS status = BCryptHash(alg, nullptr, 0,
                                       reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                       static_cast<ULONG>(bytes.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return status == 0;
}

// True when `sig` verifies over `digest` under one public key (128 hex characters).
bool verifyWithKey(const char* keyHex, const unsigned char digest[32], const unsigned char sig[64]) {
    if (!keyHex || std::strlen(keyHex) != 128) return false;
    struct {
        BCRYPT_ECCKEY_BLOB header;
        unsigned char xy[64];
    } blob = {};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    if (!decodeHex(keyHex, 128, blob.xy)) return false;

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, reinterpret_cast<PUCHAR>(&blob),
                            sizeof(blob), 0) == 0) {
        ok = BCryptVerifySignature(key, nullptr, const_cast<PUCHAR>(digest), 32,
                                   const_cast<PUCHAR>(sig), 64, 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- the schema --------------------------------------------------------------

bool failWith(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

// ---- versions ----------------------------------------------------------------

struct Version {
    std::vector<unsigned long long> core;
    std::vector<std::string> pre;  // empty for a release
    bool prerelease = false;
};

Version parseVersion(const std::string& text) {
    Version v;
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    const size_t dash = text.find('-', i);
    const std::string core = text.substr(i, dash == std::string::npos ? std::string::npos : dash - i);
    size_t pos = 0;
    while (pos <= core.size()) {
        size_t dot = core.find('.', pos);
        if (dot == std::string::npos) dot = core.size();
        unsigned long long number = 0;
        for (size_t k = pos; k < dot && core[k] >= '0' && core[k] <= '9'; ++k) {
            if (number > 0x0fffffffffffffffull) break;
            number = number * 10 + static_cast<unsigned long long>(core[k] - '0');
        }
        v.core.push_back(number);
        if (dot == core.size()) break;
        pos = dot + 1;
    }
    if (dash != std::string::npos) {
        v.prerelease = true;
        const std::string pre = text.substr(dash + 1);
        size_t start = 0;
        while (start <= pre.size()) {
            size_t dot = pre.find('.', start);
            if (dot == std::string::npos) dot = pre.size();
            v.pre.push_back(pre.substr(start, dot - start));
            if (dot == pre.size()) break;
            start = dot + 1;
        }
    }
    return v;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

int compareIdentifier(const std::string& a, const std::string& b) {
    const bool an = allDigits(a), bn = allDigits(b);
    if (an && bn) {
        // Equal-width digit strings compare as text; otherwise the longer is larger.
        const std::string x = a.substr(a.find_first_not_of('0') == std::string::npos ? a.size() : a.find_first_not_of('0'));
        const std::string y = b.substr(b.find_first_not_of('0') == std::string::npos ? b.size() : b.find_first_not_of('0'));
        if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
        const int c = x.compare(y);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (an) return -1;  // a number sorts below a word
    if (bn) return 1;
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

}  // namespace

bool loadVerifiedManifest(const std::string& bytes, const std::string& sigText, const char* const* keys,
                          Manifest* out, std::string* error, ManifestFailure* failure) {
    if (failure) *failure = ManifestFailure();
    if (bytes.size() > kMaxManifestBytes) return failWith(error, "the manifest is larger than allowed");
    unsigned char sig[64];
    if (!parseSignatureText(sigText, sig))
        return failWith(error, "the signature file is not 128 lower-case hex characters");
    unsigned char digest[32];
    if (!sha256Raw(bytes, digest)) return failWith(error, "the manifest could not be hashed");
    bool verified = false;
    for (const char* const* key = keys; key && *key && !verified; ++key) {
        verified = verifyWithKey(*key, digest, sig);
    }
    if (!verified) return failWith(error, "the signature does not match a trusted key");

    // Only now is anything parsed.
    for (unsigned char c : bytes) {
        if (c >= 0x80) return failWith(error, "the manifest is not pure ASCII");
    }
    JsonValue root;
    std::string parseError;
    if (!parseJson(bytes, &root, &parseError)) return failWith(error, "the manifest is not valid JSON: " + parseError);
    Manifest manifest;
    long long schemaSeen = 0;
    const ManifestParse parsed = parseManifest(root, &manifest, error, &schemaSeen);
    if (parsed == ManifestParse::SchemaTooNew && failure) {
        failure->schemaTooNew = true;
        failure->schema = schemaSeen;
    }
    if (parsed != ManifestParse::Ok) return false;
    *out = std::move(manifest);
    return true;
}

int compareVersions(const std::string& a, const std::string& b) {
    const Version x = parseVersion(a), y = parseVersion(b);
    const size_t n = x.core.size() > y.core.size() ? x.core.size() : y.core.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned long long l = i < x.core.size() ? x.core[i] : 0;
        const unsigned long long r = i < y.core.size() ? y.core[i] : 0;
        if (l != r) return l < r ? -1 : 1;
    }
    if (x.prerelease != y.prerelease) return x.prerelease ? -1 : 1;
    const size_t m = x.pre.size() < y.pre.size() ? x.pre.size() : y.pre.size();
    for (size_t i = 0; i < m; ++i) {
        const int c = compareIdentifier(x.pre[i], y.pre[i]);
        if (c != 0) return c;
    }
    if (x.pre.size() != y.pre.size()) return x.pre.size() < y.pre.size() ? -1 : 1;
    return 0;
}

}  // namespace edvr::installer
