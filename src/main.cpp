#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Win32 base declarations must precede common-controls headers.
// clang-format off
#include <windows.h>
#include <ole2.h>
// clang-format on
#include "core.hpp"
#include "markdown_table.hpp"
#include "chatgpt.hpp"
#include "reader.hpp"
#include "resource.h"
#include "task_cache.hpp"
#include "text_layout.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <commctrl.h>

#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <richedit.h>
#include <set>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
namespace {
constexpr COLORREF BG = RGB(38, 38, 36), PANEL = RGB(42, 42, 40),
                   FG = RGB(218, 216, 210), MUTED = RGB(157, 155, 147),
                   ACCENT = RGB(210, 146, 114), HEADING = RGB(240, 238, 232);
void darkCaption(HWND window) {
  // Only this application's own windows; no process-wide/client theme changes.
  static HMODULE library = LoadLibraryW(L"dwmapi.dll");
  using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
  auto set = library ? reinterpret_cast<SetAttribute>(
                           GetProcAddress(library, "DwmSetWindowAttribute"))
                     : nullptr;
  if (!set)
    return;
  BOOL enabled = TRUE;
  if (FAILED(set(window, 20, &enabled, sizeof(enabled))))
    set(window, 19, &enabled, sizeof(enabled));
  COLORREF caption = BG, foreground = HEADING, border = RGB(65, 63, 58);
  set(window, 35, &caption, sizeof(caption));
  set(window, 36, &foreground, sizeof(foreground));
  set(window, 34, &border, sizeof(border));
}
void nativeDarkControl(HWND control, const wchar_t *theme) {
  static HMODULE library = LoadLibraryW(L"uxtheme.dll");
  using SetTheme = HRESULT(WINAPI *)(HWND, LPCWSTR, LPCWSTR);
  auto set = library ? reinterpret_cast<SetTheme>(
                           GetProcAddress(library, "SetWindowTheme"))
                     : nullptr;
  if (set)
    set(control, theme, nullptr);
}
HICON applicationIcon(int size) {
  return reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                            MAKEINTRESOURCEW(IDI_APP_ICON),
                                            IMAGE_ICON, size, size, LR_SHARED));
}
bool isCheckBox(HWND window) {
  auto kind = GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK;
  return kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX;
}
void paintButton(HWND button, HDC dc, RECT area, UINT state) {
  bool enabled = IsWindowEnabled(button), check = isCheckBox(button);
  bool hot = GetPropW(button, L"CCLive.Hover") != nullptr;
  bool pressed = (state & ODS_SELECTED) ||
                 (SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED);
  bool focused = GetFocus() == button && !(state & ODS_NOFOCUSRECT);
  auto primary = GetDlgCtrlID(button) == IDOK ||
                 GetPropW(button, L"CCLive.Primary") != nullptr;
  int height = area.bottom - area.top;
  int corner = std::max(4, height / 4);
  COLORREF fill = check      ? BG
                  : !enabled ? RGB(43, 43, 40)
                  : pressed  ? RGB(78, 66, 56)
                  : hot      ? RGB(65, 61, 54)
                  : primary  ? RGB(80, 59, 48)
                             : RGB(49, 49, 45);
  COLORREF border = !enabled             ? RGB(57, 56, 51)
                    : focused || primary ? ACCENT
                    : hot                ? RGB(124, 110, 93)
                                         : RGB(78, 75, 67);
  auto brush = CreateSolidBrush(fill);
  auto pen = CreatePen(PS_SOLID, 1, border);
  auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
  if (check) {
    FillRect(dc, &area, brush);
    int box = std::max(12, height * 3 / 5), y = (height - box) / 2;
    RECT mark{area.left + 1, area.top + y, area.left + 1 + box,
              area.top + y + box};
    auto markBrush = CreateSolidBrush(
        SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED ? ACCENT
                                                               : PANEL);
    SelectObject(dc, markBrush);
    RoundRect(dc, mark.left, mark.top, mark.right, mark.bottom, 5, 5);
    SelectObject(dc, brush);
    DeleteObject(markBrush);
    if (SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED) {
      auto tick = CreatePen(PS_SOLID, std::max(1, box / 8), BG);
      SelectObject(dc, tick);
      MoveToEx(dc, mark.left + box / 4, mark.top + box / 2, nullptr);
      LineTo(dc, mark.left + box * 2 / 5, mark.top + box * 3 / 4);
      LineTo(dc, mark.left + box * 4 / 5, mark.top + box / 4);
      SelectObject(dc, pen);
      DeleteObject(tick);
    }
    area.left = mark.right + std::max(5, height / 5);
  } else
    RoundRect(dc, area.left, area.top, area.right, area.bottom, corner, corner);
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(pen);
  DeleteObject(brush);
  auto previousFont = SelectObject(
      dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, enabled ? FG : RGB(111, 109, 102));
  wchar_t caption[128]{};
  GetWindowTextW(button, caption, 128);
  DrawTextW(dc, caption, -1, &area,
            DT_SINGLELINE | DT_VCENTER | (check ? DT_LEFT : DT_CENTER) |
                ((state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
  SelectObject(dc, previousFont);
  if (focused && !check) {
    InflateRect(&area, -4, -4);
    SetTextColor(dc, ACCENT);
    DrawFocusRect(dc, &area);
  }
}
LRESULT CALLBACK themedButtonProc(HWND window, UINT message, WPARAM wp,
                                  LPARAM lp, UINT_PTR id, DWORD_PTR) {
  if (message == WM_NCDESTROY) {
    RemovePropW(window, L"CCLive.Hover");
    RemovePropW(window, L"CCLive.Primary");
    RemoveWindowSubclass(window, themedButtonProc, id);
    return DefSubclassProc(window, message, wp, lp);
  }
  if (message == WM_MOUSEMOVE && !GetPropW(window, L"CCLive.Hover")) {
    SetPropW(window, L"CCLive.Hover", reinterpret_cast<HANDLE>(1));
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
    InvalidateRect(window, nullptr, FALSE);
  } else if (message == WM_MOUSELEAVE) {
    RemovePropW(window, L"CCLive.Hover");
    InvalidateRect(window, nullptr, FALSE);
  }
  if ((message == WM_PAINT || message == WM_PRINTCLIENT) &&
      isCheckBox(window)) {
    PAINTSTRUCT paint{};
    HDC dc = message == WM_PAINT ? BeginPaint(window, &paint)
                                 : reinterpret_cast<HDC>(wp);
    RECT area{};
    GetClientRect(window, &area);
    paintButton(window, dc, area, 0);
    if (message == WM_PAINT)
      EndPaint(window, &paint);
    return 0;
  }
  auto result = DefSubclassProc(window, message, wp, lp);
  if (message == WM_GETDLGCODE &&
      GetPropW(window, L"CCLive.Primary") != nullptr)
    return (result & ~DLGC_UNDEFPUSHBUTTON) | DLGC_DEFPUSHBUTTON;
  if (message == WM_ENABLE || message == WM_SETFOCUS ||
      message == WM_KILLFOCUS || message == BM_SETCHECK ||
      message == BM_SETSTATE || message == WM_UPDATEUISTATE)
    InvalidateRect(window, nullptr, FALSE);
  return result;
}
void themeButton(HWND button, bool primary = false) {
  if (primary)
    SetPropW(button, L"CCLive.Primary", reinterpret_cast<HANDLE>(1));
  SetWindowSubclass(button, themedButtonProc, 2, 0);
}
std::wstring wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              (int)s.size(), nullptr, 0);
  if (n <= 0)
    return L"文字编码无效";
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}
std::string utf8(const std::wstring &s) {
  if (s.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string out(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n,
                      nullptr, nullptr);
  return out;
}
std::wstring textOf(HWND w) {
  int n = GetWindowTextLengthW(w);
  std::wstring s(n + 1, 0);
  GetWindowTextW(w, s.data(), n + 1);
  s.resize(n);
  return s;
}
fs::path exePath() {
  std::vector<wchar_t> p(32768);
  auto n = GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size());
  return std::wstring(p.data(), n);
}
void writeFile(const fs::path &p, const std::string &s) {
  if (!p.parent_path().empty())
    fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f || !f.write(s.data(), (std::streamsize)s.size()))
    throw std::runtime_error("文件保存失败");
}
void stdoutText(const std::string &s) {
  DWORD n = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &n,
            nullptr);
}
struct Handle {
  HANDLE h = nullptr;
  Handle() = default;
  explicit Handle(HANDLE v) : h(v) {}
  ~Handle() {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  HANDLE release() {
    auto v = h;
    h = nullptr;
    return v;
  }
};
std::size_t logicalLines(const std::string &text) {
  auto end = text.size();
  while (end && (text[end - 1] == '\r' || text[end - 1] == '\n'))
    --end;
  if (!end)
    return 0;
  std::size_t lines = 1;
  for (std::size_t index = 0; index < end; ++index) {
    if (text[index] == '\r') {
      ++lines;
      if (index + 1 < end && text[index + 1] == '\n')
        ++index;
    } else if (text[index] == '\n')
      ++lines;
  }
  return lines;
}
std::string tailLines(const std::string &text, std::size_t keep) {
  if (auto table = cclive::markdown::table_tail(text, keep)) return *table;
  if (!keep)
    return {};
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
    if (lines++ == keep)
      return text.substr(begin);
  }
  return text;
}
std::string contentSignature(const std::string &text) {
  uint64_t value = 1469598103934665603ULL;
  for (unsigned char c : text) {
    value ^= c;
    value *= 1099511628211ULL;
  }
  return std::to_string(value);
}
std::size_t characterCount(const std::string &text) {
  return std::count_if(text.begin(), text.end(),
                       [](unsigned char c) { return (c & 0xc0) != 0x80; });
}
void atomicStatusFile(const fs::path &path, const std::string &payload) {
  if (!path.parent_path().empty())
    fs::create_directories(path.parent_path());
  static std::atomic_uint sequence{0};
  auto temporary = path;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
               std::to_wstring(++sequence) + L".tmp";
  try {
    writeFile(temporary, payload);
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("无法更新诊断状态文件");
  } catch (...) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    throw;
  }
}
struct DiagnosticWriter {
  std::atomic_bool busy{false};
  std::atomic_uint completed{0};
  std::atomic_bool failed{false};
};
struct Bus {
  std::mutex mutex;
  std::deque<json> source, misc;
  std::optional<json> reset, telemetry;
  uint64_t captureSequence = 0, resultSequence = 0;
  std::map<std::string, json> results;
  void emit(json v) {
    std::lock_guard<std::mutex> lock(mutex);
    auto t = v.value("type", "");
    {
      auto &q = (t == "task" || t == "text" || t == "reset") ? source : misc;
      if (q.size() >= 512)
        q.pop_front();
      q.push_back(std::move(v));
    }
  }
  void capture(uint64_t generation, const json &snapshot) {
    std::size_t blocks = 0, chars = 0;
    if (snapshot.contains("blocks") && snapshot["blocks"].is_array()) {
      blocks = snapshot["blocks"].size();
      for (const auto &block : snapshot["blocks"])
        if (block.is_object() && block.contains("text") &&
            block["text"].is_string())
          chars += characterCount(block["text"].get_ref<const std::string &>());
    }
    double duration = 0;
    if (snapshot.contains("duration_ms") && snapshot["duration_ms"].is_number())
      duration = snapshot["duration_ms"].get<double>();
    std::lock_guard<std::mutex> lock(mutex);
    if (telemetry && telemetry->value("_generation", 0ULL) > generation)
      return;
    telemetry = {{"_generation", generation},
                 {"capture_seq", ++captureSequence},
                 {"last_read_ms", duration},
                 {"captured_blocks", blocks},
                 {"captured_chars", chars}};
  }
  void result(json v) {
    std::lock_guard<std::mutex> lock(mutex);
    auto k = std::to_string(v.value("generation", 0ULL)) + "|" +
             v.value("key", "") + "|" +
             std::to_string(v.value("version", 0ULL));
    // Coalesce each job's stream while retaining its latest arrival time.
    // String keys such as record-10 and record-9 are not chronological.
    v["_result_sequence"] = ++resultSequence;
    results[k] = std::move(v);
    while (results.size() > 256) {
      const auto oldest = std::min_element(
          results.begin(), results.end(), [](const auto &left, const auto &right) {
            return left.second.value("_result_sequence", 0ULL) <
                   right.second.value("_result_sequence", 0ULL);
          });
      results.erase(oldest);
    }
  }
  static std::vector<json>
  orderedResults(std::map<std::string, json> completed) {
    std::vector<json> ordered;
    ordered.reserve(completed.size());
    for (auto &entry : completed)
      ordered.push_back(std::move(entry.second));
    std::sort(ordered.begin(), ordered.end(), [](const json &left, const json &right) {
      return left.value("_result_sequence", 0ULL) <
             right.value("_result_sequence", 0ULL);
    });
    return ordered;
  }
};
struct Job {
  uint64_t generation = 0, version = 0;
  std::string key, source, taskId;
  cclive::Settings settings;
  std::shared_ptr<std::atomic_bool> cancel =
      std::make_shared<std::atomic_bool>(false);
};
struct Worker {
  using Translator = std::function<std::string(
      const cclive::Settings &, const std::string &, const cclive::Delta &,
      const std::atomic_bool &)>;
  struct Cooldown {
    Clock::time_point until{};
    unsigned strikes = 0;
  };
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::shared_ptr<Job>> pending;
  std::shared_ptr<Job> active;
  std::map<std::string, Cooldown> cooldowns;
  bool shutdown = false;
  std::atomic_uint64_t submitted{0};
  std::shared_ptr<Bus> bus;
  Translator translator;
  explicit Worker(std::shared_ptr<Bus> b,
                  Translator translate = cclive::translate)
      : bus(std::move(b)), translator(std::move(translate)) {}
  static std::string serviceKey(const cclive::Settings &s) {
    return cclive::translation_service_key(s);
  }
  void cancel(const std::string &key) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto i = pending.begin(); i != pending.end();)
      if ((*i)->key == key) {
        (*i)->cancel->store(true);
        i = pending.erase(i);
      } else
        ++i;
    if (active && active->key == key)
      active->cancel->store(true);
    changed.notify_all();
  }
  void cancelAll() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto &j : pending)
      j->cancel->store(true);
    pending.clear();
    if (active)
      active->cancel->store(true);
    changed.notify_all();
  }
  bool submit(std::shared_ptr<Job> j) {
    std::lock_guard<std::mutex> lock(mutex);
    if (shutdown || pending.size() >= 128)
      return false;
    pending.push_back(std::move(j));
    ++submitted;
    changed.notify_one();
    return true;
  }
  void stop() {
    cancelAll();
    std::lock_guard<std::mutex> lock(mutex);
    shutdown = true;
    changed.notify_all();
  }
  void run() {
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return shutdown || !pending.empty(); });
        if (shutdown)
          return;
        j = pending.front();
        pending.pop_front();
        active = j;
      }
      std::string service;
      std::string partial;
      auto publish = [&](const std::string &state,
                         const std::string &error = "") {
        bus->result({{"generation", j->generation},
                     {"key", j->key},
                     {"task_id", j->taskId},
                     {"version", j->version},
                     {"state", state},
                     {"translation", partial},
                     {"error", error}});
      };
      unsigned retries = 0;
      while (!j->cancel->load()) {
        try {
          if (service.empty())
            service = serviceKey(j->settings);
        } catch (const std::exception &e) {
          publish("error", e.what());
          break;
        }
        // A service cooldown also holds queued sentences and manual retries.
        // Wait only on the worker; pause, cancellation and shutdown wake it.
        {
          std::unique_lock<std::mutex> lock(mutex);
          Clock::time_point announced{};
          for (;;) {
            if (shutdown || j->cancel->load())
              break;
            const auto gate = cooldowns.find(service);
            if (gate == cooldowns.end() || gate->second.until <= Clock::now())
              break;
            const auto until = gate->second.until;
            if (announced != until) {
              const auto seconds =
                  (std::chrono::duration_cast<std::chrono::milliseconds>(
                       until - Clock::now())
                       .count() +
                   999) /
                  1000;
              announced = until;
              lock.unlock();
              publish("rate_limited",
                      "翻译服务限流，等待约 " +
                          std::to_string((std::max)(int64_t(1), seconds)) +
                          " 秒后自动重试；可暂停或停止");
              lock.lock();
              continue;
            }
            changed.wait_until(lock, until);
          }
          if (shutdown || j->cancel->load())
            break;
        }
        partial.clear();
        try {
          publish("translating");
          auto last = Clock::now();
          auto result = translator(
              j->settings, j->source,
              [&](const std::string &delta) {
                if (j->cancel->load())
                  return;
                partial += delta;
                if (Clock::now() - last >= std::chrono::milliseconds(80)) {
                  publish("translating");
                  last = Clock::now();
                }
              },
              *j->cancel);
          if (!j->cancel->load()) {
            partial = std::move(result);
            {
              std::lock_guard<std::mutex> lock(mutex);
              cooldowns.erase(service);
            }
            publish("done");
          }
          break;
        } catch (const cclive::TranslationRateLimited &e) {
          partial.clear();
          {
            std::lock_guard<std::mutex> lock(mutex);
            auto &gate = cooldowns[service];
            const auto exponent = (std::min)(gate.strikes, 4U);
            gate.strikes = (std::min)(gate.strikes + 1, 5U);
            const auto backoff =
                (std::min)(int64_t(900000), int64_t(60000) << exponent);
            const auto delay = (std::max)(e.retry_after_ms, backoff);
            gate.until =
                (std::max)(gate.until,
                           Clock::now() + std::chrono::milliseconds(delay));
          }
          if (j->cancel->load())
            break;
          if (retries++ < 3)
            continue;
          publish("error", "翻译服务持续限流，已停止本段自动重试。"
                           "请稍后点击重试失败，或在设置中选择其他翻译服务");
          break;
        } catch (const std::exception &e) {
          if (!j->cancel->load())
            publish("error", e.what());
          break;
        } catch (...) {
          if (!j->cancel->load())
            publish("error", "翻译服务发生错误，请检查设置");
          break;
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (active == j)
          active.reset();
      }
    }
  }
};
json workerRateLimitChecks() {
  // Inject a provider: no network. Advance the deadline only after verifying
  // that the actual worker holds both the limited job and following sentences.
  json checks = json::array();
  auto check = [&](const char *name, bool passed) {
    checks.push_back({{"name", name}, {"passed", passed}});
  };
  auto await = [](const std::function<bool()> &ready) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    do {
      if (ready())
        return true;
      Sleep(5);
    } while (Clock::now() < deadline);
    return ready();
  };
  auto job = [](const char *key, const char *provider = "google") {
    auto j = std::make_shared<Job>();
    j->key = j->source = key;
    j->settings.provider = provider;
    return j;
  };
  auto state = [](const std::shared_ptr<Bus> &bus, const std::string &key) {
    std::lock_guard<std::mutex> lock(bus->mutex);
    const auto found = bus->results.find("0|" + key + "|0");
    return found == bus->results.end() ? std::string()
                                       : found->second.value("state", "");
  };
  auto expire = [](const std::shared_ptr<Worker> &w,
                   const std::shared_ptr<Job> &j) {
    std::lock_guard<std::mutex> lock(w->mutex);
    w->cooldowns[Worker::serviceKey(j->settings)].until = Clock::now();
    w->changed.notify_all();
  };
  {
    auto bus = std::make_shared<Bus>();
    std::atomic_uint calls{0};
    auto w = std::make_shared<Worker>(
        bus, [&](const cclive::Settings &, const std::string &text,
                 const cclive::Delta &delta, const std::atomic_bool &) {
          if (++calls == 1) {
            if (delta)
              delta("discarded partial");
            throw cclive::TranslationRateLimited(1000);
          }
          if (delta)
            delta("translated " + text);
          return "translated " + text;
        });
    auto first = job("first"), second = job("second");
    w->submit(first);
    w->submit(second);
    std::thread thread([w] { w->run(); });
    const bool cooling =
        await([&] { return state(bus, "first") == "rate_limited"; });
    Sleep(30);
    check("http_429_holds_following_sentences_without_request_storm",
          cooling && calls.load() == 1 && state(bus, "second").empty());
    expire(w, first);
    const bool resumed = await([&] { return state(bus, "second") == "done"; });
    check("rate_limited_sentence_and_queue_resume_after_cooldown",
          resumed && calls.load() == 3 && state(bus, "first") == "done");
    {
      std::lock_guard<std::mutex> lock(bus->mutex);
      const auto found = bus->results.find("0|first|0");
      check("http_429_retry_discards_partial_translation_before_restart",
            found != bus->results.end() &&
                found->second.value("translation", "") == "translated first");
    }
    w->stop();
    thread.join();
  }
  {
    auto bus = std::make_shared<Bus>();
    std::atomic_uint calls{0};
    auto w = std::make_shared<Worker>(
        bus, [&](const cclive::Settings &s, const std::string &,
                 const cclive::Delta &, const std::atomic_bool &) {
          ++calls;
          if (s.provider == "google")
            throw cclive::TranslationRateLimited(600000);
          return std::string("local translation");
        });
    auto first = job("cancelled"), other = job("other", "ollama");
    w->submit(first);
    w->submit(other);
    std::thread thread([w] { w->run(); });
    const bool cooling =
        await([&] { return state(bus, "cancelled") == "rate_limited"; });
    {
      std::lock_guard<std::mutex> lock(w->mutex);
      const auto gate = w->cooldowns.find(Worker::serviceKey(first->settings));
      check("server_retry_after_longer_than_local_backoff_is_respected",
            cooling && gate != w->cooldowns.end() &&
                gate->second.until - Clock::now() > std::chrono::seconds(595));
    }
    w->cancel(first->key);
    const bool switched = await([&] { return state(bus, "other") == "done"; });
    check("cancel_during_cooldown_allows_another_service_to_proceed",
          cooling && switched && calls.load() == 2);
    auto retried = job("manual-retry");
    w->submit(retried);
    const bool held =
        await([&] { return state(bus, "manual-retry") == "rate_limited"; });
    check("manual_retry_does_not_bypass_shared_service_cooldown",
          held && calls.load() == 2);
    const auto beforeStop = Clock::now();
    w->stop();
    thread.join();
    check("worker_shutdown_interrupts_long_rate_limit_wait",
          Clock::now() - beforeStop < std::chrono::seconds(1));
  }
  {
    auto bus = std::make_shared<Bus>();
    std::atomic_uint calls{0};
    auto w = std::make_shared<Worker>(
        bus,
        [&](const cclive::Settings &, const std::string &,
            const cclive::Delta &, const std::atomic_bool &) -> std::string {
          ++calls;
          throw cclive::TranslationRateLimited(1000);
        });
    auto first = job("persistent-limit");
    w->submit(first);
    std::thread thread([w] { w->run(); });
    bool bounded = true, backoff = true;
    for (unsigned attempt = 1; attempt <= 4; ++attempt) {
      const std::string wanted = attempt < 4 ? "rate_limited" : "error";
      bounded =
          await([&] {
            return calls.load() == attempt && state(bus, first->key) == wanted;
          }) &&
          bounded;
      {
        std::lock_guard<std::mutex> lock(w->mutex);
        const auto gate =
            w->cooldowns.find(Worker::serviceKey(first->settings));
        const auto expected =
            std::chrono::milliseconds(int64_t(60000) << (attempt - 1));
        backoff = gate != w->cooldowns.end() &&
                  gate->second.until - Clock::now() >=
                      expected - std::chrono::seconds(2) &&
                  backoff;
      }
      if (attempt < 4)
        expire(w, first);
    }
    check("repeated_http_429_uses_increasing_service_backoff", backoff);
    check("http_429_auto_retry_is_bounded_to_three_retries",
          bounded && calls.load() == 4);
    w->stop();
    thread.join();
  }
  {
    auto bus = std::make_shared<Bus>();
    std::atomic_uint calls{0};
    auto w = std::make_shared<Worker>(bus,
        [&](const cclive::Settings &, const std::string &,
            const cclive::Delta &, const std::atomic_bool &) {
          ++calls;
          return std::string("valid translation");
        });
    auto invalid = job("invalid-service", "openai");
    invalid->settings.base_url = "https://user:pass@example.com";
    auto valid = job("after-invalid", "demo");
    w->submit(invalid);
    w->submit(valid);
    std::thread thread([w] { w->run(); });
    const bool survived = await([&] { return state(bus, valid->key) == "done"; });
    check("invalid_service_address_reports_error_and_worker_continues",
          survived && state(bus, invalid->key) == "error" && calls.load() == 1);
    w->stop();
    thread.join();
  }
  return checks;
}
std::vector<wchar_t> readerEnvironment(const std::vector<std::string> &secrets) {
  std::set<std::wstring> allowed = {L"SYSTEMROOT",   L"WINDIR",
                                    L"PATH",         L"TEMP",
                                    L"TMP",          L"USERPROFILE",
                                    L"APPDATA",      L"LOCALAPPDATA",
                                    L"HOMEDRIVE",    L"HOMEPATH",
                                    L"COMSPEC",      L"PROCESSOR_ARCHITECTURE",
                                    L"PROGRAMFILES", L"PROGRAMFILES(X86)"};
  std::set<std::wstring> excluded;
  for (const auto &secret : secrets) {
    auto name = wide(secret);
    std::transform(name.begin(), name.end(), name.begin(), towupper);
    if (!name.empty())
      excluded.insert(std::move(name));
  }
  std::vector<wchar_t> env;
  for (auto &name : allowed) {
    if (excluded.count(name))
      continue;
    DWORD n = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (!n)
      continue;
    std::wstring val(n, 0);
    GetEnvironmentVariableW(name.c_str(), val.data(), n);
    if (!val.empty() && val.back() == 0)
      val.pop_back();
    auto pair = name + L"=" + val;
    env.insert(env.end(), pair.begin(), pair.end());
    env.push_back(0);
  }
  env.push_back(0);
  if (env.size() == 1)
    env.push_back(0);
  return env;
}
struct Watcher {
  uint64_t hwnd, generation;
  uint64_t lastClaudeHwnd = 0, lastForeground = 0;
  std::string currentTaskId;
  unsigned consistentTaskSnapshots = 0;
  std::map<std::string, std::size_t> sourceIndices;
  std::map<std::string, json> forwardedSlots;
  std::map<std::string, std::string> acceptedBodies;
  std::map<std::string, std::string> blockAliases, acceptedKinds;
  uint64_t forwardedOrder = 0;
  bool sourceIdentityPending = false;
  std::set<std::string> previousBlockKeys;
  std::size_t previousSourceCount = 0, previousMinimum = 0;
  uint64_t windowReplayNonce = 0;
  std::size_t minimumSourceIndex = 0;
  json currentTask;
  int settle;
  std::vector<std::string> secretEnvs;
  std::atomic_bool stopped{false};
  std::mutex mutex;
  HANDLE process = nullptr;
  std::shared_ptr<Bus> bus;
  std::string lastStatus;
  Watcher(uint64_t h, uint64_t g, int s, std::vector<std::string> keys,
          std::shared_ptr<Bus> b)
      : hwnd(h), generation(g), settle(s), secretEnvs(std::move(keys)),
        bus(std::move(b)) {}
  void emit(json v) {
    if (!stopped.load()) {
      v["_generation"] = generation;
      bus->emit(std::move(v));
    }
  }
  void status(const std::string &s) {
    if (s != lastStatus) {
      lastStatus = s;
      emit({{"type", "status"}, {"text", s}});
    }
  }
  void stop() {
    stopped.store(true);
    std::lock_guard<std::mutex> lock(mutex);
    if (process)
      TerminateProcess(process, 0);
  }
  static bool claudeWindow(uint64_t handle) {
    HWND window = reinterpret_cast<HWND>(handle);
    if (!window || !IsWindow(window))
      return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.h)
      return false;
    wchar_t path[32768]{};
    DWORD count = 32768;
    if (!QueryFullProcessImageNameW(process.h, 0, path, &count))
      return false;
    std::wstring name(path, count);
    auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
      name.erase(0, slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), towlower);
    return name == L"claude.exe";
  }
  uint64_t chooseWindow() {
    auto foreground =
        reinterpret_cast<uint64_t>(GetAncestor(GetForegroundWindow(), GA_ROOT));
    if (foreground != lastForeground) {
      lastForeground = foreground;
      if (claudeWindow(foreground))
        lastClaudeHwnd = foreground;
    }
    if (lastClaudeHwnd && lastClaudeHwnd != hwnd &&
        claudeWindow(lastClaudeHwnd))
      return lastClaudeHwnd;
    if (claudeWindow(hwnd))
      return hwnd;
    if (claudeWindow(lastClaudeHwnd))
      return lastClaudeHwnd;
    auto windows =
        json::parse(cclive::enumerate_windows_json(), nullptr, false);
    if (windows.is_array())
      for (const auto &item : windows) {
        auto candidate = item.value("hwnd", 0ULL);
        if (claudeWindow(candidate)) {
          lastClaudeHwnd = candidate;
          return candidate;
        }
      }
    return 0;
  }
  struct WindowChanged : std::runtime_error {
    WindowChanged() : std::runtime_error("已跟随当前 Claude 窗口") {}
  };
  void publishSnapshot(const json &snapshot, cclive::Ledger &ledger) {
    auto nextTaskId = snapshot.value("task_id", snapshot.value("context", ""));
    const auto quality = snapshot.value("identity_quality", "view_fallback");
    const auto sourceHwnd = snapshot.value("source_hwnd", hwnd);
    const auto sourcePid = snapshot.value("source_pid", 0ULL);
    // A reader restart can lose its URL cache. Never publish an unverified
    // fallback body into a task previously identified by URL on this window.
    const bool unverifiedFallback = !currentTaskId.empty() &&
        currentTask.value("identity_quality", "") == "url" && quality != "url" &&
        currentTask.value("source_hwnd", 0ULL) == sourceHwnd &&
        currentTask.value("source_pid", 0ULL) == sourcePid;
    sourceIdentityPending = snapshot.value("identity_pending", false) ||
                            unverifiedFallback;
    if (nextTaskId.empty() || unverifiedFallback ||
        (sourceIdentityPending &&
         (nextTaskId == currentTaskId || quality != "url")))
      return;
    const bool taskChanged = nextTaskId != currentTaskId;
    if (taskChanged) {
      consistentTaskSnapshots = 0;
      forwardedSlots.clear();
      acceptedBodies.clear();
      blockAliases.clear();
      acceptedKinds.clear();
      forwardedOrder = 0;
      sourceIndices.clear();
      previousBlockKeys.clear();
      previousSourceCount = previousMinimum = minimumSourceIndex = 0;
      windowReplayNonce = 0;
    }
    currentTaskId = nextTaskId;
    currentTask = {{"task_id", currentTaskId},
                   {"task_title", snapshot.value("task_title", "Claude 任务")},
                   {"identity_quality", quality},
                   {"source_hwnd", sourceHwnd},
                   {"source_pid", sourcePid}};
    auto identityOnly = [&] {
      auto event = currentTask;
      event["type"] = "task";
      event["identity_pending"] = sourceIdentityPending;
      emit(std::move(event));
    };
    auto ledgerContext = [&] {
      return currentTaskId + (windowReplayNonce
                                  ? "|window:" + std::to_string(windowReplayNonce)
                                  : "");
    };
    auto unavailableBody = [&] {
      if (taskChanged) {
        identityOnly();
        return;
      }
      // A temporarily empty view must suspend pending source, while completed
      // captions and their aliases remain intact for the same task.
      for (auto event : ledger.observe({{"context", ledgerContext()},
                                       {"blocks", json::array()}}))
        publishLedger(std::move(event));
    };
    if (sourceIdentityPending) {
      // A new explicit URL is sufficient to switch tasks, but its body is not
      // trusted until the reader verifies that identity after extraction.
      identityOnly();
      return;
    }
    if (!snapshot.contains("blocks") || !snapshot["blocks"].is_array() ||
        snapshot["blocks"].empty()) {
      unavailableBody();
      return;
    }
    auto effective = snapshot;
    effective["blocks"] = json::array();
    std::map<std::string, std::string> nextBodies, nextKinds, nextAliases;
    std::set<std::string> surviving, reused;
    for (const auto &raw : snapshot["blocks"])
      if (raw.is_object() && raw.contains("key") && raw["key"].is_string()) {
        auto key = raw["key"].get<std::string>();
        if (auto found = blockAliases.find(key); found != blockAliases.end())
          key = found->second;
        if (acceptedBodies.count(key)) surviving.insert(key);
      }
    auto overlapCount = [](const std::string &older, const std::string &newer) {
      const auto before = cclive::split_sentences(older);
      const auto after = cclive::split_sentences(newer);
      std::size_t prefix = 0;
      while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix])
        ++prefix;
      std::size_t suffix = 0;
      for (auto count = std::min(before.size(), after.size()); count > 0; --count)
        if (std::equal(before.end() - count, before.end(), after.begin())) {
          suffix = count;
          break;
        }
      std::size_t expanded = 0;
      if (before.size() >= 2 && after.size() > before.size())
        for (std::size_t offset = 1; offset + before.size() <= after.size(); ++offset)
          if (std::equal(before.begin(), before.end(), after.begin() + offset)) {
            expanded = before.size(); break;
          }
      return std::max({prefix, suffix, expanded});
    };
    bool slidingWindow = false, expandedWindow = false;
    for (const auto &raw : snapshot["blocks"]) {
      if (effective["blocks"].size() >= 128)
        break;
      if (!raw.is_object() || !raw.contains("key") ||
          !raw["key"].is_string() || !raw.contains("text") ||
          !raw["text"].is_string())
        continue;
      auto key = raw["key"].get<std::string>();
      auto body = raw["text"].get<std::string>();
      // Match the reader's 128 blocks / 64000 UTF-16-unit acquisition bound.
      if (key.empty() || key.size() > 4096 || body.empty() ||
          body.size() > 64000 * 3)
        continue;
      const auto rawKey = key;
      const auto kind = raw.value("kind", std::string("answer"));
      if (auto found = blockAliases.find(key); found != blockAliases.end())
        key = found->second;
      if (!acceptedBodies.count(key)) {
        // Rebuilds replace runtime IDs while preserving ordered source text.
        // Only absent blocks can be remapped, once each; a repeated new block
        // beside a surviving block remains an independent occurrence.
        std::string matched;
        std::size_t best = 0;
        bool ambiguous = false;
        for (const auto &previous : acceptedBodies) {
          if (surviving.count(previous.first) || reused.count(previous.first) ||
              acceptedKinds[previous.first] != kind)
            continue;
          auto score = overlapCount(previous.second, body);
          if (score < 2 && !(score == 1 && body == previous.second && body.size() >= 80))
            continue;
          if (score > best) { best = score; matched = previous.first; ambiguous = false; }
          else if (score == best) ambiguous = true;
        }
        if (!matched.empty() && !ambiguous) key = matched;
      }
      reused.insert(key);
      auto previous = acceptedBodies.find(key);
      if (previous != acceptedBodies.end() && previous->second != body) {
        auto before = cclive::split_sentences(previous->second);
        auto after = cclive::split_sentences(body);
        if (before.size() >= 2 && after.size() > before.size())
          for (std::size_t offset = 1; offset + before.size() <= after.size(); ++offset)
            if (std::equal(before.begin(), before.end(), after.begin() + offset)) {
              expandedWindow = true; break;
            }
        for (auto count = std::min(before.size(), after.size()); count >= 2; --count)
          if (count < before.size() &&
              std::equal(before.end() - count, before.end(), after.begin())) {
            slidingWindow = true;
            break;
          }
      }
      if (previous != acceptedBodies.end() &&
          body.size() < previous->second.size() &&
          previous->second.compare(0, body.size(), body) == 0)
        body = previous->second;
      auto block = raw;
      block["key"] = key;
      block["text"] = body;
      effective["blocks"].push_back(std::move(block));
      nextBodies[key] = std::move(body);
      nextAliases[rawKey] = key;
      nextKinds[key] = kind;
    }
    if (effective["blocks"].empty()) {
      unavailableBody();
      return;
    }
    // Keep only the currently observed block set; no historical body cache.
    acceptedBodies = std::move(nextBodies);
    blockAliases = std::move(nextAliases);
    acceptedKinds = std::move(nextKinds);
    ++consistentTaskSnapshots;
    if (consistentTaskSnapshots < 2 && previousSourceCount)
      return; // Reconnection must not publish an empty source window.
    auto meta = currentTask;
    meta["type"] = "task";
    meta["source_block_keys"] = json::array();
    meta["source_signatures"] = json::array();
    meta["source_index_blocks"] = json::array();
    sourceIndices.clear();
    std::vector<std::size_t> englishPositions;
    std::set<std::string> blockKeys;
    if (consistentTaskSnapshots >= 2) {
      for (const auto &block : effective["blocks"]) {
        if (!block.is_object() || !block.contains("key") ||
            !block["key"].is_string())
          continue;
        meta["source_block_keys"].push_back(block["key"]);
        blockKeys.insert(block["key"].get<std::string>());
        if (!block.contains("text") || !block["text"].is_string())
          continue;
        auto sentences =
            cclive::split_sentences(block["text"].get<std::string>());
        if (sentences.size() > 4096)
          sentences.resize(4096);
        for (std::size_t index = 0; index < sentences.size(); ++index) {
          if (std::any_of(sentences[index].begin(), sentences[index].end(),
                          [](unsigned char c) {
                            return (c >= 'A' && c <= 'Z') ||
                                   (c >= 'a' && c <= 'z');
                          }))
            englishPositions.push_back(meta["source_signatures"].size());
          sourceIndices[json::array({block["key"], index}).dump()] =
              meta["source_signatures"].size();
          meta["source_signatures"].push_back(
              contentSignature(sentences[index]));
          meta["source_index_blocks"].push_back(block["key"]);
        }
      }
    }
    minimumSourceIndex = englishPositions.size() > 100
                             ? englishPositions[englishPositions.size() - 100]
                             : 0;
    meta["source_minimum_index"] = minimumSourceIndex;
    meta["source_last_index"] =
        englishPositions.empty() ? 0 : englishPositions.back();
    // Missing blocks become history. A smaller same-block correction normally
    // updates Ledger slots in place; only an earlier cropped window needs replay.
    bool replayWindow = consistentTaskSnapshots >= 2 &&
                        blockKeys == previousBlockKeys && !slidingWindow &&
                        minimumSourceIndex < previousMinimum;
    meta["source_window_replay"] = replayWindow;
    meta["source_window_rebind"] = expandedWindow;
    emit(std::move(meta));
    if (consistentTaskSnapshots < 2) {
      auto identityOnly = effective;
      identityOnly["blocks"] = json::array();
      identityOnly["context"] = ledgerContext();
      for (auto event : ledger.observe(identityOnly))
        publishLedger(std::move(event));
      return;
    }
    if (expandedWindow) {
      // Earlier source becoming visible again is a rebind, not new history.
      // App seeds one-to-one cached occurrences before this fresh Ledger epoch.
      forwardedSlots.clear();
      ++windowReplayNonce;
    } else if (replayWindow) {
      // Only the <=100 slots actually forwarded can delete live caption cards.
      // Delete with their old keys before advancing Ledger's epoch.
      for (const auto &item : forwardedSlots) {
        auto removed = item.second;
        removed["text"] = "";
        removed.erase("source_index");
        emit(std::move(removed));
      }
      forwardedSlots.clear();
      ++windowReplayNonce;
    } else {
      // Different message blocks become history; do not delete their cache.
      for (auto item = forwardedSlots.begin(); item != forwardedSlots.end();) {
        const auto &block = item->second.value("block_key", "");
        if (!blockKeys.count(block))
          item = forwardedSlots.erase(item);
        else
          ++item;
      }
    }
    previousBlockKeys = std::move(blockKeys);
    previousSourceCount = sourceIndices.size();
    previousMinimum = minimumSourceIndex;
    effective["context"] = ledgerContext();
    for (auto event : ledger.observe(effective)) {
      if (expandedWindow && event.value("type", "") == "reset")
        event["source_rebind"] = true;
      publishLedger(std::move(event));
    }
  }
  void publishLedger(json event) {
    for (auto &entry : currentTask.items())
      event[entry.key()] = entry.value();
    if (event.value("type", "") == "text" && event.contains("block_key") &&
        event.contains("segment")) {
      auto slot = json::array({event["block_key"], event["segment"]}).dump();
      auto sourceSlot = json::array({event["block_key"],
          event.contains("source_segment") ? event["source_segment"] : event["segment"]}).dump();
      auto index = sourceIndices.find(sourceSlot);
      if (event.value("text", "").empty()) {
        auto previous = forwardedSlots.find(slot);
        if (previous == forwardedSlots.end())
          return;
        forwardedSlots.erase(previous);
        event.erase("source_index");
      } else {
        if (index == sourceIndices.end() || index->second < minimumSourceIndex)
          return;
        event["source_index"] = index->second;
        const auto prior = forwardedSlots.find(slot);
        auto tracked = event;
        tracked["forwarded_order"] = prior == forwardedSlots.end()
            ? ++forwardedOrder : prior->second.value("forwarded_order", uint64_t(0));
        forwardedSlots[slot] = std::move(tracked);
        while (forwardedSlots.size() > 100) {
          auto oldest = std::min_element(
              forwardedSlots.begin(), forwardedSlots.end(),
              [](const auto &left, const auto &right) {
                return left.second.value("forwarded_order", uint64_t(0)) <
                       right.second.value("forwarded_order", uint64_t(0));
              });
          forwardedSlots.erase(oldest);
        }
      }
    }
    event["stable"] = true;
    emit(std::move(event));
  }
  void once(cclive::Ledger &ledger) {
    auto chosen = chooseWindow();
    if (!chosen)
      throw std::runtime_error("等待可用的 Claude 桌面窗口");
    hwnd = chosen;
    consistentTaskSnapshots = 0;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0))
      throw std::runtime_error("无法创建只读采集通道");
    Handle read(r), write(w);
    SetHandleInformation(read.h, HANDLE_FLAG_INHERIT, 0);
    Handle null(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, nullptr));
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    start.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    start.wShowWindow = SW_HIDE;
    start.hStdOutput = write.h;
    start.hStdError = null.h;
    start.hStdInput = null.h;
    auto executable = exePath();
    std::wstring command =
        L"\"" + executable.wstring() + L"\" --reader " + std::to_wstring(hwnd);
    auto env = readerEnvironment(secretEnvs);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                BELOW_NORMAL_PRIORITY_CLASS,
            env.data(), executable.parent_path().c_str(), &start, &pi))
      throw std::runtime_error("无法启动只读采集器");
    Handle child(pi.hProcess), thread(pi.hThread);
    CloseHandle(write.release());
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (job.h) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
      info.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &info,
                              sizeof(info));
      AssignProcessToJobObject(job.h, child.h);
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      process = child.h;
      if (stopped.load())
        TerminateProcess(process, 0);
    }
    std::string buffer;
    auto last = Clock::now();
    auto lastWindowCheck = Clock::now();
    try {
      while (!stopped.load()) {
        if (Clock::now() - lastWindowCheck > std::chrono::milliseconds(500)) {
          lastWindowCheck = Clock::now();
          auto next = chooseWindow();
          if (next && next != hwnd) {
            hwnd = next;
            throw WindowChanged();
          }
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(read.h, nullptr, 0, nullptr, &avail, nullptr))
          throw std::runtime_error("采集器已退出，正在重连");
        if (avail) {
          char chunk[32768];
          DWORD n = 0;
          if (!ReadFile(read.h, chunk, std::min<DWORD>(avail, sizeof(chunk)),
                        &n, nullptr) ||
              !n)
            throw std::runtime_error("采集器已退出，正在重连");
          buffer.append(chunk, n);
          if (buffer.size() > 4 * 1024 * 1024)
            throw std::runtime_error("采集数据超过上限，正在重启采集器");
        }
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
          auto line = buffer.substr(0, pos);
          buffer.erase(0, pos + 1);
          auto value = json::parse(line, nullptr, false);
          if (!value.is_object())
            continue;
          last = Clock::now();
          if (value.value("type", "") == "snapshot") {
            if (!stopped.load())
              bus->capture(generation, value);
            status(value.value("status", "正在读取助手文字"));
            publishSnapshot(value, ledger);
          } else if (value.value("type", "") == "error")
            status(value.value("text", "辅助功能读取暂不可用"));
        }
        if (consistentTaskSnapshots >= 2 && !sourceIdentityPending)
          for (auto event : ledger.flush())
            publishLedger(std::move(event));
        if (WaitForSingleObject(child.h, 0) == WAIT_OBJECT_0)
          throw std::runtime_error("只读采集器已退出，正在重新连接");
        if (Clock::now() - last > std::chrono::seconds(15))
          throw std::runtime_error("辅助功能响应较慢，正在重启本工具的采集器");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      process = nullptr;
      TerminateProcess(child.h, 0);
      throw;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      process = nullptr;
      TerminateProcess(child.h, 0);
    }
  }
  void run() {
    cclive::Ledger ledger(settle);
    while (!stopped.load()) {
      try {
        once(ledger);
      } catch (const WindowChanged &) {
        continue;
      } catch (const std::exception &e) {
        if (!stopped.load())
          status(e.what());
      }
      for (int i = 0; i < 20 && !stopped.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
};
struct Record {
  std::string key, source, translation, error, sourceSignature,
      state = "waiting";
  uint64_t serial = 0, version = 0, displayedUpdate = 0;
  bool pendingReplacement = false;
  std::string layoutBlock;
  std::size_t layoutParagraph = 0;
  bool hasLayout = false, visible = true;
};
class App;
struct Config {
  App *app = nullptr;
  HWND hwnd = nullptr, previousFocus = nullptr;
  // Keep edits[0..7] stable for the native GUI regressions. Baidu uses
  // independent controls [8..10]; ChatGPT uses [11], and Codex uses [12..13].
  std::vector<HWND> edits, labels;
  HWND error = nullptr, keyHint = nullptr, serviceHint = nullptr;
  HWND chatgptLogin = nullptr, chatgptSignout = nullptr,
       chatgptStatus = nullptr, chatgptModels = nullptr,
       chatgptAccounts = nullptr, chatgptAdd = nullptr,
       chatgptRefresh = nullptr, chatgptAccountLabel = nullptr,
       chatgptModelLabel = nullptr, chatgptStatusLabel = nullptr;
  std::shared_ptr<std::atomic_bool> authCancel =
      std::make_shared<std::atomic_bool>(false);
  uint64_t authTicket = 0;
  bool authBusy = false, authLoaded = false;
  fs::path accountPath;
  cclive::chatgpt::AccountStatus account;
  std::vector<cclive::chatgpt::AccountStatus> accountList;
  std::vector<cclive::chatgpt::Model> catalog;
  std::string selectedChatgptModel;
};
std::atomic_uint64_t chatgptSettingsSequence{0};
std::atomic_uint64_t chatgptIdentitySequence{0};
std::mutex chatgptSettingsActionsMutex;
void cancelConfigAuthorization(Config &config) {
  config.authCancel->store(true);
  config.authTicket = ++chatgptSettingsSequence;
  config.authBusy = false;
  config.authLoaded = false;
}
struct ConfigProvider {
  const char *id;
  const wchar_t *label;
};
constexpr ConfigProvider configProviders[] = {
    {"google", L"Google公共翻译"}, {"baidu", L"百度翻译API"},
    {"openai", L"兼容API"}, {"ollama", L"Ollama本地"},
    {"chatgpt", L"ChatGPT账号"}, {"codex", L"Codex CLI（本机登录）"}};
int configProviderIndex(const std::string &provider) {
  for (int i = 0; i < static_cast<int>(std::size(configProviders)); ++i)
    if (provider == configProviders[i].id)
      return i;
  return 0;
}
const char *selectedConfigProvider(const Config &config) {
  const auto index = SendMessageW(config.edits[0], CB_GETCURSEL, 0, 0);
  return index >= 0 && index < static_cast<LRESULT>(std::size(configProviders))
             ? configProviders[index].id
             : "google";
}
void updateConfigProviderFields(Config &config) {
  const std::string provider = selectedConfigProvider(config);
  const bool baidu = provider == "baidu", api = provider == "openai",
             ollama = provider == "ollama", chatgpt = provider == "chatgpt",
             codex = provider == "codex";
  auto field = [&](int index, bool visible, bool enabled) {
    for (auto control : {config.edits[index], config.labels[index]}) {
      EnableWindow(control, enabled);
      ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
  };
  field(1, !baidu && !chatgpt && !codex, api || ollama);
  field(2, !baidu && !chatgpt && !codex, api || ollama);
  field(3, !baidu && !chatgpt && !codex, api);
  field(7, !baidu && !chatgpt && !codex, api);
  for (int index : {8, 9, 10})
    field(index, baidu, baidu);
  for (int index : {12, 13})
    field(index, codex, codex);
  SetWindowTextW(config.keyHint,
                 codex ? L"使用本机 Codex CLI 现有登录；无需本应用的 OAuth 或 API Key。"
                 : chatgpt ? L"Continue with ChatGPT 会打开系统浏览器完成授权；无需填写 API 密钥。"
                 : baidu ? L"百度密钥只在本次运行使用，不写入配置。留空则读取指定环境变量。"
                       : L"密钥只在本次运行使用，不写入配置。留空则读取指定环境变量。");
  const wchar_t *hint =
      baidu ? L"待译文字通过 HTTPS 发送至百度翻译 API。\r\n请先开通百度通用翻译 API，填写 APPID 和对应密钥。\r\n环境变量名默认 BAIDU_TRANSLATE_KEY，与兼容 API 分开。"
      : api ? L"待译文字发送至你填写的 API 地址。\r\n填写兼容 Chat Completions 的地址、模型与密钥。\r\n环境变量名与本次密钥均仅用于兼容 API。"
      : ollama ? L"待译文字发送至你填写的 Ollama 地址。\r\n本机通常为 http://127.0.0.1:11434；填写已安装的模型名。\r\n使用远程地址时，文字会发送至该远程服务。"
      : codex ? L"复用 Codex CLI 当前登录与额度，待译文字发送至 Codex 服务。\r\n路径留空自动检测，模型留空使用 CLI 默认。\r\n未安装或未登录时，请先安装 Codex CLI 并运行 codex login。"
      : chatgpt ? L"使用 ChatGPT 账号授权；待译文字发送至账号授权的服务。\r\n登录后从账号读取可用模型。"
                : L"待译文字通过 HTTPS 发送至 Google 公共翻译接口。\r\n无需密钥；接口可能限流或暂时不可用。\r\n此服务不使用 API 地址、模型或密钥设置。";
  SetWindowTextW(config.serviceHint, hint);
  for (auto control : {config.chatgptLogin, config.chatgptSignout,
                       config.chatgptStatus, config.chatgptModels,
                       config.chatgptAccounts, config.chatgptAdd,
                       config.chatgptRefresh, config.chatgptAccountLabel,
                       config.chatgptModelLabel, config.chatgptStatusLabel})
    if (control)
      ShowWindow(control, chatgpt ? SW_SHOW : SW_HIDE);
  if (config.chatgptLogin) {
    EnableWindow(config.chatgptLogin, chatgpt && !config.authBusy);
    EnableWindow(config.chatgptAdd, chatgpt && !config.authBusy);
    // Local cleanup must remain available if an owned credential file is corrupt.
    EnableWindow(config.chatgptSignout,
                 chatgpt && !config.authBusy && config.authLoaded);
    EnableWindow(config.chatgptRefresh,
                 chatgpt && !config.authBusy && config.account.signed_in &&
                     config.account.plan_enabled);
    EnableWindow(config.chatgptAccounts,
                 chatgpt && !config.authBusy && !config.accountList.empty());
    EnableWindow(config.chatgptModels,
                 chatgpt && !config.authBusy && config.account.signed_in &&
                     config.account.plan_enabled && !config.catalog.empty());
    std::wstring status =
        config.authBusy ? L"正在读取账号或可用模型…"
        : !config.authLoaded ? L"尚未读取账号"
        : !config.account.signed_in ? L"未登录，请继续完成 ChatGPT 授权"
        : !config.account.plan_enabled ? L"账号未开放本服务所需权限"
        : config.catalog.empty() ? L"账号暂未返回可用模型"
                                : L"已授权，可选择账号提供的模型";
    SetWindowTextW(config.chatgptStatus, status.c_str());
  }
}
void launchChatgptSettingsAction(Config &config, std::shared_ptr<Bus> bus,
                                bool offline, std::string action,
                                std::string clientId = {}) {
  config.authCancel->store(true);
  config.authCancel = std::make_shared<std::atomic_bool>(false);
  config.authTicket = ++chatgptSettingsSequence;
  config.authBusy = true;
  config.authLoaded = false;
  config.catalog.clear();
  SendMessageW(config.chatgptModels, CB_RESETCONTENT, 0, 0);
  SetWindowTextW(config.error, L"");
  updateConfigProviderFields(config);
  if (offline)
    return; // Self-tests supply safe synthetic events and never read login files.
  const auto path = config.accountPath;
  const auto cancel = config.authCancel;
  const auto ticket = config.authTicket;
  std::thread([bus, path, cancel, ticket, action, clientId] {
    // Serialize auth-file mutations. A superseded action checks cancellation
    // after obtaining this lock, before it can select or replace an account.
    std::unique_lock<std::mutex> actionLock(chatgptSettingsActionsMutex);
    if (cancel->load())
      return;
    json event = {{"type", "chatgpt_settings"}, {"ticket", ticket},
                  {"account", json::object()}, {"accounts", json::array()},
                  {"models", json::array()}, {"error", ""}};
    auto accountJson = [](const cclive::chatgpt::AccountStatus &account) {
      return json{{"signed_in", account.signed_in},
                  {"plan_enabled", account.plan_enabled},
                  {"email", account.email}, {"name", account.name},
                  {"client_id", account.client_id},
                  {"expires_at", account.expires_at}};
    };
    auto publishIdentity = [&] {
      cclive::chatgpt::AccountStatus account;
      bool readable = true;
      try {
        account = cclive::chatgpt::status(path);
      } catch (...) {
        readable = false;
      }
      // Disk mutations may already have committed even when models/OAuth was
      // cancelled. Identity delivery must not depend on this settings ticket.
      bus->emit({{"type", "chatgpt_identity"},
                 {"revision", ++chatgptIdentitySequence}, {"readable", readable},
                 {"signed_in", account.signed_in},
                 {"plan_enabled", account.plan_enabled},
                 {"client_id", account.client_id}});
      return account;
    };
    try {
      if (cancel->load())
        return;
      if (action == "signin" || action == "add")
        cclive::chatgpt::sign_in(path, *cancel, {}, action == "add");
      else if (action == "switch")
        cclive::chatgpt::select_account(path, clientId);
      else if (action == "signout" &&
               !cclive::chatgpt::sign_out(path, *cancel))
        event["error"] = "已清除本机授权；未能确认服务端撤销，请在账号中检查授权。";
      const auto account = publishIdentity();
      if (cancel->load())
        return;
      event["account"] = accountJson(account);
      for (const auto &saved : cclive::chatgpt::accounts(path))
        event["accounts"].push_back(accountJson(saved));
      if (account.signed_in && account.plan_enabled)
        for (const auto &model : cclive::chatgpt::models(path, *cancel))
          event["models"].push_back({{"id", model.id}, {"name", model.name}});
    } catch (const cclive::TranslationError &error) {
      publishIdentity();
      // Public auth errors are sanitized by the module, with no server bodies or tokens.
      event["error"] = error.what();
    } catch (...) {
      publishIdentity();
      event["error"] = "ChatGPT 操作未完成，请检查网络、账号权限或重新授权。";
    }
    if (!cancel->load())
      bus->emit(std::move(event));
  }).detach();
}
enum {
  IDC_WINDOW = 100,
  IDC_REFRESH,
  IDC_START,
  IDC_PAUSE,
  IDC_ORIGINAL,
  IDC_TOP,
  IDC_SETTINGS,
  IDC_CLEAR,
  IDC_RETRY,
  IDC_LATEST,
  IDC_SOURCE,
  IDC_TRANSLATION
};
class App {
public:
  HWND hwnd = nullptr, combo = nullptr, refreshButton = nullptr,
       startButton = nullptr, pauseButton = nullptr, sourceEdit = nullptr,
       translationEdit = nullptr, originalCheck = nullptr, topCheck = nullptr,
       statusLabel = nullptr, providerLabel = nullptr, countLabel = nullptr,
       title = nullptr, taskLabel = nullptr, eyebrow = nullptr,
       subtitle = nullptr, sourceLabel = nullptr,
       foot = nullptr;
  HWND settingsButton = nullptr, clearButton = nullptr, retryButton = nullptr,
       latestButton = nullptr, iconLabel = nullptr;
  HFONT font = nullptr, headingFont = nullptr, smallFont = nullptr;
  HBRUSH background = CreateSolidBrush(BG), panel = CreateSolidBrush(PANEL);
  int dpi = 96;
  bool autoStart = false;
  bool followingSource = true, followingTranslation = true,
       internalScroll = false;
  bool demo = false, selfTest = false, running = false, paused = false,
       closed = false, dirty = true, refreshing = false,
       startAfterRefresh = false;
  fs::path reportPath, configPath, statusPath;
  std::shared_ptr<DiagnosticWriter> diagnosticWriter =
      std::make_shared<DiagnosticWriter>();
  Clock::time_point lastDiagnostic = Clock::time_point::min();
  uint64_t captureSeq = 0, selectedSourceHwnd = 0;
  std::size_t capturedBlocks = 0, capturedChars = 0;
  double lastReadMs = 0;
  cclive::Settings settings;
  uint64_t generation = 0, sourceGeneration = 0, serial = 0, revision = 0,
           refreshGeneration = 0, displayedUpdateSequence = 0;
  int ledgerEpoch = -1;
  std::vector<Record> records;
  std::vector<json> windows;
  std::shared_ptr<Bus> bus = std::make_shared<Bus>();
  std::shared_ptr<Worker> worker = std::make_shared<Worker>(bus);
  std::shared_ptr<Watcher> watcher;
  Clock::time_point lastRender = Clock::now(), demoStart, testStart;
  struct BodyRender {
    std::wstring input, displayed;
    uint64_t styleRevision = 0, replacements = 0, formats = 0;
    int fontPoints = 0, dpi = 0, bodyWidth = 0;
    LONG nativeLength = 0;
    bool hasTables = false;
    LONG latestInputStart = -1, latestInputLength = 0;
    CHARRANGE latestDisplay{-1, -1};
    bool valid = false;
  };
  std::map<HWND, BodyRender> renderedBodies;
  uint64_t bodyStyleRevision = 0;
  int demoStep = -1;
  std::unique_ptr<cclive::Ledger> demoLedger;
  Config *config = nullptr;
  std::unique_ptr<cclive::TaskCache> taskCache;
  std::string activeTaskId, activeTaskTitle, activeIdentityQuality, chatgptAccountHash;
  uint64_t chatgptIdentityRevision = 0;
  std::function<cclive::chatgpt::AccountStatus()> chatgptIdentityReader = [] {
    return cclive::chatgpt::status(); // Only this application's own DPAPI file.
  };
  struct SourceAlias {
    std::string record, block, signature;
  };
  std::map<std::string, SourceAlias> sourceAliases;
  std::set<std::string> replayCandidates;
  std::vector<std::string> clearedSignatures;
  std::set<std::string> clearedBlocks;
  std::set<std::size_t> clearedSourceIndices;
  bool cacheDirty = false, sourceNeedsReplay = false, replayPendingBody = false;
  json lastTaskMetadata, activeProfile;
  std::map<std::string, std::string> windowReplayMemo;
  std::size_t windowReplayLastIndex = 0;
  std::size_t minimumSourceIndex = 0;
  Clock::time_point lastCacheSave = Clock::now();
  App(bool d, bool t, fs::path r, bool automatic, fs::path diagnostic)
      : autoStart(automatic), demo(d), selfTest(t), reportPath(std::move(r)),
        configPath(exePath().parent_path() / "data" / "config.json"),
        statusPath(std::move(diagnostic)) {
    taskCache = std::make_unique<cclive::TaskCache>(fs::path{}, false);
    if (!selfTest)
      try {
        settings = cclive::load_settings(configPath);
      } catch (...) {
        settings = cclive::Settings();
      }
    if (demo || selfTest) {
      settings.provider = "demo";
      chatgptIdentityReader = [] { return cclive::chatgpt::AccountStatus{}; };
    }
    // Bind the saved local identity before collecting any task, so merely
    // opening Settings later does not invalidate already translated records.
    // This reads only our DPAPI record before creating the UI; no network.
    if (!demo && !selfTest && settings.provider == "chatgpt") {
      try {
        const auto account = cclive::chatgpt::status();
        if (account.signed_in)
          chatgptAccountHash = contentSignature(account.client_id);
      } catch (const std::exception &) {
        // Settings keeps local cleanup and reconnect available for damaged files.
      }
    }
  }
  ~App() {
    shutdown();
    if (font)
      DeleteObject(font);
    if (headingFont)
      DeleteObject(headingFont);
    if (smallFont)
      DeleteObject(smallFont);
    DeleteObject(background);
    DeleteObject(panel);
  }
  int s(int n) const { return MulDiv(n, dpi, 96); }
  void status(const std::string &v) {
    if (!closed && statusLabel)
      SetWindowTextW(statusLabel, wide(v).c_str());
  }
  HWND control(const wchar_t *klass, const wchar_t *text, DWORD style, int id,
               HFONT f = nullptr) {
    bool button = wcscmp(klass, L"BUTTON") == 0;
    bool primary = button && (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
    if (button && ((style & BS_TYPEMASK) == BS_PUSHBUTTON || primary))
      style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    auto w = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, 0,
                             0, 0, 0, hwnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (!w)
      throw std::runtime_error("创建窗口控件失败");
    SendMessageW(w, WM_SETFONT, (WPARAM)(f ? f : smallFont), TRUE);
    if (button)
      themeButton(w, primary);
    else if (wcscmp(klass, L"COMBOBOX") == 0)
      nativeDarkControl(w, L"DarkMode_CFD");
    return w;
  }
  void fonts() {
    ++bodyStyleRevision;
    HFONT nextFont = CreateFontW(
        -MulDiv(settings.font_size, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT nextHeading = CreateFontW(
        -s(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
        CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT nextSmall = CreateFontW(-s(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                  FALSE, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    if (!nextFont || !nextHeading || !nextSmall) {
      if (nextFont)
        DeleteObject(nextFont);
      if (nextHeading)
        DeleteObject(nextHeading);
      if (nextSmall)
        DeleteObject(nextSmall);
      return;
    }
    struct FontChange {
      HFONT oldFont, oldHeading, oldSmall, newFont, newHeading, newSmall;
    };
    FontChange change{font,     headingFont, smallFont,
                      nextFont, nextHeading, nextSmall};
    font = nextFont;
    headingFont = nextHeading;
    smallFont = nextSmall;
    auto replace = [](HWND child, LPARAM payload) -> BOOL {
      auto &f = *reinterpret_cast<FontChange *>(payload);
      auto current =
          reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0));
      HFONT next = nullptr;
      if (f.oldFont && current == f.oldFont)
        next = f.newFont;
      else if (f.oldHeading && current == f.oldHeading)
        next = f.newHeading;
      else if (f.oldSmall && current == f.oldSmall)
        next = f.newSmall;
      if (next)
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
      return TRUE;
    };
    if (hwnd)
      EnumChildWindows(hwnd, replace, reinterpret_cast<LPARAM>(&change));
    if (config && config->hwnd)
      EnumChildWindows(config->hwnd, replace,
                       reinterpret_cast<LPARAM>(&change));
    // RichEdit may return its own synthesized HFONT rather than the handle
    // passed to WM_SETFONT, so update both bodies explicitly as well.
    for (HWND body : {sourceEdit, translationEdit}) {
      if (!body)
        continue;
      SendMessageW(body, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      SendMessageW(body, EM_SETZOOM, 1, 1);
      CHARFORMAT2W format{};
      format.cbSize = sizeof(format);
      format.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BACKCOLOR;
      format.crTextColor = cclive::text_layout_palette.body;
      format.crBackColor = cclive::text_layout_palette.background;
      format.yHeight = settings.font_size * 20;
      wcsncpy(format.szFaceName, L"Microsoft YaHei UI", LF_FACESIZE - 1);
      SendMessageW(body, EM_SETCHARFORMAT, SCF_DEFAULT,
                   reinterpret_cast<LPARAM>(&format));
      SendMessageW(body, EM_SETCHARFORMAT, SCF_ALL,
                   reinterpret_cast<LPARAM>(&format));
    }
    // Controls now hold replacement fonts, so the old GDI handles can be
    // released.
    if (change.oldFont)
      DeleteObject(change.oldFont);
    if (change.oldHeading)
      DeleteObject(change.oldHeading);
    if (change.oldSmall)
      DeleteObject(change.oldSmall);
  }
  void provider() {
    auto name = settings.provider == "google"
                    ? std::string("Google 公共翻译 · 待译文字发送至 Google")
                : settings.provider == "baidu"
                    ? std::string("百度翻译 API · 待译文字发送至百度")
                : settings.provider == "chatgpt"
                    ? "ChatGPT · " + settings.chatgpt_model + " · 待译文字发送至 OpenAI"
                : settings.provider == "codex"
                    ? "Codex CLI（本机登录） · " +
                          (settings.codex_model.empty() ? "CLI 默认模型" : settings.codex_model) +
                          " · 待译文字发送至 Codex"
                : settings.provider == "ollama" ? "Ollama · " + settings.model
                : settings.provider == "openai"
                    ? "API · " + settings.model
                    : std::string("离线演示 · 无网络");
    SetWindowTextW(providerLabel, wide(name).c_str());
  }
  void build(HWND w) {
    hwnd = w;
    HDC dc = GetDC(w);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(w, dc);
    fonts();
    darkCaption(w);
    SendMessageW(w, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(applicationIcon(s(32))));
    SendMessageW(w, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(applicationIcon(s(16))));
    iconLabel = control(L"STATIC", L"", SS_ICON | SS_CENTERIMAGE, 0);
    SendMessageW(iconLabel, STM_SETICON,
                 reinterpret_cast<WPARAM>(applicationIcon(s(28))), 0);
    eyebrow =
        control(L"STATIC", L"CLAUDE  /  LIVE TRANSLATION", 0, 0, smallFont);
    title = control(L"STATIC", L"实时翻译", 0, 0, headingFont);
    taskLabel = control(L"STATIC", L"正在翻译：等待选择任务", SS_ENDELLIPSIS, 0,
                        smallFont);
    subtitle = control(L"STATIC", L"保留 Claude 原窗口，在这里看中文", 0, 0,
                       smallFont);
    providerLabel = control(L"STATIC", L"", SS_RIGHT | SS_ENDELLIPSIS, 0, smallFont);
    provider();
    sourceLabel = control(L"STATIC", L"来源  Claude 桌面客户端 · 辅助功能文字",
                          0, 0, smallFont);
    combo = control(L"COMBOBOX", L"",
                    CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_WINDOW);
    SendMessageW(combo, CB_ADDSTRING, 0,
                 (LPARAM)(demo || selfTest ? L"离线演示"
                                           : L"点击刷新，选择 Claude 窗口"));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    refreshButton =
        control(L"BUTTON", L"刷新", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
    startButton =
        control(L"BUTTON", L"开始", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_START);
    pauseButton =
        control(L"BUTTON", L"暂停", BS_PUSHBUTTON | WS_TABSTOP, IDC_PAUSE);
    EnableWindow(pauseButton, FALSE);
    originalCheck =
        control(L"BUTTON", L"显示原文", BS_AUTOCHECKBOX | WS_TABSTOP,
                IDC_ORIGINAL, smallFont);
    SendMessageW(originalCheck, BM_SETCHECK,
                 settings.show_original ? BST_CHECKED : BST_UNCHECKED, 0);
    topCheck = control(L"BUTTON", L"置顶", BS_AUTOCHECKBOX | WS_TABSTOP,
                       IDC_TOP, smallFont);
    SendMessageW(topCheck, BM_SETCHECK,
                 settings.always_on_top ? BST_CHECKED : BST_UNCHECKED, 0);
    settingsButton =
        control(L"BUTTON", L"设置", BS_PUSHBUTTON | WS_TABSTOP, IDC_SETTINGS);

    clearButton =
        control(L"BUTTON", L"清空", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLEAR);
    retryButton =
        control(L"BUTTON", L"重试失败", BS_PUSHBUTTON | WS_TABSTOP, IDC_RETRY);
    latestButton =
        control(L"BUTTON", L"回到最新", BS_PUSHBUTTON | WS_TABSTOP, IDC_LATEST);
    sourceEdit = control(MSFTEDIT_CLASS, L"",
                         WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                             ES_AUTOVSCROLL,
                         IDC_SOURCE, font);
    translationEdit = control(MSFTEDIT_CLASS, L"",
                              WS_VSCROLL | WS_TABSTOP | ES_MULTILINE |
                                  ES_READONLY | ES_AUTOVSCROLL,
                              IDC_TRANSLATION, font);
    for (auto edit : {sourceEdit, translationEdit}) {
      if (!SetWindowSubclass(edit, bodyProc, 1,
                             reinterpret_cast<DWORD_PTR>(this)))
        throw std::runtime_error("无法创建正文阅读控制");
      nativeDarkControl(edit, L"DarkMode_Explorer");
      SendMessageW(edit, EM_SETBKGNDCOLOR, 0, PANEL);
      SendMessageW(edit, EM_EXLIMITTEXT, 0, 2000000);
      SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                   MAKELPARAM(s(14), s(14)));
      CHARFORMAT2W cf{};
      cf.cbSize = sizeof(cf);
      cf.dwMask = CFM_COLOR;
      cf.crTextColor = FG;
      SendMessageW(edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
      SendMessageW(edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    }
    statusLabel = control(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, smallFont);
    countLabel = control(L"STATIC", L"0 段", SS_RIGHT | SS_ENDELLIPSIS, 0, smallFont);
    foot = control(L"STATIC", L"后台只读 · 不截图、不点击、不修改客户端", 0, 0,
                   smallFont);
    EnableWindow(refreshButton, TRUE);
    EnableWindow(combo, !(demo || selfTest));
    std::thread([work = worker] { work->run(); }).detach();
    SetTimer(w, 1, 50, nullptr);
    status(demo || selfTest ? "离线演示 · 合成示例，不读取 Claude"
                            : "打开 Claude 正文会话，刷新后点击开始");
    SetWindowPos(w, settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    layout();
    render();
    testStart = Clock::now();
    if (demo || selfTest)
      start();
    else if (autoStart) {
      startAfterRefresh = true;
      refresh();
    }
    publishDiagnostic(true);
  }
  struct ReadingGeometry {
    RECT source{}, translated{};
    int separator = 0;
  };
  ReadingGeometry readingGeometry(int width, int height, bool dual) const {
    const int margin = s(16), gap = s(10);
    const int available = std::max(1, width - 2 * margin);
    const int top = s(125), bottom = std::max(top + 1, height - s(36));
    auto column = [&](int left, int regionWidth) {
      const int region = std::max(1, regionWidth);
      const int readingWidth = std::min(s(760), region);
      const int x = left + (region - readingWidth) / 2;
      return RECT{x, top, x + readingWidth, bottom};
    };
    ReadingGeometry geometry;
    if (dual) {
      const int leftWidth = std::max(1, (available - gap) / 2);
      geometry.source = column(margin, leftWidth);
      geometry.translated = column(margin + leftWidth + gap,
                                   available - leftWidth - gap);
      geometry.separator = margin + leftWidth + gap / 2;
    } else {
      geometry.translated = column(margin, available);
    }
    return geometry;
  }
  void layout() {
    if (!hwnd)
      return;
    ScrollGuard guard(internalScroll);
    struct View { HWND window; POINT scroll{}; };
    View views[] = {{sourceEdit}, {translationEdit}};
    for (auto &view : views)
      if (view.window)
        SendMessageW(view.window, EM_GETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&view.scroll));
    RECT area{};
    GetClientRect(hwnd, &area);
    const int width = area.right, height = area.bottom, m = s(16), gap = s(10);
    const int bodyW = std::max(1, width - 2 * m);
    auto move = [](HWND window, int x, int y, int w, int h) {
      MoveWindow(window, x, y, std::max(1, w), std::max(1, h), TRUE);
    };
    auto moveRect = [&](HWND window, const RECT &rect) {
      move(window, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    };
    ShowWindow(eyebrow, SW_HIDE);
    ShowWindow(subtitle, SW_HIDE);
    ShowWindow(sourceLabel, SW_HIDE);
    ShowWindow(foot, SW_HIDE);
    move(iconLabel, m, s(8), s(28), s(28));
    move(title, m + s(38), s(9), s(114), s(27));
    const int providerW = std::min(s(350),
        std::max(s(180), (bodyW - s(162)) * 2 / 5));
    const int taskW = std::max(1, bodyW - s(162) - providerW - s(12));
    move(taskLabel, m + s(162), s(13), taskW, s(21));
    move(providerLabel, width - m - providerW, s(13), providerW, s(21));
    int y = s(43), bh = s(29), bw = s(66);
    move(combo, m, y, bodyW - 3 * (bw + gap), s(280));
    int x = width - m - 3 * bw - 2 * gap;
    for (HWND button : {refreshButton, startButton, pauseButton}) {
      move(button, x, y, bw, bh);
      x += bw + gap;
    }
    y = s(81);
    move(originalCheck, m, y, s(104), s(27));
    move(topCheck, m + s(110), y, s(62), s(27));
    x = width - m;
    for (auto item : std::vector<std::pair<HWND, int>>{{latestButton, 94},
                                                       {retryButton, 82},
                                                       {clearButton, 56},
                                                       {settingsButton, 56}}) {
      x -= s(item.second);
      move(item.first, x, y, s(item.second), s(27));
      x -= s(6);
    }
    const auto geometry = readingGeometry(width, height, settings.show_original);
    ShowWindow(sourceEdit, settings.show_original ? SW_SHOW : SW_HIDE);
    if (settings.show_original)
      moveRect(sourceEdit, geometry.source);
    moveRect(translationEdit, geometry.translated);
    move(statusLabel, m, height - s(26), bodyW - s(366), s(20));
    move(countLabel, width - m - s(360), height - s(26), s(360), s(20));
    // Resizing changes the wrap width without changing source records. Preserve
    // manual reading, or explicitly remain at the new end for a following pane.
    for (const auto &view : views) {
      if (!view.window)
        continue;
      SendMessageW(view.window, EM_SETTARGETDEVICE, 0, 0);
      SendMessageW(view.window, EM_SETRECT, 0, 0);
      SendMessageW(view.window, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                   MAKELPARAM(s(14), s(14)));
      SendMessageW(view.window, EM_GETLINECOUNT, 0, 0);
      if (follows(view.window))
        scrollToLatest(view.window);
      else
        SendMessageW(view.window, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&view.scroll));
      repaintBody(view.window);
    }
    if (std::any_of(renderedBodies.begin(), renderedBodies.end(),
                    [](const auto &item) { return item.second.hasTables; }))
      render(); // Table cell boundaries follow the new reading-pane width.
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  void paintDividers(HDC dc) const {
    RECT area{};
    GetClientRect(hwnd, &area);
    const int width = area.right, height = area.bottom, margin = s(16);
    const auto geometry = readingGeometry(width, height, settings.show_original);
    const int top = s(117), bottom = height - s(36);
    const auto pen = CreatePen(PS_SOLID, 1, RGB(72, 70, 64));
    const auto previous = SelectObject(dc, pen);
    auto line = [&](int x1, int y1, int x2, int y2) {
      MoveToEx(dc, x1, y1, nullptr);
      LineTo(dc, x2, y2);
    };
    line(margin, s(39), width - margin, s(39));
    if (settings.show_original) {
      line(geometry.source.left, top, geometry.source.right, top);
      line(geometry.separator, top, geometry.separator, bottom);
    }
    line(geometry.translated.left, top, geometry.translated.right, top);
    line(margin, height - s(31), width - margin, height - s(31));
    SelectObject(dc, previous);
    DeleteObject(pen);
  }
  json translationProfile() const {
    const bool api = settings.provider == "openai" ||
                     settings.provider == "ollama";
    const bool baidu = settings.provider == "baidu",
               chatgpt = settings.provider == "chatgpt",
               codex = settings.provider == "codex";
    return {{"provider", settings.provider},
            {"model", api ? settings.model : std::string()},
            {"target", settings.target},
            {"baidu_account_hash", baidu ? contentSignature(settings.baidu_app_id)
                                         : std::string()},
            {"chatgpt_model", chatgpt ? settings.chatgpt_model : std::string()},
            {"codex_model", codex ? settings.codex_model : std::string()},
            {"codex_path_hash", codex ? contentSignature(settings.codex_path)
                                      : std::string()},
            {"chatgpt_account_hash", chatgpt ? chatgptAccountHash : std::string()},
            {"endpoint_hash", api ? contentSignature(settings.base_url)
                                   : std::string()}};
  }
  void updateTaskTitle() {
    std::string titleText =
        activeTaskTitle.empty() ? "等待选择任务" : activeTaskTitle;
    SetWindowTextW(taskLabel, wide("正在翻译：" + titleText).c_str());
    SetWindowTextW(hwnd, wide("Claude 实时翻译 · " + titleText).c_str());
  }
  std::size_t recordLines(const Record &record) const {
    auto translated = logicalLines(record.translation);
    if (record.state == "done" && translated)
      return translated;
    return std::max(std::size_t(1),
                    std::max(logicalLines(record.source), translated));
  }
  std::size_t retainedLines() const {
    std::size_t count = 0;
    for (const auto &record : records)
      count += recordLines(record);
    return count;
  }
  void invalidateAliases(const Record &record) {
    worker->cancel(record.key);
    replayCandidates.erase(record.key);
    for (auto i = sourceAliases.begin(); i != sourceAliases.end();)
      if (i->second.record == record.key)
        i = sourceAliases.erase(i);
      else
        ++i;
  }
  void trimRecords() {
    for (auto &record : records)
      if (logicalLines(record.source) > 100) {
        if (record.sourceSignature.empty())
          record.sourceSignature = contentSignature(record.source);
        invalidateAliases(record);
        record.source = tailLines(record.source, 100);
        record.version = ++revision;
        if (record.state != "done") {
          record.state = paused ? "paused" : "stopped";
          if (running && !paused)
            enqueue(record);
        }
        dirty = cacheDirty = true;
      }
    auto count = retainedLines();
    while (count > 100 && !records.empty()) {
      auto oldLines = recordLines(records.front());
      auto excess = count - 100;
      invalidateAliases(records.front());
      if (oldLines <= excess) {
        records.erase(records.begin());
        count -= oldLines;
      } else {
        auto &record = records.front();
        auto keep = oldLines - excess;
        if (record.sourceSignature.empty())
          record.sourceSignature = contentSignature(record.source);
        auto previousSource = record.source;
        bool unfinished = record.state != "done";
        record.source = tailLines(record.source, keep);
        record.translation = tailLines(record.translation, keep);
        record.version = ++revision;
        if (unfinished) {
          record.state = paused ? "paused" : "stopped";
          if (record.source != previousSource && running && !paused)
            enqueue(record);
        }
        count = retainedLines();
      }
      dirty = cacheDirty = true;
    }
  }
  json taskPayload() const {
    json saved = json::array();
    for (const auto &record : records)
      saved.push_back({{"key", record.key},
                       {"source", record.source},
                       {"translation", record.translation},
                       {"state", record.state},
                       {"pending_replacement", record.pendingReplacement},
                       {"has_layout", record.hasLayout},
                       {"visible", record.visible},
                       {"layout_block", record.layoutBlock},
                       {"layout_paragraph", record.layoutParagraph},
                       {"error", record.error},
                       {"source_signature", record.sourceSignature},
                       {"serial", record.serial},
                       {"displayed_update", record.displayedUpdate}});
    POINT source{}, translated{};
    if (sourceEdit)
      SendMessageW(sourceEdit, EM_GETSCROLLPOS, 0,
                   reinterpret_cast<LPARAM>(&source));
    if (translationEdit)
      SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                   reinterpret_cast<LPARAM>(&translated));
    return {{"task_id", activeTaskId},
            {"task_title", activeTaskTitle},
            {"identity_quality", activeIdentityQuality},
            {"translation_profile",
             activeProfile.is_object() ? activeProfile : translationProfile()},
            {"records", saved},
            {"cleared_signatures", clearedSignatures},
            {"cleared_blocks", clearedBlocks},
            {"source_scroll_x", source.x},
            {"source_scroll_y", source.y},
            {"translation_scroll_x", translated.x},
            {"translation_scroll_y", translated.y},
            {"follow_source", followingSource},
            {"follow_translation", followingTranslation}};
  }
  void saveActiveTask() {
    if (!activeTaskId.empty() && taskCache) {
      try {
        if (records.empty() && clearedSignatures.empty())
          taskCache->erase(activeTaskId);
        else
          taskCache->put(activeTaskId, taskPayload());
      } catch (...) {
      }
    }
    cacheDirty = false;
    lastCacheSave = Clock::now();
  }
  void updateClearBoundary(const json &metadata, bool restored = false) {
    if (!metadata.contains("source_signatures") ||
        !metadata["source_signatures"].is_array() ||
        metadata["source_signatures"].empty())
      return;
    clearedSourceIndices.clear();
    if (clearedSignatures.empty())
      return;
    bool sameBlock = false;
    if (metadata.contains("source_block_keys") && metadata["source_block_keys"].is_array())
      for (const auto &key : metadata["source_block_keys"])
        if (key.is_string() && clearedBlocks.count(key.get<std::string>()))
          sameBlock = true;
    if (!sameBlock && !restored)
      return;
    const auto &sources = metadata["source_signatures"];
    const std::set<std::string> dismissed(clearedSignatures.begin(), clearedSignatures.end());
    std::vector<std::size_t> candidates;
    const auto first = metadata.value("source_minimum_index", std::size_t(0));
    const bool blockPositions = metadata.contains("source_index_blocks") &&
        metadata["source_index_blocks"].is_array() &&
        metadata["source_index_blocks"].size() == sources.size();
    for (auto index = first; index < sources.size(); ++index)
      if (sources[index].is_string() && dismissed.count(sources[index].get<std::string>()) &&
          (restored || !blockPositions ||
           clearedBlocks.count(metadata["source_index_blocks"][index].get<std::string>())))
        candidates.push_back(index);
    // Match occurrences in order, preferring the earliest equal occurrence.
    // A cropped prefix must not consume an identical sentence newly appended
    // at the end (A,B,C -> B,C,D,A still exposes the new final A).
    const auto rows = clearedSignatures.size(), columns = candidates.size();
    std::vector<std::vector<unsigned short>> length(rows + 1,
        std::vector<unsigned short>(columns + 1));
    for (auto i = rows; i-- > 0;)
      for (auto j = columns; j-- > 0;)
        length[i][j] = sources[candidates[j]] == clearedSignatures[i]
            ? static_cast<unsigned short>(1 + length[i + 1][j + 1])
            : std::max(length[i + 1][j], length[i][j + 1]);
    std::size_t i = 0, j = 0;
    while (i < rows && j < columns) {
      if (sources[candidates[j]] == clearedSignatures[i]) {
        clearedSourceIndices.insert(candidates[j]);
        ++i; ++j;
      } else if (length[i + 1][j] >= length[i][j + 1]) ++i;
      else ++j;
    }
    const bool exactReplay = sources.size() == clearedSignatures.size() &&
        std::equal(clearedSignatures.begin(), clearedSignatures.end(), sources.begin());
    // A cleared occurrence plus a retained pending occurrence also proves a
    // rebuilt source window (R,R). Count each retained occurrence only once,
    // without letting the dismissed first R steal its still-running sibling.
    std::map<std::string, std::size_t> retained;
    for (const auto &record : records)
      ++retained[record.sourceSignature.empty() ? contentSignature(record.source) : record.sourceSignature];
    std::vector<std::size_t> replayEvidence;
    if (restored)
      for (auto index = first; index < sources.size(); ++index) {
        if (clearedSourceIndices.count(index) || !sources[index].is_string()) continue;
        auto found = retained.find(sources[index].get<std::string>());
        if (found != retained.end() && found->second) {
          replayEvidence.push_back(index);
          --found->second;
        }
      }
    if (!sameBlock && restored && clearedSourceIndices.size() + replayEvidence.size() < 2 && !exactReplay)
      clearedSourceIndices.clear();
    if (restored && !clearedSourceIndices.empty()) {
      if (blockPositions) {
        std::map<std::string, std::size_t> matchedBlocks;
        for (auto index : clearedSourceIndices)
          ++matchedBlocks[metadata["source_index_blocks"][index].get<std::string>()];
        for (auto index : replayEvidence)
          ++matchedBlocks[metadata["source_index_blocks"][index].get<std::string>()];
        for (const auto &entry : matchedBlocks)
          if (entry.second >= 2 || exactReplay || clearedBlocks.count(entry.first))
            clearedBlocks.insert(entry.first);
        for (auto index = clearedSourceIndices.begin(); index != clearedSourceIndices.end();)
          if (!clearedBlocks.count(metadata["source_index_blocks"][*index].get<std::string>()))
            index = clearedSourceIndices.erase(index);
          else ++index;
      } else if (metadata.contains("source_block_keys") && metadata["source_block_keys"].is_array()) {
        for (const auto &key : metadata["source_block_keys"])
          if (key.is_string()) clearedBlocks.insert(key.get<std::string>());
      }
    }
    while (clearedBlocks.size() > 128) clearedBlocks.erase(clearedBlocks.begin());
  }
  void seedReplay(const json &metadata) {
    updateClearBoundary(metadata, true);
    replayCandidates.clear();
    std::map<std::string, std::size_t> visible;
    if (metadata.contains("source_signatures") &&
        metadata["source_signatures"].is_array()) {
      for (const auto &item : metadata["source_signatures"])
        if (item.is_string())
          ++visible[item.get<std::string>()];
    } else if (metadata.contains("text") && metadata["text"].is_string()) {
      ++visible[contentSignature(metadata["text"].get<std::string>())];
    }
    sourceNeedsReplay = false;
    replayPendingBody = (!records.empty() || !clearedSignatures.empty()) && visible.empty();
    // Prefer the latest matching occurrence, not an arbitrarily old "Done"
    // card.
    for (auto i = records.rbegin(); i != records.rend(); ++i) {
      auto signature = i->sourceSignature.empty() ? contentSignature(i->source)
                                                  : i->sourceSignature;
      auto found = visible.find(signature);
      if (found != visible.end() && found->second) {
        replayCandidates.insert(i->key);
        --found->second;
      }
    }
  }
  void switchTask(const json &metadata) {
    const auto id = metadata.value("task_id", "");
    if (id.empty())
      return;
    auto titleText = metadata.value("task_title", std::string("Claude 任务"));
    auto source = metadata.value("source_hwnd", selectedSourceHwnd);
    lastTaskMetadata = metadata;
    if (id == activeTaskId && metadata.value("source_window_replay", false)) {
      windowReplayMemo.clear();
      for (const auto &record : records)
        if (record.state == "done" && !record.pendingReplacement &&
            !record.translation.empty())
          windowReplayMemo[record.sourceSignature.empty()
                               ? contentSignature(record.source)
                               : record.sourceSignature] = record.translation;
      windowReplayLastIndex =
          metadata.value("source_last_index", std::size_t(0));
    }
    auto sourceCount = metadata.contains("source_signatures") &&
                               metadata["source_signatures"].is_array()
                           ? metadata["source_signatures"].size()
                           : 0;
    minimumSourceIndex =
        metadata.value("source_minimum_index",
                       sourceCount > 100 ? sourceCount - 100 : std::size_t(0));
    if (id == activeTaskId) {
      if (metadata.value("source_window_rebind", false)) {
        sourceAliases.clear();
        seedReplay(metadata);
      }
      updateClearBoundary(metadata);
      if (activeProfile != translationProfile()) {
        windowReplayMemo.clear();
        windowReplayLastIndex = 0;
        ++generation;
        cancelAll();
        activeProfile = translationProfile();
        sourceNeedsReplay = true;
        for (auto &record : records) {
          record.translation.clear();
          record.pendingReplacement = false;
          record.error.clear();
          record.version = ++revision;
          record.state = paused ? "paused" : "waiting";
          if (!paused && running)
            enqueue(record);
        }
        dirty = cacheDirty = true;
      }
      bool bodyAvailable = metadata.contains("source_signatures") &&
                           metadata["source_signatures"].is_array() &&
                           !metadata["source_signatures"].empty();
      if (source != selectedSourceHwnd || sourceNeedsReplay ||
          (replayPendingBody && bodyAvailable)) {
        sourceAliases.clear();
        seedReplay(metadata);
      }
      selectedSourceHwnd = source;
      if (titleText != activeTaskTitle) {
        activeTaskTitle = titleText;
        updateTaskTitle();
        cacheDirty = true;
      }
      return;
    }
    saveActiveTask();
    ++generation;
    cancelAll();
    records.clear();
    sourceAliases.clear();
    replayCandidates.clear();
    serial = 0;
    ledgerEpoch = -1;
    windowReplayMemo.clear();
    clearedSignatures.clear();
    clearedBlocks.clear();
    clearedSourceIndices.clear();
    activeTaskId = id;
    activeProfile = translationProfile();
    activeTaskTitle = titleText;
    activeIdentityQuality = metadata.value("identity_quality", "view_fallback");
    selectedSourceHwnd = source;
    followingSource = followingTranslation = true;
    POINT sourceScroll{}, translatedScroll{};
    if (auto cached = taskCache->get(id)) {
      try {
        clearedSignatures = cached->value("cleared_signatures", std::vector<std::string>{});
        if (clearedSignatures.size() > 100)
          clearedSignatures.erase(clearedSignatures.begin(), clearedSignatures.end() - 100);
        clearedBlocks = cached->value("cleared_blocks", std::set<std::string>{});
        bool matching = cached->value("translation_profile", json::object()) ==
                        translationProfile();
        followingSource = cached->value("follow_source", true);
        followingTranslation = cached->value("follow_translation", true);
        sourceScroll = {cached->value("source_scroll_x", 0L),
                        cached->value("source_scroll_y", 0L)};
        translatedScroll = {cached->value("translation_scroll_x", 0L),
                            cached->value("translation_scroll_y", 0L)};
        if (cached->contains("records") && (*cached)["records"].is_array())
          for (const auto &item : (*cached)["records"]) {
            if (!item.is_object() || !item.contains("source") ||
                !item["source"].is_string())
              continue;
            Record record;
            record.serial = item.value("serial", serial + 1);
            serial = std::max(serial, record.serial);
            record.key =
                item.value("key", "record-" + std::to_string(record.serial));
            record.source = item["source"].get<std::string>();
            record.visible = item.value("visible", true);
            record.displayedUpdate = item.value("displayed_update", uint64_t(0));
            displayedUpdateSequence = std::max(displayedUpdateSequence, record.displayedUpdate);
            record.sourceSignature =
                item.value("source_signature", contentSignature(record.source));
            record.translation = matching ? item.value("translation", "") : "";
            record.error = matching ? item.value("error", "") : "";
            record.pendingReplacement =
                matching && item.value("pending_replacement", false) &&
                !record.translation.empty();
            auto state = item.value("state", "waiting");
            record.state =
                matching && state == "done" && !record.pendingReplacement &&
                        !record.translation.empty()
                    ? "done"
                : matching && state == "error" ? "error"
                                               : "waiting";
            if (item.value("has_layout", false) && item.contains("layout_block") &&
                item["layout_block"].is_string() &&
                !item["layout_block"].get_ref<const std::string &>().empty() &&
                item.contains("layout_paragraph") &&
                item["layout_paragraph"].is_number_integer() &&
                (item["layout_paragraph"].is_number_unsigned() ||
                 item["layout_paragraph"].get<int64_t>() >= 0)) {
              record.hasLayout = true;
              record.layoutBlock = item["layout_block"].get<std::string>();
              record.layoutParagraph = item["layout_paragraph"].get<std::size_t>();
            }
            record.version = ++revision;
            records.push_back(std::move(record));
            if (records.size() >= 240)
              break;
          }
      } catch (...) {
        records.clear();
        serial = 0;
      }
    }
    trimRecords();
    seedReplay(metadata);
    updateTaskTitle();
    dirty = true;
    render();
    {
      ScrollGuard guard(internalScroll);
      if (!followingSource)
        SendMessageW(sourceEdit, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&sourceScroll));
      if (!followingTranslation)
        SendMessageW(translationEdit, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&translatedScroll));
      repaintBody(sourceEdit);
      repaintBody(translationEdit);
    }
    for (auto &record : records)
      if (record.state == "waiting") {
        if (paused)
          record.state = "paused";
        else if (running)
          enqueue(record);
      }
    cacheDirty = true;
  }
  void cancelAll() { worker->cancelAll(); }
  void clear(bool continueWork = false) {
    if (continueWork) {
      std::set<std::string> removed;
      for (auto current = records.begin(); current != records.end();) {
        const bool pending = current->state == "waiting" || current->state == "queued" ||
            current->state == "translating" || current->state == "rate_limited" ||
            current->state == "paused";
        if (pending) {
          current->visible = false;
          current->translation.clear();
          current->pendingReplacement = false;
          current->error.clear();
          ++current;
        } else {
          clearedSignatures.push_back(current->sourceSignature.empty()
              ? contentSignature(current->source) : current->sourceSignature);
          if (current->hasLayout) clearedBlocks.insert(current->layoutBlock);
          removed.insert(current->key);
          replayCandidates.erase(current->key);
          current = records.erase(current);
        }
      }
      if (clearedSignatures.size() > 100)
        clearedSignatures.erase(clearedSignatures.begin(), clearedSignatures.end() - 100);
      for (auto &alias : sourceAliases)
        if (removed.count(alias.second.record)) alias.second.record.clear();
      updateClearBoundary(lastTaskMetadata);
      windowReplayMemo.clear();
      windowReplayLastIndex = 0;
      followingSource = followingTranslation = true;
      dirty = cacheDirty = true;
      saveActiveTask();
      render();
      status(paused ? "已清空显示；采集继续，翻译保持暂停"
             : running ? "已清空显示；正在执行和排队的翻译继续，新内容从下方加入"
                       : "已清空当前任务；其他任务缓存保留");
      return;
    }
    clearedSignatures.clear();
    clearedBlocks.clear();
    clearedSourceIndices.clear();
    ++generation;
    cancelAll();
    records.clear();
    sourceAliases.clear();
    replayCandidates.clear();
    windowReplayMemo.clear();
    windowReplayLastIndex = 0;
    sourceNeedsReplay = replayPendingBody = false;
    if (!activeTaskId.empty())
      taskCache->erase(activeTaskId);
    cacheDirty = false;
    followingSource = followingTranslation = true;
    serial = 0;
    dirty = true;
    status("已清空当前任务；其他任务的缓存保留");
  }
  void enqueue(Record &r) {
    worker->cancel(r.key);
    auto j = std::make_shared<Job>();
    j->generation = generation;
    j->version = r.version;
    j->key = r.key;
    j->taskId = activeTaskId;
    j->source = r.source;
    j->settings = settings;
    if (worker->submit(j)) {
      r.state = "queued";
      r.error.clear();
    } else {
      r.state = "error";
      r.error = "翻译队列已满，可稍后重试";
    }
    dirty = true;
  }
  void applyChatgptIdentity(bool readable,
                           const cclive::chatgpt::AccountStatus &account,
                           uint64_t revision) {
    if (revision <= chatgptIdentityRevision)
      return;
    chatgptIdentityRevision = revision;
    const auto nextHash = readable && account.signed_in
                              ? contentSignature(account.client_id) : "";
    const bool changed = nextHash != chatgptAccountHash;
    const bool usable = readable && account.signed_in && account.plan_enabled;
    if (changed || !usable) {
      // Keep completed text under its original activeProfile. Never relabel A's
      // cached translations as B merely because B's account file was committed.
      if (settings.provider == "chatgpt") {
        if (running)
          stop();
        else {
          saveActiveTask();
          ++generation;
          cancelAll();
        }
      }
      chatgptAccountHash = nextHash;
      settings.chatgpt_model.clear();
      saveOptions();
      provider();
      if (settings.provider == "chatgpt")
        status(usable ? "ChatGPT 账号已变化，请在设置中重新选择并保存模型。"
                      : "无法确认可用的 ChatGPT 授权，请在设置中重新授权并选择模型。");
    }
  }
  bool confirmChatgptIdentity() {
    if (settings.provider != "chatgpt")
      return true;
    // Auth actions can be awaiting a browser or model request. Never block the
    // UI on that mutex, and never send a request using an unconfirmed identity.
    std::unique_lock<std::mutex> lock(chatgptSettingsActionsMutex,
                                      std::try_to_lock);
    if (!lock.owns_lock()) {
      status("ChatGPT 账号操作尚未结束，请稍后开始或重试。");
      return false;
    }
    cclive::chatgpt::AccountStatus account;
    bool readable = true;
    try {
      account = chatgptIdentityReader();
    } catch (...) {
      readable = false;
    }
    applyChatgptIdentity(readable, account, ++chatgptIdentitySequence);
    if (!readable || !account.signed_in || !account.plan_enabled)
      return false;
    if (settings.chatgpt_model.empty()) {
      status("请在设置中为当前 ChatGPT 账号选择并保存可用模型。");
      return false;
    }
    return true;
  }
  void chatgptSettingsAction(const std::string &action,
                            const std::string &clientId = {}) {
    if (!config)
      return;
    if (action == "switch" || action == "signin" || action == "add" ||
        action == "signout") {
      // Invalidate the previous account's model before the commit, including
      // the case where the app exits before its identity notification arrives.
      settings.chatgpt_model.clear();
      saveOptions();
      provider();
      if (settings.provider == "chatgpt") {
        if (running)
          stop();
        else {
          ++generation;
          cancelAll();
        }
      }
    }
    launchChatgptSettingsAction(*config, bus, selfTest, action, clientId);
  }
  void acceptChatgptSettings(const json &event) {
    if (!config || config->authTicket != event.value("ticket", 0ULL) ||
        config->authCancel->load() ||
        std::string(selectedConfigProvider(*config)) != "chatgpt")
      return;
    auto &dialog = *config;
    auto readAccount = [](const json &value) {
      cclive::chatgpt::AccountStatus account;
      if (value.is_object()) {
        account.signed_in = value.value("signed_in", false);
        account.plan_enabled = value.value("plan_enabled", false);
        account.email = value.value("email", "");
        account.name = value.value("name", "");
        account.client_id = value.value("client_id", "");
        account.expires_at = value.value("expires_at", int64_t(0));
      }
      return account;
    };
    dialog.authBusy = false;
    dialog.authLoaded = true;
    dialog.account = readAccount(event.value("account", json::object()));
    dialog.accountList.clear();
    dialog.catalog.clear();
    SendMessageW(dialog.chatgptAccounts, CB_RESETCONTENT, 0, 0);
    int accountIndex = -1;
    for (const auto &value : event.value("accounts", json::array())) {
      auto account = readAccount(value);
      if (account.client_id.empty())
        continue;
      if (account.client_id == dialog.account.client_id)
        accountIndex = static_cast<int>(dialog.accountList.size());
      auto caption = !account.email.empty() ? account.email
                     : !account.name.empty() ? account.name : "已保存的账号";
      SendMessageW(dialog.chatgptAccounts, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(wide(caption).c_str()));
      dialog.accountList.push_back(std::move(account));
    }
    SendMessageW(dialog.chatgptAccounts, CB_SETCURSEL, accountIndex, 0);
    SendMessageW(dialog.chatgptModels, CB_RESETCONTENT, 0, 0);
    const std::string wanted = dialog.selectedChatgptModel.empty()
                                   ? settings.chatgpt_model
                                   : dialog.selectedChatgptModel;
    int modelIndex = -1;
    for (const auto &value : event.value("models", json::array())) {
      const auto id = value.value("id", "");
      if (id.empty())
        continue;
      auto name = value.value("name", id);
      if (id == wanted)
        modelIndex = static_cast<int>(dialog.catalog.size());
      const auto caption = name == id ? name : name + " (" + id + ")";
      SendMessageW(dialog.chatgptModels, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(wide(caption).c_str()));
      dialog.catalog.push_back({id, name});
    }
    if (modelIndex < 0 && !dialog.catalog.empty())
      modelIndex = 0;
    SendMessageW(dialog.chatgptModels, CB_SETCURSEL, modelIndex, 0);
    dialog.selectedChatgptModel = modelIndex >= 0
        ? dialog.catalog[static_cast<std::size_t>(modelIndex)].id : "";

    updateConfigProviderFields(dialog);
    SetWindowTextW(dialog.error, wide(event.value("error", "")).c_str());
  }
  void accept(json v) {
    auto type = v.value("type", "");
    if (type == "chatgpt_identity") {
      cclive::chatgpt::AccountStatus account;
      account.signed_in = v.value("signed_in", false);
      account.plan_enabled = v.value("plan_enabled", false);
      account.client_id = v.value("client_id", "");
      applyChatgptIdentity(v.value("readable", false), account,
                          v.value("revision", 0ULL));
      return;
    }
    if (type == "chatgpt_settings") {
      acceptChatgptSettings(v);
      return;
    }
    if (type == "windows") {
      if (v.value("_refresh_ticket", 0ULL) != refreshGeneration)
        return;
      refreshing = false;
      EnableWindow(refreshButton, TRUE);
      windows.clear();
      SendMessageW(combo, CB_RESETCONTENT, 0, 0);
      if (v.contains("windows") && v["windows"].is_array())
        for (auto &item : v["windows"]) {
          windows.push_back(item);
          auto label = wide(item.value("title", "Claude")) + L" · PID " +
                       std::to_wstring(item.value("pid", 0));
          SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        }
      if (windows.empty()) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"未找到 Claude 桌面窗口");
        startAfterRefresh = false;
        status("请先打开 Claude 桌面客户端，并进入有正文的会话");
      } else
        status("已找到 Claude 窗口，点击开始");
      SendMessageW(combo, CB_SETCURSEL, 0, 0);
      if (startAfterRefresh) {
        startAfterRefresh = false;
        start();
      }
      return;
    }
    if (v.value("_generation", sourceGeneration) != sourceGeneration)
      return;
    if (type == "status" || type == "error") {
      status(v.value("text", ""));
      return;
    }
    if (type == "task") {
      switchTask(v);
      if (v.value("task_id", "") == activeTaskId &&
          v.contains("source_block_keys") &&
          v["source_block_keys"].is_array()) {
        std::set<std::string> present;
        for (const auto &key : v["source_block_keys"])
          if (key.is_string())
            present.insert(key.get<std::string>());
        for (auto i = sourceAliases.begin(); i != sourceAliases.end();)
          if (!i->second.block.empty() && !present.count(i->second.block))
            i = sourceAliases.erase(i);
          else
            ++i;
      }
      return;
    }
    if (type != "text" && type != "reset")
      return;
    if (v.value("task_id", activeTaskId) != activeTaskId)
      return;
    int incoming = v.value("epoch", -1);
    if (incoming >= 0 && ledgerEpoch > incoming)
      return;
    if (type == "reset") {
      if (incoming >= 0 && ledgerEpoch >= incoming)
        return;
      sourceAliases.clear();
      if (ledgerEpoch >= 0 && !v.value("source_rebind", false)) {
        ++generation;
        cancelAll();
        for (auto &record : records)
          if (record.state != "done") {
            record.state = paused ? "paused" : "waiting";
            if (!paused && running)
              enqueue(record);
          }
      }
      ledgerEpoch = incoming;
      status("已连接当前任务，继续使用已缓存译文");
      return;
    }
    if (incoming >= 0) {
      if (ledgerEpoch >= 0 && incoming > ledgerEpoch)
        sourceAliases.clear();
      ledgerEpoch = incoming;
    }
    if (v.contains("source_index") && v["source_index"].is_number_integer() &&
        v["source_index"].get<std::size_t>() < minimumSourceIndex)
      return;
    auto key = v.value("key", ""), source = v.value("text", "");
    auto incomingSignature = contentSignature(source);
    if (key.empty())
      return;
    auto alias = sourceAliases.find(key);
    if (alias != sourceAliases.end() && alias->second.record.empty() &&
        alias->second.signature == incomingSignature)
      return;
    auto it = alias == sourceAliases.end()
                  ? records.end()
                  : std::find_if(records.begin(), records.end(),
                                 [&](const Record &record) {
                                   return record.key == alias->second.record;
                                 });
    if (source.empty()) {
      if (it != records.end()) {
        worker->cancel(it->key);
        records.erase(it);
      }
      sourceAliases.erase(key);
      dirty = cacheDirty = true;
      return;
    }
    if (it == records.end() && v.contains("source_index") &&
        v["source_index"].is_number_integer() &&
        (v["source_index"].is_number_unsigned() || v["source_index"].get<int64_t>() >= 0) &&
        clearedSourceIndices.count(v["source_index"].get<std::size_t>()) &&
        (!v.contains("block_key") || clearedBlocks.count(v.value("block_key", "")))) {
      sourceAliases[key] = {"", v.value("block_key", ""), incomingSignature};
      return;
    }
    if (it == records.end()) {
      it = std::find_if(records.begin(), records.end(),
                        [&](const Record &record) {
                          return replayCandidates.count(record.key) &&
                                 (record.source == source ||
                                  record.sourceSignature == incomingSignature);
                        });
      if (it != records.end())
        replayCandidates.erase(it->key);
    }

    bool createdRecord = false;
    if (it == records.end()) {
      std::string memo;
      if (auto replay = windowReplayMemo.find(incomingSignature);
          replay != windowReplayMemo.end())
        memo = replay->second;
      for (auto previous = records.rbegin(); previous != records.rend();
           ++previous)
        if ((previous->source == source ||
             previous->sourceSignature == incomingSignature) &&
            previous->state == "done" && !previous->pendingReplacement &&
            !previous->translation.empty()) {
          memo = previous->translation;
          break;
        }
      if (records.size() >= 240) {
        const auto evicted = records.front().key;
        worker->cancel(evicted);
        records.erase(records.begin());
        replayCandidates.erase(evicted);
        for (auto i = sourceAliases.begin(); i != sourceAliases.end();)
          if (i->second.record == evicted)
            i = sourceAliases.erase(i);
          else
            ++i;
      }
      Record record;
      record.serial = ++serial;
      record.key = "record-" + std::to_string(record.serial);
      record.source = source;
      record.sourceSignature = incomingSignature;
      record.version = ++revision;
      record.translation = memo;
      record.state = memo.empty() ? "waiting" : "done";
      if (!memo.empty()) record.displayedUpdate = ++displayedUpdateSequence;
      records.push_back(std::move(record));
      it = std::prev(records.end());
      createdRecord = true;
    }
    // Layout changes do not invalidate a translation or change its source key.
    bool layoutChanged = false;
    if (v.contains("block_key") && v["block_key"].is_string() &&
        !v["block_key"].get_ref<const std::string &>().empty() &&
        v.contains("paragraph") && v["paragraph"].is_number_integer() &&
        (v["paragraph"].is_number_unsigned() ||
         v["paragraph"].get<int64_t>() >= 0)) {
      const auto block = v["block_key"].get<std::string>();
      const auto paragraph = v["paragraph"].get<std::size_t>();
      layoutChanged = !it->hasLayout || it->layoutBlock != block ||
                      it->layoutParagraph != paragraph;
      it->hasLayout = true;
      it->layoutBlock = block;
      it->layoutParagraph = paragraph;
    }
    sourceAliases[key] = {it->key, v.value("block_key", ""), incomingSignature};
    if (!windowReplayMemo.empty() && v.contains("source_index") &&
        v["source_index"].is_number_integer() &&
        v["source_index"].get<std::size_t>() >= windowReplayLastIndex)
      windowReplayMemo.clear();
    if ((it->source == source || it->sourceSignature == incomingSignature) &&
        ((!createdRecord && layoutChanged) ||
         it->state == "done" || it->state == "queued" ||
         it->state == "translating" || it->state == "rate_limited" ||
         it->state == "paused")) {
      dirty = cacheDirty = true;
      trimRecords();
      return;
    }
    worker->cancel(it->key);
    it->pendingReplacement = it->pendingReplacement ||
        (it->state == "done" && !it->translation.empty());
    if (it->source != source) it->visible = true;
    it->source = std::move(source);
    it->sourceSignature = incomingSignature;
    if (!it->pendingReplacement)
      it->translation.clear();
    it->error.clear();
    it->version = ++revision;
    it->state = paused ? "paused" : "waiting";
    if (!paused)
      enqueue(*it);
    dirty = cacheDirty = true;
    trimRecords();
  }
  void reflowBodies() {
    ScrollGuard guard(internalScroll);
    struct View { HWND window; POINT scroll{}; CHARRANGE selected{}; };
    View views[] = {{sourceEdit}, {translationEdit}};
    for (auto &view : views) {
      SendMessageW(view.window, EM_GETSCROLLPOS, 0,
                   reinterpret_cast<LPARAM>(&view.scroll));
      SendMessageW(view.window, EM_EXGETSEL, 0,
                   reinterpret_cast<LPARAM>(&view.selected));
      // Use this control's current client width, including after a manual refresh.
      SendMessageW(view.window, EM_SETTARGETDEVICE, 0, 0);
      SendMessageW(view.window, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                   MAKELPARAM(s(14), s(14)));
    }
    ++bodyStyleRevision;
    layout();
    render();
    for (const auto &view : views) {
      SendMessageW(view.window, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&view.selected));
      if (!follows(view.window))
        SendMessageW(view.window, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&view.scroll));
      repaintBody(view.window);
    }
  }
  void refresh() {
    reflowBodies();
    if (demo || selfTest) {
      status("已重新排版当前内容 · 离线演示，不读取 Claude");
      return;
    }
    if (refreshing)
      return;
    refreshing = true;
    EnableWindow(refreshButton, FALSE);
    status("正在查找 Claude 桌面窗口…");
    auto b = bus;
    auto ticket = ++refreshGeneration;
    std::thread([b, ticket] {
      try {
        auto values = json::parse(cclive::enumerate_windows_json());
        b->emit({{"type", "windows"},
                 {"windows", values},
                 {"_refresh_ticket", ticket}});
      } catch (...) {
        b->emit({{"type", "windows"},
                 {"windows", json::array()},
                 {"_refresh_ticket", ticket}});
      }
    }).detach();
  }
  void start() {
    if (running || closed)
      return;
    if (!confirmChatgptIdentity())
      return;
    if (!demo && !selfTest) {
      int index = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
      if (index < 0 || index >= (int)windows.size()) {
        startAfterRefresh = true;
        refresh();
        return;
      }
      ++sourceGeneration;
      saveActiveTask();
      ++generation;
      cancelAll();
      sourceAliases.clear();
      sourceNeedsReplay = true;
      ledgerEpoch = -1;
      selectedSourceHwnd = windows[index].value("hwnd", 0ULL);
      captureSeq = capturedBlocks = capturedChars = 0;
      lastReadMs = 0;
      watcher = std::make_shared<Watcher>(selectedSourceHwnd, sourceGeneration,
                                          settings.settle_ms,
                                          std::vector<std::string>{
                                              settings.api_key_env,
                                              settings.baidu_key_env}, bus);
      std::thread([w = watcher] { w->run(); }).detach();
    } else {
      ++sourceGeneration;
      clear();
      ledgerEpoch = -1;
      demoStart = Clock::now();
      demoStep = -1;
      demoLedger = std::make_unique<cclive::Ledger>(100);
    }
    running = true;
    paused = false;
    SetWindowTextW(startButton, L"停止");
    EnableWindow(pauseButton, TRUE);
    EnableWindow(combo, FALSE);
    SetWindowTextW(pauseButton, L"暂停");
    status(demo || selfTest ? "离线演示 · 合成文字，不读取 Claude"
                            : "正在连接 Claude，等待可访问的助手正文…");
  }
  void stop() {
    saveActiveTask();
    running = false;
    paused = false;
    startAfterRefresh = false;
    ++sourceGeneration;
    ++generation;
    cancelAll();
    if (watcher) {
      watcher->stop();
      watcher.reset();
    }
    demoLedger.reset();
    for (auto &r : records)
      if (r.state != "done")
        r.state = "stopped";
    saveActiveTask();
    if (!closed) {
      SetWindowTextW(startButton, L"开始");
      SetWindowTextW(pauseButton, L"暂停");
      EnableWindow(pauseButton, FALSE);
      EnableWindow(combo, !(demo || selfTest));
      status("已停止；当前译文保留");
      dirty = true;
    }
  }
  void pause() {
    if (!running)
      return;
    paused = !paused;
    ++generation;
    cancelAll();
    for (auto &r : records)
      if (r.state != "done") {
        r.state = paused ? "paused" : "waiting";
        if (!paused)
          enqueue(r);
      }
    SetWindowTextW(pauseButton, paused ? L"继续" : L"暂停");
    status(paused ? "已暂停翻译，继续收集新增文字" : "已继续翻译");
    dirty = true;
  }
  void retry() {
    if (paused) {
      status("请先点击继续");
      return;
    }
    if (!confirmChatgptIdentity())
      return;
    if ((settings.provider == "chatgpt" || settings.provider == "codex") &&
        !records.empty() && activeProfile != translationProfile()) {
      status(settings.provider == "codex"
                 ? "Codex CLI 路径或模型已变化，请点击开始重新读取后再重试。"
                 : "ChatGPT 账号或模型已变化，请点击开始重新读取后再重试。");
      return;
    }
    for (auto &r : records)
      if (r.state == "error" || r.state == "stopped") {
        r.version = ++revision;
        enqueue(r);
      }
  }
  void saveOptions() {
    if (demo || selfTest)
      return;
    try {
      cclive::save_settings(configPath, settings);
    } catch (const std::exception &e) {
      status(e.what());
    }
  }
  static std::wstring displayContent(const std::string &raw) {
    auto value = wide(raw);
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
      return {};
    // Remove empty edge lines, preserving the first content line's indentation.
    const auto newline = value.find_last_of(L"\r\n", first);
    const auto begin = newline == std::wstring::npos ? 0 : newline + 1;
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(begin, last - begin + 1);
  }
  static bool cjkBoundary(wchar_t value) {
    return (value >= 0x2e80 && value <= 0x9fff) ||
           (value >= 0xac00 && value <= 0xd7af) ||
           (value >= 0xf900 && value <= 0xfaff) ||
           (value >= 0xff00 && value <= 0xffef);
  }
  static std::pair<wchar_t, wchar_t> displayEdges(const std::wstring &content) {
    const auto visible = cclive::make_text_layout(content).text;
    const auto first = visible.find_first_not_of(L" \t\r\n\"'\u2018\u201c([{");
    const auto last = visible.find_last_not_of(L" \t\r\n\"'\u2019\u201d)]}");
    return {first == std::wstring::npos ? 0 : visible[first],
            last == std::wstring::npos ? 0 : visible[last]};
  }
  static bool displaySeamNeedsSpace(wchar_t left, wchar_t right) {
    auto asciiWord = [](wchar_t value) {
      return (value >= L'A' && value <= L'Z') ||
             (value >= L'a' && value <= L'z') ||
             (value >= L'0' && value <= L'9');
    };
    const bool leftCjk = cjkBoundary(left), rightCjk = cjkBoundary(right);
    return (!leftCjk && !rightCjk) ||
           (leftCjk && asciiWord(right)) ||
           (asciiWord(left) && rightCjk) ||
           ((left == L'.' || left == L'!' || left == L'?') && rightCjk);
  }
  struct BodyOutput {
    std::wstring text;
    LONG latestStart = -1, latestLength = 0;
  };
  const Record *latestVisibleRecord(std::size_t begin = 0) const {
    const Record *latest = nullptr;
    for (std::size_t index = begin; index < records.size(); ++index) {
      const auto &record = records[index];
      if (record.visible && !displayContent(record.translation).empty() &&
          (!latest || record.displayedUpdate >= latest->displayedUpdate))
        latest = &record;
    }
    return latest;
  }
  BodyOutput bodyOutput(bool original) const {
    BodyOutput body;
    if (records.empty())
      return body;
    auto &value = body.text;
    size_t begin = records.size(), chars = 0;
    while (begin > 0) {
      const auto &record = records[begin - 1];
      const size_t add = !record.visible ? 0 : original ? record.source.size() : record.translation.size();
      if (chars + add > 1000000 && begin < records.size())
        break;
      chars += add;
      --begin;
    }
    if (begin)
      value = L"仅显示最近保留的字幕内容。";
    const auto *latest = latestVisibleRecord(begin);
    const Record *previous = nullptr;
    wchar_t previousEdge = 0;
    for (size_t index = begin; index < records.size(); ++index) {
      const auto &record = records[index];
      if (!record.visible) { previous = nullptr; continue; }
      auto content = displayContent(original ? record.source : record.translation);
      if (content.empty()) {
        // Do not join completed text across an untranslated or failed sentence.
        previous = nullptr;
        continue;
      }
      const auto edges = displayEdges(content);
      if (!value.empty()) {
        const bool sameParagraph = previous && previous->hasLayout &&
            record.hasLayout && previous->layoutBlock == record.layoutBlock &&
            previous->layoutParagraph == record.layoutParagraph;
        if (!sameParagraph)
          value += L"\r\n";
        else if (!iswspace(value.back()) && !iswspace(content.front()) &&
                 displaySeamNeedsSpace(previousEdge, edges.first))
          value += L' ';
      }
      if (&record == latest) {
        body.latestStart = static_cast<LONG>(value.size());
        body.latestLength = static_cast<LONG>(content.size());
      }
      value += content;
      previous = &record;
      previousEdge = edges.second;
    }
    return body;
  }
  std::wstring output(bool original) const { return bodyOutput(original).text; }
  std::wstring progressCaption() const {
    std::size_t done = 0, waiting = 0, updating = 0, errors = 0;
    for (const auto &record : records) {
      if (record.pendingReplacement)
        ++updating;
      if (record.state == "error")
        ++errors;
      else if (record.state == "done" && !record.pendingReplacement)
        ++done;
      else if (!record.pendingReplacement)
        ++waiting;
    }
    return L"已译 " + std::to_wstring(done) + L" · 待译 " +
        std::to_wstring(waiting) + L" · 更新中 " + std::to_wstring(updating) +
        L" · 失败 " + std::to_wstring(errors);
  }
  bool &follows(HWND edit) {
    return edit == sourceEdit ? followingSource : followingTranslation;
  }
  bool isAtBottom(HWND edit) const {
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    return GetScrollInfo(edit, SB_VERT, &info) &&
           info.nPos + static_cast<int>(info.nPage) >= info.nMax - 3;
  }
  struct ScrollGuard {
    bool &value;
    bool previous;
    explicit ScrollGuard(bool &v) : value(v), previous(v) { value = true; }
    ~ScrollGuard() { value = previous; }
  };
  static void repaintBody(HWND edit) {
    if (!edit || !IsWindow(edit))
      return;
    // RichEdit may otherwise keep old glyphs in the exposed blank area after
    // scrolling or WM_SETREDRAW. Finish erasing/painting before returning, even
    // when no more translation events arrive.
    RedrawWindow(edit, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW |
                     RDW_NOCHILDREN);
  }
  void scrollToLatest(HWND edit) {
    auto found = renderedBodies.find(edit);
    if (found == renderedBodies.end() || found->second.latestDisplay.cpMin < 0 ||
        found->second.latestDisplay.cpMax >= found->second.nativeLength) {
      SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
      return;
    }
    const auto end = std::max(found->second.latestDisplay.cpMin,
                              found->second.latestDisplay.cpMax - 1);
    POINTL position{};
    POINT scroll{};
    RECT viewport{};
    SendMessageW(edit, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&position), end);
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&viewport));
    // Use the text's native position, without moving the user's caret/selection.
    // This also works when the read-only control has no focus or is hidden.
    scroll.y = std::max(0L, scroll.y + position.y - viewport.top -
                            std::max(0L, (viewport.bottom - viewport.top) / 4));
    SendMessageW(edit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
  }
  void goLatest() {
    followingSource = followingTranslation = true;
    ScrollGuard guard(internalScroll);
    for (HWND edit : {translationEdit, sourceEdit}) {
      if (!edit)
        continue;
      SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
      scrollToLatest(edit);
      repaintBody(edit);
    }
    status("青色底纹标记最新更新的字幕；正在跟随，向上滚动可暂停跟随");
  }
  static LRESULT CALLBACK bodyProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, DWORD_PTR reference) {
    auto *app = reinterpret_cast<App *>(reference);
    if (msg == WM_NCDESTROY) {
      RemoveWindowSubclass(edit, bodyProc, id);
      return DefSubclassProc(edit, msg, wp, lp);
    }
    if (msg == WM_ERASEBKGND && app && wp) {
      // Native RichEdit acknowledges this message without reliably clearing
      // unused pixels. Fill the invalid DC region, including below the text.
      RECT client{};
      GetClientRect(edit, &client);
      FillRect(reinterpret_cast<HDC>(wp), &client, app->panel);
      return 1;
    }
    bool navigation =
        msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR ||
                              wp == VK_NEXT || wp == VK_HOME || wp == VK_END);
    bool manual = app && !app->closed && !app->internalScroll &&
                  (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL ||
                   msg == EM_LINESCROLL || msg == EM_SETSCROLLPOS || navigation);
    POINT before{};
    if (manual)
      SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&before));
    auto result = DefSubclassProc(edit, msg, wp, lp);
    if (manual) {
      POINT after{};
      SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&after));
      if (after.y < before.y || !app->isAtBottom(edit)) {
        app->follows(edit) = false;
        app->cacheDirty = true;
      }
      if (after.x != before.x || after.y != before.y)
        repaintBody(edit);
    }
    return result;
  }
  void editText(HWND edit, const std::wstring &value, LONG latestStart = -1, LONG latestLength = 0) {
    auto &rendered = renderedBodies[edit];
    RECT bodyArea{};
    GetClientRect(edit, &bodyArea);
    const auto bodyWidth = bodyArea.right - bodyArea.left;
    if (rendered.valid && rendered.input == value &&
        rendered.styleRevision == bodyStyleRevision &&
        rendered.fontPoints == settings.font_size && rendered.dpi == dpi &&
        (!rendered.hasTables || rendered.bodyWidth == bodyWidth) &&
        rendered.latestInputStart == latestStart && rendered.latestInputLength == latestLength)
      return;
    auto formatted = cclive::make_text_layout(value, latestStart, latestLength);
    bool bottom = follows(edit);
    ScrollGuard guard(internalScroll);
    POINT scroll{};
    SendMessageW(edit, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    CHARRANGE selected{};
    SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&selected);
    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    // A style refresh need not replace identical text or disturb its selection.
    const bool tableReflow = formatted.has_tables &&
        (rendered.input != value || rendered.styleRevision != bodyStyleRevision || rendered.fontPoints != settings.font_size ||
         rendered.dpi != dpi || rendered.bodyWidth != bodyWidth);
    if (!rendered.valid || rendered.displayed != formatted.text ||
        rendered.hasTables != formatted.has_tables || tableReflow) {
      if (!cclive::replace_text_layout(edit, formatted)) {
        SetWindowTextW(edit, formatted.text.c_str());
        formatted.has_tables = false;
        formatted.native_positions.clear();
        for (auto &paragraph : formatted.paragraphs) paragraph.table = false;
        status("表格排版未完成，暂时显示单元格文字");
      }
      ++rendered.replacements;
    }
    cclive::apply_text_layout(edit, formatted, settings.font_size, dpi);
    ++rendered.formats;
    rendered.input = value;
    rendered.displayed = formatted.text;
    rendered.styleRevision = bodyStyleRevision;
    rendered.fontPoints = settings.font_size;
    rendered.dpi = dpi;
    rendered.bodyWidth = bodyWidth;
    rendered.hasTables = formatted.has_tables;
    rendered.nativeLength = cclive::native_text_position(formatted, static_cast<LONG>(formatted.text.size()));
    rendered.latestInputStart = latestStart;
    rendered.latestInputLength = latestLength;
    rendered.latestDisplay = {-1, -1};
    for (const auto &range : formatted.ranges)
      if (range.style == cclive::TextStyle::Latest)
        rendered.latestDisplay = {cclive::native_text_position(formatted, range.start),
                                  cclive::native_text_position(formatted, range.start + range.length)};
    rendered.valid = true;
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&selected);
    SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
    if (bottom)
      scrollToLatest(edit);
    else
      SendMessageW(edit, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    repaintBody(edit);
  }
  void render() {
    if (closed)
      return;
    if (settings.show_original) {
      const auto source = bodyOutput(true);
      editText(sourceEdit, source.text, source.latestStart, source.latestLength);
    }
    const auto translated = bodyOutput(false);
    editText(translationEdit, translated.text, translated.latestStart, translated.latestLength);
    const auto caption = (translated.latestLength > 0 ? L"青色＝最新 · " : L"") + progressCaption();
    SetWindowTextW(countLabel, caption.c_str());
    dirty = false;
    lastRender = Clock::now();
  }
  void refreshAppearance() {
    struct View {
      HWND window;
      POINT scroll{};
      CHARRANGE selected{};
    };
    View views[] = {{sourceEdit}, {translationEdit}};
    ScrollGuard guard(internalScroll);
    for (auto &view : views) {
      SendMessageW(view.window, EM_GETSCROLLPOS, 0,
                   reinterpret_cast<LPARAM>(&view.scroll));
      SendMessageW(view.window, EM_EXGETSEL, 0,
                   reinterpret_cast<LPARAM>(&view.selected));
    }
    fonts();
    layout();
    render();
    for (const auto &view : views) {
      SendMessageW(view.window, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&view.selected));
      if (!follows(view.window))
        SendMessageW(view.window, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&view.scroll));
      repaintBody(view.window);
    }
  }
  void applySettings(cclive::Settings updated, bool keyChanged) {
    if (demo || selfTest)
      updated.provider = "demo";
    const bool restart = keyChanged || settings.provider != updated.provider ||
                         settings.base_url != updated.base_url ||
                         settings.model != updated.model ||
                         settings.api_key_env != updated.api_key_env ||
                         settings.baidu_app_id != updated.baidu_app_id ||
                         settings.baidu_key_env != updated.baidu_key_env ||
                         settings.chatgpt_model != updated.chatgpt_model ||
                         settings.codex_path != updated.codex_path ||
                         settings.codex_model != updated.codex_model ||
                         settings.target != updated.target ||
                         settings.settle_ms != updated.settle_ms;
    const bool wasRunning = running, wasPaused = paused;
    if (restart && wasRunning)
      stop();
    settings = std::move(updated);
    refreshAppearance();
    provider();
    if (restart && wasRunning) {
      start();
      if (wasPaused && running)
        pause();
    }
    status(wasPaused && running ? "设置已应用；翻译保持暂停"
           : running            ? "设置已应用；继续翻译"
                                : "设置已应用");
  }
  void layoutDemo() {
    activeTaskId = "offline-layout-preview";
    activeTaskTitle = "段落与列表排版演示（合成内容）";
    activeIdentityQuality = "url";
    activeProfile = translationProfile();
    updateTaskTitle();
    auto add = [&](const std::string &source, const std::string &translated,
                   std::size_t paragraph, const std::string &state = "done",
                   bool replacement = false, const std::string &error = "") {
      Record record;
      record.serial = ++serial;
      record.version = ++revision;
      record.key = "layout-demo-" + std::to_string(record.serial);
      record.source = source;
      record.sourceSignature = contentSignature(source);
      record.translation = translated;
      record.state = state;
      record.error = error;
      record.pendingReplacement = replacement;
      record.hasLayout = true;
      record.layoutBlock = "layout-demo-answer";
      record.layoutParagraph = paragraph;
      records.push_back(std::move(record));
    };
    add("## Paragraphs that read naturally", "## 让段落自然连贯", 0);
    add("The translator preserves complete paragraphs.",
        "翻译窗口会保留完整段落。", 1);
    add("Sentences from the same paragraph stay together as the window narrows.",
        "同一段内的句子会自然衔接，缩窄窗口后也会按照中文习惯换行，不再每句话单独占一行。", 1);
    add("This is a separate paragraph with a clear boundary.",
        "这是另一个独立段落，应与上面的完整段落保持适量间距。段落边界来自原文结构，无需根据句号猜测。", 12);
    add("## A clear plan", "## 清晰的阅读顺序", 2);
    add("- Keep completed translations visible while a revised sentence is being translated, even when the source refreshes or a request takes longer than expected.",
        "- 在原文持续更新、翻译请求耗时较长时，保留已经确认的译文，直到新版本完整返回。较长的列表内容应自然换行，并与首行正文保持对齐。", 3);
    add("  - Preserve nested items and keep the detail attached to its parent so a narrower window remains easy to read.",
        "  - 保留嵌套列表及其层级关系。窗口变窄后，补充说明仍应保持清晰的缩进，不与相邻项目混在一起。", 4);
    add("1. Check the original paragraph structure, headings, and item boundaries before combining translated sentences.",
        "1. 先确认原文段落、标题和列表边界，再合并同段译句。即使一项包含较长的说明，换行后也应保持正文对齐。", 5);
    add("10. Keep file names such as `src/main.cpp`, model names, and API identifiers unchanged in the translation.",
        "10. 保留 `src/main.cpp`、模型名称和 API 标识，保证中英文混排时仍然容易辨认。较宽的两位数编号也应与换行后的正文列对齐，不把后续文字挤成另一种缩进。", 6);
    add("### Reading without interruptions", "### 阅读时减少打扰", 7);
    add("A revised paragraph is being translated.",
        "这段已确认的译文会在更新期间保留，新结果完成后再整体替换。", 8, "queued", true);
    add("This sentence is waiting for translation.", "", 9, "queued");
    add("This synthetic request failed and can be retried.", "", 10,
        "error", false, "离线演示错误，可重试");
    trimRecords();
    dirty = cacheDirty = true;
    status("离线排版示例 · 可缩窄窗口或切换原文；不读取 Claude、不访问网络");
  }
  void synthetic() {
    if (demo && !selfTest) {
      if (running && demoStep < 0) {
        demoStep = 18;
        layoutDemo();
      }
      return;
    }
    if (!running || !demoLedger)
      return;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - demoStart)
                       .count();
    int step = (int)(elapsed / 160);
    if (step != demoStep && step < 18) {
      demoStep = step;
      static const char *phrases[] = {
          "I will inspect the project files first.",
          "I found the issue and will update the implementation.",
          "All tests passed successfully."};
      int message = std::min(step / 6, 2), part = step % 6;
      std::string text = phrases[message];
      if (part < 2) {
        size_t n = text.size() * (part + 1) / 3;
        size_t cut = text.rfind(' ', n);
        text = text.substr(0, cut == std::string::npos ? n : cut);
      }
      json snapshot = {
          {"context", "offline-demo"},
          {"blocks", json::array({{{"key", "demo-" + std::to_string(message)},
                                   {"text", text},
                                   {"kind", "answer"}}})}};
      accept({{"type", "task"},
              {"task_id", "offline-demo"},
              {"task_title", "离线演示"},
              {"identity_quality", "url"},
              {"source_hwnd", 0},
              {"_generation", sourceGeneration}});
      for (auto e : demoLedger->observe(snapshot)) {
        e["task_id"] = "offline-demo";
        e["_generation"] = sourceGeneration;
        accept(std::move(e));
      }
    }
    for (auto e : demoLedger->flush()) {
      e["task_id"] = "offline-demo";
      e["_generation"] = sourceGeneration;
      accept(std::move(e));
    }
  }
  json diagnosticMetadata() const {
    std::size_t done = 0, errors = 0, translated = 0;
    for (const auto &record : records) {
      if (record.state == "done")
        ++done;
      if (record.state == "error")
        ++errors;
      translated += characterCount(record.translation);
    }
    return {{"capture_seq", captureSeq},
            {"last_read_ms", lastReadMs},
            {"captured_blocks", capturedBlocks},
            {"captured_chars", capturedChars},
            {"running", running},
            {"paused", paused},
            {"records", records.size()},
            {"done", done},
            {"error", errors},
            {"translation_chars_total", translated},
            {"last_source_minimized",
             selectedSourceHwnd != 0 &&
                 IsIconic(reinterpret_cast<HWND>(selectedSourceHwnd)) != FALSE},
            {"source_selected", selectedSourceHwnd != 0},
            {"source_generation", sourceGeneration}};
  }
  void publishDiagnostic(bool force = false) {
    if (statusPath.empty())
      return;
    auto now = Clock::now();
    if (!force && lastDiagnostic != Clock::time_point::min() &&
        now - lastDiagnostic < std::chrono::seconds(1))
      return;
    if (diagnosticWriter->busy.exchange(true))
      return;
    lastDiagnostic = now;
    auto state = diagnosticWriter;
    auto path = statusPath;
    auto payload = diagnosticMetadata().dump(2) + "\n";
    try {
      std::thread([state, path, payload = std::move(payload)] {
        try {
          atomicStatusFile(path, payload);
          ++state->completed;
          state->failed.store(false);
        } catch (...) {
          state->failed.store(true);
        }
        state->busy.store(false);
      }).detach();
    } catch (...) {
      state->failed.store(true);
      state->busy.store(false);
    }
  }
  void acceptTelemetry(const json &v) {
    if (v.value("_generation", 0ULL) != sourceGeneration)
      return;
    captureSeq = v.value("capture_seq", 0ULL);
    lastReadMs = v.value("last_read_ms", 0.0);
    capturedBlocks = v.value("captured_blocks", std::size_t(0));
    capturedChars = v.value("captured_chars", std::size_t(0));
  }
  void acceptResult(const json &v) {
    if (v.value("task_id", activeTaskId) != activeTaskId ||
        v.value("generation", 0ULL) != generation)
      return;
    auto it =
        std::find_if(records.begin(), records.end(), [&](const Record &r) {
          return r.key == v.value("key", "");
        });
    if (it == records.end() || it->version != v.value("version", 0ULL))
      return;
    const auto previousState = it->state;
    const auto previousTranslation = it->translation;
    const bool wasVisible = it->visible, wasReplacement = it->pendingReplacement;
    it->state = v.value("state", "");
    auto translated = v.value("translation", "");
    if (!translated.empty()) it->visible = true;
    if (!it->pendingReplacement)
      it->translation = std::move(translated);
    else if (it->state == "done" && !translated.empty()) {
      it->translation = std::move(translated);
      it->pendingReplacement = false;
    } else if (it->state == "done") {
      it->state = "error";
    }
    if (it->visible && !it->translation.empty() &&
        (!wasVisible || it->translation != previousTranslation ||
         (wasReplacement && !it->pendingReplacement && it->state == "done")))
      it->displayedUpdate = ++displayedUpdateSequence;
    it->error = v.value("error", "");
    if (it->pendingReplacement && it->state == "error" && it->error.empty())
      it->error = "更新翻译未完成，可重试";
    dirty = cacheDirty = true;
    if (it->state == "error")
      status(it->error.empty() ? "部分文字翻译失败，可检查设置后重试"
                               : "翻译失败：" + it->error + "（可重试）");
    else if (it->state == "rate_limited")
      status(it->error);
    else if (previousState == "rate_limited" &&
             (it->state == "translating" || it->state == "done"))
      status("限流等待已结束，继续翻译");
    trimRecords();
  }
  json checkUiRegressions() {
    // Self-test only: supplied synthetic events, hidden native windows, demo
    // provider. No window enumeration, Claude reading, clipboard or network.
    json checks = workerRateLimitChecks();
    auto check = [&](const char *name, bool passed) {
      checks.push_back({{"name", name}, {"passed", passed}});
    };
    check("auto_start_respects_offline_self_test",
          !autoStart || (running && !watcher && demoLedger));
    auto metadataSafe = [](const json &metadata) {
      const std::set<std::string> allowed = {"capture_seq",
                                             "last_read_ms",
                                             "captured_blocks",
                                             "captured_chars",
                                             "running",
                                             "paused",
                                             "records",
                                             "done",
                                             "error",
                                             "translation_chars_total",
                                             "last_source_minimized",
                                             "source_selected",
                                             "source_generation"};
      if (!metadata.is_object() || metadata.size() != allowed.size())
        return false;
      for (const auto &entry : metadata.items())
        if (!allowed.count(entry.key()) ||
            !(entry.value().is_number() || entry.value().is_boolean()))
          return false;
      return true;
    };
    check("diagnostic_schema_excludes_body_and_identifiers",
          metadataSafe(diagnosticMetadata()));
    if (!statusPath.empty()) {
      bool persisted = false;
      try {
        std::ifstream in(statusPath);
        persisted = metadataSafe(json::parse(in));
      } catch (...) {
      }
      check("explicit_status_file_written_atomically_with_metadata_only",
            diagnosticWriter->completed.load() > 0 && persisted);
    } else {
      check("diagnostics_disabled_by_default",
            diagnosticWriter->completed.load() == 0);
    }
    auto liveSequence = captureSeq;
    acceptTelemetry({{"_generation", sourceGeneration},
                     {"capture_seq", liveSequence + 1},
                     {"last_read_ms", 42.0},
                     {"captured_blocks", 2},
                     {"captured_chars", 100}});
    check("capture_telemetry_updates_without_retaining_body",
          captureSeq == liveSequence + 1 && lastReadMs == 42 &&
              capturedBlocks == 2 && capturedChars == 100);
    acceptTelemetry({{"_generation", sourceGeneration - 1},
                     {"capture_seq", 0},
                     {"last_read_ms", 0},
                     {"captured_blocks", 0},
                     {"captured_chars", 0}});
    check("obsolete_capture_telemetry_rejected",
          captureSeq == liveSequence + 1);
    Config *hiddenConfig = new Config();
    hiddenConfig->app = this;
    config = hiddenConfig;
    HWND hidden = CreateWindowExW(
        0, L"CCLiveSettings", L"Hidden settings regression fixture",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, 0, s(614), s(602), hwnd,
        nullptr, GetModuleHandleW(nullptr), hiddenConfig);
    if (!hidden) {
      if (config) {
        delete config;
        config = nullptr;
      }
      check("hidden_settings_created", false);
    } else {
      check("settings_remains_hidden", !IsWindowVisible(hidden));
    }
    HFONT oldFont = font, oldHeading = headingFont, oldSmall = smallFont;
    fonts();
    check("font_handles_replaced_before_release",
          font && headingFont && smallFont && font != oldFont &&
              headingFont != oldHeading && smallFont != oldSmall);
    auto currentFont = [](HWND child) {
      return reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0));
    };
    check("heading_font_rebound", currentFont(title) == headingFont);
    check("toolbar_font_rebound", currentFont(startButton) == smallFont &&
                                      currentFont(refreshButton) == smallFont);
    auto bodyFontMatches = [&](HWND body) {
      CHARFORMAT2W actual{};
      actual.cbSize = sizeof(actual);
      SendMessageW(body, EM_GETCHARFORMAT, SCF_DEFAULT,
                   reinterpret_cast<LPARAM>(&actual));
      return (actual.dwMask & (CFM_FACE | CFM_SIZE)) == (CFM_FACE | CFM_SIZE) &&
             actual.yHeight == settings.font_size * 20 &&
             wcscmp(actual.szFaceName, L"Microsoft YaHei UI") == 0;
    };
    check("body_font_rebound",
          bodyFontMatches(sourceEdit) && bodyFontMatches(translationEdit));
    if (hidden) {
      bool matched = !hiddenConfig->edits.empty();
      for (auto child : hiddenConfig->edits)
        matched = matched && currentFont(child) == smallFont;
      check("settings_font_rebound", matched);
    }
    struct ReleasedFonts {
      HFONT font, heading, small;
      bool found = false;
    };
    ReleasedFonts released{oldFont, oldHeading, oldSmall, false};
    auto findReleased = [](HWND child, LPARAM payload) -> BOOL {
      auto &old = *reinterpret_cast<ReleasedFonts *>(payload);
      auto used =
          reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0));
      if (used &&
          (used == old.font || used == old.heading || used == old.small))
        old.found = true;
      return TRUE;
    };
    EnumChildWindows(hwnd, findReleased, reinterpret_cast<LPARAM>(&released));
    if (hidden)
      EnumChildWindows(hidden, findReleased,
                       reinterpret_cast<LPARAM>(&released));
    check("no_control_retains_released_font", !released.found);
    check("embedded_application_icons_are_loaded",
          SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0) &&
              SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0) &&
              SendMessageW(iconLabel, STM_GETICON, 0, 0));
    check("dark_buttons_retain_native_keyboard_and_checkbox_semantics",
          (GetWindowLongPtrW(startButton, GWL_STYLE) & BS_TYPEMASK) ==
                  BS_OWNERDRAW &&
              (SendMessageW(startButton, WM_GETDLGCODE, 0, 0) &
               DLGC_DEFPUSHBUTTON) &&
              (GetWindowLongPtrW(originalCheck, GWL_STYLE) & BS_TYPEMASK) ==
                  BS_AUTOCHECKBOX);
    // Exercise the actual hidden Settings save handler, rather than just
    // comparing a font handle or the empty-control default character format.
    auto fontFixtureRecords = records;
    bool fixturePaused = paused;
    Record fontFixture;
    fontFixture.key = "font-settings-fixture";
    fontFixture.state = "done";
    fontFixture.translation = "# Heading\nBody paragraph\n**bold**\n";
    for (int line = 0; line < 60; ++line)
      fontFixture.translation += "Readability fixture paragraph.\n";
    records = {fontFixture};
    paused = true;
    render();
    SendMessageW(translationEdit, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(translationEdit, EM_LINESCROLL, 0, 5);
    CHARRANGE fontSelection{8, 12};
    SendMessageW(translationEdit, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&fontSelection));
    POINT fontScroll{};
    SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&fontScroll));
    const auto fontGeneration = generation,
               fontSourceGeneration = sourceGeneration;
    const bool fontRunning = running;
    auto fontStyleMatches = [&](const wchar_t *needle, int points, bool bold) {
      const auto displayed =
          cclive::make_text_layout(wide(fontFixture.translation)).text;
      const auto at = displayed.find(needle);
      if (at == std::wstring::npos)
        return false;
      ScrollGuard guard(internalScroll);
      CHARRANGE selected{};
      SendMessageW(translationEdit, EM_EXGETSEL, 0,
                   reinterpret_cast<LPARAM>(&selected));
      CHARRANGE range{static_cast<LONG>(at),
                      static_cast<LONG>(at + wcslen(needle))};
      SendMessageW(translationEdit, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&range));
      CHARFORMAT2W actual{};
      actual.cbSize = sizeof(actual);
      SendMessageW(translationEdit, EM_GETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&actual));
      SendMessageW(translationEdit, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&selected));
      return actual.yHeight == points * 20 &&
             !!(actual.dwEffects & CFE_BOLD) == bold;
    };
    auto fixtureFontsMatch = [&](int points) {
      return fontStyleMatches(L"Heading", points + 3, true) &&
             fontStyleMatches(L"Body paragraph", points, false) &&
             fontStyleMatches(L"bold", points, true);
    };
    if (hidden) {
      SetWindowTextW(hiddenConfig->edits[6], L"16");
      SendMessageW(hidden, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check(
          "settings_save_immediately_applies_16pt_to_existing_body_and_styles",
          !config && settings.font_size == 16 && fixtureFontsMatch(16));
      render();
      check("settings_font_size_survives_subsequent_render",
            fixtureFontsMatch(16));
      CHARRANGE after{};
      POINT afterScroll{};
      SendMessageW(translationEdit, EM_EXGETSEL, 0,
                   reinterpret_cast<LPARAM>(&after));
      SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                   reinterpret_cast<LPARAM>(&afterScroll));
      check("font_only_save_preserves_capture_pause_selection_and_scroll",
            running == fontRunning && paused && generation == fontGeneration &&
                sourceGeneration == fontSourceGeneration &&
                after.cpMin == fontSelection.cpMin &&
                after.cpMax == fontSelection.cpMax && !followingTranslation &&
                abs(afterScroll.y - fontScroll.y) <= 2);
      auto *resetConfig = new Config();
      resetConfig->app = this;
      config = resetConfig;
      HWND reset = CreateWindowExW(
          0, L"CCLiveSettings", L"Hidden font reset fixture",
          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, 0, s(614), s(602), hwnd,
          nullptr, GetModuleHandleW(nullptr), resetConfig);
      if (reset) {
        SetWindowTextW(resetConfig->edits[6], L"10");
        SendMessageW(reset, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        render();
        check("settings_save_restores_10pt_without_losing_markdown_styles",
              !config && settings.font_size == 10 && fixtureFontsMatch(10));
      } else {
        delete resetConfig;
        config = nullptr;
        check("settings_save_restores_10pt_without_losing_markdown_styles",
              false);
      }
    } else {
      check(
          "settings_save_immediately_applies_16pt_to_existing_body_and_styles",
          false);
    }
    records = std::move(fontFixtureRecords);
    paused = fixturePaused;
    followingSource = followingTranslation = true;

    // Use the real modal open/close paths with hidden windows. Observe the
    // owner during WM_DESTROY: enabling it only at WM_NCDESTROY is too late
    // for Windows to choose it as the next active window.
    struct SettingsCloseObservation {
      HWND owner = nullptr;
      bool destroyed = false, ownerEnabled = false;
    };
    auto observeSettingsClose = [](HWND window, UINT message, WPARAM wp,
                                   LPARAM lp, UINT_PTR,
                                   DWORD_PTR payload) -> LRESULT {
      auto &observation =
          *reinterpret_cast<SettingsCloseObservation *>(payload);
      if (message == WM_DESTROY) {
        observation.destroyed = true;
        observation.ownerEnabled = IsWindowEnabled(observation.owner);
      }
      return DefSubclassProc(window, message, wp, lp);
    };
    const bool ownerInitiallyVisible = IsWindowVisible(hwnd),
               ownerInitiallyIconic = IsIconic(hwnd);
    const char *settingsCloseChecks[] = {
        "settings_ok_reenables_owner_before_destroy",
        "settings_cancel_reenables_owner_before_destroy",
        "settings_close_reenables_owner_before_destroy",
        "settings_escape_reenables_owner_before_destroy"};
    for (int route = 0; route < 4; ++route) {
      const HWND foregroundBefore = GetForegroundWindow();
      openConfig();
      if (!config) {
        check(settingsCloseChecks[route], false);
        continue;
      }
      const HWND dialog = config->hwnd;
      const bool hiddenModal = !IsWindowVisible(dialog) && !IsWindowEnabled(hwnd);
      SettingsCloseObservation observation{hwnd};
      SetWindowSubclass(dialog, observeSettingsClose, 1,
                        reinterpret_cast<DWORD_PTR>(&observation));
      if (route < 2)
        SendMessageW(dialog, WM_COMMAND,
                     MAKEWPARAM(route == 0 ? IDOK : IDCANCEL, BN_CLICKED), 0);
      else if (route == 2)
        SendMessageW(dialog, WM_SYSCOMMAND, SC_CLOSE, 0);
      else {
        MSG escape{};
        escape.hwnd = config->edits[1];
        escape.message = WM_KEYDOWN;
        escape.wParam = VK_ESCAPE;
        IsDialogMessageW(dialog, &escape);
      }
      check(settingsCloseChecks[route],
            hiddenModal && observation.destroyed && observation.ownerEnabled &&
                !config && !IsWindow(dialog) && IsWindowEnabled(hwnd) &&
                IsWindowVisible(hwnd) == ownerInitiallyVisible &&
                IsIconic(hwnd) == ownerInitiallyIconic &&
                GetForegroundWindow() == foregroundBefore);
      if (config)
        closeConfig(false);
    }
    openConfig();
    if (config) {
      const HWND dialog = config->hwnd;
      SetWindowTextW(config->edits[6], L"invalid");
      SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check("settings_invalid_save_keeps_dialog_and_owner_modal",
            config && config->hwnd == dialog && IsWindow(dialog) &&
                !IsWindowEnabled(hwnd) && !textOf(config->error).empty());
      closeConfig(false);
    } else
      check("settings_invalid_save_keeps_dialog_and_owner_modal", false);
    check("settings_self_test_keeps_main_hidden_and_offline",
          !IsWindowVisible(hwnd) && !watcher && settings.provider == "demo");

    // Real settings handlers with synthetic credentials only. The app remains
    // a hidden demo, so applying a provider never starts a reader or network.
    const auto providerFixtureSettings = settings;
    const bool providerFixtureRunning = running;
    running = false;
    struct TestEnvironment {
      std::wstring name;
      std::optional<std::wstring> previous;
      explicit TestEnvironment(std::wstring n) : name(std::move(n)) {
        const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (size) {
          std::wstring value(size, L'\0');
          GetEnvironmentVariableW(name.c_str(), value.data(), size);
          if (!value.empty() && value.back() == L'\0')
            value.pop_back();
          previous = std::move(value);
        }
      }
      std::wstring value() const {
        const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (!size)
          return {};
        std::wstring value(size, L'\0');
        GetEnvironmentVariableW(name.c_str(), value.data(), size);
        if (!value.empty() && value.back() == L'\0')
          value.pop_back();
        return value;
      }
      ~TestEnvironment() {
        SetEnvironmentVariableW(name.c_str(),
                                previous ? previous->c_str() : nullptr);
      }
    };
    TestEnvironment apiEnvironment(L"CCLIVE_GUI_API_TEST_KEY_" +
                                   std::to_wstring(GetCurrentProcessId()));
    TestEnvironment baiduEnvironment(L"CCLIVE_GUI_BAIDU_TEST_KEY_" +
                                     std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(apiEnvironment.name.c_str(),
                            L"CCLIVE_EXISTING_API_TEST_VALUE");
    openConfig();
    if (config) {
      auto select = [&](const char *provider) {
        SendMessageW(config->edits[0], CB_SETCURSEL,
                     configProviderIndex(provider), 0);
        SendMessageW(config->hwnd, WM_COMMAND,
                     MAKEWPARAM(200, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(config->edits[0]));
      };
      auto shown = [](HWND control) {
        return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
      };
      check("provider_choices_display_friendly_names",
            SendMessageW(config->edits[0], CB_GETCOUNT, 0, 0) ==
                static_cast<LRESULT>(std::size(configProviders)) &&
                textOf(config->edits[0]) == L"Google公共翻译");
      check("google_disables_unneeded_credentials",
            !IsWindowEnabled(config->edits[1]) &&
                !IsWindowEnabled(config->edits[3]) &&
                !IsWindowEnabled(config->edits[7]) &&
                !shown(config->edits[8]));
      select("openai");
      SetWindowTextW(config->edits[1], L"https://api.example.invalid/v1");
      SetWindowTextW(config->edits[2], L"fixture-model");
      SetWindowTextW(config->edits[3], apiEnvironment.name.c_str());
      SetWindowTextW(config->edits[7], L"CCLIVE_HIDDEN_API_FAKE_KEY");
      check("compatible_api_enables_its_own_fields",
            IsWindowEnabled(config->edits[1]) &&
                IsWindowEnabled(config->edits[2]) &&
                IsWindowEnabled(config->edits[3]) &&
                IsWindowEnabled(config->edits[7]));
      select("ollama");
      check("ollama_enables_address_and_model_without_api_credentials",
            IsWindowEnabled(config->edits[1]) &&
                IsWindowEnabled(config->edits[2]) &&
                !IsWindowEnabled(config->edits[3]) &&
                !IsWindowEnabled(config->edits[7]));
      select("baidu");
      check("baidu_uses_independent_fields_and_preserves_api_values",
            selectedConfigProvider(*config) == std::string("baidu") &&
                shown(config->edits[8]) && shown(config->edits[9]) &&
                shown(config->edits[10]) &&
                IsWindowEnabled(config->edits[8]) &&
                IsWindowEnabled(config->edits[9]) &&
                IsWindowEnabled(config->edits[10]) &&
                !shown(config->edits[1]) &&
                !IsWindowEnabled(config->edits[7]) &&
                textOf(config->edits[1]) == L"https://api.example.invalid/v1" &&
                textOf(config->edits[2]) == L"fixture-model" &&
                textOf(config->edits[7]) == L"CCLIVE_HIDDEN_API_FAKE_KEY" &&
                textOf(config->edits[10]).empty() &&
                textOf(config->serviceHint).find(L"百度") != std::wstring::npos);
      SetWindowTextW(config->edits[8], L"2026102300000001");
      SetWindowTextW(config->edits[9], baiduEnvironment.name.c_str());
      SetWindowTextW(config->edits[10], L"CCLIVE_BAIDU_FAKE_KEY");
      const HWND dialog = config->hwnd;
      SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check("baidu_appid_and_environment_name_apply_through_save_handler",
            !config && settings.baidu_app_id == "2026102300000001" &&
                settings.baidu_key_env == utf8(baiduEnvironment.name) &&
                settings.base_url == "https://api.example.invalid/v1" &&
                settings.model == "fixture-model" &&
                settings.provider == "demo" && !watcher);
      check("baidu_key_does_not_overwrite_hidden_api_key_environment",
            baiduEnvironment.value() == L"CCLIVE_BAIDU_FAKE_KEY" &&
                apiEnvironment.value() == L"CCLIVE_EXISTING_API_TEST_VALUE");
      if (config)
        closeConfig(false);
      auto fixturePath = reportPath;
      fixturePath += L".baidu-settings-test.json";
      bool persistedSafely = false;
      try {
        cclive::save_settings(fixturePath, settings);
        const auto restored = cclive::load_settings(fixturePath);
        std::ifstream input(fixturePath, std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(input)), {});
        persistedSafely = restored.baidu_app_id == "2026102300000001" &&
            restored.baidu_key_env == utf8(baiduEnvironment.name) &&
            saved.find("CCLIVE_BAIDU_FAKE_KEY") == std::string::npos &&
            saved.find("CCLIVE_HIDDEN_API_FAKE_KEY") == std::string::npos &&
            saved.find("CCLIVE_EXISTING_API_TEST_VALUE") == std::string::npos;
      } catch (...) {
      }
      std::error_code ignored;
      fs::remove(fixturePath, ignored);
      check("baidu_settings_persist_appid_and_env_name_without_secret_values",
            persistedSafely);
      const auto diagnostic = diagnosticMetadata().dump();
      const auto cached = taskPayload().dump();
      check("baidu_secrets_never_enter_diagnostics_or_task_cache",
            diagnostic.find("CCLIVE_BAIDU_FAKE_KEY") == std::string::npos &&
                diagnostic.find("2026102300000001") == std::string::npos &&
                cached.find("CCLIVE_BAIDU_FAKE_KEY") == std::string::npos &&
                cached.find(utf8(baiduEnvironment.name)) == std::string::npos);
      openConfig();
      if (config) {
        select("baidu");
        SetWindowTextW(config->edits[9], apiEnvironment.name.c_str());
        SendMessageW(config->hwnd, WM_COMMAND,
                     MAKEWPARAM(IDOK, BN_CLICKED), 0);
        check("baidu_rejects_shared_api_key_environment_name",
              config && !textOf(config->error).empty());
        closeConfig(false);
      } else
        check("baidu_rejects_shared_api_key_environment_name", false);
    } else
      check("baidu_settings_fixture_created", false);
    const auto isolatedEnvironment = readerEnvironment({"pAtH", "tEmP"});
    bool excludedBoth = true, validEnvironment = isolatedEnvironment.size() > 1 &&
        isolatedEnvironment[isolatedEnvironment.size() - 1] == L'\0' &&
        isolatedEnvironment[isolatedEnvironment.size() - 2] == L'\0';
    for (const wchar_t *entry = isolatedEnvironment.data(); *entry;
         entry += wcslen(entry) + 1) {
      const std::wstring value(entry);
      const auto name = value.substr(0, value.find(L'='));
      excludedBoth = excludedBoth && name != L"PATH" && name != L"TEMP";
    }
    check("reader_environment_excludes_both_configured_secret_names",
          validEnvironment && excludedBoth);
    settings = providerFixtureSettings;
    running = providerFixtureRunning;
    provider();
    check("provider_settings_self_test_remains_hidden_and_offline",
          settings.provider == "demo" && !watcher && !IsWindowVisible(hwnd));

    // ChatGPT UI uses only constructed account/catalog events here. No auth
    // file, browser, OAuth flow or model service is accessed by this fixture.
    const auto chatgptFixtureSettings = settings;
    const auto chatgptFixtureHash = chatgptAccountHash;
    const auto unrelatedProviderProfile = translationProfile();
    auto chatgptProfileForTest = [&] {
      const auto provider = settings.provider;
      settings.provider = "chatgpt";
      const auto profile = translationProfile();
      settings.provider = provider;
      return profile;
    };
    const bool chatgptFixtureRunning = running;
    running = false;
    openConfig();
    if (config) {
      auto selectProvider = [&](const char *provider) {
        SendMessageW(config->edits[0], CB_SETCURSEL,
                     configProviderIndex(provider), 0);
        SendMessageW(config->hwnd, WM_COMMAND,
                     MAKEWPARAM(200, CBN_SELCHANGE), 0);
      };
      selectProvider("chatgpt");
      const auto ticket = config->authTicket;
      auto safeEvent = [&](bool signedIn, bool enabled, json models) {
        json account = {{"signed_in", signedIn}, {"plan_enabled", enabled},
                        {"email", "fixture@example.invalid"},
                        {"name", "Offline account fixture"},
                        {"client_id", "offline-client-id"}};
        return json{{"type", "chatgpt_settings"}, {"ticket", ticket},
                    {"account", account}, {"accounts", json::array({account})},
                    {"models", std::move(models)}, {"error", ""}};
      };
      accept(safeEvent(false, false, json::array()));
      SendMessageW(config->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check("chatgpt_settings_requires_account_authorization",
            config && !config->account.signed_in &&
                !IsWindowEnabled(config->chatgptModels) &&
                !textOf(config->error).empty());
      accept(safeEvent(true, false,
                       json::array({{{"id", "fixture-model-a"},
                                      {"name", "Offline Model A"}}})));
      SendMessageW(config->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check("chatgpt_settings_rejects_account_without_plan_permission",
            config && !config->account.plan_enabled &&
                !IsWindowEnabled(config->chatgptModels) &&
                !textOf(config->error).empty());
      accept(safeEvent(true, true, json::array()));
      SendMessageW(config->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check("chatgpt_settings_rejects_empty_server_model_catalog",
            config && config->catalog.empty() &&
                !IsWindowEnabled(config->chatgptModels) &&
                !textOf(config->error).empty());
      auto obsolete = safeEvent(true, true,
          json::array({{{"id", "obsolete-model"}, {"name", "Obsolete"}}}));
      obsolete["ticket"] = ticket - 1;
      accept(obsolete);
      check("chatgpt_settings_discards_obsolete_result_ticket",
            config && config->catalog.empty());
      accept(safeEvent(true, true,
          json::array({{{"id", "fixture-model-a"}, {"name", "Offline Model A"}},
                       {{"id", "fixture-model-b"}, {"name", "Offline Model B"}}})));
      check("chatgpt_models_are_supplied_by_account_catalog",
            config->catalog.size() == 2 &&
                IsWindowEnabled(config->chatgptModels) &&
                config->catalog[0].id == "fixture-model-a" &&
                SendMessageW(config->chatgptModels, CB_GETCOUNT, 0, 0) == 2);
      const auto beforeModelProfile = chatgptProfileForTest();
      SendMessageW(config->chatgptModels, CB_SETCURSEL, 1, 0);
      const auto cancelOnClose = config->authCancel;
      SendMessageW(config->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check("chatgpt_selected_catalog_model_is_saved_and_changes_cache_profile",
            !config && settings.chatgpt_model == "fixture-model-b" &&
                chatgptProfileForTest() != beforeModelProfile &&
                settings.provider == "demo");
      check("viewing_chatgpt_account_does_not_change_other_provider_cache_profile",
            translationProfile() == unrelatedProviderProfile);
      check("chatgpt_close_cancels_outstanding_authorization",
            !config && cancelOnClose->load());
      if (config)
        closeConfig(false);
      openConfig();
      if (config) {
        selectProvider("chatgpt");
        const auto newTicket = config->authTicket;
        accept(safeEvent(true, true,
            json::array({{{"id", "obsolete-model"}, {"name", "Obsolete"}}})));
        check("chatgpt_reopened_settings_ignores_previous_dialog_results",
              newTicket != ticket && config->authBusy &&
                  !config->authLoaded && config->catalog.empty());
        const auto cancelOnSwitch = config->authCancel;
        selectProvider("google");
        check("chatgpt_switching_provider_cancels_account_loading",
              cancelOnSwitch->load() && !config->authLoaded);
        selectProvider("chatgpt");
        settings.provider = "chatgpt";
        const auto beforeAccountChange = generation;
        chatgptSettingsAction("switch", "offline-other-client");
        check("chatgpt_account_change_cancels_previous_translation_generation",
              generation > beforeAccountChange && !running);
        closeConfig(false);
      } else
        check("chatgpt_reopened_settings_ignores_previous_dialog_results", false);
    } else
      check("chatgpt_settings_fixture_created", false);
    if (config)
      closeConfig(false);
    settings = chatgptFixtureSettings;
    chatgptAccountHash = chatgptFixtureHash;
    running = chatgptFixtureRunning;
    provider();
    check("chatgpt_ui_selftest_never_reads_accounts_or_uses_network",
          settings.provider == "demo" && !watcher && !IsWindowVisible(hwnd));

    // Codex settings use synthetic paths and models only. Save handlers must
    // never resolve an executable, inspect CLI login state or start a process.
    const auto codexFixtureSettings = settings;
    const bool codexFixtureRunning = running;
    const auto codexApiEnvironment = apiEnvironment.value();
    const auto codexBaiduEnvironment = baiduEnvironment.value();
    running = false;
    settings.codex_path = "C:\\Offline Fixture\\codex.exe";
    settings.codex_model = "offline-codex-model";
    openConfig();
    if (config) {
      auto selectCodexProvider = [&](const char *provider) {
        SendMessageW(config->edits[0], CB_SETCURSEL,
                     configProviderIndex(provider), 0);
        SendMessageW(config->hwnd, WM_COMMAND,
                     MAKEWPARAM(200, CBN_SELCHANGE), 0);
      };
      auto codexShown = [](HWND control) {
        return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
      };
      selectCodexProvider("codex");
      check("codex_provider_follows_chatgpt_with_native_login_label",
            configProviderIndex("codex") == configProviderIndex("chatgpt") + 1 &&
                textOf(config->edits[0]) == L"Codex CLI（本机登录）");
      check("codex_displays_only_its_independent_provider_fields",
            config->edits.size() == 14 && config->labels.size() == 14 &&
                codexShown(config->edits[12]) && codexShown(config->edits[13]) &&
                IsWindowEnabled(config->edits[12]) &&
                IsWindowEnabled(config->edits[13]) &&
                !(GetWindowLongPtrW(config->edits[12], GWL_STYLE) & ES_PASSWORD) &&
                !codexShown(config->edits[1]) && !codexShown(config->edits[2]) &&
                !codexShown(config->edits[3]) && !codexShown(config->edits[7]) &&
                !codexShown(config->edits[8]) && !codexShown(config->edits[9]) &&
                !codexShown(config->edits[10]) && !codexShown(config->edits[11]) &&
                !codexShown(config->chatgptLogin) &&
                textOf(config->serviceHint).find(L"codex login") != std::wstring::npos);
      SetWindowTextW(config->edits[12], L"C:\\Synthetic CLI\\codex.exe");
      SetWindowTextW(config->edits[13], L"fixture-codex-model");
      selectCodexProvider("openai");
      const auto preservedApi = textOf(config->edits[1]);
      const auto preservedApiModel = textOf(config->edits[2]);
      const auto preservedBaidu = textOf(config->edits[8]);
      selectCodexProvider("baidu");
      check("codex_fields_remain_separate_while_other_providers_are_visible",
            !codexShown(config->edits[12]) && !codexShown(config->edits[13]) &&
                codexShown(config->edits[8]) &&
                textOf(config->edits[12]) == L"C:\\Synthetic CLI\\codex.exe" &&
                textOf(config->edits[13]) == L"fixture-codex-model");
      selectCodexProvider("chatgpt");
      const auto cancelAccountLoading = config->authCancel;
      selectCodexProvider("codex");
      check("codex_selection_cancels_unneeded_oauth_loading",
            cancelAccountLoading->load() && !config->authBusy &&
                !config->authLoaded && textOf(config->edits[1]) == preservedApi &&
                textOf(config->edits[2]) == preservedApiModel &&
                textOf(config->edits[8]) == preservedBaidu);
      SendMessageW(config->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      check("codex_save_preserves_path_model_without_oauth_or_cli_probe",
            !config && settings.codex_path == "C:\\Synthetic CLI\\codex.exe" &&
                settings.codex_model == "fixture-codex-model" &&
                settings.base_url == utf8(preservedApi) &&
                settings.model == utf8(preservedApiModel) &&
                settings.baidu_app_id == utf8(preservedBaidu) &&
                settings.provider == "demo" && !watcher);
      check("codex_save_never_updates_other_provider_secret_environments",
            apiEnvironment.value() == codexApiEnvironment &&
                baiduEnvironment.value() == codexBaiduEnvironment);
      if (config)
        closeConfig(false);
      auto codexProfile = [&] {
        const auto current = settings.provider;
        settings.provider = "codex";
        const auto profile = translationProfile();
        settings.provider = current;
        return profile;
      };
      const auto originalCodexProfile = codexProfile();
      settings.codex_model = "another-fixture-model";
      const bool modelChangesProfile = codexProfile() != originalCodexProfile;
      settings.codex_model = "fixture-codex-model";
      settings.codex_path = "C:\\Different CLI\\codex.exe";
      check("codex_cache_identity_tracks_its_model_and_executable_path",
            modelChangesProfile && codexProfile() != originalCodexProfile &&
                originalCodexProfile.dump().find("Synthetic CLI") == std::string::npos);
      const auto unrelatedProfile = translationProfile();
      settings.codex_path = "C:\\Synthetic CLI\\codex.exe";
      settings.codex_model = "another-fixture-model";
      check("codex_settings_do_not_change_other_provider_cache_identity",
            translationProfile() == unrelatedProfile);
      const auto codexRetryRecords = records;
      const auto codexRetryProfile = activeProfile;
      Record retryFixture;
      retryFixture.key = "codex-retry-profile-fixture";
      retryFixture.source = "Offline CLI profile fixture.";
      retryFixture.state = "stopped";
      records = {retryFixture};
      activeProfile = originalCodexProfile;
      const auto codexRetryProvider = settings.provider;
      settings.provider = "codex";
      const auto codexSubmissions = worker->submitted.load();
      retry();
      check("codex_retry_rejects_old_profile_without_launching_cli",
            worker->submitted.load() == codexSubmissions &&
                records.front().state == "stopped" &&
                activeProfile == originalCodexProfile);
      settings.provider = codexRetryProvider;
      records = codexRetryRecords;
      activeProfile = codexRetryProfile;
      settings.codex_model = "fixture-codex-model";
      auto codexFixturePath = reportPath;
      codexFixturePath += L".codex-settings-test.json";
      bool codexRoundtrip = false;
      try {
        cclive::save_settings(codexFixturePath, settings);
        const auto restored = cclive::load_settings(codexFixturePath);
        codexRoundtrip = restored.codex_path == settings.codex_path &&
                         restored.codex_model == settings.codex_model;
      } catch (...) {
      }
      std::error_code codexIgnored;
      fs::remove(codexFixturePath, codexIgnored);
      check("codex_settings_roundtrip_keeps_independent_path_and_model", codexRoundtrip);
      openConfig();
      if (config) {
        selectCodexProvider("codex");
        check("codex_reopening_settings_restores_its_saved_fields",
              textOf(config->edits[12]) == L"C:\\Synthetic CLI\\codex.exe" &&
                  textOf(config->edits[13]) == L"fixture-codex-model");
        SetWindowTextW(config->edits[12], L"");
        SetWindowTextW(config->edits[13], L"");
        SendMessageW(config->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        check("codex_empty_path_and_model_save_as_automatic_defaults",
              !config && settings.codex_path.empty() && settings.codex_model.empty());
        if (config)
          closeConfig(false);
      } else
        check("codex_reopening_settings_restores_its_saved_fields", false);
    } else
      check("codex_settings_fixture_created", false);
    if (config)
      closeConfig(false);
    settings = codexFixtureSettings;
    running = codexFixtureRunning;
    provider();
    check("codex_ui_selftest_remains_hidden_without_reader_or_network",
          settings.provider == "demo" && !watcher && !IsWindowVisible(hwnd));
    // A committed account change outlives the settings dialog and its model
    // lookup. Inject only this app's safe local-status reader, never OAuth/files.
    const auto identityFixtureSettings = settings;
    const auto identityFixtureHash = chatgptAccountHash;
    const auto identityFixtureReader = chatgptIdentityReader;
    const auto identityFixtureRecords = records;
    const auto identityFixtureProfile = activeProfile;
    const auto identityFixtureTask = activeTaskId;
    const bool identityFixtureRunning = running, identityFixturePaused = paused;
    running = paused = false;
    settings.provider = "chatgpt";
    settings.chatgpt_model = "offline-model-a";
    chatgptAccountHash = contentSignature("offline-account-a");
    activeTaskId = "offline-identity-regression";
    activeProfile = translationProfile();
    const auto profileA = activeProfile;
    Record identityRecord;
    identityRecord.key = "offline-identity-record";
    identityRecord.source = "Synthetic account-bound source.";
    identityRecord.translation = "Synthetic account A translation.";
    identityRecord.state = "done";
    records = {identityRecord};
    auto identityEvent = [](const char *clientId, bool signedIn = true,
                            bool readable = true, bool enabled = true) {
      return json{{"type", "chatgpt_identity"},
                  {"revision", ++chatgptIdentitySequence},
                  {"readable", readable}, {"signed_in", signedIn},
                  {"plan_enabled", enabled}, {"client_id", clientId}};
    };
    openConfig();
    if (config) {
      const auto closedCancel = config->authCancel;
      closeConfig(false);
      accept(identityEvent("offline-account-b"));
      auto cachedA = taskCache->get(activeTaskId);
      check("committed_chatgpt_identity_updates_after_settings_was_closed",
            !config && closedCancel->load() &&
                chatgptAccountHash == contentSignature("offline-account-b") &&
                settings.chatgpt_model.empty());
      check("chatgpt_identity_change_keeps_old_translations_under_old_profile",
            activeProfile == profileA && cachedA &&
                cachedA->value("translation_profile", json::object()) == profileA &&
                records[0].translation == "Synthetic account A translation.");
    } else
      check("committed_chatgpt_identity_updates_after_settings_was_closed", false);
    std::atomic_uint localStatusReads{0};
    chatgptIdentityReader = [&] {
      ++localStatusReads;
      cclive::chatgpt::AccountStatus account;
      account.signed_in = account.plan_enabled = true;
      account.client_id = "offline-account-b";
      return account;
    };
    chatgptAccountHash = contentSignature("offline-account-a");
    settings.chatgpt_model = "offline-model-a";
    const auto submittedBeforeIdentityCheck = worker->submitted.load();
    start();
    check("chatgpt_start_rechecks_local_identity_and_blocks_previous_model",
          !running && localStatusReads.load() == 1 &&
              chatgptAccountHash == contentSignature("offline-account-b") &&
              settings.chatgpt_model.empty() &&
              worker->submitted.load() == submittedBeforeIdentityCheck);
    chatgptAccountHash = contentSignature("offline-account-a");
    settings.chatgpt_model = "offline-model-a";
    records[0].state = "error";
    retry();
    check("chatgpt_retry_rechecks_local_identity_and_blocks_previous_model",
          localStatusReads.load() == 2 && settings.chatgpt_model.empty() &&
              worker->submitted.load() == submittedBeforeIdentityCheck);
    settings.chatgpt_model = "offline-model-b";
    auto staleIdentity = identityEvent("offline-account-a");
    staleIdentity["revision"] = chatgptIdentityRevision - 1;
    accept(staleIdentity);
    check("chatgpt_old_identity_event_cannot_undo_local_status_confirmation",
          chatgptAccountHash == contentSignature("offline-account-b") &&
              settings.chatgpt_model == "offline-model-b");
    retry();
    check("chatgpt_retry_cannot_write_new_account_results_under_old_profile",
          chatgptAccountHash == contentSignature("offline-account-b") &&
              settings.chatgpt_model == "offline-model-b" &&
              activeProfile == profileA &&
              worker->submitted.load() == submittedBeforeIdentityCheck);
    chatgptIdentityReader = []() -> cclive::chatgpt::AccountStatus {
      throw std::runtime_error("offline unreadable account fixture");
    };
    retry();
    check("chatgpt_unreadable_authorization_clears_old_hash_and_model",
          chatgptAccountHash.empty() && settings.chatgpt_model.empty() &&
              worker->submitted.load() == submittedBeforeIdentityCheck);
    chatgptAccountHash = contentSignature("offline-account-b");
    settings.chatgpt_model = "offline-model-b";
    accept(identityEvent("", false));
    check("chatgpt_signout_notification_clears_old_hash_and_model",
          chatgptAccountHash.empty() && settings.chatgpt_model.empty());
    settings.provider = "google";
    running = true;
    records[0].state = "queued";
    const auto googleGeneration = generation;
    const auto googleProfile = activeProfile;
    accept(identityEvent("offline-account-b"));
    accept(identityEvent("", false, false));
    check("chatgpt_identity_notifications_do_not_interrupt_other_services",
          running && generation == googleGeneration &&
              records[0].state == "queued" && activeProfile == googleProfile &&
              worker->submitted.load() == submittedBeforeIdentityCheck);
    taskCache->erase(activeTaskId);
    settings = identityFixtureSettings;
    chatgptAccountHash = identityFixtureHash;
    chatgptIdentityReader = identityFixtureReader;
    chatgptIdentityRevision = ++chatgptIdentitySequence;
    records = identityFixtureRecords;
    activeProfile = identityFixtureProfile;
    activeTaskId = identityFixtureTask;
    running = identityFixtureRunning;
    paused = identityFixturePaused;
    provider();
    check("chatgpt_identity_regressions_remain_hidden_and_offline",
          settings.provider == "demo" && !watcher && !IsWindowVisible(hwnd));

    // Exercise long incremental output in the actual RichEdit control.
    // Synthetic text only: follow the end until a deliberate user scroll moves
    // away.
    auto lines = [](int count) {
      std::wstring value;
      for (int i = 0; i < count; ++i)
        value += L"Synthetic translation row " + std::to_wstring(i) +
                 L" with enough content to test scrolling.\r\n";
      return value;
    };
    auto atBottom = [&](HWND edit) {
      SCROLLINFO info{};
      info.cbSize = sizeof(info);
      info.fMask = SIF_ALL;
      return GetScrollInfo(edit, SB_VERT, &info) &&
             info.nPos + static_cast<int>(info.nPage) >= info.nMax - 3;
    };
    editText(translationEdit, lines(180));
    SendMessageW(translationEdit, WM_VSCROLL, SB_BOTTOM, 0);
    check("long_output_initially_scrolls_to_end", atBottom(translationEdit));
    CHARRANGE selection{8, 28};
    SendMessageW(translationEdit, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&selection));
    goLatest();
    editText(translationEdit, lines(240));
    check("growing_output_continues_following_end", atBottom(translationEdit));
    CHARRANGE afterSelection{};
    SendMessageW(translationEdit, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&afterSelection));
    check("following_output_preserves_selection",
          afterSelection.cpMin == selection.cpMin &&
              afterSelection.cpMax == selection.cpMax);
    SendMessageW(translationEdit, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(translationEdit, EM_LINESCROLL, 0, 5);
    POINT manual{};
    SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&manual));
    editText(translationEdit, lines(300));
    POINT afterManual{};
    SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&afterManual));
    check("manual_scroll_position_survives_new_output",
          !atBottom(translationEdit) && abs(afterManual.y - manual.y) <= 2);
    SendMessageW(translationEdit, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&afterSelection));
    check("manual_reading_preserves_selection",
          afterSelection.cpMin == selection.cpMin &&
              afterSelection.cpMax == selection.cpMax);
    check("manual_scroll_disables_explicit_follow", !followingTranslation);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_LATEST, BN_CLICKED),
                 reinterpret_cast<LPARAM>(latestButton));
    check("latest_button_immediately_scrolls_without_new_text",
          latestButton && followingTranslation && atBottom(translationEdit));
    editText(translationEdit, lines(360));
    check("latest_button_restores_continuous_follow",
          followingTranslation && atBottom(translationEdit));
    // Fill a synthetic bitmap with stale pixels. Native RichEdit claims to
    // erase its background but leaves these pixels untouched without bodyProc.
    for (HWND edit : {sourceEdit, translationEdit}) {
      RECT savedBounds{}, client{};
      GetWindowRect(edit, &savedBounds);
      GetClientRect(edit, &client);
      const bool needsSize = client.right <= 0 || client.bottom <= 0;
      if (needsSize) {
        SetWindowPos(edit, nullptr, 0, 0, 640, 320,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetClientRect(edit, &client);
      }
      BITMAPINFO bitmap{};
      bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bitmap.bmiHeader.biWidth = client.right;
      bitmap.bmiHeader.biHeight = -client.bottom;
      bitmap.bmiHeader.biPlanes = 1;
      bitmap.bmiHeader.biBitCount = 32;
      bitmap.bmiHeader.biCompression = BI_RGB;
      void *pixels = nullptr;
      HDC dc = CreateCompatibleDC(nullptr);
      HBITMAP surface = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels,
                                       nullptr, 0);
      HGDIOBJ previous = surface ? SelectObject(dc, surface) : nullptr;
      bool blank = dc && surface && pixels;
      if (blank) {
        auto *colors = static_cast<DWORD *>(pixels);
        const auto size = static_cast<std::size_t>(client.right) * client.bottom;
        std::fill_n(colors, size, 0x00ff00ff);
        SendMessageW(edit, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0);
        GdiFlush();
        const DWORD expected = RGB(GetBValue(PANEL), GetGValue(PANEL), GetRValue(PANEL));
        blank = std::all_of(colors, colors + size, [&](DWORD color) {
          return (color & 0x00ffffff) == expected;
        });
      }
      check(edit == sourceEdit ? "source_blank_area_erases_stale_pixels"
                               : "translation_blank_area_erases_stale_pixels", blank);
      if (previous) SelectObject(dc, previous);
      if (surface) DeleteObject(surface);
      if (dc) DeleteDC(dc);
      if (needsSize)
        SetWindowPos(edit, nullptr, 0, 0, savedBounds.right - savedBounds.left,
                     savedBounds.bottom - savedBounds.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // An offscreen, non-activating synthetic window lets Windows deliver real
    // WM_PAINT synchronously while the main self-test window stays hidden.
    HWND paintFixture = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, MSFTEDIT_CLASS, L"",
        WS_POPUP | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        -32000, -32000, 640, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check("offscreen_repaint_fixture_created", paintFixture != nullptr);
    if (paintFixture) {
      struct PaintCounts { unsigned paints = 0, erases = 0; } paintCounts;
      SetWindowSubclass(paintFixture, bodyProc, 1, reinterpret_cast<DWORD_PTR>(this));
      SetWindowSubclass(paintFixture,
          [](HWND edit, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT {
            auto &counts = *reinterpret_cast<PaintCounts *>(data);
            if (message == WM_PAINT) ++counts.paints;
            if (message == WM_ERASEBKGND) ++counts.erases;
            return DefSubclassProc(edit, message, wp, lp);
          }, 2, reinterpret_cast<DWORD_PTR>(&paintCounts));
      const auto savedSource = sourceEdit, savedTranslation = translationEdit;
      const bool savedSourceFollow = followingSource, savedTranslationFollow = followingTranslation;
      sourceEdit = nullptr;
      translationEdit = paintFixture;
      followingTranslation = true;
      auto paintedNow = [&](PaintCounts before) {
        return paintCounts.paints > before.paints &&
               !GetUpdateRect(paintFixture, nullptr, FALSE);
      };
      auto before = paintCounts;
      editText(paintFixture, lines(180));
      check("output_update_finishes_erase_and_paint_before_return", paintedNow(before));
      before = paintCounts;
      SendMessageW(paintFixture, WM_VSCROLL, SB_TOP, 0);
      check("manual_scroll_repaints_without_new_output", paintedNow(before) && !followingTranslation);
      before = paintCounts;
      goLatest();
      check("latest_repaints_without_new_output", paintedNow(before) && followingTranslation);
      before = paintCounts;
      editText(paintFixture, L"Only one remaining line.");
      check("shorter_output_clears_old_rows_immediately", paintedNow(before));
      before = paintCounts;
      editText(paintFixture, L"Only one remaining line.");
      check("unchanged_output_does_not_repaint_in_idle_loop",
            paintCounts.paints == before.paints && paintCounts.erases == before.erases);
      sourceEdit = savedSource;
      translationEdit = savedTranslation;
      followingSource = savedSourceFollow;
      followingTranslation = savedTranslationFollow;
      renderedBodies.erase(paintFixture);
      DestroyWindow(paintFixture);
    }
    refreshing = true;
    EnableWindow(refreshButton, FALSE);
    auto ticket = ++refreshGeneration;
    ++sourceGeneration;
    accept({{"type", "windows"},
            {"_refresh_ticket", ticket},
            {"windows", json::array()}});
    check("refresh_survives_source_generation_change",
          !refreshing && IsWindowEnabled(refreshButton));
    refreshing = true;
    EnableWindow(refreshButton, FALSE);
    ++refreshGeneration;
    accept({{"type", "windows"},
            {"_refresh_ticket", ticket},
            {"windows",
             json::array(
                 {{{"hwnd", 1}, {"pid", 1}, {"title", "stale fixture"}}})}});
    check("stale_refresh_ticket_ignored",
          refreshing && windows.empty() && !IsWindowEnabled(refreshButton));
    accept({{"type", "windows"},
            {"_refresh_ticket", refreshGeneration},
            {"windows", json::array()}});
    check("latest_refresh_ticket_recovers_controls",
          !refreshing && IsWindowEnabled(refreshButton));

    auto textEvent = [&](const std::string &key, const std::string &text,
                         int epoch, uint64_t source) {
      return json{{"type", "text"},        {"key", key},
                  {"text", text},          {"epoch", epoch},
                  {"_generation", source}, {"stable", true}};
    };
    clear();
    ledgerEpoch = 0;
    paused = false;
    running = true;
    pause();
    accept(textEvent("pause-test", "I will inspect the project files first.", 0,
                     sourceGeneration));
    bool pendingEmpty = false;
    {
      std::lock_guard<std::mutex> lock(worker->mutex);
      pendingEmpty = worker->pending.empty();
    }
    check("paused_source_collected_without_translation",
          paused && records.size() == 1 && records[0].state == "paused" &&
              records[0].translation.empty() && pendingEmpty);
    auto pauseGeneration = generation;
    pause();
    check("resume_requeues_collected_source",
          !paused && generation > pauseGeneration && records.size() == 1 &&
              records[0].state == "queued");
    pause();

    clear();
    ledgerEpoch = 0;
    accept(textEvent("epoch-old", "Old pending source.", 0, sourceGeneration));
    auto epochRecords = records.size();
    accept(textEvent("epoch-new", "New conversation source.", 2,
                     sourceGeneration));
    check("same_task_epoch_preserves_cached_records",
          ledgerEpoch == 2 && records.size() == epochRecords + 1);
    accept(textEvent("epoch-stale", "Old conversation source.", 1,
                     sourceGeneration));
    check("older_source_epoch_rejected",
          records.size() == 2 &&
              records.back().source == "New conversation source.");
    accept(
        {{"type", "reset"}, {"epoch", 2}, {"_generation", sourceGeneration}});
    check("late_same_epoch_reset_preserves_current_text",
          records.size() == 2 &&
              records.back().source == "New conversation source.");
    accept(textEvent("old-watcher", "Late watcher source.", 2,
                     sourceGeneration - 1));
    check("old_watcher_generation_rejected",
          records.size() == 2 &&
              records.back().source == "New conversation source.");
    accept(
        {{"type", "reset"}, {"epoch", 3}, {"_generation", sourceGeneration}});
    check("same_task_reset_keeps_cached_records",
          records.size() == 2 && ledgerEpoch == 3);
    clear();

    accept(textEvent("rewrite", "First source sentence.", 3, sourceGeneration));
    auto oldVersion = records[0].version;
    auto oldSerial = records[0].serial;
    accept(textEvent("rewrite", "Corrected source sentence.", 3,
                     sourceGeneration));
    check("same_key_rewrite_replaces_card_and_version",
          records.size() == 1 && records[0].serial == oldSerial &&
              records[0].version > oldVersion &&
              records[0].source == "Corrected source sentence.");
    auto currentVersion = records[0].version;
    accept(textEvent("rewrite", "Corrected source sentence.", 3,
                     sourceGeneration));
    check("identical_source_deduplicates",
          records.size() == 1 && records[0].version == currentVersion);
    auto result = [&](uint64_t gen, uint64_t version, const char *value) {
      return json{{"generation", gen},    {"key", records[0].key},
                  {"version", version},   {"state", "done"},
                  {"translation", value}, {"error", ""}};
    };
    acceptResult(result(generation, currentVersion, "current translation"));
    check("matching_async_result_applied",
          records[0].state == "done" &&
              records[0].translation == "current translation");
    acceptResult(result(generation, oldVersion, "stale version"));
    check("obsolete_result_version_rejected",
          records[0].translation == "current translation");
    acceptResult(result(generation - 1, currentVersion, "stale generation"));
    check("obsolete_result_generation_rejected",
          records[0].translation == "current translation");
    const auto completedVersion = records[0].version;
    accept(textEvent("rewrite", "A genuinely revised source sentence.", 3,
                     sourceGeneration));
    const auto replacementVersion = records[0].version;
    check("completed_rewrite_keeps_verified_translation_until_done",
          records[0].pendingReplacement &&
              records[0].source == "A genuinely revised source sentence." &&
              records[0].translation == "current translation" &&
              replacementVersion > completedVersion &&
              output(false) == L"current translation" &&
              progressCaption().find(L"更新中 1") != std::wstring::npos);
    acceptResult(result(generation, completedVersion, "stale completed source"));
    check("previous_source_result_cannot_commit_replacement",
          records[0].translation == "current translation" &&
              records[0].pendingReplacement);
    for (const auto &state : {"translating", "rate_limited", "error"}) {
      auto update = result(generation, replacementVersion, "partial replacement");
      update["state"] = state;
      update["error"] = state == std::string("error") ? "离线模拟失败" : "";
      acceptResult(update);
      check(state == std::string("translating")
                ? "replacement_partial_keeps_verified_translation"
            : state == std::string("rate_limited")
                ? "replacement_rate_limit_keeps_verified_translation"
                : "replacement_error_keeps_verified_translation",
            records[0].translation == "current translation" &&
                records[0].pendingReplacement);
    }
    acceptResult(result(generation, replacementVersion, ""));
    check("empty_done_cannot_commit_replacement",
          records[0].state == "error" && records[0].pendingReplacement &&
              records[0].translation == "current translation");
    acceptResult(result(generation, replacementVersion, "new complete translation"));
    check("current_nonempty_done_atomically_commits_replacement",
          records[0].state == "done" && !records[0].pendingReplacement &&
              records[0].translation == "new complete translation");
    auto markdown = cclive::make_text_layout(L"# Heading\n**bold**\n- item");
    check("markdown_markers_are_not_shown_as_source_syntax",
          markdown.text.find(L"# ") == std::wstring::npos &&
              markdown.text.find(L"**") == std::wstring::npos &&
              markdown.text.find(L"Heading") != std::wstring::npos &&
              markdown.text.find(L"bold") != std::wstring::npos);
    editText(translationEdit, L"# Heading\n**bold**\n- item");
    {
      ScrollGuard guard(internalScroll);
      CHARRANGE heading{0, 7};
      SendMessageW(translationEdit, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&heading));
      CHARFORMAT2W style{};
      style.cbSize = sizeof(style);
      SendMessageW(translationEdit, EM_GETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&style));
      check("markdown_heading_has_native_bold_and_size",
            (style.dwEffects & CFE_BOLD) &&
                style.yHeight > settings.font_size * 20);
      auto bullet =
          std::find_if(markdown.paragraphs.begin(), markdown.paragraphs.end(),
                       [](const cclive::TextParagraphStyle &paragraph) {
                         return paragraph.bullet;
                       });
      bool nativeBullet = false;
      if (bullet != markdown.paragraphs.end()) {
        CHARRANGE range{bullet->start, bullet->start + bullet->length};
        SendMessageW(translationEdit, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&range));
        PARAFORMAT2 format{};
        format.cbSize = sizeof(format);
        SendMessageW(translationEdit, EM_GETPARAFORMAT, 0,
                     reinterpret_cast<LPARAM>(&format));
        nativeBullet = format.wNumbering == PFN_BULLET;
      }
      check("markdown_list_uses_native_bullet_paragraph", nativeBullet);
    }
    editText(sourceEdit, L"# Heading\n**bold**\n- item");
    check("both_panes_render_identical_markdown_content",
          textOf(sourceEdit) == textOf(translationEdit) &&
              textOf(sourceEdit).find(L"**") == std::wstring::npos &&
              textOf(sourceEdit).find(L"# ") == std::wstring::npos);
    auto paneStyle = [&](HWND edit, LONG start, LONG length) {
      ScrollGuard guard(internalScroll);
      CHARRANGE saved{};
      SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
      CHARRANGE range{start, start + length};
      SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
      CHARFORMAT2W style{};
      style.cbSize = sizeof(style);
      SendMessageW(edit, EM_GETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&style));
      SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
      return style;
    };
    auto samePaneStyle = [&](LONG start, LONG length, int points, bool bold,
                             COLORREF color) {
      const auto source = paneStyle(sourceEdit, start, length);
      const auto translation = paneStyle(translationEdit, start, length);
      return source.yHeight == points * 20 &&
             source.yHeight == translation.yHeight &&
             !!(source.dwEffects & CFE_BOLD) == bold &&
             (source.dwEffects & CFE_BOLD) ==
                 (translation.dwEffects & CFE_BOLD) &&
             source.crTextColor == color &&
             source.crTextColor == translation.crTextColor &&
             source.crBackColor == cclive::text_layout_palette.background &&
             source.crBackColor == translation.crBackColor &&
             !(source.dwEffects & CFE_AUTOCOLOR) &&
             !(translation.dwEffects & CFE_AUTOCOLOR);
    };
    check("both_panes_share_native_heading_size_weight_and_color",
          samePaneStyle(0, 7, settings.font_size + 3, true,
                        cclive::text_layout_palette.heading));
    const auto boldAt = static_cast<LONG>(markdown.text.find(L"bold"));
    check("both_panes_share_native_bold_size_weight_and_color",
          samePaneStyle(boldAt, 4, settings.font_size, true,
                        cclive::text_layout_palette.body));
    const auto itemAt = static_cast<LONG>(markdown.text.find(L"item"));
    auto paneParagraph = [&](HWND edit) {
      ScrollGuard guard(internalScroll);
      CHARRANGE saved{};
      SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
      CHARRANGE range{itemAt, itemAt + 4};
      SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
      PARAFORMAT2 paragraph{};
      paragraph.cbSize = sizeof(paragraph);
      SendMessageW(edit, EM_GETPARAFORMAT, 0,
                   reinterpret_cast<LPARAM>(&paragraph));
      SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
      return paragraph;
    };
    const auto sourceList = paneParagraph(sourceEdit);
    const auto translatedList = paneParagraph(translationEdit);
    check("both_panes_share_native_list_bullet_indent_and_text_style",
          sourceList.wNumbering == PFN_BULLET &&
              sourceList.wNumbering == translatedList.wNumbering &&
              sourceList.dxStartIndent == translatedList.dxStartIndent &&
              sourceList.dxOffset == translatedList.dxOffset &&
              sourceList.wNumberingTab == translatedList.wNumberingTab &&
              samePaneStyle(itemAt, 4, settings.font_size, false,
                            cclive::text_layout_palette.body));

    // Reproduce inherited black source formatting, then refresh styled text.
    auto sourceThemeMatches = [&] {
      ScrollGuard guard(internalScroll);
      CHARRANGE selected{};
      SendMessageW(sourceEdit, EM_EXGETSEL, 0,
                   reinterpret_cast<LPARAM>(&selected));
      CHARFORMAT2W defaults{};
      defaults.cbSize = sizeof(defaults);
      SendMessageW(sourceEdit, EM_GETCHARFORMAT, SCF_DEFAULT,
                   reinterpret_cast<LPARAM>(&defaults));
      CHARRANGE sample{0, 1};
      SendMessageW(sourceEdit, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&sample));
      CHARFORMAT2W actual{};
      actual.cbSize = sizeof(actual);
      SendMessageW(sourceEdit, EM_GETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&actual));
      SendMessageW(sourceEdit, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&selected));
      const auto latest = renderedBodies[sourceEdit].latestDisplay;
      const bool marked = latest.cpMin == 0 && latest.cpMax > 0;
      return defaults.crTextColor == cclive::text_layout_palette.body &&
             actual.crTextColor == (marked ? cclive::text_layout_palette.latest : cclive::text_layout_palette.body) &&
             !(actual.dwEffects & CFE_AUTOCOLOR) &&
             actual.crBackColor == (marked ? cclive::text_layout_palette.latest_background : cclive::text_layout_palette.background);
    };
    CHARFORMAT2W black{};
    black.cbSize = sizeof(black);
    black.dwMask = CFM_COLOR;
    black.crTextColor = RGB(0, 0, 0);
    SendMessageW(sourceEdit, EM_SETCHARFORMAT, SCF_DEFAULT,
                 reinterpret_cast<LPARAM>(&black));
    SendMessageW(sourceEdit, EM_SETCHARFORMAT, SCF_ALL,
                 reinterpret_cast<LPARAM>(&black));
    editText(sourceEdit, L"Literal **source**\r\nSecond line");
    check("source_refresh_replaces_black_with_shared_body_color",
          sourceThemeMatches());
    const auto sourceBoldAt = static_cast<LONG>(textOf(sourceEdit).find(L"source"));
    const auto sourceBold = paneStyle(sourceEdit, sourceBoldAt, 6);
    check("source_color_formatting_renders_native_markdown_bold",
          textOf(sourceEdit).find(L"**") == std::wstring::npos &&
              textOf(sourceEdit).find(L"Literal source") != std::wstring::npos &&
              (sourceBold.dwEffects & CFE_BOLD) &&
              sourceBold.yHeight == settings.font_size * 20 &&
              sourceBold.crTextColor == cclive::text_layout_palette.body);
    editText(sourceEdit, L"Updated original text");
    check("source_color_survives_incremental_text_replacement",
          sourceThemeMatches());
    const auto previousFontSize = settings.font_size;
    const auto previousOriginal = settings.show_original;
    settings.font_size = 16;
    refreshAppearance();
    check("source_color_survives_font_setting_refresh", sourceThemeMatches());
    settings.font_size = previousFontSize;
    settings.show_original = false;
    layout();
    settings.show_original = true;
    layout();
    render();
    check("source_hide_show_uses_shared_body_color", sourceThemeMatches());
    settings.show_original = previousOriginal;
    refreshAppearance();

    clear();
    paused = true;
    running = true;
    for (int n = 0; n < 130; ++n) {
      Record record;
      record.serial = ++serial;
      record.key = "limit-" + std::to_string(n);
      record.source = "Source " + std::to_string(n);
      record.translation = "Caption " + std::to_string(n);
      record.state = "done";
      record.version = ++revision;
      records.push_back(std::move(record));
    }
    sourceAliases["removed-runtime"] = {records[0].key, "old-block", ""};
    trimRecords();
    check("subtitle_cache_keeps_latest_100_logical_lines",
          retainedLines() == 100 && records.size() == 100 &&
              records.front().translation == "Caption 30" &&
              records.back().translation == "Caption 129");
    check("trimmed_record_aliases_are_removed",
          !sourceAliases.count("removed-runtime"));
    std::wstring structuredProse;
    for (const auto &part : cclive::split_sentences(
             "## Progress\n- First list item\n- Second list item"))
      structuredProse += wide(part) + L"\r\n";
    const auto structuredLayout = cclive::make_text_layout(structuredProse);
    check("structured_source_survives_sentence_split_and_layout",
          std::count_if(structuredLayout.paragraphs.begin(),
                        structuredLayout.paragraphs.end(),
                        [](const auto &p) { return p.heading_level == 2; }) ==
                  1 &&
              std::count_if(structuredLayout.paragraphs.begin(),
                            structuredLayout.paragraphs.end(),
                            [](const auto &p) { return p.bullet; }) == 2);
    check("display_has_no_software_serial_prefix",
          output(false).rfind(L"Caption 30", 0) == 0 &&
              output(false).find(L"31. Caption") == std::wstring::npos);
    clear();
    auto layoutRecord = [&](const std::string &source, const std::string &translated,
                            const std::string &block, std::size_t paragraph,
                            bool known = true) {
      Record record;
      record.key = "paragraph-fixture-" + std::to_string(++serial);
      record.source = source;
      record.translation = translated;
      record.state = "done";
      record.hasLayout = known;
      record.layoutBlock = block;
      record.layoutParagraph = paragraph;
      record.version = ++revision;
      return record;
    };
    records = {layoutRecord("First sentence.", "第一句。", "block-A", 0),
               layoutRecord("Second sentence.", "第二句。", "block-A", 0),
               layoutRecord("New paragraph.", "新的段落。", "block-A", 1),
               layoutRecord("Other block.", "另一个块。", "block-B", 1),
               layoutRecord("Legacy source.", "历史独立段。", "", 0, false)};
    check("same_paragraph_CJK_sentences_join_without_artificial_spaces",
          output(false) == L"第一句。第二句。\r\n新的段落。\r\n另一个块。\r\n历史独立段。");
    check("same_paragraph_English_sentences_keep_one_space",
          output(true) == L"First sentence. Second sentence.\r\nNew paragraph.\r\nOther block.\r\nLegacy source.");
    const auto paragraphFixtureRecords = records;
    struct SeamCase { const char *left, *right; const wchar_t *expected; };
    const SeamCase seamCases[] = {
        {"构建已完成。", "Run smoke tests next.", L"构建已完成。 Run smoke tests next."},
        {"Ready.", "下一句。", L"Ready. 下一句。"},
        {"中文", "ASCII123", L"中文 ASCII123"},
        {"v2", "中文", L"v2 中文"},
        {"第一句。", "第二句。", L"第一句。第二句。"},
        {"**Ready.**", "下一句。", L"**Ready.** 下一句。"},
        {"构建完成。", "`Run` smoke tests.", L"构建完成。 `Run` smoke tests."}};
    bool allSeams = true, immutableSeams = true;
    const auto beforeSeamSubmit = worker->submitted.load();
    const auto beforeSeamGeneration = generation;
    for (const auto &test : seamCases) {
      records = {layoutRecord("Stable source A.", test.left, "seam-fixture", 0),
                 layoutRecord("Stable source B.", test.right, "seam-fixture", 0)};
      records[0].pendingReplacement = true;
      records[0].state = "translating";
      const auto beforeSeamRecords = taskPayload()["records"];
      const auto firstVersion = records[0].version, secondVersion = records[1].version;
      allSeams = allSeams && output(false) == test.expected;
      immutableSeams = immutableSeams && taskPayload()["records"] == beforeSeamRecords &&
          records[0].version == firstVersion && records[1].version == secondVersion &&
          records[0].pendingReplacement;
    }
    check("cross_record_CJK_ASCII_and_sentence_seams_are_readable_without_spacing_CJK_prose",
          allSeams);
    check("display_seam_repair_keeps_raw_records_versions_pending_state_and_queue_unchanged",
          immutableSeams && worker->submitted.load() == beforeSeamSubmit &&
              generation == beforeSeamGeneration);
    const std::string gluedFixture = "Rerunning.Run build.cmd --tests.No output. "
        "https://Example.com/Run.Build?v=1.2.3 C:\\Work\\App.exe src/main.cpp "
        "v1.2.3 3.14 Dr.Smith e.g.Tools U.S.A.Paths `build.cmd --tests`";
    records = {layoutRecord(gluedFixture, gluedFixture, "raw-token-fixture", 0)};
    const auto rawTokenRecords = taskPayload()["records"];
    const auto rawTokenVersion = records[0].version;
    refresh();
    check("refresh_does_not_guess_internal_sentence_boundaries_or_rewrite_protected_tokens",
          output(true) == wide(gluedFixture) && output(false) == wide(gluedFixture) &&
              records[0].source == gluedFixture && records[0].translation == gluedFixture &&
              records[0].version == rawTokenVersion &&
              taskPayload()["records"] == rawTokenRecords &&
              generation == beforeSeamGeneration &&
              worker->submitted.load() == beforeSeamSubmit);
    records = paragraphFixtureRecords;
    records[0].translation = "\r\n\n第一句。\r\n";
    records[1].translation = "\n第二句。\n\n";
    check("display_removes_empty_edge_lines_without_changing_cached_translation",
          output(false).rfind(L"第一句。第二句。\r\n", 0) == 0 &&
              records[0].translation == "\r\n\n第一句。\r\n");
    records[0].translation = "保留的旧译文。";
    records[0].pendingReplacement = true;
    records[0].state = "translating";
    records[1].translation.clear();
    records[1].state = "queued";
    records[2].translation.clear();
    records[2].state = "error";
    records[2].error = "synthetic_long_error_details";
    check("translation_body_excludes_waiting_updating_and_error_messages",
          output(false) == L"保留的旧译文。\r\n另一个块。\r\n历史独立段。" &&
              output(false).find(L"翻译中") == std::wstring::npos &&
              output(false).find(L"更新中") == std::wstring::npos &&
              output(false).find(L"synthetic_long_error_details") == std::wstring::npos);
    check("footer_reports_waiting_updates_and_failures_outside_body",
          progressCaption() == L"已译 2 · 待译 1 · 更新中 1 · 失败 1");
    const bool layoutOriginal = settings.show_original;
    const bool layoutFollowing = followingSource;
    settings.show_original = true;
    render();
    const auto sourceReplacements = renderedBodies[sourceEdit].replacements;
    const auto sourceFormats = renderedBodies[sourceEdit].formats;
    const auto translationReplacements = renderedBodies[translationEdit].replacements;
    render();
    check("unchanged_render_skips_both_RichEdit_replacements_and_formatting",
          renderedBodies[sourceEdit].replacements == sourceReplacements &&
              renderedBodies[sourceEdit].formats == sourceFormats &&
              renderedBodies[translationEdit].replacements == translationReplacements);
    followingSource = false;
    CHARRANGE layoutSelection{1, 4};
    SendMessageW(sourceEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&layoutSelection));
    records[3].translation += "增量内容。";
    render();
    CHARRANGE layoutSelectionAfter{};
    SendMessageW(sourceEdit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&layoutSelectionAfter));
    check("translation_delta_does_not_rewrite_unchanged_original_or_selection",
          renderedBodies[sourceEdit].replacements == sourceReplacements &&
              renderedBodies[sourceEdit].formats == sourceFormats &&
              layoutSelectionAfter.cpMin == layoutSelection.cpMin &&
              layoutSelectionAfter.cpMax == layoutSelection.cpMax);
    const auto layoutFont = settings.font_size;
    const auto beforeFontFormat = renderedBodies[translationEdit].formats;
    const auto beforeFontReplace = renderedBodies[translationEdit].replacements;
    settings.font_size = layoutFont == 40 ? 39 : layoutFont + 1;
    render();
    CHARRANGE fontProbe{0, 1};
    SendMessageW(translationEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&fontProbe));
    CHARFORMAT2W updatedStyle{};
    updatedStyle.cbSize = sizeof(updatedStyle);
    SendMessageW(translationEdit, EM_GETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&updatedStyle));
    check("font_size_change_forces_format_without_replacing_identical_body",
          renderedBodies[translationEdit].formats == beforeFontFormat + 1 &&
              renderedBodies[translationEdit].replacements == beforeFontReplace &&
              updatedStyle.yHeight == settings.font_size * 20);
    settings.font_size = layoutFont;
    auto rectWidth = [](const RECT &rect) { return rect.right - rect.left; };
    const auto wideGeometry = readingGeometry(s(2400), s(800), false);
    const auto normalGeometry = readingGeometry(s(1040), s(800), false);
    const auto narrowGeometry = readingGeometry(s(680), s(800), false);
    check("wide_single_reading_column_is_capped_at_760_DIP_and_centered",
          rectWidth(wideGeometry.translated) == s(760) &&
              abs(wideGeometry.translated.left + wideGeometry.translated.right - s(2400)) <= 1);
    check("normal_single_reading_column_stays_centered_with_same_cap",
          rectWidth(normalGeometry.translated) == s(760) &&
              abs(normalGeometry.translated.left + normalGeometry.translated.right - s(1040)) <= 1);
    check("narrow_single_column_shrinks_with_client_area",
          narrowGeometry.translated.left == s(16) &&
              narrowGeometry.translated.right == s(680) - s(16));
    const auto wideDual = readingGeometry(s(2400), s(800), true);
    const auto narrowDual = readingGeometry(s(680), s(800), true);
    const auto leftRegion = (s(2400) - 2 * s(16) - s(10)) / 2;
    check("wide_dual_columns_are_capped_and_centered_in_each_half",
          rectWidth(wideDual.source) == s(760) &&
              rectWidth(wideDual.translated) == s(760) &&
              abs(wideDual.source.left + wideDual.source.right -
                  (2 * s(16) + leftRegion)) <= 1 &&
              wideDual.source.right < wideDual.separator &&
              wideDual.translated.left > wideDual.separator);
    check("narrow_dual_columns_fit_without_crossing_the_separator",
          narrowDual.source.left == s(16) &&
              narrowDual.translated.right == s(680) - s(16) &&
              narrowDual.source.right < narrowDual.separator &&
              narrowDual.translated.left > narrowDual.separator);
    RECT outerBefore{};
    GetWindowRect(hwnd, &outerBefore);
    const bool translatedFollowBefore = followingTranslation;
    std::string wrapFixture;
    for (int line = 0; line < 20; ++line) {
      for (int word = 0; word < 24; ++word)
        wrapFixture += "Readable paragraph content stays within a comfortable column. ";
      wrapFixture += "\n";
    }
    records = {layoutRecord(wrapFixture, wrapFixture, "reading-width-fixture", 0)};
    followingSource = followingTranslation = true;
    settings.show_original = false;
    SetWindowPos(hwnd, nullptr, 0, 0, s(1800), s(760),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    render();
    const auto beforeViewportPayload = taskPayload()["records"];
    const auto beforeViewportGeneration = generation;
    const auto beforeViewportSource = sourceGeneration;
    const auto beforeViewportSubmit = worker->submitted.load();
    const auto wideLines = SendMessageW(translationEdit, EM_GETLINECOUNT, 0, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, s(720), s(760),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const auto narrowLines = SendMessageW(translationEdit, EM_GETLINECOUNT, 0, 0);
    auto childRect = [&](HWND child) {
      RECT rect{};
      GetWindowRect(child, &rect);
      MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&rect), 2);
      return rect;
    };
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto narrowPane = childRect(translationEdit);
    check("720_DIP_window_wraps_body_inside_client_and_keeps_following_bottom",
          narrowPane.left >= 0 && narrowPane.right <= client.right &&
              rectWidth(narrowPane) < s(760) && narrowLines > wideLines &&
              followingTranslation && atBottom(translationEdit));
    check("720_DIP_header_and_toolbar_controls_do_not_overlap_or_overflow",
          childRect(taskLabel).right < childRect(providerLabel).left &&
              childRect(topCheck).right < childRect(settingsButton).left &&
              childRect(combo).right < childRect(refreshButton).left &&
              childRect(pauseButton).right <= client.right &&
              childRect(countLabel).right <= client.right);
    settings.show_original = true;
    layout();
    check("720_DIP_dual_RichEdit_columns_stay_inside_separate_halves",
          childRect(sourceEdit).left >= 0 &&
              childRect(sourceEdit).right < childRect(translationEdit).left &&
              childRect(translationEdit).right <= client.right);
    settings.show_original = false;
    layout();
    followingTranslation = false;
    SendMessageW(translationEdit, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(translationEdit, EM_LINESCROLL, 0, 6);
    CHARRANGE refreshSelection{3, 10};
    SendMessageW(translationEdit, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&refreshSelection));
    POINT beforeRefreshScroll{};
    SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&beforeRefreshScroll));
    const auto beforeRefreshText = textOf(translationEdit);
    const auto beforeRefreshReplacements = renderedBodies[translationEdit].replacements;
    const auto beforeRefreshFormats = renderedBodies[translationEdit].formats;
    RECT expectedFormatRect{};
    SendMessageW(translationEdit, EM_GETRECT, 0,
                 reinterpret_cast<LPARAM>(&expectedFormatRect));
    CHARFORMAT2W staleText{};
    staleText.cbSize = sizeof(staleText);
    staleText.dwMask = CFM_COLOR;
    staleText.crTextColor = RGB(0, 0, 0);
    SendMessageW(translationEdit, EM_SETCHARFORMAT, SCF_ALL,
                 reinterpret_cast<LPARAM>(&staleText));
    CHARRANGE allBody{0, -1};
    SendMessageW(translationEdit, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&allBody));
    PARAFORMAT2 staleParagraph{};
    staleParagraph.cbSize = sizeof(staleParagraph);
    staleParagraph.dwMask = PFM_ALIGNMENT;
    staleParagraph.wAlignment = PFA_CENTER;
    SendMessageW(translationEdit, EM_SETPARAFORMAT, 0,
                 reinterpret_cast<LPARAM>(&staleParagraph));
    SendMessageW(translationEdit, EM_SETZOOM, 3, 2);
    SendMessageW(translationEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(s(40), s(40)));
    RECT staleFormatRect{0, 0, s(180), s(400)};
    SendMessageW(translationEdit, EM_SETRECT, 0,
                 reinterpret_cast<LPARAM>(&staleFormatRect));
    SendMessageW(translationEdit, EM_EXSETSEL, 0,
                 reinterpret_cast<LPARAM>(&refreshSelection));
    // Preserve the caller's selected viewport even while an enumeration is in flight.
    SendMessageW(translationEdit, EM_SETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&beforeRefreshScroll));
    const bool wasRefreshing = refreshing;
    const auto beforeLocalRefreshTicket = refreshGeneration;
    refreshing = true;
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_REFRESH, BN_CLICKED),
                 reinterpret_cast<LPARAM>(refreshButton));
    CHARRANGE afterRefreshSelection{};
    POINT afterRefreshScroll{};
    SendMessageW(translationEdit, EM_EXGETSEL, 0,
                 reinterpret_cast<LPARAM>(&afterRefreshSelection));
    SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&afterRefreshScroll));
    CHARFORMAT2W restoredText{};
    restoredText.cbSize = sizeof(restoredText);
    SendMessageW(translationEdit, EM_GETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&restoredText));
    PARAFORMAT2 restoredParagraph{};
    restoredParagraph.cbSize = sizeof(restoredParagraph);
    SendMessageW(translationEdit, EM_GETPARAFORMAT, 0,
                 reinterpret_cast<LPARAM>(&restoredParagraph));
    RECT restoredFormatRect{};
    SendMessageW(translationEdit, EM_GETRECT, 0,
                 reinterpret_cast<LPARAM>(&restoredFormatRect));
    int zoomNumerator = 0, zoomDenominator = 0;
    SendMessageW(translationEdit, EM_GETZOOM,
                 reinterpret_cast<WPARAM>(&zoomNumerator),
                 reinterpret_cast<LPARAM>(&zoomDenominator));
    check("refresh_restores_char_paragraph_zoom_and_current_reading_margins",
          restoredText.crTextColor == (renderedBodies[translationEdit].latestDisplay.cpMin <= refreshSelection.cpMin &&
              renderedBodies[translationEdit].latestDisplay.cpMax >= refreshSelection.cpMax
                  ? cclive::text_layout_palette.latest : cclive::text_layout_palette.body) &&
              restoredText.yHeight == settings.font_size * 20 &&
              restoredParagraph.wAlignment == PFA_LEFT &&
              zoomNumerator == 1 && zoomDenominator == 1 &&
              abs(restoredFormatRect.left - expectedFormatRect.left) <= 1 &&
              abs(restoredFormatRect.right - expectedFormatRect.right) <= 1);
    check("refresh_reformats_same_text_without_replacement_translation_or_enumeration",
          IsWindowEnabled(refreshButton) &&
              renderedBodies[translationEdit].formats == beforeRefreshFormats + 1 &&
              renderedBodies[translationEdit].replacements == beforeRefreshReplacements &&
              textOf(translationEdit) == beforeRefreshText &&
              taskPayload()["records"] == beforeViewportPayload &&
              generation == beforeViewportGeneration && sourceGeneration == beforeViewportSource &&
              worker->submitted.load() == beforeViewportSubmit &&
              refreshGeneration == beforeLocalRefreshTicket && refreshing);
    check("refresh_preserves_manual_scroll_selection_and_follow_choice",
          !followingTranslation &&
              afterRefreshSelection.cpMin == refreshSelection.cpMin &&
              afterRefreshSelection.cpMax == refreshSelection.cpMax &&
              abs(afterRefreshScroll.y - beforeRefreshScroll.y) <= 2);
    refreshing = wasRefreshing;
    SetWindowPos(hwnd, nullptr, 0, 0, s(1800), s(760),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    check("resize_after_manual_reading_keeps_selection_and_follow_disabled",
          !followingTranslation && textOf(translationEdit) == beforeRefreshText);
    followingTranslation = true;
    SetWindowPos(hwnd, nullptr, 0, 0, s(720), s(760),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    check("resize_rewrap_keeps_explicit_follow_at_the_new_bottom",
          followingTranslation && atBottom(translationEdit) &&
              taskPayload()["records"] == beforeViewportPayload &&
              generation == beforeViewportGeneration &&
              worker->submitted.load() == beforeViewportSubmit);
    settings.show_original = layoutOriginal;
    followingSource = layoutFollowing;
    followingTranslation = translatedFollowBefore;
    SetWindowPos(hwnd, nullptr, outerBefore.left, outerBefore.top,
                 outerBefore.right - outerBefore.left, outerBefore.bottom - outerBefore.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    settings.show_original = layoutOriginal;
    followingSource = layoutFollowing;
    clear();
    std::string manyLines;
    for (int n = 0; n < 140; ++n)
      manyLines += "Row " + std::to_string(n) + "\n";
    Record oversized;
    oversized.key = "oversized";
    oversized.serial = ++serial;
    oversized.version = ++revision;
    oversized.source = manyLines;
    oversized.translation = manyLines;
    oversized.state = "done";
    auto oversizedVersion = oversized.version;
    records.push_back(oversized);
    sourceAliases["oversized-runtime"] = {oversized.key, "large-block", ""};
    trimRecords();
    check("single_record_keeps_only_last_100_lines",
          records.size() == 1 && retainedLines() == 100 &&
              logicalLines(records[0].source) == 100 &&
              records[0].translation.rfind("Row 40\n", 0) == 0);
    std::string mixedLines;
    for (int index = 0; index < 140; ++index)
      mixedLines += "Mixed " + std::to_string(index) +
                    (index % 3 == 0   ? "\r"
                     : index % 3 == 1 ? "\r\n"
                                      : "\n");
    check("logical_line_limit_accepts_LF_CRLF_and_CR",
          logicalLines(mixedLines) == 140 &&
              logicalLines(tailLines(mixedLines, 100)) == 100 &&
              tailLines(mixedLines, 100).rfind("Mixed 40", 0) == 0);
    auto croppedText = records[0].translation;
    acceptResult({{"task_id", activeTaskId},
                  {"generation", generation},
                  {"key", "oversized"},
                  {"version", oversizedVersion},
                  {"state", "done"},
                  {"translation", manyLines}});
    check("old_job_cannot_refill_cropped_subtitles",
          records[0].translation == croppedText &&
              records[0].version != oversizedVersion &&
              !sourceAliases.count("oversized-runtime"));
    clear();
    paused = false;
    oversized.translation.clear();
    oversized.state = "waiting";
    oversized.version = ++revision;
    records.push_back(oversized);
    auto submittedBeforeCrop = worker->submitted.load();
    trimRecords();
    check("cropped_pending_source_tail_is_requeued",
          retainedLines() == 100 && records[0].state == "queued" &&
              worker->submitted.load() == submittedBeforeCrop + 1);
    cancelAll();
    paused = true;
    clear();
    // Task identity and cache regressions use only supplied synthetic events.
    paused = true;
    running = true;
    auto taskMeta = [&](const std::string &id,
                        std::vector<std::string> sources) {
      json signatures = json::array();
      for (const auto &source : sources)
        signatures.push_back(contentSignature(source));
      return json{{"type", "task"},
                  {"task_id", id},
                  {"task_title", "同名任务"},
                  {"identity_quality", "url"},
                  {"source_hwnd", 0},
                  {"_generation", sourceGeneration},
                  {"source_signatures", signatures}};
    };
    auto taskText = [&](const std::string &id, const std::string &key,
                        const std::string &source, int epoch = 0) {
      auto event = textEvent(key, source, epoch, sourceGeneration);
      event["task_id"] = id;
      return event;
    };
    auto complete = [&](std::size_t index, const std::string &translated) {
      const auto &record = records.at(index);
      acceptResult({{"generation", generation},
                    {"task_id", activeTaskId},
                    {"key", record.key},
                    {"version", record.version},
                    {"state", "done"},
                    {"translation", translated},
                    {"error", ""}});
    };
    accept(taskMeta("latest-marker-task", {"First marker source.", "Second marker source."}));
    accept(taskText("latest-marker-task", "marker-first", "First marker source."));
    complete(0, "**相同字幕**");
    accept(taskText("latest-marker-task", "marker-second", "Second marker source."));
    complete(1, "**相同字幕**");
    render();
    auto markerRange = renderedBodies[translationEdit].latestDisplay;
    const auto repeatedVisible = renderedBodies[translationEdit].displayed;
    check("latest_marker_selects_new_occurrence_of_identical_subtitle",
          latestVisibleRecord() == &records[1] && markerRange.cpMin > 0 &&
          repeatedVisible.substr(markerRange.cpMin, markerRange.cpMax - markerRange.cpMin) == L"相同字幕" &&
          paneStyle(translationEdit, markerRange.cpMin, 1).crTextColor == cclive::text_layout_palette.latest &&
          paneStyle(translationEdit, 0, 1).crTextColor == cclive::text_layout_palette.body);
    complete(0, "第一条刚刚更新");
    render();
    markerRange = renderedBodies[translationEdit].latestDisplay;
    check("latest_marker_follows_actual_earlier_row_update",
          latestVisibleRecord() == &records[0] && markerRange.cpMin == 0 &&
          paneStyle(translationEdit, 0, 1).crTextColor == cclive::text_layout_palette.latest &&
          paneStyle(translationEdit, markerRange.cpMax + 1, 1).crTextColor == cclive::text_layout_palette.body);
    const auto markerSequence = records[0].displayedUpdate;
    acceptResult({{"generation", generation - 1}, {"task_id", activeTaskId},
                  {"key", records[1].key}, {"version", records[1].version},
                  {"state", "done"}, {"translation", "过期结果"}});
    check("obsolete_result_does_not_move_latest_marker",
          latestVisibleRecord() == &records[0] && records[0].displayedUpdate == markerSequence);
    accept(taskText("latest-marker-task", "marker-first", "Revised first marker source."));
    complete(1, "第二条现在更新");
    acceptResult({{"generation", generation}, {"task_id", activeTaskId},
                  {"key", records[0].key}, {"version", records[0].version},
                  {"state", "translating"}, {"translation", "尚未提交的新译文"}});
    render();
    check("undisplayed_replacement_delta_does_not_move_latest_marker",
          latestVisibleRecord() == &records[1] && records[0].translation == "第一条刚刚更新");
    const auto markerReplacements = renderedBodies[translationEdit].replacements;
    const auto markerFormats = renderedBodies[translationEdit].formats;
    complete(0, "第一条刚刚更新");
    render();
    check("marker_only_move_reformats_without_replacing_identical_text",
          latestVisibleRecord() == &records[0] &&
          renderedBodies[translationEdit].latestDisplay.cpMin == 0 &&
          renderedBodies[translationEdit].replacements == markerReplacements &&
          renderedBodies[translationEdit].formats == markerFormats + 1);
    const auto restoredMarkerSequence = records[0].displayedUpdate;
    accept(taskMeta("latest-marker-other", {}));
    accept(taskMeta("latest-marker-task", {"Revised first marker source.", "Second marker source."}));
    check("RAM_task_restore_preserves_latest_updated_record",
          records.size() == 2 && records[0].displayedUpdate == restoredMarkerSequence &&
          latestVisibleRecord() == &records[0]);
    // Exercise the same coalescing and ordering path used by tick(), including
    // the lexical record-9 / record-10 boundary that a map alone misorders.
    accept(taskMeta("latest-marker-bus-order", {}));
    for (int n = 0; n < 10; ++n)
      accept(taskText("latest-marker-bus-order", "bus-marker-" + std::to_string(n),
                      "Bus marker source " + std::to_string(n) + "."));
    Bus markerResults;
    auto markerResult = [&](std::size_t index, const std::string &state,
                            const std::string &translated) {
      const auto &record = records.at(index);
      return json{{"generation", generation}, {"task_id", activeTaskId},
                  {"key", record.key}, {"version", record.version},
                  {"state", state}, {"translation", translated}, {"error", ""}};
    };
    auto drainMarkerResults = [&] {
      std::map<std::string, json> completed;
      {
        std::lock_guard<std::mutex> lock(markerResults.mutex);
        completed.swap(markerResults.results);
      }
      auto ordered = Bus::orderedResults(std::move(completed));
      for (const auto &result : ordered)
        acceptResult(result);
      return ordered;
    };
    markerResults.result(markerResult(8, "done", "第九条"));
    markerResults.result(markerResult(9, "done", "第十条"));
    const auto initialMarkerBatch = drainMarkerResults();
    render();
    check("same_batch_record_9_and_10_follow_result_arrival_not_string_key_order",
          records[8].key == "record-9" && records[9].key == "record-10" &&
          initialMarkerBatch.size() == 2 &&
          initialMarkerBatch[0]["key"] == records[8].key &&
          initialMarkerBatch[1]["key"] == records[9].key &&
          latestVisibleRecord() == &records[9] &&
          records[8].displayedUpdate < records[9].displayedUpdate &&
          paneStyle(translationEdit, renderedBodies[translationEdit].latestDisplay.cpMin, 1).crTextColor ==
              cclive::text_layout_palette.latest);
    const auto beforeCoalescedMarker = displayedUpdateSequence;
    markerResults.result(markerResult(8, "translating", "尚未显示的累计片段"));
    markerResults.result(markerResult(9, "done", "第十条再次完成"));
    markerResults.result(markerResult(8, "done", "第九条最后完成"));
    const auto coalescedMarkerBatch = drainMarkerResults();
    check("same_job_stream_coalesces_to_latest_snapshot_at_its_actual_arrival_order",
          coalescedMarkerBatch.size() == 2 &&
          coalescedMarkerBatch[0]["key"] == records[9].key &&
          coalescedMarkerBatch[1]["key"] == records[8].key &&
          coalescedMarkerBatch[1]["state"] == "done" &&
          records[8].translation == "第九条最后完成" &&
          latestVisibleRecord() == &records[8] &&
          displayedUpdateSequence == beforeCoalescedMarker + 2);
    const auto beforeObsoleteMarker = displayedUpdateSequence;
    auto obsoleteMarkerVersion = markerResult(9, "done", "过期版本");
    obsoleteMarkerVersion["version"] = records[9].version - 1;
    markerResults.result(std::move(obsoleteMarkerVersion));
    auto obsoleteMarkerGeneration = markerResult(9, "done", "过期代次");
    obsoleteMarkerGeneration["generation"] = generation - 1;
    markerResults.result(std::move(obsoleteMarkerGeneration));
    drainMarkerResults();
    check("chronological_batch_obsolete_results_cannot_move_latest_marker",
          latestVisibleRecord() == &records[8] &&
          records[9].translation == "第十条再次完成" &&
          displayedUpdateSequence == beforeObsoleteMarker);
    Bus boundedMarkerResults;
    for (int n = 1; n <= 257; ++n)
      boundedMarkerResults.result({{"generation", 0},
                                    {"key", "record-" + std::to_string(n)},
                                    {"version", 1}, {"state", "done"}});
    const auto boundedMarkerBatch =
        Bus::orderedResults(std::move(boundedMarkerResults.results));
    check("bounded_result_bus_evicts_oldest_arrival_and_preserves_newest",
          boundedMarkerBatch.size() == 256 &&
          boundedMarkerBatch.front()["key"] == "record-2" &&
          boundedMarkerBatch.back()["key"] == "record-257");
    clear(true);
    check("clear_removes_latest_highlight_with_old_subtitles",
          !latestVisibleRecord() && textOf(translationEdit).empty() &&
          renderedBodies[translationEdit].latestDisplay.cpMin == -1);
    const std::string tableSource = "| Item | Time | Notes |\n| --- | ---: | --- |\n| All tests passed. | 387 | a\\|b |\n| Working on your request... | 736 | second<br>line |";
    const std::string tableTranslation = "| 项目 | 时间 | 说明 |\n| --- | ---: | --- |\n| 所有测试通过。 | 387 | a\\|b |\n| 正在处理请求。 | 736 | 第二行<br>续行 |";
    accept(taskMeta("native-table-task", {tableSource}));
    accept(taskText("native-table-task", "native-table-source", tableSource));
    complete(0, tableTranslation);
    render();
    const auto nativeTable = cclive::make_text_layout(wide(tableTranslation));
    const auto nativeCell = cclive::native_text_position(nativeTable,
        static_cast<LONG>(nativeTable.text.find(L"387")));
    PARAFORMAT2 tableFormat{}; tableFormat.cbSize = sizeof(tableFormat);
    CHARRANGE tableCell{nativeCell, nativeCell + 1};
    SendMessageW(translationEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tableCell));
    SendMessageW(translationEdit, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&tableFormat));
    check("translated_table_uses_native_grid_with_intact_cells",
          renderedBodies[translationEdit].hasTables && (tableFormat.wEffects & PFE_TABLE) &&
          textOf(translationEdit).find(L"387") != std::wstring::npos &&
          textOf(translationEdit).find(L"---") == std::wstring::npos &&
          textOf(translationEdit).find(L"a|b") != std::wstring::npos);
    check("translated_table_latest_span_uses_native_character_positions",
          paneStyle(translationEdit, nativeCell, 1).crTextColor == cclive::text_layout_palette.latest);
    accept(taskText("native-table-task", "native-table-tail", "A new caption after the table."));
    complete(1, "表后新字幕");
    render();
    auto nativeMarker = renderedBodies[translationEdit].latestDisplay;
    std::wstring markedText(64, L'\0');
    SendMessageW(translationEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&nativeMarker));
    const auto markedLength = SendMessageW(translationEdit, EM_GETSELTEXT, 0,
                                          reinterpret_cast<LPARAM>(markedText.data()));
    markedText.resize(static_cast<std::size_t>(markedLength));
    check("caption_after_native_table_has_correct_latest_text_and_color",
          markedText == L"表后新字幕" &&
          paneStyle(translationEdit, nativeMarker.cpMin, 1).crTextColor == cclive::text_layout_palette.latest &&
          paneStyle(translationEdit, nativeCell, 1).crTextColor == cclive::text_layout_palette.body);
    const auto oldTableVersion = records[0].version;
    auto revisedTable = tableSource;
    revisedTable.replace(revisedTable.find("387"), 3, "867");
    accept(taskText("native-table-task", "native-table-source", revisedTable));
    check("table_revision_keeps_verified_grid_until_complete",
          records.size() == 2 && records[0].pendingReplacement &&
          records[0].version > oldTableVersion && records[0].translation == tableTranslation);
    auto revisedTranslation = tableTranslation;
    revisedTranslation.replace(revisedTranslation.find("387"), 3, "867");
    complete(0, revisedTranslation);
    render();
    check("table_revision_updates_original_grid_and_latest_marker_in_place",
          records.size() == 2 && !records[0].pendingReplacement &&
          records[0].translation == revisedTranslation &&
          latestVisibleRecord() == &records[0] &&
          textOf(translationEdit).find(L"867") != std::wstring::npos &&
          textOf(translationEdit).find(L"387") == std::wstring::npos &&
          paneStyle(translationEdit, nativeCell, 1).crTextColor == cclive::text_layout_palette.latest);
    accept(taskMeta("native-table-other", {}));
    accept(taskMeta("native-table-task", {revisedTable, "A new caption after the table."}));
    render();
    check("native_table_and_latest_marker_survive_RAM_task_restore",
          records.size() == 2 && records[0].translation == revisedTranslation &&
          renderedBodies[translationEdit].hasTables && latestVisibleRecord() == &records[0]);
    const std::wstring tableStructureA = L"| A |\n| --- |\n| B |\nC\nAfter";
    const std::wstring tableStructureB = L"A\n| B |\n| --- |\n| C |\nAfter";
    editText(translationEdit, tableStructureA);
    const auto structureReplacements = renderedBodies[translationEdit].replacements;
    editText(translationEdit, tableStructureB);
    const auto structureLayout = cclive::make_text_layout(tableStructureB);
    CHARRANGE structureCell{cclive::native_text_position(structureLayout,
        static_cast<LONG>(structureLayout.text.find(L"C"))), 0};
    structureCell.cpMax = structureCell.cpMin + 1;
    SendMessageW(translationEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&structureCell));
    PARAFORMAT2 structureStyle{}; structureStyle.cbSize = sizeof(structureStyle);
    SendMessageW(translationEdit, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&structureStyle));
    check("same_visible_text_with_changed_table_structure_rebuilds_native_rows",
          cclive::make_text_layout(tableStructureA).text == structureLayout.text &&
          renderedBodies[translationEdit].replacements == structureReplacements + 1 &&
          (structureStyle.wEffects & PFE_TABLE));
    clear(true);
    check("clear_native_table_immediately_removes_grid_and_latest_marker",
          records.empty() && textOf(translationEdit).empty() &&
          !renderedBodies[translationEdit].hasTables &&
          renderedBodies[translationEdit].latestDisplay.cpMin == -1);
    accept(taskMeta("latest-navigation-task", {}));
    for (int n = 0; n < 100; ++n) {
      accept(taskText("latest-navigation-task", "navigation-" + std::to_string(n),
                      "Navigation source " + std::to_string(n) + "."));
      complete(records.size() - 1, "历史字幕第 " + std::to_string(n) + " 行");
    }
    render();
    followingTranslation = false;
    complete(0, "顶部字幕是刚刚更新的内容");
    render();
    const auto markerTextLength = static_cast<LONG>(renderedBodies[translationEdit].displayed.size());
    CHARRANGE markerSelection{markerTextLength - 8, markerTextLength - 4};
    SendMessageW(translationEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&markerSelection));
    goLatest();
    POINTL markerPoint{};
    SendMessageW(translationEdit, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&markerPoint),
                 renderedBodies[translationEdit].latestDisplay.cpMin);
    RECT markerClient{}; GetClientRect(translationEdit, &markerClient);
    CHARRANGE afterMarkerSelection{};
    SendMessageW(translationEdit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&afterMarkerSelection));
    check("latest_button_reveals_updated_old_row_and_preserves_selection",
          followingTranslation && markerPoint.y >= 0 && markerPoint.y < markerClient.bottom &&
          !isAtBottom(translationEdit) && afterMarkerSelection.cpMin == markerSelection.cpMin &&
          afterMarkerSelection.cpMax == markerSelection.cpMax);
    records.erase(records.begin());
    trimRecords();
    check("evicted_latest_marker_falls_back_to_newest_remaining_subtitle",
          latestVisibleRecord() == &records.back() && retainedLines() <= 100);
    clear();
    accept(taskMeta("layout-cache-task", {"First layout source.", "Second layout source."}));
    auto firstLayoutEvent = taskText("layout-cache-task", "layout-runtime-1", "First layout source.");
    firstLayoutEvent["block_key"] = "layout-cache-block";
    firstLayoutEvent["paragraph"] = 0;
    accept(firstLayoutEvent);
    complete(0, "第一句缓存译文。");
    auto secondLayoutEvent = taskText("layout-cache-task", "layout-runtime-2", "Second layout source.");
    secondLayoutEvent["block_key"] = "layout-cache-block";
    secondLayoutEvent["paragraph"] = 0;
    accept(secondLayoutEvent);
    complete(1, "第二句缓存译文。");
    const auto layoutVersion = records[0].version;
    const auto beforeLayoutMetadata = worker->submitted.load();
    firstLayoutEvent["paragraph"] = 3;
    accept(firstLayoutEvent);
    check("metadata_only_paragraph_change_keeps_completed_translation_and_version",
          records[0].hasLayout && records[0].layoutBlock == "layout-cache-block" &&
              records[0].layoutParagraph == 3 && records[0].version == layoutVersion &&
              records[0].state == "done" &&
              records[0].translation == "第一句缓存译文。" &&
              worker->submitted.load() == beforeLayoutMetadata);
    std::size_t changedParagraph = 4;
    for (const auto &state : {"error", "stopped", "waiting"}) {
      records[0].state = state;
      firstLayoutEvent["paragraph"] = changedParagraph++;
      accept(firstLayoutEvent);
      check(state == std::string("error")
                ? "paragraph_only_update_does_not_retry_failed_translation"
            : state == std::string("stopped")
                ? "paragraph_only_update_does_not_restart_stopped_translation"
                : "paragraph_only_update_does_not_enqueue_waiting_translation",
            records[0].state == state && records[0].version == layoutVersion &&
                records[0].translation == "第一句缓存译文。" &&
                worker->submitted.load() == beforeLayoutMetadata);
    }
    records[0].state = "done";
    firstLayoutEvent["paragraph"] = 3;
    accept(firstLayoutEvent);
    const auto savedLayoutPayload = taskPayload();
    check("RAM_payload_preserves_layout_block_and_paragraph_metadata",
          savedLayoutPayload["records"][0].value("has_layout", false) &&
              savedLayoutPayload["records"][0].value("layout_block", "") == "layout-cache-block" &&
              savedLayoutPayload["records"][0].value("layout_paragraph", 0) == 3);
    accept(taskMeta("layout-cache-other", {}));
    accept(taskMeta("layout-cache-task", {"First layout source.", "Second layout source."}));
    check("RAM_task_restore_preserves_independent_paragraph_boundaries",
          records.size() == 2 && records[0].hasLayout && records[1].hasLayout &&
              records[0].layoutBlock == "layout-cache-block" &&
              records[0].layoutParagraph == 3 && records[1].layoutParagraph == 0 &&
              output(false) == L"第一句缓存译文。\r\n第二句缓存译文。");
    // Use the real Ledger path: the reader restores paragraph boundaries while
    // the existing sentence strings and subtitle slots remain unchanged.
    const std::string repairTask = "reader-layout-repair-task";
    const std::vector<std::string> repairSources = {
        "Rerunning.", "Run build.cmd --tests.", "No output."};
    accept(taskMeta(repairTask, repairSources));
    cclive::Ledger repairLedger(0);
    json repairSnapshot = {{"context", repairTask},
        {"blocks", json::array({{{"key", "reader-shaped-answer"},
                                  {"kind", "answer"},
                                  {"text", "Rerunning.\nRun build.cmd --tests.\nNo output."}}})}};
    auto acceptRepairEvents = [&](const std::vector<json> &events) {
      for (auto event : events) {
        event["task_id"] = repairTask;
        event["_generation"] = sourceGeneration;
        accept(std::move(event));
      }
    };
    acceptRepairEvents(repairLedger.observe(repairSnapshot));
    complete(0, "正在重新运行。");
    complete(1, "Run build.cmd --tests.");
    complete(2, "没有输出。");
    check("reader_shaped_single_LF_fixture_has_three_existing_sentences_in_one_paragraph",
          records.size() == 3 &&
              std::all_of(records.begin(), records.end(), [](const Record &record) {
                return record.hasLayout && record.layoutParagraph == 0;
              }) &&
              output(false) == L"正在重新运行。 Run build.cmd --tests. 没有输出。");
    auto immutableTranslationState = [&] {
      json state = json::array();
      for (const auto &record : records)
        state.push_back({{"key", record.key}, {"source", record.source},
                         {"translation", record.translation},
                         {"source_signature", record.sourceSignature},
                         {"version", record.version}, {"state", record.state},
                         {"pending", record.pendingReplacement}});
      return state;
    };
    const auto beforeRepairState = immutableTranslationState();
    const auto beforeRepairSubmit = worker->submitted.load();
    const auto beforeRepairGeneration = generation;
    const auto beforeRepairSourceGeneration = sourceGeneration;
    repairSnapshot["blocks"][0]["text"] =
        "Rerunning.\n\nRun build.cmd --tests.\n\nNo output.";
    const auto paragraphOnlyEvents = repairLedger.observe(repairSnapshot);
    acceptRepairEvents(paragraphOnlyEvents);
    refresh();
    check("reader_paragraph_only_events_and_refresh_restore_three_visible_paragraphs",
          paragraphOnlyEvents.size() == 2 && records.size() == 3 &&
              records[0].layoutParagraph == 0 && records[1].layoutParagraph == 1 &&
              records[2].layoutParagraph == 2 &&
              output(false) == L"正在重新运行。\r\nRun build.cmd --tests.\r\n没有输出。");
    check("reader_layout_repair_preserves_existing_translation_keys_versions_and_queue",
          immutableTranslationState() == beforeRepairState &&
              generation == beforeRepairGeneration &&
              sourceGeneration == beforeRepairSourceGeneration &&
              worker->submitted.load() == beforeRepairSubmit);
    const std::string taskA = "test-task-A", taskB = "test-task-B";
    const std::string sourceA = "First source for task A.",
                      sourceA2 = "Second source for task A.";
    accept(taskMeta(taskA, {sourceA, sourceA2}));
    accept(taskText(taskA, "runtime-A-1", sourceA));
    accept(taskText(taskA, "runtime-A-2", sourceA2));
    complete(0, "缓存译文 A\r\n" + utf8(lines(50)));
    complete(1, "缓存译文 A2");
    render();
    SendMessageW(translationEdit, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(translationEdit, EM_LINESCROLL, 0, 4);
    POINT taskAScroll{};
    SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&taskAScroll));
    auto aGeneration = generation;
    auto aVersion = records[0].version;
    auto aKey = records[0].key;
    auto aTranslation = records[0].translation;
    accept(taskMeta(taskB, {"Source for task B."}));
    check("switch_task_saves_previous_records",
          taskCache->get(taskA).has_value() && records.empty());
    accept(taskText(taskB, "runtime-B", "Source for task B."));
    complete(0, "缓存译文 B");
    acceptResult({{"task_id", taskA},
                  {"generation", aGeneration},
                  {"key", aKey},
                  {"version", aVersion},
                  {"state", "done"},
                  {"translation", "LATE A RESULT"}});
    check("late_other_task_result_rejected",
          records.size() == 1 && records[0].translation == "缓存译文 B");
    check("identical_titles_keep_distinct_task_cache",
          activeTaskTitle == "同名任务" && activeTaskId == taskB &&
              taskCache->get(taskA).has_value());
    // First restored snapshot can identify a task before body nodes are loaded.
    accept(taskMeta(taskA, {}));
    check("task_A_B_A_restores_all_cached_records",
          records.size() == 2 && records[0].translation == aTranslation &&
              records[1].translation == "缓存译文 A2");
    POINT restoredScroll{};
    SendMessageW(translationEdit, EM_GETSCROLLPOS, 0,
                 reinterpret_cast<LPARAM>(&restoredScroll));
    check("task_cache_restores_scroll_and_follow",
          !followingTranslation && abs(taskAScroll.y - restoredScroll.y) <= 2);
    accept(taskMeta(taskA, {sourceA, sourceA2}));
    auto submissionsBefore = worker->submitted.load();
    accept({{"type", "reset"},
            {"epoch", 7},
            {"task_id", taskA},
            {"_generation", sourceGeneration}});
    accept(taskText(taskA, "new-runtime-A-1", sourceA, 7));
    accept(taskText(taskA, "new-runtime-A-2", sourceA2, 7));
    check("cached_replay_after_empty_snapshot_does_not_duplicate",
          records.size() == 2 && records[0].translation == aTranslation);
    check("completed_cached_replay_does_not_retranslate",
          worker->submitted.load() == submissionsBefore &&
              records[0].state == "done" && records[1].state == "done");
    sourceAliases.clear();
    sourceNeedsReplay = true;
    ledgerEpoch = -1;
    accept(taskMeta(taskA, {sourceA, sourceA2}));
    accept(taskText(taskA, "restart-runtime-A-1", sourceA, 0));
    accept(taskText(taskA, "restart-runtime-A-2", sourceA2, 0));
    check("same_task_source_restart_reuses_completed_records",
          records.size() == 2 && worker->submitted.load() == submissionsBefore);
    accept(taskText(taskA, "new-content", "New source after returning.", 0));
    check("new_text_after_task_restore_appends",
          records.size() == 3 &&
              records.back().source == "New source after returning." &&
              records[0].translation == aTranslation);
    accept(taskText(taskA, "later-identical-source", sourceA, 0));
    check("new_identical_sentence_reuses_translation_at_latest_position",
          records.size() == 4 && records.back().source == sourceA &&
              records.back().state == "done" &&
              records.back().translation == aTranslation &&
              worker->submitted.load() == submissionsBefore);
    accept(taskText(taskB, "late-old-task-source", "Late B body.", 0));
    check("late_other_task_source_rejected",
          records.size() == 4 && activeTaskId == taskA);
    auto payload = taskPayload();

    check("cache_payload_has_no_api_credentials_or_endpoint",
          !payload.contains("api_key") && !payload.contains("api_key_env") &&
              !payload["translation_profile"].contains("base_url"));
    auto oldTarget = settings.target;
    settings.target = "zh-TW";
    accept(taskMeta(taskA, {sourceA, sourceA2}));
    check("translation_profile_change_invalidates_cached_output",
          records[0].translation.empty() && records[0].state == "paused");
    settings.target = oldTarget;
    clear();
    check("manual_clear_erases_only_current_task",
          !taskCache->get(taskA).has_value() &&
              taskCache->get(taskB).has_value());
    accept(taskMeta("replacement-cache-A", {"Original source A."}));
    accept(taskText("replacement-cache-A", "replace-A", "Original source A."));
    complete(0, "已确认旧译文 A");
    accept(taskText("replacement-cache-A", "replace-A", "Revised source A."));
    const auto pendingCachePayload = taskPayload();
    check("replacement_marker_is_preserved_in_ram_payload",
          pendingCachePayload["records"][0].value("pending_replacement", false) &&
              records[0].pendingReplacement &&
              records[0].translation == "已确认旧译文 A");
    accept(taskMeta("replacement-cache-B", {"Revised source A."}));
    accept(taskText("replacement-cache-B", "replace-B", "Revised source A."));
    check("other_task_never_reuses_pending_replacement_translation",
          records.size() == 1 && records[0].translation.empty() &&
              !records[0].pendingReplacement);
    complete(0, "独立译文 B");
    accept(taskMeta("replacement-cache-A", {"Revised source A."}));
    accept(taskText("replacement-cache-A", "replace-A-restored", "Revised source A."));
    check("pending_replacement_survives_A_B_A_ram_restore",
          records.size() == 1 && records[0].pendingReplacement &&
              records[0].state == "paused" &&
              records[0].translation == "已确认旧译文 A");
    accept(taskText("replacement-cache-A", "replace-A-new-occurrence", "Revised source A."));
    check("new_source_cannot_memoize_retained_old_translation",
          records.size() == 2 && records[1].translation.empty() &&
              !records[1].pendingReplacement);
    complete(0, "已确认新译文 A");
    check("restored_pending_replacement_commits_only_current_done",
          !records[0].pendingReplacement && records[0].state == "done" &&
              records[0].translation == "已确认新译文 A");
    accept(taskMeta("replacement-cache-B", {"Revised source A."}));
    check("replacement_task_B_cache_remains_independent",
          records.size() == 1 && records[0].state == "done" &&
              records[0].translation == "独立译文 B");
    accept(taskMeta("replacement-cache-A", {"Revised source A."}));
    accept(taskText("replacement-cache-A", "replace-A-again", "Revised source A."));
    accept(taskText("replacement-cache-A", "replace-A-again", "Third revision source A."));
    settings.target = "zh-TW";
    accept(taskMeta("replacement-cache-A", {"Third revision source A."}));
    check("profile_change_clears_pending_replacement_and_old_translation",
          std::all_of(records.begin(), records.end(), [](const Record &record) {
            return !record.pendingReplacement && record.translation.empty();
          }));
    settings.target = oldTarget;
    clear();
    // Actual Watcher -> Ledger -> ordered Bus replay, with no process/UIA
    // calls.
    paused = true;
    running = true;
    auto replayBus = std::make_shared<Bus>();
    Watcher supplied(0, sourceGeneration, 0, {}, replayBus);
    cclive::Ledger suppliedLedger(0);
    auto drainSupplied = [&] {
      std::deque<json> events;
      {
        std::lock_guard<std::mutex> lock(replayBus->mutex);
        events.swap(replayBus->source);
      }
      for (auto &event : events)
        accept(std::move(event));
    };
    auto largeSnapshot = [&](const std::string &id, const std::string &block,
                             int count) {
      std::string body;
      for (int n = 0; n < count; ++n)
        body += "Large source " + std::to_string(n) + ".\n\n";
      return json{{"context", id},
                  {"task_id", id},
                  {"task_title", "大字幕任务"},
                  {"source_hwnd", 0},
                  {"source_pid", 0},
                  {"identity_quality", "url"},
                  {"blocks", json::array({{{"key", block},
                                           {"kind", "answer"},
                                           {"text", body}}})}};
    };
    // Same-task runtime rebuilds and rolling source windows must keep actual
    // occurrences, rather than appending copies or overwriting old captions.
    auto remount = largeSnapshot("remount-task", "runtime-original", 3);
    supplied.publishSnapshot(remount, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(remount, suppliedLedger); drainSupplied();
    for (std::size_t n = 0; n < records.size(); ++n) complete(n, "译文 " + records[n].source);
    const auto firstRemountKey = records.front().key;
    const auto remountSubmissions = worker->submitted.load();
    remount["blocks"][0]["key"] = "runtime-rebuilt";
    supplied.publishSnapshot(remount, suppliedLedger); drainSupplied();
    check("same_task_runtime_rebuild_does_not_duplicate_completed_rows",
          records.size() == 3 && records.front().key == firstRemountKey &&
          worker->submitted.load() == remountSubmissions);
    remount = largeSnapshot("remount-task", "runtime-rebuilt", 4);
    supplied.publishSnapshot(remount, suppliedLedger); drainSupplied();
    check("rebuilt_runtime_appends_only_new_tail", records.size() == 4 &&
          records.back().source == "Large source 3.");
    complete(records.size() - 1, "译文 " + records.back().source);
    for (int next = 4; next < 105; ++next) {
      std::string body;
      for (int n = next - 3; n <= next; ++n)
        body += "Large source " + std::to_string(n) + ".\n\n";
      remount["blocks"][0]["text"] = body;
      supplied.publishSnapshot(remount, suppliedLedger); drainSupplied();
      complete(records.size() - 1, "译文 " + records.back().source);
    }
    render();
    check("105_rolling_sources_keep_latest_100_once_in_order",
          records.size() == 100 && retainedLines() == 100 &&
          records.front().source == "Large source 5." &&
          records.back().source == "Large source 104." && isAtBottom(translationEdit));
    bool orderedRolling = true;
    for (std::size_t n = 0; n < records.size(); ++n)
      orderedRolling = orderedRolling && records[n].source == "Large source " + std::to_string(n + 5) + ".";
    check("rolling_source_overlap_never_reorders_or_retranslates_retained_rows", orderedRolling);
    auto retainedState = [&] {
      json state = json::array();
      for (const auto &record : records)
        state.push_back({{"key", record.key}, {"version", record.version},
                         {"source", record.source}, {"translation", record.translation},
                         {"state", record.state}, {"pending", record.pendingReplacement}});
      return state;
    };
    const auto beforeFullRestore = retainedState();
    const auto beforeFullSubmissions = worker->submitted.load();
    auto fullSource = largeSnapshot("remount-task", "runtime-expanded", 105);
    supplied.publishSnapshot(fullSource, suppliedLedger); drainSupplied();
    check("full_source_restoration_after_rolling_reuses_all_cached_occurrences",
          records.size() == 100 && retainedState() == beforeFullRestore &&
          worker->submitted.load() == beforeFullSubmissions);
    supplied.publishSnapshot(largeSnapshot("rolling-other", "rolling-other-block", 1), suppliedLedger); drainSupplied();
    supplied.publishSnapshot(largeSnapshot("rolling-other", "rolling-other-block", 1), suppliedLedger); drainSupplied();
    supplied.publishSnapshot(remount, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(remount, suppliedLedger); drainSupplied();
    check("task_switch_restores_rolling_100_line_ram_cache",
          records.size() == 100 && records.front().source == "Large source 5." &&
          records.back().source == "Large source 104.");

    // Use real queued Jobs, without running this isolated worker. Clearing the
    // display must preserve the exact pending jobs and accept their results.
    auto originalWorker = worker;
    worker = std::make_shared<Worker>(bus);
    paused = false;
    auto pendingRebind = largeSnapshot("pending-rebind", "pending-rebind-block", 3);
    supplied.publishSnapshot(pendingRebind, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(pendingRebind, suppliedLedger); drainSupplied();
    std::shared_ptr<Job> rebindActive;
    { std::lock_guard<std::mutex> lock(worker->mutex);
      worker->active = worker->pending.front(); worker->pending.pop_front();
      rebindActive = worker->active; }
    pendingRebind["blocks"][0]["text"] = "Large source 1.\n\nLarge source 2.\n\nLarge source 3.";
    supplied.publishSnapshot(pendingRebind, suppliedLedger); drainSupplied();
    const auto pendingRebindGeneration = generation;
    const auto pendingRebindSubmissions = worker->submitted.load();
    const auto pendingRebindState = retainedState();
    pendingRebind = largeSnapshot("pending-rebind", "pending-rebind-expanded", 4);
    supplied.publishSnapshot(pendingRebind, suppliedLedger); drainSupplied();
    bool rebindJobsKept = worker->active == rebindActive && !rebindActive->cancel->load();
    { std::lock_guard<std::mutex> lock(worker->mutex);
      for (const auto &job : worker->pending) rebindJobsKept = rebindJobsKept && !job->cancel->load(); }
    check("full_source_rebind_keeps_active_and_queued_jobs_without_cancellation",
          rebindJobsKept && generation == pendingRebindGeneration &&
          worker->submitted.load() == pendingRebindSubmissions && retainedState() == pendingRebindState);
    auto liveClear = largeSnapshot("live-clear-task", "live-clear-block", 3);
    supplied.publishSnapshot(liveClear, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(liveClear, suppliedLedger); drainSupplied();
    complete(0, "清除前已完成");
    acceptResult({{"generation", generation}, {"task_id", activeTaskId},
                  {"key", records[1].key}, {"version", records[1].version},
                  {"state", "translating"}, {"translation", "清除前的部分译文"}});
    const auto clearGeneration = generation, clearSourceGeneration = sourceGeneration;
    const auto pendingKey = records[1].key;
    const auto pendingVersion = records[1].version;
    const auto pendingSerial = serial;
    const auto clearSubmitCount = worker->submitted.load();
    std::vector<std::shared_ptr<Job>> queuedBefore;
    std::shared_ptr<Job> activeBefore;
    { std::lock_guard<std::mutex> lock(worker->mutex);
      worker->active = worker->pending.at(1);
      worker->pending.erase(worker->pending.begin() + 1);
      activeBefore = worker->active;
      queuedBefore.assign(worker->pending.begin(), worker->pending.end()); }
    clear(true);
    check("live_clear_immediately_blanks_both_panes_and_keeps_running",
          running && !paused && output(true).empty() && output(false).empty() &&
          textOf(translationEdit).empty() && records.size() == 2 &&
          generation == clearGeneration && sourceGeneration == clearSourceGeneration &&
          records[0].key == pendingKey && records[0].version == pendingVersion && serial == pendingSerial);
    bool jobsPreserved = worker->submitted.load() == clearSubmitCount;
    { std::lock_guard<std::mutex> lock(worker->mutex);
      jobsPreserved = jobsPreserved && worker->pending.size() == queuedBefore.size() &&
          worker->active == activeBefore && activeBefore && !activeBefore->cancel->load();
      for (const auto &job : queuedBefore) jobsPreserved = jobsPreserved && !job->cancel->load(); }
    check("live_clear_keeps_inflight_and_queued_jobs_without_resubmit", jobsPreserved);
    complete(0, "清除后继续完成的译文");
    complete(1, "排队内容继续完成");
    check("pending_results_continue_after_clear_without_old_partial_text",
          records[0].visible && records[1].visible &&
          output(false).find(L"清除后继续完成的译文") != std::wstring::npos &&
          output(false).find(L"清除前") == std::wstring::npos);
    liveClear = largeSnapshot("live-clear-task", "live-clear-block", 4);
    supplied.publishSnapshot(liveClear, suppliedLedger); drainSupplied();
    check("live_clear_accepts_new_source_with_unique_record_key",
          records.size() == 3 && records.back().source == "Large source 3." &&
          records.back().key != pendingKey && serial > pendingSerial);
    complete(records.size() - 1, "新的末尾译文");
    clear(true);
    check("empty_cleared_task_keeps_only_ram_dismissal_boundary",
          records.empty() && taskCache->get("live-clear-task").has_value() &&
          taskCache->get("live-clear-task")->at("records").empty());
    auto clearOther = largeSnapshot("live-clear-other", "live-clear-other-block", 1);
    supplied.publishSnapshot(clearOther, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(clearOther, suppliedLedger); drainSupplied();
    complete(0, "另一个窗口的缓存");
    liveClear["blocks"][0]["key"] = "live-clear-rebuilt";
    supplied.publishSnapshot(liveClear, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(liveClear, suppliedLedger); drainSupplied();
    check("cleared_task_does_not_refill_on_A_B_A_or_runtime_rebuild", records.empty());
    liveClear = largeSnapshot("live-clear-task", "live-clear-rebuilt", 5);
    supplied.publishSnapshot(liveClear, suppliedLedger); drainSupplied();
    check("new_content_after_cleared_task_restore_still_translates",
          records.size() == 1 && records.back().source == "Large source 4." && records.back().state == "queued");
    complete(0, "切换回来后的新内容");
    supplied.publishSnapshot(clearOther, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(clearOther, suppliedLedger); drainSupplied();
    check("clear_preserves_other_window_ram_translation",
          records.size() == 1 && records[0].translation == "另一个窗口的缓存");
    auto duplicateClear = largeSnapshot("duplicate-clear-task", "duplicate-clear-block", 1);
    duplicateClear["blocks"][0]["text"] = "Repeated source.\n\nRepeated source.";
    supplied.publishSnapshot(duplicateClear, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(duplicateClear, suppliedLedger); drainSupplied();
    complete(0, "已完成的首次出现");
    clear(true);
    const auto retainedDuplicateKey = records.front().key;
    supplied.publishSnapshot(clearOther, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(clearOther, suppliedLedger); drainSupplied();
    duplicateClear["blocks"][0]["key"] = "duplicate-clear-rebuilt";
    supplied.publishSnapshot(duplicateClear, suppliedLedger); drainSupplied();
    supplied.publishSnapshot(duplicateClear, suppliedLedger); drainSupplied();
    check("cleared_identical_occurrence_cannot_steal_pending_occurrence_on_restore",
          records.size() == 1 && records.front().key == retainedDuplicateKey &&
          !records.front().visible && records.front().state == "queued");
    duplicateClear["blocks"][0]["text"] = "Repeated source.\n\nRepeated source.\n\nRepeated source.";
    supplied.publishSnapshot(duplicateClear, suppliedLedger); drainSupplied();
    check("new_identical_occurrence_after_clear_remains_independent",
          records.size() == 2 && records.front().key == retainedDuplicateKey &&
          records.back().key != retainedDuplicateKey);
    accept(taskMeta("clear-paused-task", {"Pending paused source."}));
    paused = true;
    accept(taskText("clear-paused-task", "clear-paused-runtime", "Pending paused source."));
    clear(true);
    check("clear_while_paused_preserves_hidden_pending_work",
          running && paused && records.size() == 1 && !records[0].visible && records[0].state == "paused");
    pause();
    check("resume_after_clear_requeues_pending_work", !paused && records[0].state == "queued");
    cancelAll();
    running = false;
    records[0].state = "stopped";
    clear(true);
    check("clear_while_stopped_does_not_start_translation", !running && records.empty());
    running = true;
    clearedSignatures = {"A", "B", "C"};
    clearedBlocks = {"boundary"};
    json boundary = {{"source_block_keys", json::array({"boundary"})},
        {"source_signatures", json::array({"B", "C", "D", "A"})}};
    updateClearBoundary(boundary);
    check("clear_boundary_matches_overlap_without_hiding_new_repeated_tail",
          clearedSourceIndices == std::set<std::size_t>({0, 1}));
    clearedSignatures = {"A", "A"};
    boundary["source_signatures"] = json::array({"A", "A", "A"});
    updateClearBoundary(boundary);
    check("clear_boundary_preserves_occurrence_count_for_identical_sentences",
          clearedSourceIndices == std::set<std::size_t>({0, 1}));
    clearedSignatures = {"A"};
    boundary["source_block_keys"] = json::array({"new-message"});
    boundary["source_signatures"] = json::array({"A", "fresh"});
    updateClearBoundary(boundary, true);
    check("clear_boundary_does_not_dismiss_new_message_from_one_generic_match",
          clearedSourceIndices.empty() && !clearedBlocks.count("new-message"));
    cancelAll();
    worker = originalWorker;
    paused = true;
    auto largeA = largeSnapshot("large-task-A", "large-block-A", 150);
    supplied.publishSnapshot(largeA, suppliedLedger);
    drainSupplied();
    check("first_task_identity_snapshot_does_not_publish_body",
          activeTaskId == "large-task-A" && records.empty());
    supplied.publishSnapshot(largeA, suppliedLedger);
    drainSupplied();
    check("initial_150_sources_keep_only_latest_100",
          records.size() == 100 &&
              records.front().source == "Large source 50." &&
              records.back().source == "Large source 149.");
    for (std::size_t index = 0; index < records.size(); ++index)
      complete(index, "已译 " + records[index].source);
    auto largeB = largeSnapshot("large-task-B", "large-block-B", 1);
    supplied.publishSnapshot(largeB, suppliedLedger);
    drainSupplied();
    supplied.publishSnapshot(largeB, suppliedLedger);
    drainSupplied();
    largeA["blocks"][0]["key"] = "rebuilt-large-block-A";
    auto beforeLargeReplay = worker->submitted.load();
    supplied.publishSnapshot(largeA, suppliedLedger);
    drainSupplied();
    supplied.publishSnapshot(largeA, suppliedLedger);
    drainSupplied();
    check("150_source_task_revisit_preserves_cached_latest_100",
          records.size() == 100 &&
              records.front().source == "Large source 50." &&
              records.back().source == "Large source 149." &&
              std::all_of(
                  records.begin(), records.end(),
                  [](const Record &record) { return record.state == "done"; }));
    check("discarded_replay_prefix_does_not_requeue_translations",
          worker->submitted.load() == beforeLargeReplay);
    auto nextA = largeSnapshot("large-task-A", "rebuilt-large-block-A", 151);
    supplied.publishSnapshot(nextA, suppliedLedger);
    drainSupplied();
    check("new_151st_source_keeps_latest_52_through_151",
          records.size() == 100 &&
              records.front().source == "Large source 51." &&
              records.back().source == "Large source 150.");
    auto hugeA = largeSnapshot("huge-task-A", "huge-block-A", 700);
    supplied.publishSnapshot(hugeA, suppliedLedger);
    drainSupplied();
    supplied.publishSnapshot(hugeA, suppliedLedger);
    {
      std::lock_guard<std::mutex> lock(replayBus->mutex);
      auto hasTask = std::any_of(
          replayBus->source.begin(), replayBus->source.end(),
          [](const json &event) { return event.value("type", "") == "task"; });
      auto textCount = std::count_if(
          replayBus->source.begin(), replayBus->source.end(),
          [](const json &event) { return event.value("type", "") == "text"; });
      check("700_source_prefix_filtered_before_bounded_bus",
            hasTask && textCount == 100 && replayBus->source.size() <= 102);
    }
    drainSupplied();
    check("700_source_first_body_keeps_task_and_latest_100",
          activeTaskId == "huge-task-A" && records.size() == 100 &&
              records.front().source == "Large source 600." &&
              records.back().source == "Large source 699.");
    for (std::size_t index = 0; index < records.size(); ++index)
      complete(index, "已译 " + records[index].source);
    supplied.publishSnapshot(largeB, suppliedLedger);
    drainSupplied();
    supplied.publishSnapshot(largeB, suppliedLedger);
    drainSupplied();
    hugeA["blocks"][0]["key"] = "rebuilt-huge-block-A";
    auto beforeHugeReplay = worker->submitted.load();
    supplied.publishSnapshot(hugeA, suppliedLedger);
    drainSupplied();
    supplied.publishSnapshot(hugeA, suppliedLedger);
    drainSupplied();
    check("700_source_task_revisit_restores_without_duplicates",
          activeTaskId == "huge-task-A" && records.size() == 100 &&
              records.front().source == "Large source 600." &&
              records.back().source == "Large source 699." &&
              worker->submitted.load() == beforeHugeReplay &&
              std::all_of(
                  records.begin(), records.end(),
                  [](const Record &record) { return record.state == "done"; }));
    // Older UIA prefixes and empty snapshots are partial views of this same
    // block, not deletions of its accepted latest 100 captions.
    const auto hugeGeneration = generation;
    const auto hugeVersion = records.front().version;
    const auto hugeKey = records.front().key;
    const auto hugeAliases = sourceAliases.size();
    const auto hugeIndices = supplied.sourceIndices;
    const auto hugeSlots = supplied.forwardedSlots;
    auto shortA = largeSnapshot("huge-task-A", "rebuilt-huge-block-A", 50);
    supplied.publishSnapshot(shortA, suppliedLedger);
    {
      std::lock_guard<std::mutex> lock(replayBus->mutex);
      const auto texts = std::count_if(
          replayBus->source.begin(), replayBus->source.end(),
          [](const json &event) { return event.value("type", "") == "text"; });
      check("700_to_50_old_prefix_emits_no_text_deletions",
            texts == 0 && replayBus->source.size() == 1);
    }
    drainSupplied();
    check("700_to_50_old_prefix_preserves_latest_100_translations",
          activeTaskId == "huge-task-A" && records.size() == 100 &&
              records.front().source == "Large source 600." &&
              records.back().source == "Large source 699." &&
              records.front().translation == "已译 Large source 600." &&
              records.back().translation == "已译 Large source 699." &&
              generation == hugeGeneration && records.front().version == hugeVersion &&
              records.front().key == hugeKey && sourceAliases.size() == hugeAliases);
    auto emptyA = hugeA;
    emptyA["blocks"] = json::array();
    supplied.publishSnapshot(emptyA, suppliedLedger);
    check("empty_snapshot_preserves_accepted_ledger_indices_and_slots",
          replayBus->source.empty() && supplied.sourceIndices == hugeIndices &&
              supplied.forwardedSlots == hugeSlots);
    drainSupplied();
    emptyA = hugeA;
    emptyA["blocks"][0]["text"] = "";
    supplied.publishSnapshot(emptyA, suppliedLedger);
    drainSupplied();
    check("empty_same_block_body_keeps_latest_verified_translation",
          records.size() == 100 && records.front().key == hugeKey &&
              records.back().translation == "已译 Large source 699.");
    auto pendingIdentity = hugeA;
    pendingIdentity["identity_pending"] = true;
    pendingIdentity["blocks"][0]["text"] = "Unverified navigation body.";
    supplied.publishSnapshot(pendingIdentity, suppliedLedger);
    check("identity_pending_same_task_does_not_update_source_or_aliases",
          replayBus->source.empty() && supplied.sourceIdentityPending &&
              supplied.sourceIndices == hugeIndices &&
              supplied.forwardedSlots == hugeSlots && sourceAliases.size() == hugeAliases);
    auto fallbackIdentity = pendingIdentity;
    fallbackIdentity["identity_pending"] = false;
    fallbackIdentity["identity_quality"] = "view_fallback";
    fallbackIdentity["task_id"] = "unverified-restarted-reader";
    supplied.publishSnapshot(fallbackIdentity, suppliedLedger);
    check("reader_restart_url_to_fallback_pauses_unknown_body",
          replayBus->source.empty() && supplied.currentTaskId == "huge-task-A" &&
              supplied.sourceIdentityPending && records.front().key == hugeKey);
    supplied.consistentTaskSnapshots = 0;
    supplied.publishSnapshot(hugeA, suppliedLedger);
    check("same_task_reader_reconnect_first_snapshot_keeps_aliases",
          replayBus->source.empty() && sourceAliases.size() == hugeAliases);
    supplied.publishSnapshot(hugeA, suppliedLedger);
    drainSupplied();
    check("full_snapshot_after_partial_restore_adds_no_duplicate_records",
          records.size() == 100 && records.front().key == hugeKey &&
              records.front().translation == "已译 Large source 600." &&
              worker->submitted.load() == beforeHugeReplay &&
              !supplied.sourceIdentityPending);
    auto nextHugeA = largeSnapshot("huge-task-A", "rebuilt-huge-block-A", 701);
    supplied.publishSnapshot(nextHugeA, suppliedLedger);
    {
      std::lock_guard<std::mutex> lock(replayBus->mutex);
      const auto texts = std::count_if(
          replayBus->source.begin(), replayBus->source.end(),
          [](const json &event) { return event.value("type", "") == "text"; });
      check("701st_source_after_partial_snapshot_emits_only_one_increment",
            texts == 1 && replayBus->source.size() == 2);
    }
    drainSupplied();
    check("701st_source_keeps_latest_100_without_retranslating_old_99",
          records.size() == 100 && retainedLines() == 100 &&
              records.front().source == "Large source 601." &&
              records.front().translation == "已译 Large source 601." &&
              records.back().source == "Large source 700." &&
              records.back().translation.empty() &&
              worker->submitted.load() == beforeHugeReplay);
    auto correction = largeSnapshot("real-correction-task", "correction-block", 2);
    supplied.publishSnapshot(correction, suppliedLedger);
    drainSupplied();
    supplied.publishSnapshot(correction, suppliedLedger);
    drainSupplied();
    complete(0, "原来的首句译文");
    complete(1, "原来的末句译文");
    correction["blocks"][0]["text"] = "Corrected first sentence.\n\nLarge source 1.\n\n";
    supplied.publishSnapshot(correction, suppliedLedger);
    drainSupplied();
    check("real_nonprefix_source_correction_keeps_old_translation_pending",
          records.size() == 2 && records[0].source == "Corrected first sentence." &&
              records[0].pendingReplacement &&
              records[0].translation == "原来的首句译文" &&
              records[1].translation == "原来的末句译文");
    complete(0, "纠正后的首句译文");
    check("real_nonprefix_correction_commits_current_complete_result",
          records[0].state == "done" && !records[0].pendingReplacement &&
              records[0].translation == "纠正后的首句译文");
    correction["blocks"][0]["text"] = "Shortened actual replacement.\n\n";
    supplied.publishSnapshot(correction, suppliedLedger);
    drainSupplied();
    check("real_nonprefix_shrink_updates_existing_slot_without_bulk_replay",
          records.size() == 1 &&
              records[0].source == "Shortened actual replacement." &&
              records[0].pendingReplacement &&
              records[0].translation == "纠正后的首句译文");
    complete(0, "缩短纠正后的完整译文");
    check("real_nonprefix_shrink_commits_new_version_and_removes_true_tail",
          records.size() == 1 && records[0].state == "done" &&
              !records[0].pendingReplacement &&
              records[0].translation == "缩短纠正后的完整译文");
    auto multiBlock = largeSnapshot("multiple-block-task", "thinking-block", 1);
    multiBlock["blocks"][0]["kind"] = "thinking";
    multiBlock["blocks"][0]["text"] = "Thinking source sentence.\n\n";
    multiBlock["blocks"].push_back({{"key", "answer-block"}, {"kind", "answer"},
                                      {"text", "Answer source sentence.\n\n"}});
    supplied.publishSnapshot(multiBlock, suppliedLedger);
    drainSupplied();
    supplied.publishSnapshot(multiBlock, suppliedLedger);
    drainSupplied();
    complete(0, "已确认思考译文");
    complete(1, "已确认回答译文");
    auto onlyAnswer = multiBlock;
    onlyAnswer["blocks"].erase(0);
    supplied.publishSnapshot(onlyAnswer, suppliedLedger);
    {
      std::lock_guard<std::mutex> lock(replayBus->mutex);
      check("missing_thinking_block_emits_no_translation_deletions",
            std::none_of(replayBus->source.begin(), replayBus->source.end(),
                         [](const json &event) {
                           return event.value("type", "") == "text";
                         }));
    }
    drainSupplied();
    check("thinking_and_answer_to_answer_keeps_both_verified_translations",
          records.size() == 2 && records[0].translation == "已确认思考译文" &&
              records[1].translation == "已确认回答译文");
    supplied.publishSnapshot(multiBlock, suppliedLedger);
    drainSupplied();
    check("restored_thinking_and_answer_blocks_do_not_duplicate_translations",
          records.size() == 2 && retainedLines() == 2 &&
              records[0].translation == "已确认思考译文" &&
              records[1].translation == "已确认回答译文" &&
              supplied.acceptedBodies.size() == 2);
    auto pendingNewTask = largeB;
    pendingNewTask["identity_pending"] = true;
    supplied.publishSnapshot(pendingNewTask, suppliedLedger);
    {
      std::lock_guard<std::mutex> lock(replayBus->mutex);
      check("pending_new_explicit_URL_emits_identity_only",
            replayBus->source.size() == 1 &&
                replayBus->source.front().value("type", "") == "task" &&
                !replayBus->source.front().contains("source_block_keys"));
    }
    drainSupplied();
    check("pending_new_URL_switches_to_its_independent_cached_task",
          activeTaskId == "large-task-B" && records.size() == 1 &&
              records[0].source == "Large source 0." &&
              !records[0].pendingReplacement);
    // An empty verified view suspends an unsettled fragment, without deleting
    // a completed caption or resetting a cropped-window Ledger context.
    for (int emptyMode = 0; emptyMode < 2; ++emptyMode) {
      auto pendingBus = std::make_shared<Bus>();
      Watcher pendingSource(0, sourceGeneration, 60000, {}, pendingBus);
      cclive::Ledger pendingLedger(60000);
      const auto pendingTask = "empty-pending-task-" + std::to_string(emptyMode);
      pendingSource.currentTaskId = pendingTask;
      pendingSource.windowReplayNonce = 7;
      auto pendingSnapshot = largeSnapshot(pendingTask, "pending-block", 1);
      pendingSource.currentTask = {
          {"task_id", pendingTask},
          {"task_title", pendingSnapshot.at("task_title")},
          {"identity_quality", pendingSnapshot.at("identity_quality")},
          {"source_hwnd", pendingSnapshot.at("source_hwnd")},
          {"source_pid", pendingSnapshot.at("source_pid")}};
      pendingSnapshot["blocks"][0]["text"] = "Stable completed source.\n\n";
      auto drainPending = [&] {
        std::deque<json> events;
        {
          std::lock_guard<std::mutex> lock(pendingBus->mutex);
          events.swap(pendingBus->source);
        }
        for (auto &event : events)
          accept(std::move(event));
      };
      auto futureTick = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now().time_since_epoch()).count() + 60001;
      };
      pendingSource.publishSnapshot(pendingSnapshot, pendingLedger);
      drainPending();
      pendingSource.publishSnapshot(pendingSnapshot, pendingLedger);
      drainPending();
      for (auto event : pendingLedger.flush_at(futureTick()))
        pendingSource.publishLedger(std::move(event));
      drainPending();
      const auto mode = std::to_string(emptyMode);
      check(("empty_view_completed_setup_" + mode).c_str(),
            records.size() == 1 && records.front().source == "Stable completed source.");
      if (records.size() != 1)
        continue;
      complete(0, "已完成且应保留的字幕");
      const auto completedKey = records.front().key;
      const auto completedVersion = records.front().version;
      pendingSnapshot["blocks"][0]["text"] =
          "Stable completed source.\n\nOutdated pending fragment";
      pendingSource.publishSnapshot(pendingSnapshot, pendingLedger);
      drainPending();
      const auto pendingIndices = pendingSource.sourceIndices;
      const auto pendingSlots = pendingSource.forwardedSlots;
      const auto pendingBodies = pendingSource.acceptedBodies;
      const auto pendingAliasCount = sourceAliases.size();
      auto unavailable = pendingSnapshot;
      if (emptyMode == 0)
        unavailable["blocks"] = json::array();
      else
        unavailable["blocks"][0]["text"] = "";
      pendingSource.publishSnapshot(unavailable, pendingLedger);
      const auto unavailableEvents = pendingLedger.flush_at(futureTick());
      for (auto event : unavailableEvents)
        pendingSource.publishLedger(std::move(event));
      check(("empty_view_does_not_flush_old_pending_fragment_" + mode).c_str(),
            unavailableEvents.empty() && pendingBus->source.empty() &&
                pendingSource.sourceIndices == pendingIndices &&
                pendingSource.forwardedSlots == pendingSlots &&
                pendingSource.acceptedBodies == pendingBodies &&
                sourceAliases.size() == pendingAliasCount);
      drainPending();
      check(("empty_view_keeps_completed_caption_and_version_" + mode).c_str(),
            records.size() == 1 && records.front().key == completedKey &&
                records.front().version == completedVersion &&
                records.front().state == "done" &&
                records.front().translation == "已完成且应保留的字幕");
      pendingSnapshot["blocks"][0]["text"] =
          "Stable completed source.\n\nCurrent live fragment";
      pendingSource.publishSnapshot(pendingSnapshot, pendingLedger);
      drainPending();
      check(("restored_view_waits_for_current_fragment_to_settle_" + mode).c_str(),
            pendingLedger.flush().empty() && records.size() == 1 &&
                records.front().key == completedKey);
      auto restoredEvents = pendingLedger.flush_at(futureTick());
      for (auto event : restoredEvents)
        pendingSource.publishLedger(std::move(event));
      drainPending();
      check(("restored_view_emits_only_current_fragment_" + mode).c_str(),
            restoredEvents.size() == 1 &&
                restoredEvents.front().value("text", "") == "Current live fragment" &&
                records.size() == 2 && records.front().key == completedKey &&
                records.front().translation == "已完成且应保留的字幕" &&
                records.back().source == "Current live fragment");
    }
    accept(taskMeta("pending-task-C", {"Pending cached source."}));
    accept(taskText("pending-task-C", "pending-runtime",
                    "Pending cached source."));
    accept(taskMeta("pending-task-D", {}));
    paused = false;
    auto beforeResume = worker->submitted.load();
    accept(taskMeta("pending-task-C", {"Pending cached source."}));
    check("unfinished_cached_task_resumes_when_revisited",
          records.size() == 1 && records[0].state == "queued" &&
              worker->submitted.load() == beforeResume + 1);
    cancelAll();
    paused = true;
    Bus order;
    order.emit({{"type", "task"}});
    order.emit({{"type", "reset"}});
    order.emit({{"type", "text"}});
    check("task_reset_text_share_source_FIFO",
          order.source.size() == 3 && order.source[0]["type"] == "task" &&
              order.source[1]["type"] == "reset" &&
              order.source[2]["type"] == "text" && !order.reset.has_value());
    return checks;
  }
  void tick() {
    if (closed)
      return;
    std::deque<json> items;
    std::optional<json> reset, telemetry;
    std::map<std::string, json> results;
    {
      std::lock_guard<std::mutex> lock(bus->mutex);
      reset.swap(bus->reset);
      telemetry.swap(bus->telemetry);
      items.swap(bus->misc);
      while (!bus->source.empty()) {
        items.push_back(std::move(bus->source.front()));
        bus->source.pop_front();
      }
      results.swap(bus->results);
    }
    if (telemetry)
      acceptTelemetry(*telemetry);
    if (reset)
      accept(std::move(*reset));
    for (auto &v : items)
      accept(std::move(v));
    for (const auto &result : Bus::orderedResults(std::move(results)))
      acceptResult(result);
    if (demo || selfTest)
      synthetic();
    if (cacheDirty && Clock::now() - lastCacheSave > std::chrono::seconds(1))
      saveActiveTask();
    if (dirty && Clock::now() - lastRender > std::chrono::milliseconds(120))
      render();
    publishDiagnostic();
    if (selfTest) {
      size_t done =
          std::count_if(records.begin(), records.end(), [](const Record &r) {
            return r.state == "done" && !r.translation.empty();
          });
      bool timeout = Clock::now() - testStart > std::chrono::seconds(10);
      if ((done >= 3 && demoStep >= 17) || timeout) {
        json uiChecks = done >= 3 ? checkUiRegressions() : json::array();
        bool uiPassed = !uiChecks.empty() &&
                        std::all_of(uiChecks.begin(), uiChecks.end(),
                                    [](const json &check) {
                                      return check.value("passed", false);
                                    });
        json report = {{"native_cpp", true},
                       {"ui", sourceEdit && translationEdit},
                       {"offline_translations", done},
                       {"claude_read", false},
                       {"network_used", false},
                       {"ui_checks", uiChecks},
                       {"passed", done >= 3 && uiPassed}};
        try {
          writeFile(reportPath, report.dump(2));
        } catch (...) {
        }
        DestroyWindow(hwnd);
      }
    }
  }
  void shutdown() {
    if (closed)
      return;
    if (config)
      cancelConfigAuthorization(*config);
    stop();
    closed = true;
    if (hwnd)
      KillTimer(hwnd, 1);
    worker->stop();
    publishDiagnostic(true);
  }
  void openConfig();
  void closeConfig(bool restoreActivation = true);
  static LRESULT CALLBACK windowProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    App *a = (App *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
      a = (App *)((CREATESTRUCTW *)lp)->lpCreateParams;
      SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)a);
      a->hwnd = w;
    }
    if (!a)
      return DefWindowProcW(w, m, wp, lp);
    switch (m) {
    case WM_CREATE:
      try {
        a->build(w);
      } catch (...) {
        return -1;
      }
      return 0;
    case WM_SIZE:
      a->layout();
      return 0;
    case WM_TIMER:
      a->tick();
      return 0;
    case WM_GETMINMAXINFO: {
      auto *p = (MINMAXINFO *)lp;
      p->ptMinTrackSize = {a->s(720), a->s(640)};
      return 0;
    }
    case WM_DPICHANGED: {
      a->dpi = HIWORD(wp);
      auto *r = (RECT *)lp;
      SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      a->refreshAppearance();
      return 0;
    }
    case WM_DRAWITEM: {
      auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lp);
      if (item && item->CtlType == ODT_BUTTON) {
        paintButton(item->hwndItem, item->hDC, item->rcItem, item->itemState);
        return TRUE;
      }
      break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      SetBkColor(reinterpret_cast<HDC>(wp), PANEL);
      SetTextColor(reinterpret_cast<HDC>(wp), FG);
      return reinterpret_cast<LRESULT>(a->panel);
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      HDC dc = (HDC)wp;
      SetBkColor(dc, BG);
      SetTextColor(dc,
                   (HWND)lp == a->title ? HEADING
                   : (HWND)lp == a->eyebrow
                       ? ACCENT
                       : MUTED);
      return (LRESULT)a->background;
    }
    case WM_ERASEBKGND: {
      RECT r{};
      GetClientRect(w, &r);
      FillRect((HDC)wp, &r, a->background);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const HDC dc = BeginPaint(w, &paint);
      RECT area{};
      GetClientRect(w, &area);
      FillRect(dc, &area, a->background);
      a->paintDividers(dc);
      EndPaint(w, &paint);
      return 0;
    }
    case DM_GETDEFID:
      return MAKELRESULT(IDC_START, DC_HASDEFID);
    case WM_COMMAND:
      if (HIWORD(wp) == BN_CLICKED)
        switch (LOWORD(wp)) {
        case IDC_REFRESH:
          a->refresh();
          break;
        case IDC_START:
          if (a->running)
            a->stop();
          else
            a->start();
          break;
        case IDC_PAUSE:
          a->pause();
          break;
        case IDC_CLEAR:
          a->clear(true);
          break;
        case IDC_LATEST:
          a->goLatest();
          break;
        case IDC_RETRY:
          a->retry();
          break;
        case IDC_SETTINGS:
          a->openConfig();
          break;

        case IDC_ORIGINAL:
          a->settings.show_original =
              SendMessageW(a->originalCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
          a->saveOptions();
          a->layout();
          a->dirty = true;
          break;
        case IDC_TOP:
          a->settings.always_on_top =
              SendMessageW(a->topCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
          a->saveOptions();
          SetWindowPos(
              w, a->settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
              0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          break;
        }
      return 0;
    case WM_CLOSE:
      a->closeConfig(false);
      DestroyWindow(w);
      return 0;
    case WM_DESTROY:
      a->shutdown();
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
  }
};
LRESULT CALLBACK configProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  auto *c = (Config *)GetWindowLongPtrW(w, GWLP_USERDATA);
  if (msg == WM_NCCREATE) {
    c = (Config *)((CREATESTRUCTW *)lp)->lpCreateParams;
    c->hwnd = w;
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)c);
  }
  if (!c)
    return DefWindowProcW(w, msg, wp, lp);
  App &a = *c->app;
  if (msg == WM_CREATE) {
    darkCaption(w);
    SendMessageW(w, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(applicationIcon(a.s(32))));
    SendMessageW(w, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(applicationIcon(a.s(16))));
    auto make = [&](const wchar_t *klass, const wchar_t *text, DWORD style,
                    int x, int y, int width, int h, int id) {
      bool button = wcscmp(klass, L"BUTTON") == 0;
      bool primary = button && (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
      if (button)
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
      auto child =
          CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, a.s(x),
                          a.s(y), a.s(width), a.s(h), w, (HMENU)(INT_PTR)id,
                          GetModuleHandleW(nullptr), nullptr);
      SendMessageW(child, WM_SETFONT, (WPARAM)a.smallFont, TRUE);
      if (button)
        themeButton(child, primary);
      else if (wcscmp(klass, L"EDIT") == 0 || wcscmp(klass, L"COMBOBOX") == 0)
        nativeDarkControl(child, L"DarkMode_CFD");
      return child;
    };
    const wchar_t *labels[] = {L"翻译服务",
                               L"API 基础地址",
                               L"模型",
                               L"Key 环境变量名",
                               L"目标语言",
                               L"稳定等待（毫秒）",
                               L"正文字号 (pt，8–40)",
                               L"本次使用的 Key"};
    std::vector<std::wstring> values = {wide(a.settings.provider),
                                        wide(a.settings.base_url),
                                        wide(a.settings.model),
                                        wide(a.settings.api_key_env),
                                        wide(a.settings.target),
                                        std::to_wstring(a.settings.settle_ms),
                                        std::to_wstring(a.settings.font_size),
                                        L""};
    for (int i = 0; i < 8; ++i) {
      c->labels.push_back(
          make(L"STATIC", labels[i], 0, 24, 24 + i * 37, 158, 25, 0));
      HWND edit = make(i ? L"EDIT" : L"COMBOBOX", i ? values[i].c_str() : L"",
                       i ? (WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP |
                            (i == 7 ? ES_PASSWORD : 0))
                         : (CBS_DROPDOWNLIST | WS_TABSTOP),
                       190, 20 + i * 37, 382, i ? 28 : 220, 200 + i);
      c->edits.push_back(edit);
      if (!i) {
        for (const auto &provider : configProviders)
          SendMessageW(edit, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(provider.label));
        SendMessageW(edit, CB_SETCURSEL,
                     configProviderIndex(a.settings.provider), 0);
      }
    }
    const wchar_t *baiduLabels[] = {L"百度 APPID", L"百度密钥环境变量",
                                    L"本次百度密钥"};
    const std::wstring baiduValues[] = {wide(a.settings.baidu_app_id),
                                        wide(a.settings.baidu_key_env), L""};
    for (int i = 0; i < 3; ++i) {
      const int row = i + 1;
      c->labels.push_back(make(L"STATIC", baiduLabels[i], 0, 24,
                               24 + row * 37, 158, 25, 0));
      c->edits.push_back(make(L"EDIT", baiduValues[i].c_str(),
                              WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP |
                                  (i == 2 ? ES_PASSWORD : 0),
                              190, 20 + row * 37, 382, 28, 208 + i));
    }
    c->keyHint = make(L"STATIC", L"", 0, 24, 326, 552, 42, 0);
    c->serviceHint = make(L"STATIC", L"", 0, 24, 370, 552, 74, 0);
    c->accountPath = a.selfTest ? fs::path{} : cclive::chatgpt::default_account_path();
    c->selectedChatgptModel = a.settings.chatgpt_model;
    c->chatgptAccountLabel = make(L"STATIC", L"ChatGPT 账号", 0,
                                 24, 61, 158, 25, 0);
    c->chatgptAccounts = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                              190, 57, 278, 220, 220);
    c->chatgptAdd = make(L"BUTTON", L"添加账号", BS_PUSHBUTTON | WS_TABSTOP,
                         478, 57, 94, 28, 221);
    c->chatgptModelLabel = make(L"STATIC", L"可用模型", 0,
                               24, 98, 158, 25, 0);
    c->chatgptModels = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                            190, 94, 382, 220, 211);
    c->edits.push_back(c->chatgptModels); // edits[11]: server-provided models.
    c->labels.push_back(c->chatgptModelLabel);
    c->chatgptStatusLabel = make(L"STATIC", L"授权状态", 0,
                                24, 135, 158, 25, 0);
    c->chatgptStatus = make(L"STATIC", L"", 0, 190, 131, 382, 28, 0);
    c->chatgptLogin = make(L"BUTTON", L"Continue with ChatGPT",
                           BS_PUSHBUTTON | WS_TABSTOP, 190, 279, 190, 28, 222);
    c->chatgptSignout = make(L"BUTTON", L"退出账号", BS_PUSHBUTTON | WS_TABSTOP,
                             388, 279, 88, 28, 223);
    c->chatgptRefresh = make(L"BUTTON", L"刷新模型", BS_PUSHBUTTON | WS_TABSTOP,
                             484, 279, 88, 28, 224);
    const wchar_t *codexLabels[] = {L"CLI 路径（空自动检测）", L"Codex 模型（可空）"};
    const std::wstring codexValues[] = {wide(a.settings.codex_path),
                                       wide(a.settings.codex_model)};
    for (int i = 0; i < 2; ++i) {
      const int row = i + 1;
      c->labels.push_back(make(L"STATIC", codexLabels[i], 0, 24,
                              24 + row * 37, 158, 25, 0));
      c->edits.push_back(make(L"EDIT", codexValues[i].c_str(),
                             WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                             190, 20 + row * 37, 382, 28, 212 + i));
    }
    updateConfigProviderFields(*c);
    c->error = make(L"STATIC", L"", 0, 24, 450, 552, 48, 0);
    make(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 380, 510, 88, 32,
         IDCANCEL);
    make(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 484, 510, 88, 32,
         IDOK);
    if (std::string(selectedConfigProvider(*c)) == "chatgpt")
      a.chatgptSettingsAction("status");
    return 0;
  }
  if (msg == WM_COMMAND && LOWORD(wp) == 200 &&
      HIWORD(wp) == CBN_SELCHANGE) {
    if (std::string(selectedConfigProvider(*c)) == "chatgpt")
      a.chatgptSettingsAction("status");
    else
      cancelConfigAuthorization(*c);
    updateConfigProviderFields(*c);
    SetWindowTextW(c->error, L"");
    return 0;
  }
  if (msg == WM_COMMAND && LOWORD(wp) == 220 &&
      HIWORD(wp) == CBN_SELCHANGE) {
    const auto index = SendMessageW(c->chatgptAccounts, CB_GETCURSEL, 0, 0);
    if (index >= 0 && static_cast<std::size_t>(index) < c->accountList.size())
      a.chatgptSettingsAction("switch", c->accountList[index].client_id);
    return 0;
  }
  if (msg == WM_COMMAND && LOWORD(wp) == 211 &&
      HIWORD(wp) == CBN_SELCHANGE) {
    const auto index = SendMessageW(c->chatgptModels, CB_GETCURSEL, 0, 0);
    c->selectedChatgptModel = index >= 0 &&
            static_cast<std::size_t>(index) < c->catalog.size()
        ? c->catalog[index].id : "";
    return 0;
  }
  if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
    if (LOWORD(wp) >= 221 && LOWORD(wp) <= 224) {
      const char *actions[] = {"add", "signin", "signout", "models"};
      a.chatgptSettingsAction(actions[LOWORD(wp) - 221]);
      return 0;
    }
    if (LOWORD(wp) == IDCANCEL) {
      a.closeConfig();
      return 0;
    }
    if (LOWORD(wp) == IDOK) {
      try {
        auto updated = a.settings;
        updated.provider = selectedConfigProvider(*c);
        if (updated.provider == "chatgpt") {
          if (c->authBusy || !c->authLoaded)
            throw std::runtime_error("请等待账号与模型列表加载完成。");
          if (!c->account.signed_in)
            throw std::runtime_error("请先完成 ChatGPT 账号授权。");
          if (!c->account.plan_enabled)
            throw std::runtime_error("该账号尚未开放本服务所需权限。");
          const auto index = SendMessageW(c->chatgptModels, CB_GETCURSEL, 0, 0);
          if (index < 0 || static_cast<std::size_t>(index) >= c->catalog.size())
            throw std::runtime_error("请从账号返回的可用模型中选择一项。");
          updated.chatgpt_model = c->catalog[index].id;
        }
        updated.base_url = utf8(textOf(c->edits[1]));
        updated.model = utf8(textOf(c->edits[2]));
        updated.api_key_env = utf8(textOf(c->edits[3]));
        updated.baidu_app_id = utf8(textOf(c->edits[8]));
        updated.baidu_key_env = utf8(textOf(c->edits[9]));
        updated.codex_path = utf8(textOf(c->edits[12]));
        updated.codex_model = utf8(textOf(c->edits[13]));
        updated.target = utf8(textOf(c->edits[4]));
        auto number = [](const std::wstring &value) {
          size_t n = 0;
          int result = std::stoi(value, &n);
          if (n != value.size())
            throw std::runtime_error("请填写有效整数");
          return result;
        };
        updated.settle_ms = number(textOf(c->edits[5]));
        updated.font_size = number(textOf(c->edits[6]));
        // Read only the selected service's independent password control.
        const auto key = updated.provider == "baidu" ? textOf(c->edits[10])
                       : updated.provider == "openai" ? textOf(c->edits[7])
                                                      : std::wstring();
        if (updated.provider == "baidu" && updated.baidu_key_env.empty())
          updated.baidu_key_env = "BAIDU_TRANSLATE_KEY";
        if (updated.provider == "openai" && !key.empty() &&
            updated.api_key_env.empty())
          updated.api_key_env = "CCLIVE_TRANSLATION_API_KEY";
        if (!updated.api_key_env.empty() && !updated.baidu_key_env.empty() &&
            _wcsicmp(wide(updated.api_key_env).c_str(),
                     wide(updated.baidu_key_env).c_str()) == 0)
          throw std::runtime_error("百度与兼容 API 请使用不同的密钥环境变量名。");
        if (updated.provider == "baidu" && updated.baidu_app_id.empty())
          throw std::runtime_error("请填写百度翻译 APPID。");
        updated.validate();
        if (key.find_first_of(L"\r\n") != std::wstring::npos)
          throw std::runtime_error("密钥不能包含换行符");
        const auto keyEnvironment = updated.provider == "baidu"
                                        ? updated.baidu_key_env
                                        : updated.api_key_env;
        if (!key.empty() && !SetEnvironmentVariableW(
                                wide(keyEnvironment).c_str(), key.c_str()))
          throw std::runtime_error("无法设置本次密钥");
        if (!(a.demo || a.selfTest))
          cclive::save_settings(a.configPath, updated);
        a.applySettings(std::move(updated), !key.empty());
        a.closeConfig();
      } catch (const std::exception &e) {
        SetWindowTextW(c->error, wide(e.what()).c_str());
      }
      return 0;
    }
  }
  if (msg == DM_GETDEFID)
    return MAKELRESULT(IDOK, DC_HASDEFID);
  if (msg == WM_DRAWITEM) {
    auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lp);
    if (item && item->CtlType == ODT_BUTTON) {
      paintButton(item->hwndItem, item->hDC, item->rcItem, item->itemState);
      return TRUE;
    }
  }
  if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
    SetBkColor(reinterpret_cast<HDC>(wp), PANEL);
    SetTextColor(reinterpret_cast<HDC>(wp), FG);
    return reinterpret_cast<LRESULT>(a.panel);
  }
  if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
    SetBkColor((HDC)wp, BG);
    SetTextColor((HDC)wp, (HWND)lp == c->error ? RGB(245, 186, 156) : MUTED);
    return (LRESULT)a.background;
  }
  if (msg == WM_ERASEBKGND) {
    RECT r{};
    GetClientRect(w, &r);
    FillRect((HDC)wp, &r, a.background);
    return 1;
  }
  if (msg == WM_CLOSE) {
    a.closeConfig();
    return 0;
  }
  if (msg == WM_NCDESTROY) {
    cancelConfigAuthorization(*c);
    if (a.config == c)
      a.config = nullptr;
    // Defensive cleanup for failed creation or owner-driven destruction.
    // Normal closes already enabled the owner before DestroyWindow.
    if (IsWindow(a.hwnd))
      EnableWindow(a.hwnd, TRUE);
    SetWindowLongPtrW(w, GWLP_USERDATA, 0);
    delete c;
    return DefWindowProcW(w, msg, wp, lp);
  }
  return DefWindowProcW(w, msg, wp, lp);
}
void App::closeConfig(bool restoreActivation) {
  if (!config)
    return;
  cancelConfigAuthorization(*config);
  const HWND dialog = config->hwnd;
  const HWND previousFocus = config->previousFocus;
  const bool wasForeground = GetForegroundWindow() == dialog;
  // An active window's owner must be enabled before destroying that window,
  // otherwise Windows can activate another application instead of the owner.
  if (IsWindow(hwnd))
    EnableWindow(hwnd, TRUE);
  DestroyWindow(dialog);
  if (restoreActivation && wasForeground && !selfTest && IsWindow(hwnd) &&
      IsWindowVisible(hwnd)) {
    if (IsIconic(hwnd))
      ShowWindow(hwnd, SW_RESTORE);
    SetActiveWindow(hwnd);
    const HWND focus = IsWindow(previousFocus) && IsChild(hwnd, previousFocus) &&
                               IsWindowEnabled(previousFocus) &&
                               IsWindowVisible(previousFocus)
                           ? previousFocus
                           : settingsButton;
    if (IsWindow(focus))
      SetFocus(focus);
  }
}
void App::openConfig() {
  if (config) {
    if (!selfTest)
      ShowWindow(config->hwnd, SW_SHOW);
    return;
  }
  config = new Config();
  config->app = this;
  config->previousFocus = GetFocus();
  RECT r{};
  GetWindowRect(hwnd, &r);
  HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"CCLiveSettings", L"翻译设置",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                           r.left + s(120), r.top + s(80), s(614), s(602), hwnd,
                           nullptr, GetModuleHandleW(nullptr), config);
  if (!w) {
    delete config;
    config = nullptr;
    status("设置窗口创建失败");
    return;
  }
  EnableWindow(hwnd, FALSE);
  if (!selfTest) {
    ShowWindow(w, SW_SHOW);
    SetFocus(config->edits.front());
  }
}
int gui(bool demo, bool check, const fs::path &report, bool automatic,
        const fs::path &statusPath) {

  LoadLibraryW(L"Msftedit.dll");
  INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&ic);
  auto dpiFn = (BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT))GetProcAddress(
      GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
  if (dpiFn)
    dpiFn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  struct OleScope {
    HRESULT result = OleInitialize(nullptr);
    ~OleScope() {
      if (SUCCEEDED(result))
        OleUninitialize();
    }
  } ole;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = App::windowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = applicationIcon(32);
  wc.hIconSm = applicationIcon(16);
  wc.lpszClassName = L"CCLiveNative";
  RegisterClassExW(&wc);
  wc.lpfnWndProc = configProc;
  wc.lpszClassName = L"CCLiveSettings";
  RegisterClassExW(&wc);
  UINT initialDpi = 96;
  auto getSystemDpi = reinterpret_cast<UINT(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
  if (getSystemDpi)
    initialDpi = getSystemDpi();
  else {
    HDC screen = GetDC(nullptr);
    if (screen) {
      initialDpi = GetDeviceCaps(screen, LOGPIXELSX);
      ReleaseDC(nullptr, screen);
    }
  }
  App app(demo, check, report, automatic, statusPath);
  HWND w = CreateWindowExW(
      0, L"CCLiveNative",
      demo ? L"Claude 实时翻译 · C++ 离线演示" : L"Claude 实时翻译 · C++",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      MulDiv(1180, initialDpi, 96), MulDiv(800, initialDpi, 96), nullptr,
      nullptr, GetModuleHandleW(nullptr), &app);
  if (!w)
    throw std::runtime_error("翻译窗口创建失败");
  if (!check) {
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
  }
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (app.config && IsDialogMessageW(app.config->hwnd, &msg))
      continue;
    if (!app.config && IsDialogMessageW(w, &msg))
      continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (check) {
    try {
      std::ifstream f(report);
      auto result = json::parse(f);
      return result.value("passed", false) ? 0 : 1;
    } catch (...) {
      return 1;
    }
  }
  return 0;
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  try {
    int argc = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!args)
      return 1;
    std::vector<std::wstring> argv(args, args + argc);
    LocalFree(args);
    if (argc >= 3 && argv[1] == L"--reader-info") {
      return cclive::reader_info_main(
          std::stoull(argv[2]),
          std::find(argv.begin(), argv.end(), L"--fixture") != argv.end());
    }
    if (argc >= 3 && argv[1] == L"--reader") {
      bool once = std::find(argv.begin(), argv.end(), L"--once") != argv.end(),
           fixture =
               std::find(argv.begin(), argv.end(), L"--fixture") != argv.end();
      return cclive::reader_main(std::stoull(argv[2]), once, fixture);
    }
    if (argc >= 2 && argv[1] == L"--list") {
      stdoutText(cclive::enumerate_windows_json() + "\n");
      return 0;
    }
    bool demo = std::find(argv.begin(), argv.end(), L"--demo") != argv.end(),
         check =
             std::find(argv.begin(), argv.end(), L"--self-test") != argv.end();
    fs::path report;
    for (size_t i = 1; i + 1 < argv.size(); ++i)
      if (argv[i] == L"--report")
        report = argv[i + 1];
    if (check && report.empty())
      return 2;
    bool automatic =
        std::find(argv.begin(), argv.end(), L"--auto-start") != argv.end();
    fs::path diagnostic;
    for (size_t i = 1; i < argv.size(); ++i) {
      if (argv[i] == L"--status-file") {
        if (i + 1 >= argv.size())
          throw std::runtime_error("--status-file 需要绝对路径");
        diagnostic = argv[++i];
        if (diagnostic.empty() || !diagnostic.is_absolute())
          throw std::runtime_error("--status-file 需要绝对路径");
      }
    }
    return gui(demo, check, report, automatic, diagnostic);
  } catch (const std::exception &e) {
    MessageBoxW(nullptr, wide(e.what()).c_str(), L"Claude 翻译启动失败",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
