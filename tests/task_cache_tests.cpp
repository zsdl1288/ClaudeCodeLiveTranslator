#include "task_cache.hpp"
#include "markdown_table.hpp"
#include <algorithm>
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
      const TaskCache &retained = cache;
      check(retained.keys().empty(), "empty cache exposes no retained identities");
      check(!cache.get("A"), "unknown task misses");
      cache.put("A", {{"title", "Task A"},
                      {"translation", "甲任务译文"},
                      {"scroll", {{"y", 250}}},
                      {"follow", false}});
      cache.put("B", {{"title", "Task B"}, {"translation", "乙任务译文"}});
      const auto identities = retained.keys();
      check(identities.size() == 2 &&
                std::find(identities.begin(), identities.end(), "A") != identities.end() &&
                std::find(identities.begin(), identities.end(), "B") != identities.end(),
            "identity snapshot includes all currently retained tasks");
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
      check(retained.keys() == std::vector<std::string>{"B"},
            "erased identity is removed from enumeration");
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
      const auto identities = cache.keys();
      check(identities.size() == 2 &&
                std::find(identities.begin(), identities.end(), "A") != identities.end() &&
                std::find(identities.begin(), identities.end(), "C") != identities.end() &&
                std::find(identities.begin(), identities.end(), "B") == identities.end(),
            "identity enumeration obeys LRU task eviction");
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
      const auto identities = cache.keys();
      check(identities.size() == 2 &&
                std::find(identities.begin(), identities.end(), "A") == identities.end(),
            "identity enumeration excludes tasks evicted by total byte bound");
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
      const auto identities = cache.keys();
      check(identities.size() == 50 &&
                std::find(identities.begin(), identities.end(), "0") == identities.end() &&
                std::find(identities.begin(), identities.end(), "50") != identities.end(),
            "identity enumeration respects default maximum task count");
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
    {
      auto data_rows = [](bool translated, int first, int count) {
        std::string text;
        for (int n = first; n != first + count; ++n)
          text += std::string("| ") + (translated ? "镜头 " : "Shot ") +
                  std::to_string(n) + " | " + std::to_string(n * 11) +
                  (translated ? " 秒" : " sec") + " | $" +
                  std::to_string(n * 7) + " |\r\n";
        return text;
      };
      auto make_table = [&](bool translated, int first, int count) {
        return std::string(translated ? "| 镜头 | 时长 | 成本 |\r\n"
                                      : "| Shot | Time | Cost |\r\n") +
               "| --- | ---: | ---: |\r\n" + data_rows(translated, first, count);
      };
      auto cell_text = [](const std::string &text,
                          const std::vector<cclive::markdown::Line> &rows,
                          std::size_t row, std::size_t column) {
        const auto &line = rows.at(row);
        const auto &cell = line.cells.at(column);
        return text.substr(line.start + cell.start, cell.length);
      };
      const json table_record{{"key", "table-source"}, {"serial", 17},
                              {"source", make_table(false, 1, 150)},
                              {"translation", make_table(true, 1, 150)},
                              {"source_signature", "original-table-signature"},
                              {"state", "done"}, {"displayed_update", 23}};
      const json original_payload{{"records", json::array({table_record})},
                                  {"title", "Table task"},
                                  {"translation_scroll_y", 250}};
      const auto table_cache_path = folder / "table-cache-must-not-exist" / "cache.json";
      TaskCache cache(table_cache_path, true);
      cache.put("table-A", original_payload);
      const auto saved_payload = *cache.get("table-A");
      const auto saved_record = saved_payload.at("records").at(0);
      const auto source = saved_record.at("source").get<std::string>();
      const auto translation = saved_record.at("translation").get<std::string>();
      const auto source_rows = cclive::markdown::table(std::string_view(source));
      const auto translated_rows = cclive::markdown::table(std::string_view(translation));
      check(cache.healthy() && source_rows.size() == 100 && translated_rows.size() == 100,
            "100-line RAM cache keeps large source and translated tables parseable");
      bool values_match = source_rows.size() == 100 && translated_rows.size() == 100;
      if (values_match)
        for (std::size_t row = 2; row < translated_rows.size(); ++row) {
          const auto n = static_cast<int>(row) + 51;
          values_match = values_match &&
              cell_text(source, source_rows, row, 0) == "Shot " + std::to_string(n) &&
              cell_text(source, source_rows, row, 1) == std::to_string(n * 11) + " sec" &&
              cell_text(source, source_rows, row, 2) == "$" + std::to_string(n * 7) &&
              cell_text(translation, translated_rows, row, 0) == "镜头 " + std::to_string(n) &&
              cell_text(translation, translated_rows, row, 1) == std::to_string(n * 11) + " 秒" &&
              cell_text(translation, translated_rows, row, 2) == "$" + std::to_string(n * 7);
        }
      check(values_match,
            "trimmed table retains matching latest 98 shot duration and cost rows");
      check(!source_rows.empty() && !translated_rows.empty() &&
                cell_text(source, source_rows, 0, 0) == "Shot" &&
                cell_text(source, source_rows, 0, 2) == "Cost" &&
                cell_text(translation, translated_rows, 0, 0) == "镜头" &&
                cell_text(translation, translated_rows, 0, 2) == "成本",
            "large table cache retains both original and translated headers");
      cache.put("table-B", {{"records", json::array({{{"source", "Other task"},
                                                      {"translation", "其他任务"},
                                                      {"state", "done"}}})}});
      check(cache.get("table-A") == std::optional<json>(saved_payload) &&
                saved_record.at("source_signature") == "original-table-signature" &&
                saved_record.at("key") == "table-source" &&
                saved_record.at("displayed_update") == 23 &&
                saved_payload.at("translation_scroll_y") == 250,
            "switching RAM table tasks preserves source signature identity highlight and reading position");
      check(original_payload.at("records").at(0) == table_record &&
                cclive::markdown::table(std::string_view(
                    table_record.at("translation").get_ref<const std::string &>())).size() == 152,
            "table tail normalization leaves caller payload and source table untouched");
      for (int available = 1; available <= 3; ++available) {
        std::string following;
        for (int n = available; n < 100; ++n)
          following += "Following " + std::to_string(n) + "\r\n";
        cache.put("small-table-budget",
                  {{"records", json::array({table_record,
                      {{"source", following}, {"translation", following}, {"state", "done"}}})}});
        const auto small_records = cache.get("small-table-budget")->at("records");
        const auto small_translation = small_records.at(0).at("translation").get<std::string>();
        const auto small_rows = cclive::markdown::table(std::string_view(small_translation));
        if (available < 3)
          check(small_records.size() == 2 && small_rows.empty() &&
                    small_translation == data_rows(true, 151 - available, available) &&
                    small_records.at(0).at("source_signature") == "original-table-signature",
                "one or two remaining lines use stable plain tail without exceeding budget");
        else
          check(small_rows.size() == 3 &&
                    cell_text(small_translation, small_rows, 2, 0) == "镜头 150" &&
                    cell_text(small_translation, small_rows, 2, 2) == "$1050",
                "three remaining lines retain table header separator and newest numeric row");
      }
      cache.flush();
      check(!std::filesystem::exists(table_cache_path.parent_path()),
            "table normalization and flush remain RAM only without creating cache files");
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
