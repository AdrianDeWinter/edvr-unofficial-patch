// Ported from Api.fs of min-ed-launcher, commit 139e4ff7ab9027925648fb27767cb87acdcf290a.
// MIT licence, "Copyright (c) 2017 Chris": see third_party\min_ed_launcher\LICENSE.
#include "frontier_api.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "update_json.h"

namespace edvr::installer {
namespace {

long long systemUnixSeconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long ticks = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // 100 ns ticks since 1601; 11644473600 s separate that from 1970.
    return static_cast<long long>(ticks / 10000000ull) - 11644473600ll;
}

std::string percentEncode(const std::string& text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(raw);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool startsWith(const std::string& text, const char* prefix) { return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0; }

// What the status and the body say, before any call looks for its own fields. Ok means a 200 that is
// not the ownership page; the caller still has to find what it needs in the body.
ApiResult classify(const HttpResponse& reply, bool steam = true) {
    ApiResult result;
    result.httpStatus = reply.status;
    if (!reply.transportOk) {
        result.outcome = ApiOutcome::Unavailable;
        result.message = reply.cancelled ? "The sign-in was stopped" : "Frontier's servers could not be reached (" + reply.error + ")";
        return result;
    }
    const std::string code = "HTTP " + std::to_string(reply.status);
    if (reply.status == 200) {
        // Frontier answers with an HTML login page when it cannot verify ownership. It sends no
        // Content-Type, so the first bytes decide, as in Api.fs.
        if (startsWith(reply.body, "<!DOCTYPE html")) {
            result.outcome = ApiOutcome::OwnershipPage;
            result.message = steam ? "Frontier could not confirm that this account owns the game, which it does now and then"
                                   : "Frontier answered with a web page instead of a sign-in reply";
            return result;
        }
        result.outcome = ApiOutcome::Ok;
        return result;
    }
    if (reply.status == 302) {
        result.outcome = ApiOutcome::RegistrationRequired;
        result.message = steam ? "Frontier wants this Steam account registered or linked (" + code + ")"
                               : "Frontier answered with a redirect, which the launcher does not follow (" + code + ")";
    } else if (reply.status == 307) {
        result.outcome = ApiOutcome::LinkAvailable;
        result.message = steam ? "Frontier offers to link this Steam account to a Frontier account (" + code + ")"
                               : "Frontier answered with a redirect, which the launcher does not follow (" + code + ")";
    } else if (reply.status == 400 || reply.status == 401 || reply.status == 403) {
        result.outcome = ApiOutcome::Denied;
        result.message = "Frontier refused the request (" + code + ")";
    } else if (reply.status >= 500 && reply.status < 600) {
        result.outcome = ApiOutcome::Unavailable;
        result.message = "Frontier's servers are unavailable (" + code + ")";
    } else {
        result.outcome = ApiOutcome::Unexpected;
        result.message = "Frontier answered in a way the launcher does not know (" + code + ")";
    }
    return result;
}

ApiResult unexpected(int status, const char* what) {
    ApiResult result;
    result.outcome = ApiOutcome::Unexpected;
    result.httpStatus = status;
    result.message = what;
    return result;
}

// A string member, or null when it is missing or not a string.
const std::string* stringMember(const JsonValue& object, const char* key) {
    const JsonValue* value = object.find(key);
    return value && value->isString() ? &value->text : nullptr;
}

std::string textOf(const JsonValue& object, const char* key) {
    const JsonValue* value = object.findNoCase(key);
    return value && value->isString() ? value->text : std::string();
}

}  // namespace

FrontierApi::FrontierApi(HttpClient& http, std::string baseUrl, const std::atomic<bool>* cancel, ApiClock clock)
    : http_(http), base_(std::move(baseUrl)), cancel_(cancel), clock_(std::move(clock)) {
    if (!clock_.unixNow) clock_.unixNow = systemUnixSeconds;
    if (!clock_.tickMs) clock_.tickMs = []() { return static_cast<unsigned long long>(GetTickCount64()); };
    baseSeconds_ = clock_.unixNow();
    baseTick_ = clock_.tickMs();
}

std::string hexOfBytes(const std::string& text) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 2);
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

HttpResponse FrontierApi::send(const std::string& path, const std::vector<std::pair<std::string, std::string>>& query,
                               const std::string& bearer, bool post) {
    HttpRequest request;
    request.method = post ? "POST" : "GET";
    request.url = base_ + path;
    for (size_t i = 0; i < query.size(); ++i) {
        request.url += i == 0 ? '?' : '&';
        request.url += query[i].first + "=" + percentEncode(query[i].second);
    }
    if (!bearer.empty()) request.headers.emplace_back("Authorization", "bearer " + bearer);
    request.maxBytes = kFrontierMaxBytes;
    request.followRedirects = false;
    request.cancel = cancel_;
    return http_.get(request);
}

bool FrontierApi::syncTime() {
    // The local clock is read before the call, as createGetRunningTime does, so that a slow answer
    // does not make the server's time look later than it is.
    const long long local = clock_.unixNow();
    const unsigned long long tick = clock_.tickMs();
    baseSeconds_ = local;
    baseTick_ = tick;
    const HttpResponse reply = send("/1.1/server/time", {}, std::string());
    if (!reply.transportOk || reply.status != 200) return false;
    JsonValue json;
    if (!parseJson(reply.body, &json, nullptr)) return false;
    const JsonValue* stamp = json.find("unixTimestamp");
    if (!stamp || !stamp->isInt()) return false;
    baseSeconds_ = stamp->integer;
    return true;
}

std::string FrontierApi::fTime() const {
    // RunningTime.ToString in the source: fixed point, always five decimals.
    const unsigned long long elapsedMs = clock_.tickMs() - baseTick_;
    char text[64];
    std::snprintf(text, sizeof(text), "%.5f", static_cast<double>(baseSeconds_) + static_cast<double>(elapsedMs) / 1000.0);
    return text;
}

ApiResult FrontierApi::steamAuth(const std::string& machineId, const std::string& steamTicketHex, FrontierSession* session) {
    // steamTicket first, then the two every call carries: the order Api.fs builds them in.
    const HttpResponse reply =
        send("/3.0/user/steam/auth", {{"steamTicket", steamTicketHex}, {"machineId", machineId}, {"fTime", fTime()}}, std::string());
    ApiResult result = classify(reply);
    if (result.outcome != ApiOutcome::Ok) return result;
    JsonValue json;
    if (!parseJson(reply.body, &json, nullptr) || !json.isObject()) return unexpected(reply.status, "Frontier's sign-in reply was not in the form the launcher expects");
    const std::string* authToken = stringMember(json, "authToken");
    const std::string* machineToken = stringMember(json, "machineToken");
    if (!authToken || !machineToken || authToken->empty() || machineToken->empty())
        return unexpected(reply.status, "Frontier's sign-in reply carried no tokens");
    session->authToken = *authToken;
    session->machineToken = *machineToken;
    return result;
}

ApiResult FrontierApi::frontierFirstSignIn(const std::string& email, const std::string& password, const std::string& machineId,
                                           std::string* encCode) {
    const HttpResponse reply = send("/3.0/user/frontier/auth",
                                    {{"email", hexOfBytes(email)},
                                     {"password", hexOfBytes(password)},
                                     {"machineId", machineId},
                                     {"lang", kFrontierLang},
                                     {"fTime", fTime()}},
                                    std::string(), true);
    ApiResult result = classify(reply, false);
    if (result.outcome != ApiOutcome::Ok) return result;
    JsonValue json;
    if (!parseJson(reply.body, &json, nullptr) || !json.isObject()) return unexpected(reply.status, "Frontier's sign-in reply was not in the form the launcher expects");
    const std::string* found = stringMember(json, "encCode");
    if (!found || found->empty()) return unexpected(reply.status, "Frontier's sign-in reply carried no verification code");
    *encCode = *found;
    return result;
}

ApiResult FrontierApi::frontierMachineToken(const std::string& machineId, const std::string& plainCode, const std::string& encCode,
                                            std::string* machineToken) {
    const HttpResponse reply = send("/3.0/user/frontier/token",
                                    {{"machineId", machineId}, {"plainCode", plainCode}, {"encCode", encCode}, {"lang", kFrontierLang}},
                                    std::string(), true);
    ApiResult result = classify(reply, false);
    if (result.outcome != ApiOutcome::Ok) return result;
    JsonValue json;
    if (!parseJson(reply.body, &json, nullptr) || !json.isObject()) return unexpected(reply.status, "Frontier's reply to the code was not in the form the launcher expects");
    const std::string* found = stringMember(json, "machineToken");
    if (!found || found->empty()) return unexpected(reply.status, "Frontier's reply to the code carried no machine token");
    *machineToken = *found;
    return result;
}

ApiResult FrontierApi::frontierAuth(const std::string& email, const std::string& password, const std::string& machineId,
                                    const std::string& machineToken, FrontierSession* session) {
    const HttpResponse reply = send("/3.0/user/frontier/auth",
                                    {{"email", hexOfBytes(email)},
                                     {"password", hexOfBytes(password)},
                                     {"machineId", machineId},
                                     {"machineToken", machineToken},
                                     {"lang", kFrontierLang},
                                     {"fTime", fTime()}},
                                    std::string(), true);
    ApiResult result = classify(reply, false);
    if (result.outcome != ApiOutcome::Ok) return result;
    JsonValue json;
    if (!parseJson(reply.body, &json, nullptr) || !json.isObject()) return unexpected(reply.status, "Frontier's sign-in reply was not in the form the launcher expects");
    const std::string* authToken = stringMember(json, "authToken");
    if (!authToken || authToken->empty()) return unexpected(reply.status, "Frontier's sign-in reply carried no token");
    session->authToken = *authToken;
    session->machineToken = machineToken;
    session->frontierStore = true;
    return result;
}

ApiResult FrontierApi::purchases(const FrontierSession& session, PurchaseList* out) {
    // A Frontier-store session names its tokens in the query, after fTime; a Steam one sends the bearer header.
    HttpResponse reply;
    if (session.frontierStore)
        reply = send("/3.0/user/purchases", {{"fTime", fTime()}, {"authToken", session.authToken}, {"machineToken", session.machineToken}}, std::string());
    else
        reply = send("/3.0/user/purchases", {{"fTime", fTime()}}, session.authToken);
    ApiResult result = classify(reply, !session.frontierStore);
    if (result.outcome != ApiOutcome::Ok) return result;
    JsonValue json;
    if (!parseJson(reply.body, &json, nullptr) || !json.isObject()) return unexpected(reply.status, "Frontier's list of purchases was not in the form the launcher expects");
    const JsonValue* list = json.find("purchases");
    if (!list || !list->isArray()) return unexpected(reply.status, "Frontier's list of purchases was not in the form the launcher expects");
    out->items.clear();
    out->shape.clear();
    for (const JsonValue& entry : list->items) {
        if (!entry.isObject()) continue;
        if (out->shape.empty()) {
            for (const std::string& key : entry.keys) out->shape += (out->shape.empty() ? "" : ", ") + key;
        }
        Purchase p;
        // The names Api.fs and Types.fs read. Its sortkey may be a number or a string.
        p.sku = textOf(entry, "product_sku");
        p.name = textOf(entry, "product_name");
        p.directory = textOf(entry, "directory");
        p.filter = textOf(entry, "filter");
        p.gameArgs = textOf(entry, "gameargs");
        p.serverArgs = textOf(entry, "serverargs");
        const JsonValue* test = entry.findNoCase("testapi");
        p.testApi = test && ((test->type == JsonValue::Type::Bool && test->boolean) || (test->isString() && test->text == "true"));
        const JsonValue* sort = entry.findNoCase("sortkey");
        if (sort && sort->isInt()) p.sortKey = sort->integer;
        else if (sort && sort->isString()) p.sortKey = std::strtoll(sort->text.c_str(), nullptr, 10);
        out->items.push_back(std::move(p));
    }
    return result;
}

ApiResult FrontierApi::installerVersion(const FrontierSession& session, const std::string& machineId, const std::string& sku,
                                        std::string* version) {
    const HttpResponse reply = send("/3.0/user/installer",
                                    {{"machineToken", session.machineToken},
                                     {"authToken", session.authToken},
                                     {"machineId", machineId},
                                     {"sku", sku},
                                     {"os", "win"},
                                     {"fTime", fTime()}},
                                    session.frontierStore ? std::string() : session.authToken);
    ApiResult result = classify(reply, !session.frontierStore);
    if (result.outcome != ApiOutcome::Ok) return result;
    JsonValue json;
    if (!parseJson(reply.body, &json, nullptr) || !json.isObject()) return unexpected(reply.status, "Frontier's reply about the game's version was not in the form the launcher expects");
    const std::string* found = stringMember(json, "version");
    if (!found || found->empty()) return unexpected(reply.status, "Frontier's reply about the game's version carried no version");
    *version = *found;
    return result;
}

}  // namespace edvr::installer
