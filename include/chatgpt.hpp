#pragma once
#include "core.hpp"

namespace cclive::chatgpt {
struct AccountStatus {
    bool signed_in = false;
    bool plan_enabled = false;
    std::string email, name, client_id;
    std::int64_t expires_at = 0;
};
struct Model { std::string id, name; };
using LaunchBrowser = std::function<void(const std::string& authorization_url)>;

// This app owns this file. No other application's credentials are read.
std::filesystem::path default_account_path();
AccountStatus status(const std::filesystem::path& path = default_account_path());
std::vector<AccountStatus> accounts(const std::filesystem::path& path = default_account_path());
void select_account(const std::filesystem::path& path, const std::string& client_id);
// An empty launcher opens the system browser. add_account preserves saved registrations.
void sign_in(const std::filesystem::path& path, const std::atomic_bool& cancel,
             const LaunchBrowser& launcher = {}, bool add_account = false);
// Always clears local tokens. false means remote revocation was not confirmed.
bool sign_out(const std::filesystem::path& path, const std::atomic_bool& cancel);
std::vector<Model> models(const std::filesystem::path& path, const std::atomic_bool& cancel);
std::string translate(const std::filesystem::path& path, const std::string& model,
                      const std::string& target, const std::string& text,
                      const Delta& delta, const std::atomic_bool& cancel);

#ifdef CCLIVE_CHATGPT_TESTING
// Test transports receive fixed production URLs but must never forward to a real service.
namespace testing {
struct Request { std::string method, url, body, bearer, content_type; };
struct Reply {
    int status = 200;
    std::string body, content_type = "application/json", retry_after;
    Reply(int code = 200, std::string payload = {}, std::string type = "application/json", std::string retry = {})
        : status(code), body(std::move(payload)), content_type(std::move(type)), retry_after(std::move(retry)) {}
};
using Transport = std::function<Reply(const Request&)>;
void set_transport(Transport transport);
std::string pkce_challenge(const std::string& verifier);
nlohmann::json verify_id_token(const std::string& token, const nlohmann::json& jwks,
                               const std::string& client_id, const std::string& nonce,
                               std::int64_t now);
std::string parse_stream(const std::vector<std::string>& chunks, const Delta& delta = {});
void save_record(const std::filesystem::path& path, const nlohmann::json& record);
nlohmann::json load_record(const std::filesystem::path& path);
}
#endif
}
