#include "task_cache.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using cclive::TaskCache;
using json = nlohmann::json;
namespace {
int checks = 0, failures = 0;
void check(bool value, const char *label) {
  ++checks;
  if (!value) {
    ++failures;
    std::cerr << "FAIL: " << label << '\n';
  }
}
std::string read_text(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}
} // namespace
int main() {
  const auto folder =
      std::filesystem::temp_directory_path() /
      ("cclive-task-cache-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(folder);
  try {
    {
      TaskCache cache;
      check(cache.healthy(), "empty RAM cache is healthy");
      check(!cache.get("A"), "unknown task misses");
      cache.put("A", {{"title", "Task A"},
                      {"translation", "甲任务译文"},
                      {"scroll", {{"y", 250}}},
                      {"follow", false}});
      cache.put("B", {{"title", "Task B"}, {"translation", "乙任务译文"}});
      check(cache.get("A")->at("translation") == "甲任务译文",
            "A/B/A task isolation A");
      check(cache.get("B")->at("translation") == "乙任务译文",
            "A/B/A task isolation B");
      check(cache.get("A")->at("scroll")["y"] == 250 &&
                !cache.get("A")->at("follow").get<bool>(),
            "reading position and follow state retained");
      auto copy = *cache.get("A");
      copy["translation"] = "changed caller copy";
      check(cache.get("A")->at("translation") == "甲任务译文",
            "get returns independent copy");
      cache.put("A", {{"title", "Renamed A"},
                      {"translation", "新译文"},
                      {"arbitrary", json::array({true, 20, nullptr})}});
      check(cache.get("A")->at("title") == "Renamed A",
            "same identity updates title");
      check(cache.get("A")->at("translation") == "新译文",
            "same identity updates translation");
      check(cache.get("A")->at("arbitrary").size() == 3,
            "generic JSON fields retained");
      cache.erase("A");
      check(!cache.get("A") && cache.get("B"),
            "erase current task preserves other task");
    }
    {
      const auto sentinel_path = folder / "sentinel.json";
      const std::string sentinel =
          R"({"schema_version":1,"entries":{"A":"NEVER LOAD ME"},"lru":["A"]})";
      {
        std::ofstream output(sentinel_path, std::ios::binary);
        output << sentinel;
      }
      {
        TaskCache cache(sentinel_path, true);
        check(cache.healthy() && !cache.get("A"),
              "constructor with true never loads file");
        cache.put("A", {{"translation", "Memory only"}});
        cache.flush();
        check(cache.get("A").has_value(), "compatibility true uses RAM cache");
        check(read_text(sentinel_path) == sentinel,
              "mutations and flush never touch existing file");
      }
      check(read_text(sentinel_path) == sentinel,
            "destructor never touches existing file");
      TaskCache reopened(sentinel_path, true);
      check(!reopened.get("A"),
            "new instance never retains previous instance task");
      const auto missing_path = folder / "must-not-exist" / "cache.json";
      {
        TaskCache cache(missing_path, true);
        cache.put("M", "memory");
        cache.flush();
        check(cache.get("M") == std::optional<json>("memory"),
              "missing path does not prevent RAM storage");
      }
      check(!std::filesystem::exists(missing_path.parent_path()),
            "no cache directories or files created");
      TaskCache impossible(sentinel_path / "not-a-directory" / "cache.json",
                           true);
      impossible.put("X", "works");
      impossible.flush();
      check(impossible.healthy() && impossible.get("X"),
            "invalid filesystem path is ignored");
    }
    {
      TaskCache cache({}, false, {2, 1024, 512});
      cache.put("A", "A");
      cache.put("B", "B");
      cache.get("A");
      cache.put("C", "C");
      check(!cache.get("B"), "least recently used task evicted");
      check(cache.get("A").has_value() && cache.get("C").has_value(),
            "recent tasks survive eviction");
      cache.put("A", {{"title", "Rename"}});
      check(cache.get("C") && cache.get("A")->at("title") == "Rename",
            "rename does not consume additional LRU slot");
    }
    {
      TaskCache cache({}, false, {50, 100, 80});
      cache.put("A", std::string(38, 'a'));
      cache.put("B", std::string(38, 'b'));
      cache.put("C", std::string(38, 'c'));
      check(!cache.get("A") && cache.get("B") && cache.get("C"),
            "total payload bound evicts oldest task");
      cache.put("C", std::string(1000, 'x'));
      check(cache.get("C")->get<std::string>() == std::string(38, 'c'),
            "oversized update preserves existing task");
      check(!cache.healthy(), "oversized payload reports unhealthy");
      cache.put("C", "smaller");
      check(cache.healthy(), "valid update recovers health");
    }
    {
      TaskCache cache({}, false, {50, 40, 40});
      cache.put("A", "甲乙丙丁戊己庚辛");
      cache.put("B", "甲乙丙丁戊己庚辛");
      check(!cache.get("A") && cache.get("B"),
            "capacity counts UTF-8 bytes rather than character count");
    }
    {
      TaskCache cache;
      for (int n = 0; n != 51; ++n)
        cache.put(std::to_string(n), n);
      check(!cache.get("0") && cache.get("1") && cache.get("50"),
            "default task count bound is 50");
      cache.put("oversized", std::string(2 * 1024 * 1024, 'x'));
      check(!cache.get("oversized"),
            "default single-task bound is 2 MiB serialized");
      cache.put("", "bad");
      cache.put(std::string(4097, 'x'), "bad");
      check(!cache.get("") && !cache.get(std::string(4097, 'x')),
            "empty and excessive identity rejected");
    }
    {
      TaskCache cache;
      std::vector<std::thread> callers;
      std::atomic_bool thread_failure{false};
      for (int t = 0; t != 4; ++t)
        callers.emplace_back([&, t] {
          try {
            for (int n = 0; n != 100; ++n) {
              const auto id =
                  "thread-" + std::to_string(t) + '-' + std::to_string(n % 4);
              cache.put(id, {{"thread", t}, {"revision", n}});
              const auto result = cache.get(id);
              if (!result || result->at("thread") != t)
                thread_failure = true;
            }
          } catch (...) {
            thread_failure = true;
          }
        });
      for (auto &caller : callers)
        caller.join();
      check(!thread_failure, "concurrent put/get retains per-task isolation");
      bool all_latest = true;
      for (int t = 0; t != 4; ++t)
        for (int k = 0; k != 4; ++k) {
          const auto result = cache.get("thread-" + std::to_string(t) + '-' +
                                        std::to_string(k));
          all_latest &= result && result->at("revision") == 96 + k;
        }
      check(all_latest,
            "all concurrent tasks retain latest in-memory revisions");
    }
    {
      auto lines = [](const std::string &prefix, int first, int count) {
        std::string text;
        for (int n = 0; n != count; ++n)
          text += prefix + std::to_string(first + n) + "\r\n";
        return text;
      };
      TaskCache cache;
      json records = json::array();
      for (int n = 0; n != 150; ++n)
        records.push_back({{"key", "r" + std::to_string(n)},
                           {"serial", n},
                           {"source", "source " + std::to_string(n)},
                           {"translation", "译文 " + std::to_string(n)},
                           {"state", "done"}});
      cache.put("lines",
                {{"records", records}, {"title", "Preserved"}, {"scroll", 35}});
      auto saved = *cache.get("lines");
      check(saved["records"].size() == 100 &&
                saved["records"][0]["serial"] == 50 &&
                saved["records"][99]["serial"] == 149,
            "only latest 100 subtitle records retained in order");
      check(saved["title"] == "Preserved" && saved["scroll"] == 35 &&
                saved["records"][0]["key"] == "r50",
            "line trimming preserves task metadata and record identity");
      cache.put(
          "single",
          {{"records",
            json::array({{{"key", "same-key"},
                          {"serial", 7},
                          {"state", "done"},
                          {"source", lines("source-", 0, 150)},
                          {"translation", lines("translation-", 0, 150)}}})}});
      auto single = cache.get("single")->at("records").at(0);
      check(single["source"] == lines("source-", 50, 100) &&
                single["translation"] == lines("translation-", 50, 100) &&
                single["key"] == "same-key" && single["serial"] == 7,
            "single multiline subtitle keeps latest 100 logical lines and "
            "identity");
      cache.put("partial",
                {{"records",
                  json::array({{{"key", "old"},
                                {"source", lines("old-", 0, 80)},
                                {"translation", lines("old-trans-", 0, 80)},
                                {"state", "done"}},
                               {{"key", "new"},
                                {"source", lines("new-", 0, 70)},
                                {"translation", lines("new-trans-", 0, 70)},
                                {"state", "done"}}})}});
      auto partial = cache.get("partial")->at("records");
      check(partial.size() == 2 &&
                partial[0]["source"] == lines("old-", 50, 30) &&
                partial[0]["translation"] == lines("old-trans-", 50, 30) &&
                partial[1]["translation"] == lines("new-trans-", 0, 70),
            "100-line budget spans newest records and trims oldest retained "
            "record");
      cache.put(
          "stream",
          {{"records", json::array({{{"source", lines("unfinished-", 0, 130)},
                                     {"translation", "partial translation"},
                                     {"state", "translating"}}})}});
      check(cache.get("stream")->at("records")[0]["source"] ==
                lines("unfinished-", 30, 100),
            "unfinished record budget uses larger source or translation count");
      auto prioritized = json::array();
      for (int n = 0; n != 99; ++n)
        prioritized.push_back(
            {{"source", "s"}, {"translation", "t"}, {"state", "done"}});
      prioritized.push_back({{"source", lines("long-source-", 0, 120)},
                             {"translation", "one translation line"},
                             {"state", "done"}});
      cache.put("done", {{"records", prioritized}});
      auto done = cache.get("done")->at("records");
      check(done.size() == 100 &&
                done.back()["source"] == lines("long-source-", 20, 100),
            "completed record line budget follows translation and caps source "
            "tail");
    }
    check(std::distance(std::filesystem::directory_iterator(folder),
                        std::filesystem::directory_iterator()) == 1,
          "test directory only contains its explicit sentinel");
  } catch (const std::exception &error) {
    ++failures;
    std::cerr << "Unexpected exception: " << error.what() << '\n';
  }
  std::error_code ignored;
  const auto resolved = std::filesystem::weakly_canonical(folder, ignored);
  const auto intended_parent = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path(), ignored);
  if (!ignored && resolved.parent_path() == intended_parent &&
      resolved.filename().string().rfind("cclive-task-cache-test-", 0) == 0)
    std::filesystem::remove_all(resolved, ignored);
  std::cout << checks << " RAM-cache checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
