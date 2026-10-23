// Native integration test using only a synthetic accessibility fixture.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include "task_identity.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using json = nlohmann::json;
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Child {
    PROCESS_INFORMATION process{};
    ~Child() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 0);
                WaitForSingleObject(process.hProcess, 2000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
};
std::wstring quote(const std::wstring& text) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : text) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        result += character; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'\"';
    return result;
}
void require(bool condition, const char* description) { if (!condition) throw std::runtime_error(description); }
void write_source(const fs::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    require(static_cast<bool>(output), "cannot write synthetic fixture input");
}
HWND find_window(DWORD pid) {
    struct Lookup { DWORD pid; HWND hwnd = nullptr; } lookup{pid};
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        auto& state = *reinterpret_cast<Lookup*>(parameter);
        DWORD owner = 0; GetWindowThreadProcessId(hwnd, &owner);
        if (owner == state.pid && IsWindowVisible(hwnd)) { state.hwnd = hwnd; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&lookup));
    return lookup.hwnd;
}
json read_once(const fs::path& app, HWND hwnd, bool allow_fixture = true, bool info = false) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    Handle read_pipe, write_pipe;
    require(CreatePipe(&read_pipe.value, &write_pipe.value, &attributes, 0), "CreatePipe failed");
    SetHandleInformation(read_pipe.value, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe.value; startup.hStdError = write_pipe.value;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring command = quote(app.wstring()) + (info ? L" --reader-info " : L" --reader ") + std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd)) + L" --once";
    if (allow_fixture) command += L" --fixture";
    Child helper;
    require(CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, app.parent_path().c_str(), &startup, &helper.process), "reader helper could not start");
    CloseHandle(write_pipe.value); write_pipe.value = nullptr;
    std::string bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available) {
            char buffer[16384]; DWORD count = 0;
            require(ReadFile(read_pipe.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr), "reader stdout failed");
            bytes.append(buffer, count);
            require(bytes.size() < 4 * 1024 * 1024, "reader output exceeds limit");
        } else if (WaitForSingleObject(helper.process.hProcess, 0) != WAIT_TIMEOUT) break;
        else Sleep(20);
    }
    require(WaitForSingleObject(helper.process.hProcess, 200) != WAIT_TIMEOUT, "reader helper timed out");
    const auto end = bytes.find('\n');
    require(end != std::string::npos, "reader did not emit a JSON line");
    return json::parse(bytes.substr(0, end));
}
struct ReaderStream {
    Child helper;
    Handle read_pipe;
    std::string buffered;
    ReaderStream(const fs::path& app, HWND hwnd) {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        Handle write_pipe;
        require(CreatePipe(&read_pipe.value, &write_pipe.value, &attributes, 0), "persistent reader pipe failed");
        SetHandleInformation(read_pipe.value, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = write_pipe.value; startup.hStdError = write_pipe.value;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        auto command = quote(app.wstring()) + L" --reader " + std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd)) + L" --fixture";
        require(CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, app.parent_path().c_str(), &startup, &helper.process), "persistent reader could not start");
    }
    json next() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto end = buffered.find('\n'); end != std::string::npos) {
                auto line = buffered.substr(0, end); buffered.erase(0, end + 1);
                auto snapshot = json::parse(line);
                require(snapshot.value("type", "") == "snapshot", "persistent reader returned a capture error");
                return snapshot;
            }
            DWORD available = 0;
            require(PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr, &available, nullptr), "persistent reader pipe closed");
            if (available) {
                char bytes[16384]; DWORD count = 0;
                require(ReadFile(read_pipe.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr), "persistent reader output failed");
                buffered.append(bytes, count);
                require(buffered.size() < 4 * 1024 * 1024, "persistent reader output exceeds limit");
            } else {
                require(WaitForSingleObject(helper.process.hProcess, 0) == WAIT_TIMEOUT, "persistent reader exited early");
                Sleep(20);
            }
        }
        throw std::runtime_error("persistent reader snapshot timed out");
    }
    json until(const std::function<bool(const json&)>& matches) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (std::chrono::steady_clock::now() < deadline) {
            auto snapshot = next();
            for (const auto& block : snapshot.value("blocks", json::array())) {
                const auto text = block.value("text", std::string{});
                require(text.find("UNATTRIBUTED B BODY") == std::string::npos, "unverified task body escaped persistent reader");
                require(text.find("PEER B BODY") == std::string::npos, "unrelated Document body escaped selected task scope");
            }
            if (matches(snapshot)) return snapshot;
        }
        throw std::runtime_error("persistent reader did not reach expected identity state");
    }
};
std::string block_text(const json& snapshot) {
    std::string result;
    for (const auto& block : snapshot.value("blocks", json::array())) result += block.value("text", std::string{});
    return result;
}
int wmain(int argc, wchar_t** argv) {
    json report = {{"checks", json::array()}, {"network_used", false}, {"real_claude_read", false}};
    try {
        require(argc == 3, "usage: reader_smoke fixture.exe translator.exe");
        require(cclive::canonical_task_url("https://claude.ai/epitaxy/local_abcdef?pid=123#title") == "https://claude.ai/epitaxy/local_abcdef", "Code task URL normalization failed");
        require(cclive::canonical_task_url("https://www.claude.ai/chat/aaaa-bbbb/renamed-title?x=1") == "https://claude.ai/chat/aaaa-bbbb", "chat URL normalization retained dynamic title/query");
        require(cclive::canonical_task_url("https://claude.ai/project/project-123/chat/aaaa-bbbb") == "https://claude.ai/chat/aaaa-bbbb", "project chat URL normalization failed");
        require(cclive::canonical_task_url("https://claude.ai/chat/aaaa-bbbb") != cclive::canonical_task_url("https://claude.ai/chat/cccc-dddd"), "different task URLs collided");
        require(cclive::canonical_task_url("https://claude.ai/").empty() && cclive::canonical_task_url("https://claude.ai/epitaxy").empty() && cclive::canonical_task_url("https://claude.ai/epitaxy/local_").empty() && cclive::canonical_task_url("https://claude.ai/chat/new-chat").empty(), "blank views received global task ID");
        require(cclive::canonical_task_url("https://claude.ai.evil/chat/aaaa-bbbb").empty() && cclive::canonical_task_url("https://evil.example/chat/aaaa-bbbb").empty(), "untrusted URL accepted as Claude task");
        require(cclive::clean_task_title("  Example task - Claude Code  ") == "Example task" && cclive::clean_task_title("Renamed task - Claude") == "Renamed task", "task title suffix cleanup failed");
        report["checks"].push_back("pure task URL identity ignores query/title and separates conversations without blank/foreign collisions");
        const fs::path fixture = fs::absolute(argv[1]);
        const fs::path app = fs::absolute(argv[2]);
        const fs::path input = fixture.parent_path() / "fixture-input.txt";
        write_source(input, "Reading the project files.");
        std::wstring command = quote(fixture.wstring()) + L" " + quote(input.wstring());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
        Child window;
        require(CreateProcessW(fixture.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &window.process), "fixture could not start");
        HWND hwnd = nullptr;
        for (int retry = 0; retry < 50 && !hwnd; ++retry) { Sleep(50); hwnd = find_window(window.process.dwProcessId); }
        require(hwnd != nullptr, "fixture window did not appear");
        require(GetForegroundWindow() != hwnd, "fixture stole focus");
        auto initial = read_once(app, hwnd);
        require(initial.value("type", "") == "snapshot", "first synthetic snapshot failed");
        require(block_text(initial).find("Reading the project files.") != std::string::npos, "assistant body not extracted");
        require(block_text(initial).find("Claude responded:") == std::string::npos, "screen reader marker leaked into output");
        require(block_text(initial).find("USER INPUT MUST NOT BE TRANSLATED") == std::string::npos, "user message leaked into assistant output");
        require(block_text(initial).find("PREVIOUS ANSWER MUST NOT BE TRANSLATED") == std::string::npos, "older assistant backlog leaked into output");
        report["checks"].push_back("native UI Automation extracts only newest assistant body and skips user/history");
        write_source(input, "Reading the project files. The implementation is ready.");
        Sleep(300);
        auto updated = read_once(app, hwnd);
        require(block_text(updated).find("The implementation is ready.") != std::string::npos, "growing assistant body not updated");
        require(updated.value("context", "") == initial.value("context", ""), "context changed during same-message growth");
        require(updated.at("blocks").at(0).at("key") == initial.at("blocks").at(0).at("key"), "block identity changed during growth");
        report["checks"].push_back("growing text preserves source context and block identity");
        const auto refused = read_once(app, hwnd, false);
        require(refused.value("type", "") == "error", "non-Claude process accepted outside fixture mode");
        report["checks"].push_back("non-Claude targets refused without test-only fixture flag");
        require(GetForegroundWindow() != hwnd, "reader activated fixture");
        require(GetForegroundWindow() != hwnd, "fixture activated during read-only capture");
        report["checks"].push_back("reader never activates its own synthetic source");
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        WaitForSingleObject(window.process.hProcess, 2000);
        std::wstring semantic_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --semantic";
        Child semantic_window;
        require(CreateProcessW(fixture.c_str(), semantic_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &semantic_window.process), "semantic fixture could not start");
        HWND semantic_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !semantic_hwnd; ++retry) { Sleep(50); semantic_hwnd = find_window(semantic_window.process.dwProcessId); }
        require(semantic_hwnd != nullptr, "semantic fixture window did not appear");
        const auto semantic = read_once(app, semantic_hwnd);
        require(semantic.value("type", "") == "snapshot", "semantic-only snapshot failed");
        const auto semantic_text = block_text(semantic);
        require(semantic_text.find("The implementation is ready.") != std::string::npos, "semantic assistant body not extracted");
        require(semantic_text.find("PREVIOUS ANSWER MUST NOT BE TRANSLATED") == std::string::npos, "semantic fallback queued old assistant history");
        require(semantic_text.find("USER INPUT MUST NOT BE TRANSLATED") == std::string::npos, "semantic fallback leaked user ancestor text");
        require(GetForegroundWindow() != semantic_hwnd, "semantic fallback activated fixture");
        report["checks"].push_back("semantic-only fallback selects latest assistant and rejects nested user content");
        PostMessageW(semantic_hwnd, WM_CLOSE, 0, 0);
        WaitForSingleObject(semantic_window.process.hProcess, 2000);
        std::wstring article_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --article";
        Child article_window;
        require(CreateProcessW(fixture.c_str(), article_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &article_window.process), "article fixture could not start");
        HWND article_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !article_hwnd; ++retry) { Sleep(50); article_hwnd = find_window(article_window.process.dwProcessId); }
        require(article_hwnd != nullptr, "article fixture window did not appear");
        const auto article = read_once(app, article_hwnd);
        require(article.value("type", "") == "snapshot", "Chinese article snapshot failed");
        const auto article_text = block_text(article);
        require(article_text.find("The implementation is ready.") != std::string::npos, "Chinese article lost first body paragraph");
        require(article_text.find("Second body paragraph must be preserved.") != std::string::npos, "Chinese article lost middle body paragraph");
        require(article_text.find("Short tail.") != std::string::npos, "Chinese article lost final short paragraph");
        require(article_text.find("HEADING SUMMARY") == std::string::npos, "heading summary was treated as body");
        require(article_text.find("USER INPUT") == std::string::npos, "Chinese user marker failed to reject message");
        require(article_text.find("PREVIOUS ANSWER") == std::string::npos, "Chinese article selected old history");
        report["checks"].push_back("Chinese article headings select complete multi-paragraph assistant body and exclude user/summary/history");
        const auto info = read_once(app, article_hwnd, true, true);
        require(info.value("type", "") == "reader-info" && !info.value("body_included", true), "structural diagnostic unexpectedly contains body");
        require(info.value("message_count", 0) >= 3 && info.value("assistant_markers", 0) >= 2 && info.value("user_markers", 0) >= 1, "structural diagnostic missed Chinese message markers");
        require(info.value("chars_count", 0) >= 80 && info.value("blocks_count", 0) >= 1 && !info.value("minimized", true), "structural diagnostic missing capture counts or window state");
        require(info.dump().find("The implementation") == std::string::npos && info.dump().find("SUMMARY") == std::string::npos, "diagnostic leaked source content");
        report["checks"].push_back("structural diagnostic exposes only counts, roles and minimization state");
        ShowWindow(article_hwnd, SW_SHOWMINNOACTIVE);
        const auto minimized_info = read_once(app, article_hwnd, true, true);
        require(minimized_info.value("minimized", false), "minimization diagnostic failed");
        require(minimized_info.value("chars_count", 0) == info.value("chars_count", 0), "synthetic minimized window lost accessible text");
        require(GetForegroundWindow() != article_hwnd, "localized read activated fixture");
        report["checks"].push_back("synthetic minimized source retains readable body without activation");
        PostMessageW(article_hwnd, WM_CLOSE, 0, 0);
        WaitForSingleObject(article_window.process.hProcess, 2000);
        std::wstring feed_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --feed";
        Child feed_window;
        require(CreateProcessW(fixture.c_str(), feed_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &feed_window.process), "feed fixture could not start");
        HWND feed_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !feed_hwnd; ++retry) { Sleep(50); feed_hwnd = find_window(feed_window.process.dwProcessId); }
        require(feed_hwnd != nullptr, "feed fixture window did not appear");
        const auto feed = read_once(app, feed_hwnd);
        const auto feed_text = block_text(feed);
        require(feed.value("type", "") == "snapshot", "feed snapshot failed");
        require(feed_text.find("The implementation is ready.") != std::string::npos, "feed lost sibling-row first body");
        require(feed_text.find("Second body paragraph must be preserved.") != std::string::npos, "feed lost sibling-row middle body");
        require(feed_text.find("Short tail.") != std::string::npos, "feed lost sibling-row short tail");
        require(feed_text.find("PREVIOUS ANSWER") == std::string::npos && feed_text.find("HEADING SUMMARY") == std::string::npos, "feed leaked history or heading summary");
        require(feed_text.find("USER INPUT") == std::string::npos && feed_text.find("USER SIBLING ROW") == std::string::npos && feed_text.find("SIDEBAR TEXT") == std::string::npos, "feed crossed user or feed boundary");
        const auto feed_info = read_once(app, feed_hwnd, true, true);
        require(feed_info.at("aria_roles").value("feed", 0) == 1, "synthetic native provider did not expose feed role");
        require(feed_info.value("chars_count", 0) >= 100, "feed diagnostic remained a short title-only capture");
        require(GetForegroundWindow() != feed_hwnd, "feed read activated source");
        report["checks"].push_back("native UIA Feed provider captures sibling message rows and stops at user/sidebar boundaries");
        PostMessageW(feed_hwnd, WM_CLOSE, 0, 0);
        WaitForSingleObject(feed_window.process.hProcess, 2000);
        for (const std::wstring mode : {L"--semantic-list", L"--stream-list"}) {
            write_source(input, "STREAMING");
            std::wstring list_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" " + mode;
            Child list_window;
            require(CreateProcessW(fixture.c_str(), list_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &list_window.process), "list fixture could not start");
            HWND list_hwnd = nullptr;
            for (int retry = 0; retry < 50 && !list_hwnd; ++retry) { Sleep(50); list_hwnd = find_window(list_window.process.dwProcessId); }
            require(list_hwnd != nullptr, "list fixture did not appear");
            const auto streaming = read_once(app, list_hwnd);
            const auto list_text = block_text(streaming);
            require(list_text.find("Streaming list item one") != std::string::npos && list_text.find("Streaming list item two") != std::string::npos && list_text.find("Short list tail.") != std::string::npos, "heading-free safe list body not captured");
            require(list_text.find("USER LIST") == std::string::npos && list_text.find("COMPOSER") == std::string::npos && list_text.find("INPUT CHILD") == std::string::npos, "heading-free list crossed user/input/sidebar boundary");
            if (mode == L"--stream-list") {
                write_source(input, "HEADING_READY"); Sleep(300);
                const auto completed = read_once(app, list_hwnd);
                require(block_text(completed) == list_text, "speaker heading changed captured list body");
                require(streaming.at("blocks").at(0).at("key") == completed.at("blocks").at(0).at("key"), "streaming-to-heading changed stable article key");
                require(block_text(completed).find("HEADING SUMMARY") == std::string::npos, "completion summary leaked into source");
            }
            require(GetForegroundWindow() != list_hwnd, "list reading activated source");
            PostMessageW(list_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(list_window.process.hProcess, 2000);
        }
        report["checks"].push_back("safe semantic ListItem fallback excludes user/input/sidebar");
        report["checks"].push_back("headless article after proven user captures only message rows and retains key when heading appears");
        std::wstring rich_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --rich-list";
        Child rich_window;
        require(CreateProcessW(fixture.c_str(), rich_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &rich_window.process), "rich list fixture could not start");
        HWND rich_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !rich_hwnd; ++retry) { Sleep(50); rich_hwnd = find_window(rich_window.process.dwProcessId); }
        require(rich_hwnd != nullptr, "rich list fixture did not appear");
        const auto rich = read_once(app, rich_hwnd);
        const auto rich_text = block_text(rich);
        require(rich_text.find("Progress: Media Encoder now resumes the export successfully.") != std::string::npos, "rich ListItem lost body after bold label");
        require(rich_text.find("New problem: The output path requires permission.") != std::string::npos, "second rich ListItem lost body after label");
        require(rich_text.find("Progress:", rich_text.find("Progress:") + 1) == std::string::npos, "rich ListItem label duplicated");
        require(rich_text.find("New problem:", rich_text.find("New problem:") + 1) == std::string::npos, "second rich ListItem label duplicated");
        require(rich_text.find("AUXILIARY MUST") == std::string::npos && rich_text.find("CODE AGGREGATE") == std::string::npos && rich_text.find("CODE CHILD") == std::string::npos, "aggregate list Name bypassed auxiliary/code filtering");
        require(GetForegroundWindow() != rich_hwnd, "rich list reading activated source");
        report["checks"].push_back("rich ListItem complete Name overrides partial label Text without duplication or code/auxiliary leakage");
        PostMessageW(rich_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(rich_window.process.hProcess, 2000);
        write_source(input, "https://claude.ai/epitaxy/local_identity_a?view=one#old-title\nOriginal title - Claude Code");
        std::wstring identity_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --identity";
        Child identity_window;
        require(CreateProcessW(fixture.c_str(), identity_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &identity_window.process), "identity Document fixture could not start");
        HWND identity_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !identity_hwnd; ++retry) { Sleep(50); identity_hwnd = find_window(identity_window.process.dwProcessId); }
        require(identity_hwnd != nullptr, "identity fixture did not appear");
        const auto task_a = read_once(app, identity_hwnd);
        require(task_a.value("identity_quality", "") == "url" && task_a.value("task_id", "") == "https://claude.ai/epitaxy/local_identity_a", "native Document ValuePattern task URL not read");
        require(task_a.value("context", "") == task_a.value("task_id", "") && task_a.value("task_title", "") == "Original title", "task context/title metadata invalid");
        require(task_a.value("source_hwnd", 0ULL) == reinterpret_cast<std::uintptr_t>(identity_hwnd) && task_a.value("source_pid", 0UL) == identity_window.process.dwProcessId, "source window metadata invalid");
        write_source(input, "https://claude.ai/epitaxy/local_identity_a?view=two#new-title\nRenamed title - Claude Code"); Sleep(300);
        const auto renamed = read_once(app, identity_hwnd);
        require(renamed.value("task_id", "") == task_a.value("task_id", "") && renamed.value("context", "") == task_a.value("context", ""), "same task rename changed stable identity");
        require(renamed.value("task_title", "") == "Renamed title", "current Document title rename not reflected");
        write_source(input, "https://claude.ai/chat/identity_bbbb\nRenamed title - Claude Code"); Sleep(300);
        const auto task_b = read_once(app, identity_hwnd);
        require(task_b.value("task_id", "") != task_a.value("task_id", ""), "same title different URL merged tasks");
        write_source(input, "https://claude.ai/epitaxy/local_identity_a\nRenamed title - Claude Code"); Sleep(300);
        const auto returned = read_once(app, identity_hwnd);
        require(returned.value("task_id", "") == task_a.value("task_id", ""), "returning to task A did not recover original identity");
        require(block_text(returned).find("Identity fixture assistant body.") != std::string::npos, "identity metadata broke assistant extraction");
        require(GetForegroundWindow() != identity_hwnd, "task identity reading activated source");
        report["checks"].push_back("native Document ValuePattern task metadata stays stable across rename and A-B-A navigation");
        PostMessageW(identity_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(identity_window.process.hProcess, 2000);
        write_source(input, "https://claude.ai/epitaxy/local_identity_race_a\nNavigation race - Claude Code");
        std::wstring race_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --identity-race";
        Child race_window;
        require(CreateProcessW(fixture.c_str(), race_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &race_window.process), "navigation race fixture could not start");
        HWND race_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !race_hwnd; ++retry) { Sleep(50); race_hwnd = find_window(race_window.process.dwProcessId); }
        require(race_hwnd != nullptr, "navigation race fixture did not appear");
        const auto raced = read_once(app, race_hwnd);
        require(raced.value("identity_pending", false) && raced.at("blocks").empty(), "navigation during extraction attached stale body to task");
        require(raced.value("task_id", "") == "https://claude.ai/epitaxy/local_identity_race_b" && raced.value("context", "") == raced.value("task_id", ""), "navigation race did not emit new task metadata");
        const auto settled = read_once(app, race_hwnd);
        require(!settled.value("identity_pending", true) && settled.value("task_id", "") == raced.value("task_id", "") && !settled.at("blocks").empty(), "stable task body did not resume after navigation");
        require(GetForegroundWindow() != race_hwnd, "navigation guard activated synthetic source");
        report["checks"].push_back("URL recheck discards body on mid-snapshot task navigation and resumes after stable identity");
        PostMessageW(race_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(race_window.process.hProcess, 2000);
        write_source(input, "https://claude.ai/chat/persistent_aaaa\nPersistent A - Claude Code\nVerified A assistant body.");
        Child persistent_window;
        auto persistent_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --identity";
        require(CreateProcessW(fixture.c_str(), persistent_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &persistent_window.process), "persistent identity fixture could not start");
        HWND persistent_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !persistent_hwnd; ++retry) { Sleep(50); persistent_hwnd = find_window(persistent_window.process.dwProcessId); }
        require(persistent_hwnd != nullptr, "persistent identity fixture did not appear");
        {
            ReaderStream reader(app, persistent_hwnd);
            const std::string id_a = "https://claude.ai/chat/persistent_aaaa", id_b = "https://claude.ai/chat/persistent_bbbb";
            auto confirmed = [&](const std::string& id, const std::string& body) {
                return reader.until([&](const json& value) {
                    return value.value("task_id", "") == id && !value.value("identity_pending", true) && block_text(value).find(body) != std::string::npos;
                });
            };
            auto pending_a = [&] {
                auto missing = reader.until([&](const json& value) {
                    return value.value("task_id", "") == id_a && value.value("identity_pending", false);
                });
                require(missing.value("context", "") == id_a && missing.value("identity_quality", "") == "url" && missing.at("blocks").empty(), "missing URL did not retain verified identity with empty body");
                require(missing.value("task_title", "") == "Persistent A", "unknown navigation title replaced verified task title");
                auto repeated = reader.next();
                require(repeated.value("task_id", "") == id_a && repeated.value("identity_pending", false) && repeated.at("blocks").empty(), "repeated missing URL lost identity protection");
            };
            confirmed(id_a, "Verified A assistant body.");
            write_source(input, "\nUnknown navigation - Claude Code\nUNATTRIBUTED B BODY MUST NOT BE PUBLISHED.");
            pending_a();
            write_source(input, "https://claude.ai/chat/persistent_aaaa\nPersistent A - Claude Code\nVerified A body after URL recovery.");
            confirmed(id_a, "Verified A body after URL recovery.");
            report["checks"].push_back("persistent Reader retains verified A identity across repeated missing URL and resumes A without exposing unknown B body");
            write_source(input, "\nUnknown navigation - Claude Code\nUNATTRIBUTED B BODY MUST NOT BE PUBLISHED.");
            pending_a();
            write_source(input, "https://claude.ai/chat/persistent_bbbb\nPersistent B - Claude Code\nVerified B assistant body.");
            auto task_b = confirmed(id_b, "Verified B assistant body.");
            require(task_b.value("context", "") == id_b && task_b.value("identity_quality", "") == "url", "real B URL did not replace cached A identity");
            report["checks"].push_back("persistent Reader discards unknown navigation body until real B URL is verified");
            write_source(input, "https://claude.ai/chat/persistent_aaaa\nPersistent A - Claude Code\nVerified A returned body.");
            confirmed(id_a, "Verified A returned body.");
            write_source(input, "https://claude.ai/chat/persistent_bbbb\nPersistent B - Claude Code\nVerified B direct switch body.");
            confirmed(id_b, "Verified B direct switch body.");
            write_source(input, "https://claude.ai/chat/persistent_aaaa\nPersistent A - Claude Code\nVerified A final return body.");
            confirmed(id_a, "Verified A final return body.");
            require(GetForegroundWindow() != persistent_hwnd, "persistent identity reader activated source");
            report["checks"].push_back("persistent Reader preserves distinct A-B-A URL identities after recovery without activating source");
        }
        PostMessageW(persistent_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(persistent_window.process.hProcess, 2000);
        write_source(input, "https://claude.ai/chat/current_aaaa\nCurrent A - Claude Code\nCurrent A verified body.");
        Child multi_window;
        auto multi_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --identity-multi";
        require(CreateProcessW(fixture.c_str(), multi_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &multi_window.process), "multi-Document fixture could not start");
        HWND multi_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !multi_hwnd; ++retry) { Sleep(50); multi_hwnd = find_window(multi_window.process.dwProcessId); }
        require(multi_hwnd != nullptr, "multi-Document fixture did not appear");
        {
            ReaderStream reader(app, multi_hwnd);
            reader.until([](const json& value) { return value.value("task_id", "") == "https://claude.ai/chat/current_aaaa" && !value.value("identity_pending", true) && block_text(value).find("Current A verified body.") != std::string::npos; });
            write_source(input, "\nUnknown current view - Claude Code\nUNATTRIBUTED B BODY MUST NOT BE PUBLISHED.");
            const auto missing = reader.until([](const json& value) { return value.value("task_id", "") == "https://claude.ai/chat/current_aaaa" && value.value("identity_pending", false); });
            require(missing.at("blocks").empty(), "peer Document URL bypassed known current Document pending guard");
            const auto fresh = read_once(app, multi_hwnd);
            require(fresh.value("identity_pending", false) && fresh.at("blocks").empty() && fresh.value("task_id", "") != "https://claude.ai/chat/background_aaaa", "peer Document URL captured unknown current view without verified cache");
            write_source(input, "https://claude.ai/chat/current_bbbb\nCurrent B - Claude Code\nCurrent B verified body.");
            reader.until([](const json& value) { return value.value("task_id", "") == "https://claude.ai/chat/current_bbbb" && !value.value("identity_pending", true) && block_text(value).find("Current B verified body.") != std::string::npos; });
            require(GetForegroundWindow() != multi_hwnd, "multi-Document reader activated source");
        }
        report["checks"].push_back("current Document never borrows unrelated peer URL or assistant body with or without verified identity cache");
        PostMessageW(multi_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(multi_window.process.hProcess, 2000);
        write_source(input, "\nNested content view - Claude Code\nVerified ancestor URL body.");
        Child parent_window;
        auto parent_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" --identity-parent";
        require(CreateProcessW(fixture.c_str(), parent_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &parent_window.process), "ancestor-Document fixture could not start");
        HWND parent_hwnd = nullptr;
        for (int retry = 0; retry < 50 && !parent_hwnd; ++retry) { Sleep(50); parent_hwnd = find_window(parent_window.process.dwProcessId); }
        require(parent_hwnd != nullptr, "ancestor-Document fixture did not appear");
        const auto inherited = read_once(app, parent_hwnd);
        require(inherited.value("task_id", "") == "https://claude.ai/chat/ancestor_aaaa" && !inherited.value("identity_pending", true) && block_text(inherited).find("Verified ancestor URL body.") != std::string::npos, "nested content Document lost its explicit ancestor URL identity");
        require(GetForegroundWindow() != parent_hwnd, "ancestor-Document reader activated source");
        report["checks"].push_back("nested content Document can use its containing ancestor Document URL without peer identity fallback");
        PostMessageW(parent_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(parent_window.process.hProcess, 2000);
        for (const std::wstring mode : {L"--headings", L"--semantic-headings"}) {
            std::wstring heading_command = quote(fixture.wstring()) + L" " + quote(input.wstring()) + L" " + mode;
            Child heading_window;
            require(CreateProcessW(fixture.c_str(), heading_command.data(), nullptr, nullptr, FALSE, 0, nullptr, fixture.parent_path().c_str(), &startup, &heading_window.process), "heading fixture could not start");
            HWND heading_hwnd = nullptr;
            for (int retry = 0; retry < 50 && !heading_hwnd; ++retry) { Sleep(50); heading_hwnd = find_window(heading_window.process.dwProcessId); }
            require(heading_hwnd != nullptr, "heading fixture window did not appear");
            const auto headings = read_once(app, heading_hwnd);
            require(headings.value("type", "") == "snapshot", "semantic heading snapshot failed");
            const auto heading_text = block_text(headings);
            require(heading_text.find("Opening body paragraph.\n# Heading One\nBody after first heading.") != std::string::npos, "heading did not retain separate semantic line boundaries");
            require(heading_text.find("\n## Heading Two\n### Heading Three\n") != std::string::npos, "explicit UIA heading levels 2/3 not preserved");
            require(heading_text.find("Heading One", heading_text.find("Heading One") + 1) == std::string::npos, "heading Name duplicated its child Text");
            require(heading_text.find("Unknown level remains plain.") != std::string::npos && heading_text.find("# Unknown level") == std::string::npos, "unknown heading level was guessed");
            require(heading_text.find("Unsupported level remains plain.") != std::string::npos && heading_text.find("# Unsupported level") == std::string::npos, "unsupported heading level was guessed");
            require(heading_text.find("Plain short title remains plain.") != std::string::npos && heading_text.find("# Plain short title") == std::string::npos, "plain body was guessed as heading");
            require(heading_text.find("- Progress: Full rich-list body remains complete.") != std::string::npos, "heading extraction lost complete rich ListItem body");
            require(heading_text.find("Progress:", heading_text.find("Progress:") + 1) == std::string::npos, "heading extraction duplicated rich-list partial label");
            require(heading_text.find("Claude responded:") == std::string::npos, "speaker marker leaked into formatted body");
            require(GetForegroundWindow() != heading_hwnd, "semantic heading reading activated source");
            report["checks"].push_back(mode == L"--headings" ? "named assistant preserves explicit UIA heading levels without guessing or duplicating rich list text" : "strict semantic fallback preserves explicit heading levels and complete list body");
            PostMessageW(heading_hwnd, WM_CLOSE, 0, 0); WaitForSingleObject(heading_window.process.hProcess, 2000);
        }
        fs::remove(input);
        report["passed"] = true;
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        report["passed"] = false; report["error"] = error.what();
        std::cout << report.dump(2) << '\n';
        return 1;
    }
}











