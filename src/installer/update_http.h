// HTTP GET (and body-less POST) for the updater, behind an interface so the rig can script it.
//
// WinHttpClient is the one real implementation. It speaks HTTPS only: a plain
// http:// address is refused before any socket opens, with one exception for the
// test rig, a client built with allowLoopbackHttp that may use http to
// 127.0.0.1 and nowhere else. It follows redirects itself so that each hop can
// be judged: HTTPS to HTTP is refused, and so is any hop to a plain address the
// rule above does not allow. Requests carry the user agent
// "ed-up-launcher/<version>" and the headers the caller names, and nothing that
// identifies the machine or its owner.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace edvr::installer {

struct HttpRequest {
    // "GET" or "POST". A POST has no body and goes out with Content-Length: 0 (Frontier's sign-in calls
    // are POSTs with everything in the query), and a redirect to one is never followed, whatever
    // followRedirects says: a redirect would turn it into a GET somewhere else.
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    // Empty: the body is returned in HttpResponse::body. Otherwise a 200 body is
    // written to this file, which is removed again on any failure.
    std::wstring toFile;
    // The most body bytes accepted; more fails the request. 0 means the default (64 MiB).
    unsigned long long maxBytes = 0;
    // False: a 301, 302, 303, 307 or 308 is the answer, returned as a status with no body, and is
    // never followed. The Frontier calls use this, since a reply that sends a bearer token somewhere
    // else is not one to obey.
    bool followRedirects = true;
    // When set and true, the request stops at the next chance.
    const std::atomic<bool>* cancel = nullptr;
    // Called as body bytes arrive: bytes so far, and the total when the server said.
    std::function<void(unsigned long long, unsigned long long)> progress;
};

struct HttpResponse {
    bool transportOk = false;   // an HTTP reply was received; false is "no answer"
    bool cancelled = false;
    int status = 0;             // of the final hop
    std::string error;          // when !transportOk, a sentence without the address
    std::string body;           // a 200 body when the request had no toFile
    std::string etag;           // the ETag header, if any
    unsigned long long bytes = 0;  // body bytes received
};

inline constexpr unsigned long long kDefaultMaxBytes = 64ull << 20;

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse get(const HttpRequest& request) = 0;
};

// The address rules, public so they can be tested without a network.
// `url` may be used when it is https, or http to 127.0.0.1 with allowLoopbackHttp.
bool urlAllowed(const std::string& url, bool allowLoopbackHttp, std::string* error);
// A redirect from `from` to `to` may be followed when `to` is allowed and it is
// not a step from https down to http.
bool redirectAllowed(const std::string& from, const std::string& to, bool allowLoopbackHttp,
                     std::string* error);
// `location` as the server sent it (absolute, absolute-path or relative), made
// absolute against the address of the request that got it.
std::string resolveLocation(const std::string& base, const std::string& location);

class WinHttpClient : public HttpClient {
public:
    // `userAgentVersion` is the launcher's version, for the user agent.
    explicit WinHttpClient(bool allowLoopbackHttp = false, const std::string& userAgentVersion = std::string());
    ~WinHttpClient() override;
    WinHttpClient(const WinHttpClient&) = delete;
    WinHttpClient& operator=(const WinHttpClient&) = delete;

    HttpResponse get(const HttpRequest& request) override;

private:
    bool allowLoopbackHttp_;
    void* session_ = nullptr;  // HINTERNET
};

}  // namespace edvr::installer
