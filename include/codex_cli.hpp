#pragma once
#include "core.hpp"

namespace cclive::codex_cli {
// Only locates/validates a native executable; never reads CLI credentials.
std::filesystem::path resolve_executable(const std::string& configured = {});
// Prompt goes through stdin. Authentication stays entirely inside Codex CLI.
std::string translate(const Settings& settings, const std::string& text,
                      const Delta& delta, const std::atomic_bool& cancel);
#ifdef CCLIVE_CODEX_CLI_TESTING
std::string translate_for_test(const Settings& settings, const std::string& text,
                              const Delta& delta, const std::atomic_bool& cancel,
                              std::int64_t timeout_ms);
#endif
}