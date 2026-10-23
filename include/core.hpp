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
    std::string target = "zh-CN";
    int settle_ms = 900;
    bool show_original = false;
    bool always_on_top = false;
    int font_size = 10;
    void validate() const;
};
Settings load_settings(const std::filesystem::path& path);
void save_settings(const std::filesystem::path& path, const Settings& settings);
std::wstring to_wide(const std::string& utf8);
std::string to_utf8(const std::wstring& text);
class TranslationError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class TranslationCancelled : public TranslationError { public: using TranslationError::TranslationError; };
using Delta = std::function<void(const std::string&)>;
std::string translate(const Settings&, const std::string& utf8, const Delta&, const std::atomic_bool& cancel);
// Parsing helpers are also used by the native regression checks.
std::vector<std::string> split_sentences(const std::string& utf8);
std::string parse_openai_content(const nlohmann::json& data, bool streaming);

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
