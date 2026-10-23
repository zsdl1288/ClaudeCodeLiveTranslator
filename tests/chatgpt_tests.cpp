#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include "chatgpt.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

using namespace cclive;
namespace cg = cclive::chatgpt;
using json = nlohmann::json;
namespace {
int checks = 0, failures = 0;
void check(bool value, const char* name) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
template<class F> void rejects(F function, const char* name) {
    bool threw = false; try { function(); } catch (const TranslationError& error) { threw = true; check(std::string(error.what()).find("FIXTURE_SECRET") == std::string::npos, "Errors do not expose server token or body"); }
    check(threw, name);
}
std::int64_t now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string b64(const unsigned char* bytes, std::size_t count) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"; unsigned acc = 0, bits = 0; std::string out;
    for (std::size_t i = 0; i < count; ++i) { acc = (acc << 8) | bytes[i]; bits += 8; while (bits >= 6) { bits -= 6; out += alphabet[(acc >> bits) & 63]; } }
    if (bits) out += alphabet[(acc << (6 - bits)) & 63]; return out;
}
std::string b64(const std::string& text) { return b64(reinterpret_cast<const unsigned char*>(text.data()), text.size()); }
std::string enc(const std::string& text) {
    constexpr char hex[] = "0123456789ABCDEF"; std::string out;
    for (unsigned char c : text) { if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c); else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; } }
    return out;
}
std::string dec(std::string text) {
    std::string out; for (std::size_t i = 0; i < text.size(); ++i) { if (text[i] == '%') { out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)); i += 2; } else out += text[i] == '+' ? ' ' : text[i]; } return out;
}
std::map<std::string, std::string> fields(const std::string& text) {
    std::map<std::string, std::string> out; std::size_t start = 0;
    while (start < text.size()) { auto end = text.find('&', start); if (end == std::string::npos) end = text.size(); auto equal = text.find('=', start); if (equal >= end) throw std::runtime_error("fixture form"); out[dec(text.substr(start, equal - start))] = dec(text.substr(equal + 1, end - equal - 1)); start = end + 1; }
    return out;
}
struct SigningKey {
    HCRYPTPROV provider = 0; HCRYPTKEY key = 0; json jwks;
    SigningKey() {
        if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) || !CryptGenKey(provider, AT_SIGNATURE, (2048 << 16) | CRYPT_EXPORTABLE, &key)) throw std::runtime_error("RSA fixture");
        DWORD size = 0; if (!CryptExportKey(key, 0, PUBLICKEYBLOB, 0, nullptr, &size)) throw std::runtime_error("RSA fixture export");
        std::vector<BYTE> blob(size); if (!CryptExportKey(key, 0, PUBLICKEYBLOB, 0, blob.data(), &size)) throw std::runtime_error("RSA fixture export");
        RSAPUBKEY pub{}; memcpy(&pub, blob.data() + sizeof(BLOBHEADER), sizeof(pub));
        std::vector<unsigned char> modulus(blob.begin() + sizeof(BLOBHEADER) + sizeof(RSAPUBKEY), blob.end()); std::reverse(modulus.begin(), modulus.end());
        unsigned char exponent[4]{static_cast<unsigned char>(pub.pubexp >> 24), static_cast<unsigned char>(pub.pubexp >> 16), static_cast<unsigned char>(pub.pubexp >> 8), static_cast<unsigned char>(pub.pubexp)};
        std::size_t skip = 0; while (skip < 3 && !exponent[skip]) ++skip;
        jwks = {{"keys", json::array({{{"kid", "fixture-key"}, {"kty", "RSA"}, {"alg", "RS256"}, {"use", "sig"}, {"n", b64(modulus.data(), modulus.size())}, {"e", b64(exponent + skip, 4 - skip)}}})}};
    }
    ~SigningKey() { if (key) CryptDestroyKey(key); if (provider) CryptReleaseContext(provider, 0); }
    std::string token(const json& claims, json header = {{"alg", "RS256"}, {"kid", "fixture-key"}}) {
        auto input = b64(header.dump()) + "." + b64(claims.dump()); HCRYPTHASH hash = 0;
        if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) || !CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()), static_cast<DWORD>(input.size()), 0)) throw std::runtime_error("RSA fixture hash");
        DWORD count = 0; if (!CryptSignHashW(hash, AT_SIGNATURE, nullptr, 0, nullptr, &count)) throw std::runtime_error("RSA fixture sign");
        std::vector<BYTE> signature(count); if (!CryptSignHashW(hash, AT_SIGNATURE, nullptr, 0, signature.data(), &count)) throw std::runtime_error("RSA fixture sign");
        CryptDestroyHash(hash); std::reverse(signature.begin(), signature.end()); return input + "." + b64(signature.data(), signature.size());
    }
};
json claims(const std::string& nonce, const std::string& client = "oaiapp_fixture") {
    return {{"iss", "https://auth.openai.com"}, {"sub", "fixture-subject"}, {"aud", client}, {"iat", now()}, {"exp", now() + 3600}, {"nonce", nonce}, {"email", "fixture@example.invalid"}, {"name", "Offline fixture"}};
}
std::string event(const json& value) { return "data: " + value.dump() + "\n\n"; }
std::string successful_stream() { return event({{"type", "response.output_text.delta"}, {"delta", "你好"}}) + event({{"type", "response.output_text.delta"}, {"delta", "。"}}) + event({{"type", "response.completed"}, {"response", {{"status", "completed"}}}}); }
void send_callback(const std::string& redirect, const std::string& query, const std::string& extra_headers = {}) {
    auto colon = redirect.find(':', 7), slash = redirect.find('/', colon); auto port = std::stoi(redirect.substr(colon + 1, slash - colon - 1));
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (socket == INVALID_SOCKET) throw std::runtime_error("callback fixture socket");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<unsigned short>(port));
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { closesocket(socket); throw std::runtime_error("callback fixture connect"); }
    std::string request = "GET " + redirect.substr(slash) + "?" + query + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n" + extra_headers + "Connection: close\r\n\r\n";
    send(socket, request.data(), static_cast<int>(request.size()), 0); closesocket(socket);
}
SOCKET silent_connection(const std::string& redirect) {
    auto colon = redirect.find(':', 7), slash = redirect.find('/', colon); auto port = std::stoi(redirect.substr(colon + 1, slash - colon - 1));
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (socket == INVALID_SOCKET) throw std::runtime_error("silent callback socket");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<unsigned short>(port));
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { closesocket(socket); throw std::runtime_error("silent callback connect"); }
    return socket;
}
struct OAuthFixture {
    SigningKey& key; std::map<std::string, std::string> auth;
    int exchanges = 0, refreshes = 0, revokes = 0, inferences = 0, listings = 0;
    bool wrong_nonce = false, wrong_subject = false, identity_only = false, bad_json_type = false;
    int inference_status = 200; std::string stream = successful_stream(), retry_after = "7";
    json last_inference, last_refresh;
    explicit OAuthFixture(SigningKey& k) : key(k) {
        cg::testing::set_transport([this](const cg::testing::Request& request) -> cg::testing::Reply {
            if (request.url == "https://auth.openai.com/.well-known/openid-configuration") return {200, json({{"issuer", "https://auth.openai.com"}, {"jwks_uri", "https://auth.openai.com/.well-known/jwks.json"}, {"revocation_endpoint", "https://auth.openai.com/oauth/revoke"}}).dump()};
            if (request.url == "https://auth.openai.com/.well-known/jwks.json") return {200, key.jwks.dump()};
            if (request.url == "https://auth.openai.com/oauth/revoke") { ++revokes; auto f = fields(request.body); check(f["client_id"] == "oaiapp_fixture" && f["token_type_hint"] == "refresh_token", "Revoke uses selected issued client"); return {200, ""}; }
            if (request.url == "https://auth.openai.com/api/accounts/oauth/token") {
                auto f = fields(request.body); check(request.method == "POST" && request.content_type == "application/x-www-form-urlencoded", "Token request is form POST");
                check(f["client_id"] == "oaiapp_fixture" && f["resource"] == "https://api.openai.com/v1", "Exchange uses issued client and fixed resource");
                if (f["grant_type"] == "refresh_token") { ++refreshes; check(!f.count("scope"), "Refresh omits scope"); last_refresh = f; return {200, json({{"access_token", "FIXTURE_SECRET_ACCESS_ROTATED"}, {"refresh_token", "FIXTURE_SECRET_REFRESH_ROTATED"}, {"token_type", "Bearer"}, {"expires_in", 3600}}).dump()}; }
                ++exchanges;
                check(f["redirect_uri"] == auth["redirect_uri"] && cg::testing::pkce_challenge(f["code_verifier"]) == auth["code_challenge"], "Code exchange preserves URI and PKCE");
                auto identity = claims(wrong_nonce ? "wrong" : auth["nonce"]); if (wrong_subject) identity["sub"] = "different-subject";
                json value{{"id_token", key.token(identity)}, {"scope", identity_only ? "openid profile email" : "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"}};
                if (!identity_only) { value["access_token"] = "FIXTURE_SECRET_ACCESS"; value["refresh_token"] = "FIXTURE_SECRET_REFRESH"; value["token_type"] = "Bearer"; value["expires_in"] = bad_json_type ? json("FIXTURE_SECRET_BAD_TYPE") : json(3600); }
                return {200, value.dump()};
            }
            if (request.url == "https://api.openai.com/v1/models") {
                ++listings; check(request.method == "GET" && !request.bearer.empty() && request.body.empty(), "Models use GET and bearer");
                return {200, R"({"models":[{"slug":"second","display_name":"Second","visibility":"list"},{"slug":"hidden","display_name":"Hidden","visibility":"hidden"},{"slug":"first","display_name":"First","visibility":"list"}]})"};
            }
            if (request.url == "https://api.openai.com/v1/responses") { ++inferences; last_inference = json::parse(request.body); check(request.method == "POST" && !request.bearer.empty(), "Responses use POST and bearer"); return {inference_status, inference_status == 200 ? stream : "FIXTURE_SECRET_SERVER_BODY", "text/event-stream; charset=utf-8", retry_after}; }
            throw std::runtime_error("unexpected fixture request");
        });
    }
    ~OAuthFixture() { cg::testing::set_transport({}); }
    cg::LaunchBrowser launcher(std::string mutation = {}) {
        return [this, mutation](const std::string& url) {
            check(url.find("https://auth.openai.com/api/accounts/authorize?") == 0, "Browser uses documented authorize endpoint");
            auth = fields(url.substr(url.find('?') + 1));
            check(auth["code_challenge_method"] == "S256" && auth["state"].size() >= 43 && auth["nonce"].size() >= 43, "Fresh random state nonce and S256 PKCE");
            check(auth["redirect_uri"].find("http://127.0.0.1:") == 0 && auth["redirect_uri"].find("/auth/callback") != std::string::npos, "Callback uses exact IPv4 loopback and path");
            auto state = mutation == "state" ? "wrong" : auth["state"];
            auto query = "state=" + enc(state) + "&code=fixture-code&client_id=" + (mutation == "client" ? "oaiapp_other" : "oaiapp_fixture");
            if (mutation == "duplicate") query += "&state=" + enc(state);
            if (mutation == "denied") query = "state=" + enc(state) + "&error=access_denied";
            send_callback(auth["redirect_uri"], query);
        };
    }
};
}
int main() {
    WSADATA sockets{}; if (WSAStartup(MAKEWORD(2, 2), &sockets)) return 2;
    try {
        const std::atomic_bool cancel{false}; SigningKey signing;
        check(cg::testing::pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "RFC 7636 PKCE vector");
        auto identity = claims("nonce"); auto token = signing.token(identity);
        check(cg::testing::verify_id_token(token, signing.jwks, "oaiapp_fixture", "nonce", now())["sub"] == "fixture-subject", "Valid RSA signed ID token accepted");
        auto tampered = token; tampered[tampered.size() - 8] = tampered[tampered.size() - 8] == 'a' ? 'b' : 'a';
        rejects([&] { cg::testing::verify_id_token(tampered, signing.jwks, "oaiapp_fixture", "nonce", now()); }, "Tampered signature rejected");
        for (const auto& pair : std::vector<std::pair<std::string, json>>{{"iss", "https://evil.invalid"}, {"aud", "other-client"}, {"exp", now() - 30}, {"nonce", "wrong"}, {"iat", now() + 30}, {"sub", ""}, {"nbf", now() + 30}}) {
            auto invalid = identity; invalid[pair.first] = pair.second;
            rejects([&] { cg::testing::verify_id_token(signing.token(invalid), signing.jwks, "oaiapp_fixture", "nonce", now()); }, "Incorrect signed claim rejected");
        }
        rejects([&] { cg::testing::verify_id_token(signing.token(identity, {{"alg", "none"}, {"kid", "fixture-key"}}), signing.jwks, "oaiapp_fixture", "nonce", now()); }, "Algorithm confusion rejected");
        auto no_key = signing.jwks; no_key["keys"][0]["kid"] = "different-key";
        rejects([&] { cg::testing::verify_id_token(token, no_key, "oaiapp_fixture", "nonce", now()); }, "Unknown kid rejected");
        auto multi = identity; multi["aud"] = json::array({"oaiapp_fixture", "other"});
        rejects([&] { cg::testing::verify_id_token(signing.token(multi), signing.jwks, "oaiapp_fixture", "nonce", now()); }, "Multiple audiences require azp");
        multi["azp"] = "oaiapp_fixture"; check(cg::testing::verify_id_token(signing.token(multi), signing.jwks, "oaiapp_fixture", "nonce", now())["sub"] == "fixture-subject", "Multi audience with correct azp accepted");

        std::vector<std::string> pieces; auto good = successful_stream(); for (char c : good) pieces.emplace_back(1, c);
        std::string deltas; check(cg::testing::parse_stream(pieces, [&](const std::string& part) { deltas += part; }) == "你好。" && deltas == "你好。", "Byte fragmented UTF8 SSE waits for completed and emits only text deltas");
        auto crlf = good; for (std::size_t i = 0; (i = crlf.find('\n', i)) != std::string::npos; i += 2) crlf.replace(i, 1, "\r\n");
        check(cg::testing::parse_stream({crlf}) == "你好。", "SSE CRLF accepted");
        rejects([&] { cg::testing::parse_stream({event({{"type", "response.output_text.delta"}, {"delta", "partial"}})}); }, "Truncated stream rejected");
        rejects([&] { cg::testing::parse_stream({"data: [DONE]\n\n"}); }, "DONE marker alone is not successful");
        rejects([&] { cg::testing::parse_stream({event({{"type", "response.incomplete"}, {"response", {{"status", "incomplete"}}}})}); }, "Incomplete terminal event rejected");
        rejects([&] { cg::testing::parse_stream({event({{"type", "response.failed"}, {"response", {{"error", {{"message", "FIXTURE_SECRET_SERVER_BODY"}, {"code", "unknown"}}}}}})}); }, "Failed event does not expose body");
        rejects([&] { cg::testing::parse_stream({good + event({{"type", "response.failed"}})}); }, "Failure after completed remains failure");
        rejects([&] { cg::testing::parse_stream({good.substr(0, good.size() - 1)}); }, "Incomplete final SSE frame rejected");
        rejects([&] { cg::testing::parse_stream({"data: FIXTURE_SECRET_INVALID_JSON\n\n"}); }, "Malformed stream error sanitized");
        auto folder = std::filesystem::path(L"work") / L"chatgpt-agent-tests" / std::to_wstring(GetCurrentProcessId()); std::filesystem::create_directories(folder); auto path = folder / L"account.bin";
        check(!cg::status(path).signed_in, "Missing account has signed out status");
        OAuthFixture fixture(signing);
        cg::sign_in(path, cancel, fixture.launcher());
        check(cg::status(path).signed_in && cg::status(path).plan_enabled && cg::status(path).email == "fixture@example.invalid", "Local OAuth exchanges and validates account");
        auto first_record = cg::testing::load_record(path); auto stable_host = first_record["ext_agent_host_id"];
        check(first_record["active_client_id"] == "oaiapp_fixture" && stable_host.get<std::string>().find("urn:uuid:") == 0, "Issued client and UUID host persisted");
        std::ifstream file(path, std::ios::binary); std::string ciphertext((std::istreambuf_iterator<char>(file)), {}); file.close();
        check(ciphertext.find("FIXTURE_SECRET") == std::string::npos && ciphertext.find("fixture@example.invalid") == std::string::npos && ciphertext.substr(0, 8) == "CCGPT1\r\n", "DPAPI encrypted record conceals tokens and identity");
        auto catalog = cg::models(path, cancel); check(catalog.size() == 2 && catalog[0].id == "second" && catalog[1].id == "first", "Visible model order preserved");
        deltas.clear(); check(cg::translate(path, "second", "zh-CN", "Hello `code`", [&](const std::string& s) { deltas += s; }, cancel) == "你好。", "Offline Responses translation succeeds");
        auto request = fixture.last_inference;
        check(request["store"] == false && request["stream"] == true && request["input"].is_array() && request["input"][0]["role"] == "user" && request["instructions"].is_string(), "Responses required request fields");
        check(request.size() == 5 && !request.contains("temperature") && !request.contains("max_output_tokens") && !request.contains("previous_response_id"), "Unsupported Responses parameters omitted");
        auto old_state = fixture.auth["state"], old_nonce = fixture.auth["nonce"];
        cg::sign_in(path, cancel, fixture.launcher());
        check(fixture.auth["client_id"] == "oaiapp_fixture" && !fixture.auth.count("agent_name_hint") && fixture.auth.count("id_token_hint") && fixture.auth["ext_agent_host_id"] == stable_host, "Returning authorization reuses issued client and host");
        check(fixture.auth["state"] != old_state && fixture.auth["nonce"] != old_nonce, "Returning OAuth uses fresh state and nonce");
        std::thread delayed_callback;
        auto ordinary_launcher = fixture.launcher();
        cg::sign_in(path, cancel, [&](const std::string& url) {
            auto parsed = fields(url.substr(url.find('?') + 1));
            auto silent = silent_connection(parsed.at("redirect_uri"));
            delayed_callback = std::thread([&, silent, url] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5250));
                closesocket(silent); ordinary_launcher(url);
            });
        });
        delayed_callback.join();
        check(cg::status(path).signed_in, "Silent browser preconnection does not abort the OAuth attempt");
        int exchanges = fixture.exchanges;
        rejects([&] { cg::sign_in(path, cancel, fixture.launcher("state")); }, "Wrong callback state rejected");
        rejects([&] { cg::sign_in(path, cancel, fixture.launcher("duplicate")); }, "Duplicate callback state rejected");
        rejects([&] { cg::sign_in(path, cancel, fixture.launcher("denied")); }, "Consent denial rejected");
        rejects([&] { cg::sign_in(path, cancel, fixture.launcher("client")); }, "Returning client ID mismatch rejected");
        check(fixture.exchanges == exchanges && cg::status(path).signed_in, "Invalid callbacks do not exchange codes or replace active account");
        fixture.wrong_nonce = true; rejects([&] { cg::sign_in(path, cancel, fixture.launcher()); }, "Wrong token nonce rejected before save"); fixture.wrong_nonce = false;
        fixture.wrong_subject = true; rejects([&] { cg::sign_in(path, cancel, fixture.launcher()); }, "Returning subject mismatch rejected"); fixture.wrong_subject = false;
        fixture.bad_json_type = true; rejects([&] { cg::sign_in(path, cancel, fixture.launcher()); }, "Invalid token response type sanitized"); fixture.bad_json_type = false;
        auto expiring = cg::testing::load_record(path); expiring["accounts"][0]["expires_at"] = now() - 1; cg::testing::save_record(path, expiring);
        std::thread concurrent([&] { cg::models(path, cancel); }); cg::models(path, cancel); concurrent.join(); auto refreshed = cg::testing::load_record(path);
        check(fixture.refreshes == 1 && refreshed["accounts"][0]["refresh_token"] == "FIXTURE_SECRET_REFRESH_ROTATED" && refreshed["accounts"][0]["subject"] == "fixture-subject", "Refresh atomically rotates tokens and retains validated identity without new ID token");
        fixture.inference_status = 429; bool limited = false; deltas.clear();
        try { cg::translate(path, "second", "zh-CN", "Hello", [&](const std::string& s) { deltas += s; }, cancel); } catch (const TranslationRateLimited& error) { limited = error.retry_after_ms == 7000 && std::string(error.what()).find("FIXTURE_SECRET") == std::string::npos; }
        check(limited && deltas.empty(), "HTTP429 uses retry header without body or deltas");
        fixture.retry_after = "bad"; limited = false;
        try { cg::translate(path, "second", "zh-CN", "Hello", {}, cancel); } catch (const TranslationRateLimited& error) { limited = error.retry_after_ms == 60000; }
        check(limited, "Malformed HTTP429 retry header defaults to sixty seconds");
        fixture.retry_after = "99999999999999999999999999999"; limited = false;
        try { cg::translate(path, "second", "zh-CN", "Hello", {}, cancel); } catch (const TranslationRateLimited& error) { limited = error.retry_after_ms == 86400000; }
        check(limited, "HTTP429 retry overflow saturates to one day");
        fixture.retry_after = "Thu, 01 Jan 1970 00:00:00 GMT"; limited = false;
        try { cg::translate(path, "second", "zh-CN", "Hello", {}, cancel); } catch (const TranslationRateLimited& error) { limited = error.retry_after_ms == 1000; }
        check(limited, "Past HTTP-date retry clamps to one second");
        fixture.inference_status = 200;
        auto cancel_path = folder / L"cancel.bin"; std::atomic_bool canceled{false};
        rejects([&] { cg::sign_in(cancel_path, canceled, [&](const std::string&) { canceled.store(true); }); }, "OAuth callback waiting supports cancellation");
        auto elapsed_start = std::chrono::steady_clock::now();
        canceled.store(true); rejects([&] { cg::models(path, canceled); }, "Cancelled model operation rejected");
        check(std::chrono::steady_clock::now() - elapsed_start < std::chrono::seconds(1), "Cancellation is promptly bounded");
        check(cg::sign_out(path, cancel) && fixture.revokes == 1, "Sign out revokes renewable session");
        auto logged_out = cg::testing::load_record(path);
        check(!cg::status(path).signed_in && logged_out["accounts"][0]["client_id"] == "oaiapp_fixture" && logged_out["ext_agent_host_id"] == stable_host && logged_out["accounts"][0]["id_token"] == "", "Sign out clears tokens and keeps protected registration mapping");
        cg::sign_in(path, cancel, fixture.launcher()); check(fixture.auth["client_id"] == "oaiapp_fixture" && !fixture.auth.count("id_token_hint"), "Reauthorization after signout reuses client without ID token hint");
        fixture.identity_only = true; cg::sign_in(path, cancel, fixture.launcher());
        check(cg::status(path).signed_in && !cg::status(path).plan_enabled, "Validated identity can sign in without plan grant");
        rejects([&] { cg::models(path, cancel); }, "Inference permission enforced independently of identity");
        fixture.identity_only = false; cg::sign_in(path, cancel, fixture.launcher());
        check(!cg::sign_out(path, canceled) && !cg::status(path).signed_in, "Cancelled remote revocation still clears local tokens and reports unconfirmed");
        // Corrupt only the private test record, then prove fail-closed decryption.
        auto damaged = ciphertext; damaged.back() ^= 1; { std::ofstream out(folder / L"damaged.bin", std::ios::binary); out.write(damaged.data(), static_cast<std::streamsize>(damaged.size())); }
        rejects([&] { cg::status(folder / L"damaged.bin"); }, "Modified DPAPI ciphertext rejected");
        check(!cg::sign_out(folder / L"damaged.bin", cancel) && !std::filesystem::exists(folder / L"damaged.bin"), "Corrupt owned credential record has safe single-file recovery");
        cg::sign_in(folder / L"damaged.bin", cancel, fixture.launcher());
        check(cg::status(folder / L"damaged.bin").signed_in, "Corrupt account can reconnect after local cleanup");
        { std::ofstream unrelated(folder / L"unrelated.txt"); unrelated << "unrelated document"; }
        rejects([&] { cg::sign_out(folder / L"unrelated.txt", cancel); }, "Recovery refuses unrecognized unrelated file");
        check(std::filesystem::exists(folder / L"unrelated.txt"), "Unrelated file remains intact");
        cg::testing::set_transport({}); rejects([&] { cg::sign_in(folder / L"blocked.bin", cancel, [](const std::string&) {}); }, "Tests cannot accidentally contact real auth service");
        std::cout << checks << " checks, " << failures << " failures\n";
    } catch (const std::exception& error) { ++failures; std::cerr << "Unhandled test failure: " << error.what() << '\n'; }
    WSACleanup(); return failures ? 1 : 0;
}
