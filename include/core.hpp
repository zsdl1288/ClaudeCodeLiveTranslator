#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace cclive {
struct Settings {
    std::string provider = "google";
    std::string base_url;
    std::string model;
    std::string api_key_env;
    std::string baidu_app_id;
    std::string baidu_key_env = "BAIDU_TRANSLATE_KEY";
    std::string chatgpt_model;
    std::string codex_path;
    std::string codex_model;
    std::string target = "zh-CN";
    int settle_ms = 900;
    bool show_original = false;
    bool always_on_top = false;
    int font_size = 12;
    void validate() const;
};
Settings load_settings(const std::filesystem::path& path);
void save_settings(const std::filesystem::path& path, const Settings& settings);
// Identifies the actual destination shared by requests and service cooldowns.
std::string translation_service_key(const Settings& settings);
std::wstring to_wide(const std::string& utf8);
std::string to_utf8(const std::wstring& text);
class TranslationError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class TranslationCancelled : public TranslationError { public: using TranslationError::TranslationError; };
// The caller controls cancellation and retries; this delay is always 1 second to 24 hours.
class TranslationRateLimited : public TranslationError {
public:
    explicit TranslationRateLimited(std::int64_t delay_ms);
    TranslationRateLimited(std::int64_t delay_ms, const std::string& safe_message);
    const std::int64_t retry_after_ms;
};
using Delta = std::function<void(const std::string&)>;
std::string translate(const Settings&, const std::string& utf8, const Delta&, const std::atomic_bool& cancel);
// Parsing helpers are also used by the native regression checks.
// Display grouping is separate from independent translation/stability slots.
struct TextSegment {
    std::string text;
    std::size_t paragraph = 0;
};
std::vector<TextSegment> split_text_segments(const std::string& utf8);
std::vector<std::string> split_sentences(const std::string& utf8);
std::string parse_openai_content(const nlohmann::json& data, bool streaming);
// Baidu protocol helpers perform no network calls and never include credentials in errors.
struct BaiduTextPart { bool literal = false; std::string text; };
std::vector<BaiduTextPart> split_baidu_text(const std::string& utf8);
std::string build_baidu_form(const Settings&, const std::string& query, const std::string& salt, const std::string& secret);
std::string parse_baidu_content(const nlohmann::json& data);

// Call observe/flush on one reader thread. Each sentence retains an independent
// stability timer, epoch and revision so obsolete asynchronous results can be dropped.
class Ledger {
public:
    explicit Ledger(int settle_ms = 900);
    std::vector<nlohmann::json> observe(const nlohmann::json& snapshot);
    std::vector<nlohmann::json> flush();
    std::vector<nlohmann::json> observe_at(const nlohmann::json&, std::int64_t now_ms);
    std::vector<nlohmann::json> flush_at(std::int64_t now_ms);
private:
    struct Segment {
        std::string text, emitted_text, emitted_kind;
        std::size_t paragraph = 0, emitted_paragraph = 0;
        std::int64_t changed = 0;
        unsigned revision = 0;
        bool emitted = false;
    };
    struct Block {
        std::string key, kind;
        std::vector<Segment> segments;
        bool present = true;
    };
    int settle_;
    bool has_context_ = false;
    std::string context_;
    unsigned epoch_ = 0;
    std::vector<Block> blocks_;
};
}
