#include "update_http.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "launcher_version.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

struct ParsedUrl {
    bool https = true;
    std::string host;
    unsigned short port = 443;
    std::string target;  // path and query, starting with "/"
};

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool startsWithNoCase(const std::string& text, const char* prefix) {
    const std::string p(prefix);
    return text.size() >= p.size() && lower(text.substr(0, p.size())) == p;
}

// The address as the network layer will use it. Anything unusual is refused:
// user information, a bracketed host, a space or control character, a port that
// is not 1 to 65535.
bool parseUrl(const std::string& url, ParsedUrl* out) {
    size_t i;
    if (startsWithNoCase(url, "https://")) {
        out->https = true;
        i = 8;
    } else if (startsWithNoCase(url, "http://")) {
        out->https = false;
        i = 7;
    } else {
        return false;
    }
    for (char c : url) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f) return false;
    }
    const size_t authorityEnd = url.find_first_of("/?#", i);
    const std::string authority = url.substr(i, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - i);
    if (authority.empty() || authority.find('@') != std::string::npos || authority.find('[') != std::string::npos)
        return false;
    std::string host = authority;
    unsigned long port = out->https ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string digits = authority.substr(colon + 1);
        if (digits.empty() || digits.size() > 5) return false;
        for (char c : digits) {
            if (c < '0' || c > '9') return false;
        }
        port = std::strtoul(digits.c_str(), nullptr, 10);
        if (port < 1 || port > 65535) return false;
    }
    if (host.empty()) return false;
    for (char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) return false;
    }
    out->host = lower(host);
    out->port = static_cast<unsigned short>(port);
    std::string target = authorityEnd == std::string::npos ? std::string() : url.substr(authorityEnd);
    const size_t hash = target.find('#');
    if (hash != std::string::npos) target.erase(hash);
    if (target.empty()) target = "/";
    else if (target[0] == '?') target = "/" + target;
    out->target = target;
    return true;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string queryString(HINTERNET request, DWORD level) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, level, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return std::string();
    std::wstring wide(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, level, WINHTTP_HEADER_NAME_BY_INDEX, &wide[0], &bytes,
                             WINHTTP_NO_HEADER_INDEX))
        return std::string();
    wide.resize(bytes / sizeof(wchar_t));
    return narrowUtf8(wide);
}

std::string failureText(const char* what) {
    return std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

bool isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

}  // namespace

bool urlAllowed(const std::string& url, bool allowLoopbackHttp, std::string* error) {
    ParsedUrl parsed;
    if (!parseUrl(url, &parsed)) {
        if (error) *error = "the address is not a usable http or https address";
        return false;
    }
    if (parsed.https) return true;
    if (allowLoopbackHttp && parsed.host == "127.0.0.1") return true;
    if (error) *error = "plain http is refused; only https is used";
    return false;
}

bool redirectAllowed(const std::string& from, const std::string& to, bool allowLoopbackHttp, std::string* error) {
    if (!urlAllowed(to, allowLoopbackHttp, error)) return false;
    ParsedUrl a, b;
    if (parseUrl(from, &a) && parseUrl(to, &b) && a.https && !b.https) {
        if (error) *error = "a redirect from https to http is refused";
        return false;
    }
    return true;
}

std::string resolveLocation(const std::string& base, const std::string& location) {
    if (startsWithNoCase(location, "https://") || startsWithNoCase(location, "http://")) return location;
    const size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return location;
    const std::string scheme = base.substr(0, schemeEnd);
    const size_t authorityEnd = base.find_first_of("/?#", schemeEnd + 3);
    const std::string origin = base.substr(0, authorityEnd);  // scheme://authority
    if (location.compare(0, 2, "//") == 0) return scheme + ":" + location;
    if (!location.empty() && location[0] == '/') return origin + location;
    // Relative: the base path up to its last slash, then the location.
    std::string path = authorityEnd == std::string::npos ? std::string("/") : base.substr(authorityEnd);
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path.erase(cut);
    if (!location.empty() && location[0] == '?') return origin + path + location;
    const size_t slash = path.rfind('/');
    return origin + path.substr(0, slash + 1) + location;
}

WinHttpClient::WinHttpClient(bool allowLoopbackHttp, const std::string& userAgentVersion)
    : allowLoopbackHttp_(allowLoopbackHttp) {
    const std::string version = userAgentVersion.empty() ? std::string(EDUP_LAUNCHER_VERSION) : userAgentVersion;
    const std::wstring agent = widenUtf8("ed-up-launcher/" + version);
    // The rig talks to 127.0.0.1 and must not be sent through whatever proxy the
    // machine has; the product asks Windows for the automatic proxy settings and
    // falls back to the default when that is not available.
    if (allowLoopbackHttp_) {
        session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {
            session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }
    if (session_) {
        // resolve, connect, send, receive
        WinHttpSetTimeouts(static_cast<HINTERNET>(session_), 15000, 15000, 15000, 30000);
    }
}

WinHttpClient::~WinHttpClient() {
    if (session_) WinHttpCloseHandle(static_cast<HINTERNET>(session_));
}

HttpResponse WinHttpClient::get(const HttpRequest& request) {
    HttpResponse result;
    auto cancelled = [&]() { return request.cancel && request.cancel->load(); };
    auto stop = [&](const std::string& error) {
        result.transportOk = false;
        result.error = error;
        return result;
    };
    if (!session_) return stop("the network stack could not be started");
    std::string policyError;
    if (!urlAllowed(request.url, allowLoopbackHttp_, &policyError)) return stop(policyError);
    const unsigned long long limit = request.maxBytes ? request.maxBytes : kDefaultMaxBytes;

    if (request.method != "GET" && request.method != "POST") return stop("the request method is not supported");
    const bool isPost = request.method == "POST";
    const bool follow = request.followRedirects && !isPost;
    const std::wstring verb = isPost ? L"POST" : L"GET";

    std::wstring headerText;
    for (const auto& header : request.headers) headerText += widenUtf8(header.first + ": " + header.second + "\r\n");
    if (isPost) headerText += L"Content-Length: 0\r\n";

    std::string url = request.url;
    for (int hop = 0; hop <= 10; ++hop) {
        if (cancelled()) {
            result.cancelled = true;
            return stop("cancelled");
        }
        ParsedUrl parsed;
        if (!parseUrl(url, &parsed)) return stop("the address is not usable");
        Handle connection(WinHttpConnect(static_cast<HINTERNET>(session_), widenUtf8(parsed.host).c_str(),
                                         parsed.port, 0));
        if (!connection.h) return stop(failureText("could not connect"));
        Handle req(WinHttpOpenRequest(connection.h, verb.c_str(), widenUtf8(parsed.target).c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parsed.https ? WINHTTP_FLAG_SECURE : 0));
        if (!req.h) return stop(failureText("could not start the request"));
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
        const BOOL sent = WinHttpSendRequest(req.h, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                                             headerText.empty() ? 0 : static_cast<DWORD>(-1L),
                                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sent) return stop(failureText("the request could not be sent"));
        if (!WinHttpReceiveResponse(req.h, nullptr)) return stop(failureText("no reply was received"));

        DWORD status = 0;
        DWORD statusBytes = sizeof(status);
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX))
            return stop(failureText("the reply has no status"));

        if (isRedirect(static_cast<int>(status)) && !follow) {
            result.status = static_cast<int>(status);
            result.transportOk = true;
            return result;
        }
        if (isRedirect(static_cast<int>(status))) {
            const std::string location = queryString(req.h, WINHTTP_QUERY_LOCATION);
            if (location.empty()) return stop("a redirect named no place");
            const std::string next = resolveLocation(url, location);
            std::string why;
            if (!redirectAllowed(url, next, allowLoopbackHttp_, &why)) return stop(why);
            url = next;
            continue;
        }

        result.status = static_cast<int>(status);
        result.etag = queryString(req.h, WINHTTP_QUERY_ETAG);
        if (status != 200) {
            result.transportOk = true;
            return result;
        }

        unsigned long long contentLength = 0;
        const std::string lengthText = queryString(req.h, WINHTTP_QUERY_CONTENT_LENGTH);
        if (!lengthText.empty()) contentLength = std::strtoull(lengthText.c_str(), nullptr, 10);
        if (contentLength > limit) return stop("the reply is larger than allowed");

        HANDLE file = INVALID_HANDLE_VALUE;
        if (!request.toFile.empty()) {
            file = CreateFileW(request.toFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return stop("the download file cannot be created");
        }
        auto abandon = [&](const std::string& error) {
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
                file = INVALID_HANDLE_VALUE;
                DeleteFileW(request.toFile.c_str());
            }
            result.body.clear();
            return stop(error);
        };

        if (request.progress) request.progress(0, contentLength);
        std::vector<char> chunk(64 * 1024);
        unsigned long long total = 0;
        for (;;) {
            if (cancelled()) {
                result.cancelled = true;
                return abandon("cancelled");
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req.h, &available)) return abandon(failureText("the reply broke off"));
            if (available == 0) break;
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
            DWORD got = 0;
            if (!WinHttpReadData(req.h, chunk.data(), want, &got)) return abandon(failureText("the reply broke off"));
            if (got == 0) break;
            total += got;
            if (total > limit) return abandon("the reply is larger than allowed");
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                if (!WriteFile(file, chunk.data(), got, &written, nullptr) || written != got)
                    return abandon("the download file cannot be written");
            } else {
                result.body.append(chunk.data(), got);
            }
            if (request.progress) request.progress(total, contentLength);
        }
        if (contentLength != 0 && total < contentLength) return abandon("the reply was cut short");
        if (file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(file);
            CloseHandle(file);
        }
        result.bytes = total;
        result.transportOk = true;
        return result;
    }
    return stop("too many redirects");
}

}  // namespace edvr::installer
