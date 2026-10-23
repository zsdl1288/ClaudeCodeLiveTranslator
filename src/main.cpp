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
  uint64_t captureSequence = 0;
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
    results[k] = std::move(v);
    while (results.size() > 256)
      results.erase(results.begin());
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
                      "翻译服务限流（HTTP 429），等待约 " +
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
          publish("error", "翻译服务持续限流（HTTP 429），已停止本段自动重试。"
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
std::vector<wchar_t> readerEnvironment(const std::string &secret) {
  std::set<std::wstring> allowed = {L"SYSTEMROOT",   L"WINDIR",
                                    L"PATH",         L"TEMP",
                                    L"TMP",          L"USERPROFILE",
                                    L"APPDATA",      L"LOCALAPPDATA",
                                    L"HOMEDRIVE",    L"HOMEPATH",
                                    L"COMSPEC",      L"PROCESSOR_ARCHITECTURE",
                                    L"PROGRAMFILES", L"PROGRAMFILES(X86)"};
  auto excluded = wide(secret);
  std::transform(excluded.begin(), excluded.end(), excluded.begin(), towupper);
  std::vector<wchar_t> env;
  for (auto &name : allowed) {
    if (name == excluded)
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
  std::set<std::string> previousBlockKeys;
  std::size_t previousSourceCount = 0, previousMinimum = 0;
  uint64_t windowReplayNonce = 0;
  std::size_t minimumSourceIndex = 0;
  json currentTask;
  int settle;
  std::string secret;
  std::atomic_bool stopped{false};
  std::mutex mutex;
  HANDLE process = nullptr;
  std::shared_ptr<Bus> bus;
  std::string lastStatus;
  Watcher(uint64_t h, uint64_t g, int s, std::string k, std::shared_ptr<Bus> b)
      : hwnd(h), generation(g), settle(s), secret(std::move(k)),
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
    if (nextTaskId != currentTaskId) {
      consistentTaskSnapshots = 0;
      forwardedSlots.clear();
      previousBlockKeys.clear();
      previousSourceCount = previousMinimum = 0;
      windowReplayNonce = 0;
    }
    currentTaskId = nextTaskId;
    ++consistentTaskSnapshots;
    if (currentTaskId.empty())
      return;
    currentTask = {{"task_id", currentTaskId},
                   {"task_title", snapshot.value("task_title", "Claude 任务")},
                   {"identity_quality",
                    snapshot.value("identity_quality", "view_fallback")},
                   {"source_hwnd", snapshot.value("source_hwnd", hwnd)},
                   {"source_pid", snapshot.value("source_pid", 0ULL)}};
    auto meta = currentTask;
    meta["type"] = "task";
    meta["source_block_keys"] = json::array();
    meta["source_signatures"] = json::array();
    sourceIndices.clear();
    std::vector<std::size_t> englishPositions;
    std::set<std::string> blockKeys;
    if (consistentTaskSnapshots >= 2 && snapshot.contains("blocks") &&
        snapshot["blocks"].is_array()) {
      for (const auto &block : snapshot["blocks"]) {
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
        }
      }
    }
    minimumSourceIndex = englishPositions.size() > 100
                             ? englishPositions[englishPositions.size() - 100]
                             : 0;
    meta["source_minimum_index"] = minimumSourceIndex;
    meta["source_last_index"] =
        englishPositions.empty() ? 0 : englishPositions.back();
    bool sharedBlock = std::any_of(blockKeys.begin(), blockKeys.end(),
                                   [&](const std::string &key) {
                                     return previousBlockKeys.count(key) != 0;
                                   });
    bool replayWindow = consistentTaskSnapshots >= 2 && sharedBlock &&
                        (sourceIndices.size() < previousSourceCount ||
                         minimumSourceIndex < previousMinimum);
    meta["source_window_replay"] = replayWindow;
    emit(std::move(meta));
    if (consistentTaskSnapshots < 2) {
      auto identityOnly = snapshot;
      identityOnly["blocks"] = json::array();
      identityOnly["context"] =
          currentTaskId + (windowReplayNonce
                               ? "|window:" + std::to_string(windowReplayNonce)
                               : "");
      for (auto event : ledger.observe(identityOnly))
        publishLedger(std::move(event));
      return;
    }
    if (replayWindow) {
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
    auto effective = snapshot;
    effective["context"] =
        currentTaskId + (windowReplayNonce
                             ? "|window:" + std::to_string(windowReplayNonce)
                             : "");
    for (auto event : ledger.observe(effective))
      publishLedger(std::move(event));
  }
  void publishLedger(json event) {
    for (auto &entry : currentTask.items())
      event[entry.key()] = entry.value();
    if (event.value("type", "") == "text" && event.contains("block_key") &&
        event.contains("segment")) {
      auto slot = json::array({event["block_key"], event["segment"]}).dump();
      auto index = sourceIndices.find(slot);
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
        forwardedSlots[slot] = event;
        while (forwardedSlots.size() > 100) {
          auto oldest = std::min_element(
              forwardedSlots.begin(), forwardedSlots.end(),
              [](const auto &left, const auto &right) {
                return left.second.value("source_index", std::size_t(0)) <
                       right.second.value("source_index", std::size_t(0));
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
    auto env = readerEnvironment(secret);
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
        if (consistentTaskSnapshots >= 2)
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
  uint64_t serial = 0, version = 0;
};
class App;
struct Config {
  App *app = nullptr;
  HWND hwnd = nullptr, previousFocus = nullptr;
  std::vector<HWND> edits;
  HWND error = nullptr;
};
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
           refreshGeneration = 0;
  int ledgerEpoch = -1;
  std::vector<Record> records;
  std::vector<json> windows;
  std::shared_ptr<Bus> bus = std::make_shared<Bus>();
  std::shared_ptr<Worker> worker = std::make_shared<Worker>(bus);
  std::shared_ptr<Watcher> watcher;
  Clock::time_point lastRender = Clock::now(), demoStart, testStart;
  int demoStep = -1;
  std::unique_ptr<cclive::Ledger> demoLedger;
  Config *config = nullptr;
  std::unique_ptr<cclive::TaskCache> taskCache;
  std::string activeTaskId, activeTaskTitle, activeIdentityQuality;
  struct SourceAlias {
    std::string record, block;
  };
  std::map<std::string, SourceAlias> sourceAliases;
  std::set<std::string> replayCandidates;
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
    if (demo || selfTest)
      settings.provider = "demo";
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
    providerLabel = control(L"STATIC", L"", SS_RIGHT, 0, smallFont);
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
    statusLabel = control(L"STATIC", L"", SS_LEFT, 0, smallFont);
    countLabel = control(L"STATIC", L"0 段", SS_RIGHT, 0, smallFont);
    foot = control(L"STATIC", L"后台只读 · 不截图、不点击、不修改客户端", 0, 0,
                   smallFont);
    EnableWindow(refreshButton, !(demo || selfTest));
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
  void layout() {
    if (!hwnd)
      return;
    RECT r{};
    GetClientRect(hwnd, &r);
    int width = r.right, height = r.bottom, m = s(16), gap = s(10);
    int bodyW = std::max(s(600), width - 2 * m);
    auto move = [](HWND w, int x, int y, int a, int b) {
      MoveWindow(w, x, y, std::max(1, a), std::max(1, b), TRUE);
    };
    ShowWindow(eyebrow, SW_HIDE);
    ShowWindow(subtitle, SW_HIDE);
    ShowWindow(sourceLabel, SW_HIDE);
    ShowWindow(foot, SW_HIDE);
    move(iconLabel, m, s(8), s(28), s(28));
    move(title, m + s(38), s(9), s(114), s(27));
    move(taskLabel, m + s(162), s(13), std::max(s(180), bodyW - s(528)), s(21));
    move(providerLabel, width - m - s(350), s(13), s(350), s(21));
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
    int top = s(117), bottom = height - s(36);
    int paneH = std::max(s(180), bottom - top - s(8));
    int paneW = settings.show_original ? (bodyW - gap) / 2 : bodyW;
    ShowWindow(sourceEdit, settings.show_original ? SW_SHOW : SW_HIDE);
    if (settings.show_original)
      move(sourceEdit, m, top + s(8), paneW, paneH);
    int tx = settings.show_original ? m + paneW + gap : m;
    move(translationEdit, tx, top + s(8), paneW, paneH);
    move(statusLabel, m, height - s(26), bodyW - s(130), s(20));
    move(countLabel, width - m - s(124), height - s(26), s(124), s(20));
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  void paintDividers(HDC dc) const {
    RECT area{};
    GetClientRect(hwnd, &area);
    const int width = area.right, height = area.bottom;
    const int margin = s(16), gap = s(10);
    const int bodyWidth = std::max(s(600), width - 2 * margin);
    const int paneWidth = settings.show_original ? (bodyWidth - gap) / 2
                                                 : bodyWidth;
    const int top = s(117), bottom = height - s(36);
    const auto pen = CreatePen(PS_SOLID, 1, RGB(72, 70, 64));
    const auto previous = SelectObject(dc, pen);
    auto line = [&](int x1, int y1, int x2, int y2) {
      MoveToEx(dc, x1, y1, nullptr);
      LineTo(dc, x2, y2);
    };
    line(margin, s(39), width - margin, s(39));
    int translationLeft = margin;
    if (settings.show_original) {
      line(margin, top, margin + paneWidth, top);
      const int middle = margin + paneWidth + gap / 2;
      line(middle, top, middle, bottom);
      translationLeft = margin + paneWidth + gap;
    }
    line(translationLeft, top, translationLeft + paneWidth, top);
    line(margin, height - s(31), width - margin, height - s(31));
    SelectObject(dc, previous);
    DeleteObject(pen);
  }
  json translationProfile() const {
    return {{"provider", settings.provider},
            {"model", settings.model},
            {"target", settings.target},
            {"endpoint_hash", contentSignature(settings.base_url)}};
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
                       {"error", record.error},
                       {"source_signature", record.sourceSignature},
                       {"serial", record.serial}});
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
        if (records.empty())
          taskCache->erase(activeTaskId);
        else
          taskCache->put(activeTaskId, taskPayload());
      } catch (...) {
      }
    }
    cacheDirty = false;
    lastCacheSave = Clock::now();
  }
  void seedReplay(const json &metadata) {
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
    replayPendingBody = !records.empty() && visible.empty();
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
        if (record.state == "done" && !record.translation.empty())
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
      if (activeProfile != translationProfile()) {
        windowReplayMemo.clear();
        windowReplayLastIndex = 0;
        ++generation;
        cancelAll();
        activeProfile = translationProfile();
        sourceNeedsReplay = true;
        for (auto &record : records) {
          record.translation.clear();
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
    activeTaskId = id;
    activeProfile = translationProfile();
    activeTaskTitle = titleText;
    activeIdentityQuality = metadata.value("identity_quality", "view_fallback");
    selectedSourceHwnd = source;
    followingSource = followingTranslation = true;
    POINT sourceScroll{}, translatedScroll{};
    if (auto cached = taskCache->get(id)) {
      try {
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
            record.sourceSignature =
                item.value("source_signature", contentSignature(record.source));
            record.translation = matching ? item.value("translation", "") : "";
            record.error = matching ? item.value("error", "") : "";
            auto state = item.value("state", "waiting");
            record.state =
                matching && state == "done" && !record.translation.empty()
                    ? "done"
                : matching && state == "error" ? "error"
                                               : "waiting";
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
  void clear() {
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
  void accept(json v) {
    auto type = v.value("type", "");
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
      if (ledgerEpoch >= 0) {
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
    if (it == records.end()) {
      std::string memo;
      if (auto replay = windowReplayMemo.find(incomingSignature);
          replay != windowReplayMemo.end())
        memo = replay->second;
      for (auto previous = records.rbegin(); previous != records.rend();
           ++previous)
        if ((previous->source == source ||
             previous->sourceSignature == incomingSignature) &&
            previous->state == "done" && !previous->translation.empty()) {
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
      records.push_back(std::move(record));
      it = std::prev(records.end());
    }
    sourceAliases[key] = {it->key, v.value("block_key", "")};
    if (!windowReplayMemo.empty() && v.contains("source_index") &&
        v["source_index"].is_number_integer() &&
        v["source_index"].get<std::size_t>() >= windowReplayLastIndex)
      windowReplayMemo.clear();
    if ((it->source == source || it->sourceSignature == incomingSignature) &&
        (it->state == "done" || it->state == "queued" ||
         it->state == "translating" || it->state == "rate_limited" ||
         it->state == "paused")) {
      dirty = cacheDirty = true;
      trimRecords();
      return;
    }
    worker->cancel(it->key);
    it->source = std::move(source);
    it->sourceSignature = incomingSignature;
    it->translation.clear();
    it->error.clear();
    it->version = ++revision;
    it->state = paused ? "paused" : "waiting";
    if (!paused)
      enqueue(*it);
    dirty = cacheDirty = true;
    trimRecords();
  }
  void refresh() {
    if (demo || selfTest || refreshing)
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
                                          settings.api_key_env, bus);
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
  std::wstring output(bool original) const {
    if (records.empty())
      return original ? L"英文原文将在这里显示"
                      : L"中文译文将在这里显示\r\n\r\n点击开始后，译文会随输出"
                        L"更新。";
    std::wstring value;
    size_t begin = records.size(), chars = 0;
    while (begin > 0) {
      auto &r = records[begin - 1];
      size_t add = original ? r.source.size()
                            : r.translation.size() + r.error.size() + 100;
      if (chars + add > 1000000 && begin < records.size())
        break;
      chars += add;
      --begin;
    }
    if (begin)
      value = L"仅显示最近保留的字幕内容。\r\n\r\n";
    for (size_t i = begin; i < records.size(); ++i) {
      const auto &r = records[i];

      if (original)
        value += wide(r.source);
      else {
        value += wide(r.translation);
        if (r.state != "done") {
          if (!r.translation.empty())
            value += L"  ";
          value += wide(!r.error.empty()           ? r.error
                        : r.state == "paused"      ? "已暂停"
                        : r.state == "stopped"     ? "已停止"
                        : r.state == "translating" ? "翻译中…"
                                                   : "等待翻译…");
        }
      }
      value += L"\r\n";
    }
    return value;
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
  void goLatest() {
    followingSource = followingTranslation = true;
    ScrollGuard guard(internalScroll);
    for (HWND edit : {translationEdit, sourceEdit}) {
      if (!edit)
        continue;
      SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
      SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
      InvalidateRect(edit, nullptr, FALSE);
    }
    status("正在跟随最新译文；向上滚动可暂停跟随");
  }
  static LRESULT CALLBACK bodyProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, DWORD_PTR reference) {
    auto *app = reinterpret_cast<App *>(reference);
    if (msg == WM_NCDESTROY) {
      RemoveWindowSubclass(edit, bodyProc, id);
      return DefSubclassProc(edit, msg, wp, lp);
    }
    bool navigation =
        msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR ||
                              wp == VK_NEXT || wp == VK_HOME || wp == VK_END);
    bool manual = app && !app->closed && !app->internalScroll &&
                  (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL ||
                   msg == EM_LINESCROLL || navigation);
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
    }
    return result;
  }
  void editText(HWND edit, const std::wstring &value) {
    bool bottom = follows(edit);
    ScrollGuard guard(internalScroll);
    POINT scroll{};
    SendMessageW(edit, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    CHARRANGE selected{};
    SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&selected);
    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    // Both panes use identical Markdown parsing and native RichEdit styles.
    auto formatted = cclive::make_text_layout(value);
    SetWindowTextW(edit, formatted.text.c_str());
    cclive::apply_text_layout(edit, formatted, settings.font_size, dpi);
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&selected);
    // RichEdit can defer layout while redraw is disabled. Restore redraw before
    // consulting the new extent or scrolling, otherwise the old end position
    // can be mistaken for a deliberate user scroll on the following update.
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
    if (bottom)
      SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
    else
      SendMessageW(edit, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    InvalidateRect(edit, nullptr, FALSE);
  }
  void render() {
    if (closed)
      return;
    if (settings.show_original)
      editText(sourceEdit, output(true));
    editText(translationEdit, output(false));
    size_t count =
        std::count_if(records.begin(), records.end(),
                      [](const Record &r) { return r.state == "done"; });
    auto caption = std::to_wstring(count) + L" / " +
                   std::to_wstring(records.size()) + L" 段";
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
    }
  }
  void applySettings(cclive::Settings updated, bool keyChanged) {
    if (demo || selfTest)
      updated.provider = "demo";
    const bool restart = keyChanged || settings.provider != updated.provider ||
                         settings.base_url != updated.base_url ||
                         settings.model != updated.model ||
                         settings.api_key_env != updated.api_key_env ||
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
  void synthetic() {
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
    it->state = v.value("state", "");
    it->translation = v.value("translation", "");
    it->error = v.value("error", "");
    dirty = cacheDirty = true;
    if (it->state == "error")
      status("部分文字翻译失败，可检查设置后重试");
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
      return defaults.crTextColor == cclive::text_layout_palette.body &&
             actual.crTextColor == cclive::text_layout_palette.body &&
             !(actual.dwEffects & CFE_AUTOCOLOR) &&
             actual.crBackColor == cclive::text_layout_palette.background;
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
    sourceAliases["removed-runtime"] = {records[0].key, "old-block"};
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
    sourceAliases["oversized-runtime"] = {oversized.key, "large-block"};
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
    // Actual Watcher -> Ledger -> ordered Bus replay, with no process/UIA
    // calls.
    paused = true;
    running = true;
    auto replayBus = std::make_shared<Bus>();
    Watcher supplied(0, sourceGeneration, 0, "", replayBus);
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
    // Same-block shortening must replay slots previously outside the visible
    // suffix, without allowing hundreds of empty deletions to evict task meta.
    const auto oldHugeGeneration = generation;
    const auto oldHugeKey = records.front().key;
    const auto oldHugeVersion = records.front().version;
    auto shortA = largeSnapshot("huge-task-A", "rebuilt-huge-block-A", 50);
    supplied.publishSnapshot(shortA, suppliedLedger);
    {
      std::lock_guard<std::mutex> lock(replayBus->mutex);
      const auto tasks = std::count_if(
          replayBus->source.begin(), replayBus->source.end(),
          [](const json &event) { return event.value("type", "") == "task"; });
      const auto removed =
          std::count_if(replayBus->source.begin(), replayBus->source.end(),
                        [](const json &event) {
                          return event.value("type", "") == "text" &&
                                 event.value("text", "").empty();
                        });
      check("700_to_50_deletions_keep_task_inside_bounded_bus",
            tasks == 1 && removed <= 100 && replayBus->source.size() <= 202);
    }
    drainSupplied();
    check("700_to_unchanged_first_50_replays_new_visible_window",
          activeTaskId == "huge-task-A" && records.size() == 50 &&
              records.front().source == "Large source 0." &&
              records.back().source == "Large source 49.");
    acceptResult({{"task_id", activeTaskId},
                  {"generation", oldHugeGeneration},
                  {"key", oldHugeKey},
                  {"version", oldHugeVersion},
                  {"state", "done"},
                  {"translation", "STALE BEFORE WINDOW SHRINK"}});
    check("late_result_cannot_refill_removed_700_suffix",
          records.size() == 50 &&
              std::none_of(
                  records.begin(), records.end(), [](const Record &record) {
                    return record.translation == "STALE BEFORE WINDOW SHRINK";
                  }));
    for (std::size_t index = 0; index < records.size(); ++index)
      complete(index, "已译 " + records[index].source);
    supplied.publishSnapshot(hugeA, suppliedLedger);
    {
      std::lock_guard<std::mutex> lock(replayBus->mutex);
      const auto tasks = std::count_if(
          replayBus->source.begin(), replayBus->source.end(),
          [](const json &event) { return event.value("type", "") == "task"; });
      check("50_to_700_regrowth_keeps_task_event",
            tasks == 1 && replayBus->source.size() <= 102);
    }
    drainSupplied();
    check("700_to_50_to_700_retains_only_latest_100_without_old_rows",
          activeTaskId == "huge-task-A" && records.size() == 100 &&
              records.front().source == "Large source 600." &&
              records.back().source == "Large source 699." &&
              std::none_of(records.begin(), records.end(),
                           [](const Record &record) {
                             return record.source == "Large source 0." ||
                                    record.source == "Large source 49.";
                           }));
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
    for (auto &pair : results)
      acceptResult(pair.second);
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
      p->ptMinTrackSize = {a->s(920), a->s(640)};
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
          a->clear();
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
      make(L"STATIC", labels[i], 0, 24, 24 + i * 37, 158, 25, 0);
      HWND edit = make(i ? L"EDIT" : L"COMBOBOX", i ? values[i].c_str() : L"",
                       i ? (WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP |
                            (i == 7 ? ES_PASSWORD : 0))
                         : (CBS_DROPDOWNLIST | WS_TABSTOP),
                       190, 20 + i * 37, 382, i ? 28 : 220, 200 + i);
      c->edits.push_back(edit);
      if (!i) {
        for (const auto *v : {L"google", L"openai", L"ollama"})
          SendMessageW(edit, CB_ADDSTRING, 0, (LPARAM)v);
        SendMessageW(edit, CB_SETCURSEL,
                     a.settings.provider == "openai"   ? 1
                     : a.settings.provider == "ollama" ? 2
                                                       : 0,
                     0);
      }
    }
    make(L"STATIC",
         L"Key 只在本次运行中使用，不写入配置。可留空使用已有环境变量。", 0, 24,
         326, 552, 42, 0);
    make(L"STATIC",
         L"Google：免 Key 公共接口，待译文字会发送至 Google。\r\nAPI：兼容 "
         L"Chat Completions，需填写地址及模型。\r\nOllama：通常为 "
         L"http://127.0.0.1:11434，填写本地模型。",
         0, 24, 370, 552, 74, 0);
    c->error = make(L"STATIC", L"", 0, 24, 450, 552, 48, 0);
    make(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 380, 510, 88, 32,
         IDCANCEL);
    make(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 484, 510, 88, 32,
         IDOK);
    return 0;
  }
  if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
    if (LOWORD(wp) == IDCANCEL) {
      a.closeConfig();
      return 0;
    }
    if (LOWORD(wp) == IDOK) {
      try {
        auto updated = a.settings;
        int index = (int)SendMessageW(c->edits[0], CB_GETCURSEL, 0, 0);
        updated.provider = index == 1   ? "openai"
                           : index == 2 ? "ollama"
                                        : "google";
        updated.base_url = utf8(textOf(c->edits[1]));
        updated.model = utf8(textOf(c->edits[2]));
        updated.api_key_env = utf8(textOf(c->edits[3]));
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
        auto key = textOf(c->edits[7]);
        if (!key.empty() && updated.api_key_env.empty())
          updated.api_key_env = "CCLIVE_TRANSLATION_API_KEY";
        updated.validate();
        if (key.find_first_of(L"\r\n") != std::wstring::npos)
          throw std::runtime_error("Key 不能包含换行符");
        if (!key.empty() && !SetEnvironmentVariableW(
                                wide(updated.api_key_env).c_str(), key.c_str()))
          throw std::runtime_error("无法设置本次 Key");
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
