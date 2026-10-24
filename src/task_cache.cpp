#include "task_cache.hpp"
#include "markdown_table.hpp"
#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cclive {
namespace {
using json = nlohmann::json;
constexpr std::size_t max_id_bytes = 4096;

// Count serialized UTF-8 bytes without allocating an oversized serialized copy.
class CountBuffer final : public std::streambuf {
public:
  explicit CountBuffer(std::size_t limit) : limit_(limit) {}
  std::size_t size() const { return size_; }

private:
  std::streamsize xsputn(const char *, std::streamsize n) override {
    if (n < 0 || static_cast<std::size_t>(n) > limit_ - size_)
      throw std::length_error("task payload too large");
    size_ += static_cast<std::size_t>(n);
    return n;
  }
  int_type overflow(int_type c) override {
    if (traits_type::eq_int_type(c, traits_type::eof()))
      return traits_type::not_eof(c);
    if (size_ == limit_)
      throw std::length_error("task payload too large");
    ++size_;
    return c;
  }
  std::size_t limit_, size_ = 0;
};
std::size_t payload_size(const json &value, std::size_t limit) {
  CountBuffer buffer(limit);
  std::ostream stream(&buffer);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream << value;
  return buffer.size();
}
std::string tail_lines(std::string_view text, std::size_t maximum,
                       std::size_t maximum_bytes) {
  if (!maximum)
    return {};
  if (auto table = markdown::table_tail(text, maximum)) {
    if (table->size() > maximum_bytes)
      throw std::length_error("task payload too large");
    return std::move(*table);
  }
  auto end = text.size();
  while (end && (text[end - 1] == '\r' || text[end - 1] == '\n'))
    --end;
  std::size_t lines = 1;
  while (end) {
    --end;
    if (text[end] != '\r' && text[end] != '\n')
      continue;
    const auto begin = end + 1;
    if (text[end] == '\n' && end && text[end - 1] == '\r')
      --end;
    if (lines++ == maximum) {
      text = text.substr(begin);
      break;
    }
  }
  // Check before materializing ordinary text; a retained line can itself be
  // much larger than the cache's per-task byte limit.
  if (text.size() > maximum_bytes)
    throw std::length_error("task payload too large");
  return std::string(text);
}
std::size_t logical_lines(std::string_view text) {
  auto end = text.size();
  while (end && (text[end - 1] == '\r' || text[end - 1] == '\n'))
    --end;
  if (!end)
    return 0;
  std::size_t lines = 1;
  for (std::size_t i = 0; i < end; ++i) {
    if (text[i] == '\r') {
      ++lines;
      if (i + 1 < end && text[i + 1] == '\n')
        ++i;
    } else if (text[i] == '\n')
      ++lines;
  }
  return lines;
}
std::string_view record_text(const json &record, const char *field) {
  auto value = record.find(field);
  return value != record.end() && value->is_string()
             ? std::string_view(value->get_ref<const std::string &>())
             : std::string_view{};
}
std::size_t record_lines(const json &record) {
  if (!record.is_object())
    return 1;
  const auto source = logical_lines(record_text(record, "source"));
  const auto translation = logical_lines(record_text(record, "translation"));
  const auto state = record.find("state");
  const bool done =
      state != record.end() && state->is_string() && *state == "done";
  return (std::max)(std::size_t(1), done ? (translation ? translation : source)
                                         : (std::max)(source, translation));
}
json normalized_payload(const json &payload, std::size_t limit) {
  if (!payload.is_object() || !payload.contains("records") ||
      !payload["records"].is_array()) {
    payload_size(payload, limit);
    return payload;
  }
  json result = json::object();
  // Copy only the newest bounded records, avoiding copies of discarded history.
  std::size_t budget = limit;
  auto copy_field = [&](json &object, const std::string &key,
                        const json &value) {
    const auto size = payload_size(value, budget);
    budget -= size;
    object[key] = value;
  };
  for (auto item = payload.begin(); item != payload.end(); ++item)
    if (item.key() != "records")
      copy_field(result, item.key(), item.value());
  std::vector<json> newest;
  std::size_t remaining = 100;
  const auto &records = payload["records"];
  for (auto current = records.rbegin(); current != records.rend() && remaining;
       ++current) {
    json record;
    if (current->is_object()) {
      record = json::object();
      for (auto field = current->begin(); field != current->end(); ++field) {
        if ((field.key() == "source" || field.key() == "translation") &&
            field.value().is_string()) {
          auto tail = tail_lines(field.value().get_ref<const std::string &>(),
                                 remaining, budget);
          if (tail.size() > budget)
            throw std::length_error("task payload too large");
          copy_field(record, field.key(), std::move(tail));
        } else
          copy_field(record, field.key(), field.value());
      }
    } else {
      const auto size = payload_size(*current, budget);
      budget -= size;
      record = *current;
    }
    const auto used = (std::min)(remaining, record_lines(record));
    remaining -= used;
    newest.push_back(std::move(record));
  }
  result["records"] = json::array();
  for (auto record = newest.rbegin(); record != newest.rend(); ++record)
    result["records"].push_back(std::move(*record));
  return result;
}
} // namespace

struct TaskCache::Impl {
  struct Entry {
    json payload;
    std::size_t bytes;
    std::list<std::string>::iterator lru;
  };
  Limits limits;
  mutable std::mutex mutex;
  std::unordered_map<std::string, Entry> entries;
  std::list<std::string> lru;
  std::size_t bytes = 0;
  std::atomic_bool good{true};

  explicit Impl(Limits bounds) : limits(bounds) {
    limits.max_tasks = (std::max)(std::size_t(1), (std::min)(bounds.max_tasks,
                                                             std::size_t(50)));
    limits.max_total_payload_bytes =
        (std::max)(std::size_t(1), (std::min)(bounds.max_total_payload_bytes,
                                              std::size_t(16 * 1024 * 1024)));
    limits.max_task_payload_bytes =
        (std::max)(std::size_t(1), (std::min)({bounds.max_task_payload_bytes,
                                               limits.max_total_payload_bytes,
                                               std::size_t(2 * 1024 * 1024)}));
  }
  void touch(Entry &entry) { lru.splice(lru.end(), lru, entry.lru); }
  void trim() {
    while (!lru.empty() && (entries.size() > limits.max_tasks ||
                            bytes > limits.max_total_payload_bytes)) {
      auto found = entries.find(lru.front());
      bytes -= found->second.bytes;
      entries.erase(found);
      lru.pop_front();
    }
  }
};

TaskCache::TaskCache(std::filesystem::path unused_path, bool unused_persistent)
    : TaskCache(std::move(unused_path), unused_persistent, Limits{}) {}
TaskCache::TaskCache(std::filesystem::path, bool, Limits limits)
    : impl_(std::make_unique<Impl>(limits)) {}
TaskCache::~TaskCache() = default;
std::optional<nlohmann::json> TaskCache::get(const std::string &task_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->entries.find(task_id);
  if (found == impl_->entries.end())
    return std::nullopt;
  impl_->touch(found->second);
  return found->second.payload;
}
void TaskCache::put(const std::string &task_id, const nlohmann::json &payload) {
  if (task_id.empty() || task_id.size() > max_id_bytes) {
    impl_->good = false;
    return;
  }
  try {
    json body =
        normalized_payload(payload, impl_->limits.max_task_payload_bytes);
    const auto bytes = payload_size(body, impl_->limits.max_task_payload_bytes);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto found = impl_->entries.find(task_id);
    if (found != impl_->entries.end()) {
      impl_->bytes -= found->second.bytes;
      found->second.payload = std::move(body);
      found->second.bytes = bytes;
      impl_->touch(found->second);
    } else {
      impl_->lru.push_back(task_id);
      try {
        impl_->entries.emplace(
            task_id,
            Impl::Entry{std::move(body), bytes, std::prev(impl_->lru.end())});
      } catch (...) {
        impl_->lru.pop_back();
        throw;
      }
    }
    impl_->bytes += bytes;
    impl_->trim();
    impl_->good = true;
  } catch (...) {
    impl_->good = false;
  }
}
void TaskCache::erase(const std::string &task_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->entries.find(task_id);
  if (found == impl_->entries.end())
    return;
  impl_->bytes -= found->second.bytes;
  impl_->lru.erase(found->second.lru);
  impl_->entries.erase(found);
}
void TaskCache::flush() {}
bool TaskCache::healthy() const { return impl_->good.load(); }
} // namespace cclive
