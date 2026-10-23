#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace cclive {
// Bounded, thread-safe RAM cache. No files, background writer or persistence.
// The path/bool parameters remain for source compatibility and are ignored.
class TaskCache {
public:
  struct Limits {
    std::size_t max_tasks = 50;
    std::size_t max_total_payload_bytes = 16 * 1024 * 1024;
    std::size_t max_task_payload_bytes = 2 * 1024 * 1024;
  };
  explicit TaskCache(std::filesystem::path unused_path = {},
                     bool unused_persistent = false);
  TaskCache(std::filesystem::path unused_path, bool unused_persistent,
            Limits limits);
  ~TaskCache();
  TaskCache(const TaskCache &) = delete;
  TaskCache &operator=(const TaskCache &) = delete;
  TaskCache(TaskCache &&) = delete;
  TaskCache &operator=(TaskCache &&) = delete;

  std::optional<nlohmann::json> get(const std::string &task_id);
  void put(const std::string &task_id, const nlohmann::json &payload);
  void erase(const std::string &task_id);
  void flush(); // Compatibility no-op: nothing is written to disk.
  bool healthy() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace cclive
