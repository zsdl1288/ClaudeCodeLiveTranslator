// Synthetic UI Automation test fixture; no real Claude content or network.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <UIAutomation.h>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
namespace {
HWND body = nullptr;
std::wstring source_file;
std::string last_text;
bool semantic_mode = false;
bool article_mode = false;
bool feed_mode = false, rich_list_mode = false, identity_mode = false;
HWND identity_document = nullptr, identity_background_document = nullptr;
bool identity_multi_mode = false, identity_parent_mode = false;
bool identity_race_mode = false;
bool heading_mode = false, heading_semantic_mode = false;
std::atomic<int> identity_reads{0};
std::mutex identity_mutex;
std::wstring identity_url;
bool stream_list_mode = false, semantic_list_mode = false;
bool response_structure_mode = false, row_structure_mode = false, semantic_structure_mode = false, nested_structure_mode = false;
HWND stream_article = nullptr, stream_heading = nullptr;
bool ready_mode = false, history_mode = false, history_paging_mode = false, history_headless_mode = false, history_feed_semantic_mode = false, history_feed_mode = false, history_semantic_mode = false, history_named_semantic_mode = false, history_semantic_named_mode = false;
HWND history_host = nullptr, history_status = nullptr, history_status_text = nullptr, history_latest = nullptr;
std::string history_stage;
std::wstring decode_utf8(const std::string& value);
void reload_history(const std::string& value) {
    const bool semantic_latest = history_semantic_mode || history_named_semantic_mode;
    const auto split = value.find('\n');
    const auto stage = value.substr(0, split);
    const auto text = split == std::string::npos ? std::wstring{} : decode_utf8(value.substr(split + 1));
    if (history_paging_mode) {
        SetWindowTextW(history_status_text, stage == "stream" ? L"Claude 正在回复" : L"");
        if (body) SetWindowTextW(body, text.c_str());
        if (stage == "stream" || stage == "idle") {
            if (history_latest) { DestroyWindow(history_latest); history_latest = nullptr; }
            history_stage = stage; return;
        }
    }
    if (stage != history_stage) {
        if (history_latest) DestroyWindow(history_latest);
        const bool user = stage == "waiting";
        const wchar_t* kind = semantic_latest ? (user ? L"user-message" : L"assistant-message")
            : history_feed_mode || history_paging_mode ? L"CCLiveFixtureArticle" : L"BUTTON";
        history_latest = CreateWindowExW(0, kind, semantic_latest ? L"" : user ? L"消息 7" : L"Assistant message",
            WS_CHILD | WS_VISIBLE | (semantic_latest || history_feed_mode || history_paging_mode ? 0 : BS_GROUPBOX),
            0, 350, 600, 250, history_host, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!semantic_latest) CreateWindowExW(0, L"STATIC", user ? L"您说：" : L"Claude responded:", WS_CHILD | WS_VISIBLE, 0, 0, 580, 24, history_latest, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (history_semantic_named_mode && stage == "ready") {
            const HWND thinking = CreateWindowExW(0, L"thinking", L"", WS_CHILD | WS_VISIBLE, 0, 26, 580, 28, history_latest, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"CURRENT THINKING ONLY.", WS_CHILD | WS_VISIBLE, 0, 0, 560, 24, thinking, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        body = nullptr;
        if (stage == "tool") {
            const HWND tool = CreateWindowExW(0, L"CCLiveFixtureTool", L"Inspecting project files", WS_CHILD | WS_VISIBLE, 0, 30, 580, 70, history_latest, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Inspecting project files", WS_CHILD | WS_VISIBLE, 0, 0, 560, 24, tool, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"BUTTON", L"Inspecting project files", WS_CHILD | WS_VISIBLE, 0, 25, 560, 24, tool, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND status = CreateWindowExW(0, L"CCLiveFixtureStatus", L"", WS_CHILD | WS_VISIBLE, 0, 104, 580, 28, history_latest, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Inspecting project files", WS_CHILD | WS_VISIBLE, 0, 0, 560, 24, status, nullptr, GetModuleHandleW(nullptr), nullptr);
        } else {
            const HWND response = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE, 0, 30, 580, 160, history_latest, nullptr, GetModuleHandleW(nullptr), nullptr);
            body = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 560, 120, response, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        history_stage = stage;
    }
    if (body) SetWindowTextW(body, stage == "empty" ? L"" : text.c_str());
}
std::wstring decode_utf8(const std::string& value) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
void reload() {
    std::ifstream stream(std::filesystem::path(source_file), std::ios::binary);
    if (!stream) return;
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text == last_text) return;
    last_text = text;
    if (history_mode) { reload_history(text); return; }
    if (identity_mode) {
        const auto split = text.find('\n');
        if (split != std::string::npos) {
            { std::lock_guard<std::mutex> guard(identity_mutex); identity_url = decode_utf8(text.substr(0, split)); }
            const auto body_start = text.find('\n', split + 1);
            auto title = text.substr(split + 1, body_start == std::string::npos ? std::string::npos : body_start - split - 1);
            while (!title.empty() && (title.back() == '\r' || title.back() == '\n')) title.pop_back();
            if (identity_document) SetWindowTextW(identity_document, decode_utf8(title).c_str());
            if (body && body_start != std::string::npos) SetWindowTextW(body, decode_utf8(text.substr(body_start + 1)).c_str());
        }
        return;
    }
    if (body) SetWindowTextW(body, decode_utf8(text).c_str());
    if (stream_heading && text.find("HEADING_READY") != std::string::npos) {
        SetWindowTextW(stream_heading, L"Claude 已回复：HEADING SUMMARY MUST NOT BE TRANSLATED");
        SetWindowTextW(stream_article, L"消息 2");
    }
}
class FeedProvider final : public IRawElementProviderSimple, public IValueProvider {
    std::atomic<ULONG> refs_{1};
    HWND hwnd_;
    bool article_, list_item_, document_, status_, tool_, paragraph_;
    int heading_level_ = -1;
public:
    explicit FeedProvider(HWND hwnd) : hwnd_(hwnd) {
        wchar_t name[64]{}; GetClassNameW(hwnd, name, 64); article_ = std::wstring(name) == L"CCLiveFixtureArticle"; list_item_ = std::wstring(name) == L"CCLiveFixtureRichItem"; document_ = std::wstring(name) == L"CCLiveFixtureDocument";
        status_ = std::wstring(name) == L"CCLiveFixtureStatus";
        tool_ = std::wstring(name) == L"CCLiveFixtureTool";
        paragraph_ = std::wstring(name) == L"CCLiveFixtureParagraph";
        const std::wstring class_name(name);
        const std::wstring heading_prefix = L"CCLiveFixtureHeading";
        if (class_name.find(heading_prefix) == 0 && class_name.size() == heading_prefix.size() + 1) heading_level_ = class_name.back() - L'0';
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) { *result = static_cast<IRawElementProviderSimple*>(this); AddRef(); return S_OK; }
        if (document_ && iid == __uuidof(IValueProvider)) { *result = static_cast<IValueProvider*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override {
        if (!options) return E_POINTER; *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        if (document_ && pattern == UIA_ValuePatternId) { *result = static_cast<IValueProvider*>(this); AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override {
        if (!result) return E_POINTER;
        std::lock_guard<std::mutex> guard(identity_mutex);
        const auto value = hwnd_ == identity_background_document
            ? std::wstring(identity_parent_mode ? L"https://claude.ai/chat/ancestor_aaaa" : L"https://claude.ai/chat/background_aaaa")
            : identity_race_mode && identity_reads.fetch_add(1) > 0 ? std::wstring(L"https://claude.ai/epitaxy/local_identity_race_b") : identity_url;
        *result = SysAllocString(value.c_str()); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override { if (!result) return E_POINTER; *result = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result);
        if (property == UIA_AriaRolePropertyId) { result->vt = VT_BSTR; result->bstrVal = SysAllocString(heading_level_ >= 0 ? L"heading" : document_ ? L"document" : status_ ? L"status" : paragraph_ ? L"paragraph" : tool_ ? L"group" : list_item_ ? L"listitem" : article_ ? L"article" : L"feed"); }
        else if (property == UIA_ControlTypePropertyId) { result->vt = VT_I4; result->lVal = heading_level_ >= 0 ? UIA_TextControlTypeId : document_ ? UIA_DocumentControlTypeId : status_ ? UIA_StatusBarControlTypeId : list_item_ ? UIA_ListItemControlTypeId : UIA_GroupControlTypeId; }
        else if (property == UIA_NamePropertyId && (list_item_ || document_ || status_ || tool_ || heading_level_ >= 0)) { wchar_t text[1024]{}; GetWindowTextW(hwnd_, text, 1024); result->vt = VT_BSTR; result->bstrVal = SysAllocString(text); }
        else if (property == UIA_HeadingLevelPropertyId && heading_level_ >= 0) { result->vt = VT_I4; result->lVal = 80050 + heading_level_; }
        else if (property == UIA_IsControlElementPropertyId || property == UIA_IsContentElementPropertyId) { result->vt = VT_BOOL; result->boolVal = VARIANT_TRUE; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override { return UiaHostProviderFromHwnd(hwnd_, result); }
};
LRESULT CALLBACK feed_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* provider = reinterpret_cast<FeedProvider*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_GETOBJECT && static_cast<LONG>(lparam) == UiaRootObjectId) {
        if (!provider) { provider = new FeedProvider(hwnd); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(provider)); }
        return UiaReturnRawElementProvider(hwnd, wparam, lparam, provider);
    }
    if (message == WM_NCDESTROY && provider) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); provider->Release(); }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CREATE) {
        if (history_mode) {
            if (history_paging_mode) {
                history_status = CreateWindowExW(0, L"CCLiveFixtureStatus", L"", WS_CHILD | WS_VISIBLE, 0, 0, 600, 28, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                history_status_text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 580, 24, history_status, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            history_host = history_feed_mode || history_paging_mode
                ? CreateWindowExW(0, L"CCLiveFixtureFeed", L"Synthetic history", WS_CHILD | WS_VISIBLE, 0, 30, 620, 700, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr) : hwnd;
            if (history_headless_mode) {
                const HWND previous_user = CreateWindowExW(0, L"CCLiveFixtureArticle", L"消息 4", WS_CHILD | WS_VISIBLE, 0, 0, 600, 28, history_host, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"您说：EARLIER USER", WS_CHILD | WS_VISIBLE, 0, 0, 580, 24, previous_user, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            const bool semantic_history = history_semantic_mode || history_semantic_named_mode;
            const wchar_t* old_class = semantic_history ? L"assistant-message" : history_feed_mode || history_paging_mode ? L"CCLiveFixtureArticle" : L"BUTTON";
            const HWND previous = CreateWindowExW(0, old_class, semantic_history || history_headless_mode ? L"" : L"Assistant message", WS_CHILD | WS_VISIBLE | (semantic_history || history_feed_mode || history_paging_mode ? 0 : BS_GROUPBOX), 0, 0, 600, 120, history_host, nullptr, GetModuleHandleW(nullptr), nullptr);
            if ((!semantic_history || history_feed_semantic_mode) && !history_headless_mode) CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE, 0, 0, 580, 24, previous, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND response = CreateWindowExW(0, history_headless_mode ? L"group/message-row" : L"font-claude-response", L"", WS_CHILD | WS_VISIBLE, 0, 28, 580, 80, previous, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"HISTORICAL ANSWER MUST NOT BE TRANSLATED.", WS_CHILD | WS_VISIBLE, 0, 0, 560, 64, response, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (history_paging_mode) {
                // ControlView can flatten these raw generic wrappers. The paging
                // boundary is outside every message/article, within this Feed.
                const HWND wrapper = CreateWindowExW(0, L"CCLiveFixtureBlock", L"", WS_CHILD | WS_VISIBLE, 0, 130, 600, 180, history_host, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"BUTTON", history_headless_mode ? L"Load later messages" : L"加载后续消息", WS_CHILD | WS_VISIBLE, 0, 0, 580, 26, wrapper, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND paragraph = CreateWindowExW(0, L"CCLiveFixtureParagraph", L"", WS_CHILD | WS_VISIBLE, 0, 32, 580, 130, wrapper, nullptr, GetModuleHandleW(nullptr), nullptr);
                body = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 560, 100, paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            reload(); SetTimer(hwnd, 1, 100, nullptr); return 0;
        }
        if (response_structure_mode) {
            HWND response = nullptr;
            if (row_structure_mode) {
                const HWND feed = CreateWindowExW(0, L"CCLiveFixtureFeed", L"Conversation", WS_CHILD | WS_VISIBLE, 0, 0, 630, 1000, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND article = CreateWindowExW(0, L"article", L"消息 2", WS_CHILD | WS_VISIBLE, 0, 0, 620, 40, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE, 0, 0, 600, 30, article, nullptr, GetModuleHandleW(nullptr), nullptr);
                response = CreateWindowExW(0, L"CCLiveFixtureBlock", L"", WS_CHILD | WS_VISIBLE, 0, 45, 620, 920, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
            } else {
                const HWND assistant = CreateWindowExW(0, semantic_structure_mode ? L"assistant-message" : L"BUTTON", semantic_structure_mode ? L"" : L"Assistant message", WS_CHILD | WS_VISIBLE | (semantic_structure_mode ? 0 : BS_GROUPBOX), 0, 0, 630, 1000, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (!semantic_structure_mode) CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE, 0, 0, 600, 30, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
                response = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE, 0, 32, 620, 940, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            if (nested_structure_mode) response = CreateWindowExW(0, L"CCLiveFixtureBlock", L"", WS_CHILD | WS_VISIBLE, 0, 0, 610, 930, response, nullptr, GetModuleHandleW(nullptr), nullptr);
            auto paragraph = [&](int y) {
                const HWND row = row_structure_mode ? CreateWindowExW(0, L"group/message-row", L"", WS_CHILD | WS_VISIBLE, 0, y, 610, 120, response, nullptr, GetModuleHandleW(nullptr), nullptr) : response;
                // Generic raw groups stand in for P nodes flattened by ControlView.
                return CreateWindowExW(0, nested_structure_mode ? L"CCLiveFixtureParagraph" : L"CCLiveFixtureBlock", L"", WS_CHILD | WS_VISIBLE, 0, row_structure_mode ? 0 : y, 600, 110, row, nullptr, GetModuleHandleW(nullptr), nullptr);
            };
            auto text = [&](HWND parent, const wchar_t* value, int x = 0, int y = 0) {
                return CreateWindowExW(0, L"STATIC", value, WS_CHILD | WS_VISIBLE | SS_NOPREFIX, x, y, 590, 24, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            };
            text(paragraph(0), L"First paragraph has no sentence-ending punctuation");
            const HWND inline_p = paragraph(125);
            text(inline_p, L"An ");
            const HWND strong = CreateWindowExW(0, L"CCLiveFixtureInline", L"", WS_CHILD | WS_VISIBLE, 0, 25, 590, 24, inline_p, nullptr, GetModuleHandleW(nullptr), nullptr);
            text(strong, L"im"); text(inline_p, L"plementation", 0, 50); text(inline_p, L" ", 0, 75);
            const HWND code = CreateWindowExW(0, L"CCLiveFixtureInline", L"", WS_CHILD | WS_VISIBLE, 0, 76, 590, 24, inline_p, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND code_text = text(code, L"path");
            const HFONT mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Consolas");
            SendMessageW(code_text, WM_SETFONT, reinterpret_cast<WPARAM>(mono), FALSE);
            text(inline_p, L" remains intact.", 0, 100);
            const HWND chinese = paragraph(250);
            text(chinese, L"中文"); text(chinese, L"行内", 0, 25); text(chinese, L"文字", 0, 50);
            text(paragraph(375), L"I ran 3 commands to verify the project. 正文解释：已运行 3 个命令来验证项目。");
            const HWND tool = CreateWindowExW(0, L"CCLiveFixtureTool", L"已运行 2 个命令, 已创建 fixture.txt", WS_CHILD | WS_VISIBLE, 0, 500, 600, 100, response, nullptr, GetModuleHandleW(nullptr), nullptr);
            text(tool, L"已运行 2 个命令"); text(tool, L", 已创建", 0, 25);
            CreateWindowExW(0, L"BUTTON", L"fixture.txt", WS_CHILD | WS_VISIBLE, 0, 50, 200, 24, tool, nullptr, GetModuleHandleW(nullptr), nullptr);
            text(tool, L"新增 30，删除 0", 220, 50);
            CreateWindowExW(0, L"BUTTON", L"已运行 2 个命令, 已创建 fixture.txt", WS_CHILD | WS_VISIBLE, 0, 75, 580, 24, tool, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND status = CreateWindowExW(0, L"CCLiveFixtureStatus", L"", WS_CHILD | WS_VISIBLE, 0, 605, 600, 24, response, nullptr, GetModuleHandleW(nullptr), nullptr);
            text(status, L"已运行 2 个命令, 已创建 fixture.txt");
            const HWND action = CreateWindowExW(0, L"CCLiveFixtureTool", L"Inspecting project files", WS_CHILD | WS_VISIBLE, 0, 635, 600, 60, response, nullptr, GetModuleHandleW(nullptr), nullptr);
            text(action, L"Inspecting project files");
            CreateWindowExW(0, L"BUTTON", L"Inspecting project files", WS_CHILD | WS_VISIBLE, 0, 28, 580, 24, action, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND action_status = CreateWindowExW(0, L"CCLiveFixtureStatus", L"", WS_CHILD | WS_VISIBLE, 0, 700, 600, 24, response, nullptr, GetModuleHandleW(nullptr), nullptr);
            text(action_status, L"Inspecting project files");
            text(paragraph(730), L"Final paragraph without punctuation");
            return 0;
        }
        if (heading_mode) {
            const HWND assistant = CreateWindowExW(0, heading_semantic_mode ? L"assistant-message" : L"BUTTON", heading_semantic_mode ? L"" : L"Assistant message",
                WS_CHILD | WS_VISIBLE | (heading_semantic_mode ? 0 : BS_GROUPBOX), 8, 8, 620, 600, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!heading_semantic_mode) CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE, 8, 20, 590, 24, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Opening body paragraph.", WS_CHILD | WS_VISIBLE, 8, 50, 590, 24, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND heading = CreateWindowExW(0, L"CCLiveFixtureHeading1", L"Heading One", WS_CHILD | WS_VISIBLE, 8, 80, 590, 30, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Heading One", WS_CHILD | WS_VISIBLE, 0, 0, 580, 24, heading, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Body after first heading.", WS_CHILD | WS_VISIBLE, 8, 116, 590, 24, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"CCLiveFixtureHeading2", L"Heading Two", WS_CHILD | WS_VISIBLE, 8, 148, 590, 30, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"CCLiveFixtureHeading3", L"Heading Three", WS_CHILD | WS_VISIBLE, 8, 184, 590, 30, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"CCLiveFixtureHeading0", L"Unknown level remains plain.", WS_CHILD | WS_VISIBLE, 8, 220, 590, 30, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"CCLiveFixtureHeading4", L"Unsupported level remains plain.", WS_CHILD | WS_VISIBLE, 8, 256, 590, 30, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Plain short title remains plain.", WS_CHILD | WS_VISIBLE, 8, 292, 590, 24, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND item = CreateWindowExW(0, L"CCLiveFixtureRichItem", L"Progress: Full rich-list body remains complete.", WS_CHILD | WS_VISIBLE, 8, 324, 590, 54, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Progress:", WS_CHILD | WS_VISIBLE, 0, 0, 580, 24, item, nullptr, GetModuleHandleW(nullptr), nullptr);
            return 0;
        }
        if (identity_mode) {
            if (identity_multi_mode || identity_parent_mode)
                identity_background_document = CreateWindowExW(0, L"CCLiveFixtureDocument", L"Separate URL document", WS_CHILD | WS_VISIBLE,
                    0, 0, 630, 410, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            identity_document = CreateWindowExW(0, L"CCLiveFixtureDocument", L"Identity fixture - Claude Code", WS_CHILD | WS_VISIBLE,
                8, 8, 620, 400, identity_parent_mode ? identity_background_document : hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND content = identity_parent_mode
                ? CreateWindowExW(0, L"CCLiveFixtureFeed", L"Current content", WS_CHILD | WS_VISIBLE, 0, 0, 610, 390, identity_document, nullptr, GetModuleHandleW(nullptr), nullptr)
                : identity_document;
            if (identity_multi_mode) {
                // The current document outranks its peer via its own feed anchors,
                // but its assistant uses the named-message extraction path.
                CreateWindowExW(0, L"CCLiveFixtureFeed", L"Current feed one", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, identity_document, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"CCLiveFixtureFeed", L"Current feed two", WS_CHILD | WS_VISIBLE, 2, 0, 1, 1, identity_document, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND peer_feed = CreateWindowExW(0, L"CCLiveFixtureFeed", L"Peer content", WS_CHILD | WS_VISIBLE, 0, 0, 600, 380, identity_background_document, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND peer_assistant = CreateWindowExW(0, L"BUTTON", L"Assistant message", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 8, 8, 580, 300, peer_feed, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE, 8, 24, 550, 30, peer_assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"PEER B BODY MUST NOT BE TRANSLATED.", WS_CHILD | WS_VISIBLE, 8, 58, 550, 200, peer_assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            const HWND assistant = CreateWindowExW(0, L"BUTTON", L"Assistant message", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                8, 8, 590, 300, content, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE, 8, 24, 550, 30, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            body = CreateWindowExW(0, L"STATIC", L"Identity fixture assistant body.", WS_CHILD | WS_VISIBLE, 8, 58, 550, 200, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            reload(); SetTimer(hwnd, 1, 100, nullptr); return 0;
        }
        if (rich_list_mode) {
            const HWND assistant = CreateWindowExW(0, L"BUTTON", L"Assistant message", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                8, 8, 620, 600, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE, 8, 20, 590, 24, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND first = CreateWindowExW(0, L"CCLiveFixtureRichItem", L"Progress: Media Encoder now resumes the export successfully.", WS_CHILD | WS_VISIBLE,
                8, 50, 590, 60, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Progress:", WS_CHILD | WS_VISIBLE, 0, 0, 550, 30, first, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND second = CreateWindowExW(0, L"CCLiveFixtureRichItem", L"New problem: The output path requires permission.", WS_CHILD | WS_VISIBLE,
                8, 120, 590, 60, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"New problem:", WS_CHILD | WS_VISIBLE, 0, 0, 550, 30, second, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND auxiliary = CreateWindowExW(0, L"CCLiveFixtureRichItem", L"Safe label. AUXILIARY MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE,
                8, 190, 590, 80, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Safe label.", WS_CHILD | WS_VISIBLE, 0, 0, 550, 30, auxiliary, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"BUTTON", L"AUXILIARY MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE, 0, 32, 550, 30, auxiliary, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND code_item = CreateWindowExW(0, L"CCLiveFixtureRichItem", L"CODE AGGREGATE MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE,
                8, 280, 590, 100, assistant, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND code = CreateWindowExW(0, L"group/copy", L"Sample code", WS_CHILD | WS_VISIBLE,
                0, 0, 550, 70, code_item, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"CODE CHILD MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE, 0, 0, 530, 30, code, nullptr, GetModuleHandleW(nullptr), nullptr);
            return 0;
        }
        if (stream_list_mode || semantic_list_mode) {
            auto list = [](HWND parent, int y, bool safe) {
                const HWND control = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                    8, y, 560, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(safe ? L"Streaming list item one must be translated." : L"USER LIST MUST NOT BE TRANSLATED"));
                if (safe) {
                    SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Streaming list item two must be translated."));
                    SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Short list tail."));
                }
                return control;
            };
            if (semantic_list_mode) {
                const HWND safe = CreateWindowExW(0, L"assistant-message", L"", WS_CHILD | WS_VISIBLE, 8, 8, 600, 200, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                list(safe, 8, true);
                if (!ready_mode) {
                    const HWND user = CreateWindowExW(0, L"user-message", L"", WS_CHILD | WS_VISIBLE, 8, 220, 600, 200, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                    const HWND misleading = CreateWindowExW(0, L"assistant-message", L"", WS_CHILD | WS_VISIBLE, 0, 0, 590, 150, user, nullptr, GetModuleHandleW(nullptr), nullptr);
                    list(misleading, 8, false);
                }
                list(hwnd, 450, false);
            } else {
                const HWND feed = CreateWindowExW(0, L"CCLiveFixtureFeed", L"", WS_CHILD | WS_VISIBLE, 8, 8, 600, 650, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND user = CreateWindowExW(0, L"CCLiveFixtureArticle", L"消息 1", WS_CHILD | WS_VISIBLE, 8, 8, 580, 160, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"您说：USER HEADING SUMMARY", WS_CHILD | WS_VISIBLE, 0, 0, 550, 24, user, nullptr, GetModuleHandleW(nullptr), nullptr);
                list(user, 28, false);
                stream_article = CreateWindowExW(0, L"CCLiveFixtureArticle", L"", WS_CHILD | WS_VISIBLE, 8, 180, 580, 260, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
                stream_heading = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 550, 24, stream_article, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND row = CreateWindowExW(0, L"group/message-row", L"", WS_CHILD | WS_VISIBLE, 0, 28, 570, 220, stream_article, nullptr, GetModuleHandleW(nullptr), nullptr);
                list(row, 0, true);
                const HWND input = CreateWindowExW(0, L"EDIT", L"COMPOSER MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE, 0, 110, 550, 50, row, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"INPUT CHILD MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE, 0, 0, 500, 30, input, nullptr, GetModuleHandleW(nullptr), nullptr);
                list(hwnd, 680, false);
                reload(); SetTimer(hwnd, 1, 100, nullptr);
            }
            return 0;
        }
        if (feed_mode) {
            const HWND feed = CreateWindowExW(0, L"CCLiveFixtureFeed", L"Synthetic conversation", WS_CHILD | WS_VISIBLE,
                8, 8, 620, 700, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND old_article = CreateWindowExW(0, L"article", L"消息 86", WS_CHILD | WS_VISIBLE,
                8, 8, 600, 60, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Claude 已回复：OLD HEADING SUMMARY", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 24, old_article, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"PREVIOUS ANSWER MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 26, 590, 24, old_article, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND article = CreateWindowExW(0, L"article", L"消息 88", WS_CHILD | WS_VISIBLE,
                8, 80, 600, 60, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Claude 已回复：HEADING SUMMARY MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 24, article, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"A short first tool title.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 26, 590, 24, article, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND first_row = CreateWindowExW(0, L"group/message-row", L"", WS_CHILD | WS_VISIBLE,
                8, 150, 600, 100, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
            body = CreateWindowExW(0, L"STATIC", L"Reading the project files.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 100, first_row, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND second_row = CreateWindowExW(0, L"group/message-row", L"", WS_CHILD | WS_VISIBLE,
                8, 260, 600, 60, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Second body paragraph must be preserved.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 60, second_row, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND last_row = CreateWindowExW(0, L"group/message-row", L"", WS_CHILD | WS_VISIBLE,
                8, 330, 600, 40, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Short tail.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 40, last_row, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!ready_mode) {
                const HWND user_article = CreateWindowExW(0, L"article", L"消息 89", WS_CHILD | WS_VISIBLE,
                    8, 380, 600, 60, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"您说：USER HEADING SUMMARY MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    0, 0, 590, 24, user_article, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"USER INPUT MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    0, 26, 590, 24, user_article, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND user_row = CreateWindowExW(0, L"group/message-row", L"", WS_CHILD | WS_VISIBLE,
                    8, 450, 600, 40, feed, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"USER SIBLING ROW MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    0, 0, 590, 40, user_row, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            CreateWindowExW(0, L"STATIC", L"SIDEBAR TEXT MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                8, 740, 590, 40, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            reload(); SetTimer(hwnd, 1, 100, nullptr); return 0;
        }
        if (article_mode) {
            const HWND old_article = CreateWindowExW(0, L"article", L"消息 86", WS_CHILD | WS_VISIBLE,
                8, 500, 620, 60, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Claude 已回复：OLD HEADING SUMMARY", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                8, 8, 590, 24, old_article, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"PREVIOUS ANSWER MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                8, 32, 590, 24, old_article, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND article = CreateWindowExW(0, L"article", L"消息 88", WS_CHILD | WS_VISIBLE,
                8, 8, 620, 440, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Claude 已回复：HEADING SUMMARY MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                8, 8, 590, 24, article, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND first_paragraph = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE,
                8, 40, 600, 160, article, nullptr, GetModuleHandleW(nullptr), nullptr);
            body = CreateWindowExW(0, L"STATIC", L"Reading the project files.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 160, first_paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND second_paragraph = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE,
                8, 210, 600, 50, article, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Second body paragraph must be preserved.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 40, second_paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L" Inline continuation stays in the same paragraph.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 24, 590, 24, second_paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND last_paragraph = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE,
                8, 270, 600, 50, article, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Short tail.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 40, last_paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!ready_mode) {
                const HWND user_article = CreateWindowExW(0, L"article", L"消息 89", WS_CHILD | WS_VISIBLE,
                    8, 570, 620, 90, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"您说：USER HEADING SUMMARY MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    8, 8, 590, 24, user_article, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND misleading = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE,
                    8, 40, 600, 40, user_article, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"USER INPUT MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    0, 0, 590, 30, misleading, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            reload(); SetTimer(hwnd, 1, 100, nullptr); return 0;
        }
        if (semantic_mode) {
            const HWND old_container = CreateWindowExW(0, L"assistant-message", L"Previous response", WS_CHILD | WS_VISIBLE,
                8, 350, 620, 50, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"PREVIOUS ANSWER MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                8, 8, 600, 30, old_container, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND container = CreateWindowExW(0, L"assistant-message", L"Latest response", WS_CHILD | WS_VISIBLE,
                8, 8, 620, 330, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND first_semantic_paragraph = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE,
                8, 8, 600, 210, container, nullptr, GetModuleHandleW(nullptr), nullptr);
            body = CreateWindowExW(0, L"STATIC", L"Reading the project files.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 200, first_semantic_paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND next_semantic_paragraph = CreateWindowExW(0, L"font-claude-response", L"", WS_CHILD | WS_VISIBLE,
                8, 230, 600, 70, container, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"Second semantic paragraph.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 590, 25, next_semantic_paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L" Inline continuation in that paragraph.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 30, 590, 25, next_semantic_paragraph, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!ready_mode) {
                const HWND user_container = CreateWindowExW(0, L"user-message", L"User content", WS_CHILD | WS_VISIBLE,
                    8, 420, 620, 50, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                const HWND misleading = CreateWindowExW(0, L"assistant-message", L"Nested styling", WS_CHILD | WS_VISIBLE,
                    0, 0, 600, 40, user_container, nullptr, GetModuleHandleW(nullptr), nullptr);
                CreateWindowExW(0, L"STATIC", L"USER INPUT MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    8, 8, 580, 30, misleading, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            reload();
            SetTimer(hwnd, 1, 100, nullptr);
            return 0;
        }
        const HWND old_group = CreateWindowExW(0, L"BUTTON", L"Assistant message", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            8, 350, 620, 60, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"STATIC", L"PREVIOUS ANSWER MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
            12, 24, 590, 24, old_group, nullptr, GetModuleHandleW(nullptr), nullptr);
        const HWND group = CreateWindowExW(0, L"BUTTON", L"Assistant message", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            8, 8, 620, 330, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"STATIC", L"Claude responded:", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
            12, 24, 560, 24, group, nullptr, GetModuleHandleW(nullptr), nullptr);
        body = CreateWindowExW(0, L"STATIC", L"Reading the project files.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
            12, 52, 590, 220, group, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!ready_mode) {
            const HWND user_group = CreateWindowExW(0, L"BUTTON", L"Message 3 of 3", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                8, 420, 620, 60, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"STATIC", L"USER INPUT MUST NOT BE TRANSLATED", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                12, 24, 590, 24, user_group, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        reload();
        SetTimer(hwnd, 1, 100, nullptr);
        return 0;
    }
    if (message == WM_TIMER) { reload(); return 0; }
    if (message == WM_DESTROY) { KillTimer(hwnd, 1); PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args && count > 1) source_file = args[1];
    for (int index = 2; args && index < count; ++index) if (std::wstring(args[index]) == L"--ready") ready_mode = true;
    if (args && count > 2) history_headless_mode = std::wstring(args[2]) == L"--paging-headless-history";
    if (args && count > 2) history_paging_mode = history_headless_mode || std::wstring(args[2]) == L"--paging-history";
    if (args && count > 2) history_feed_semantic_mode = std::wstring(args[2]) == L"--feed-semantic-history";
    if (args && count > 2) history_feed_mode = history_feed_semantic_mode || std::wstring(args[2]) == L"--feed-history";
    if (args && count > 2) history_semantic_mode = history_feed_semantic_mode || std::wstring(args[2]) == L"--semantic-history";
    if (args && count > 2) history_named_semantic_mode = std::wstring(args[2]) == L"--named-semantic-history";
    if (args && count > 2) history_semantic_named_mode = std::wstring(args[2]) == L"--semantic-named-history";
    if (args && count > 2) history_mode = history_paging_mode || history_feed_mode || history_semantic_mode || history_named_semantic_mode || history_semantic_named_mode || std::wstring(args[2]) == L"--named-history";
    if (args && count > 2) row_structure_mode = std::wstring(args[2]) == L"--row-structure";
    if (args && count > 2) semantic_structure_mode = std::wstring(args[2]) == L"--semantic-response-structure";
    if (args && count > 2) nested_structure_mode = std::wstring(args[2]) == L"--nested-response-structure";
    if (args && count > 2) response_structure_mode = std::wstring(args[2]) == L"--response-structure" || row_structure_mode || semantic_structure_mode || nested_structure_mode;
    if (args && count > 2) semantic_mode = std::wstring(args[2]) == L"--semantic";
    if (args && count > 2) article_mode = std::wstring(args[2]) == L"--article";
    if (args && count > 2) feed_mode = std::wstring(args[2]) == L"--feed";
    if (args && count > 2) rich_list_mode = std::wstring(args[2]) == L"--rich-list";
    if (args && count > 2) identity_multi_mode = std::wstring(args[2]) == L"--identity-multi";
    if (args && count > 2) identity_parent_mode = std::wstring(args[2]) == L"--identity-parent";
    if (args && count > 2) identity_mode = std::wstring(args[2]) == L"--identity" || std::wstring(args[2]) == L"--identity-race" || identity_multi_mode || identity_parent_mode;
    if (args && count > 2) identity_race_mode = std::wstring(args[2]) == L"--identity-race";
    if (args && count > 2) stream_list_mode = std::wstring(args[2]) == L"--stream-list";
    if (args && count > 2) semantic_list_mode = std::wstring(args[2]) == L"--semantic-list";
    if (args && count > 2) heading_mode = std::wstring(args[2]) == L"--headings" || std::wstring(args[2]) == L"--semantic-headings";
    if (args && count > 2) heading_semantic_mode = std::wstring(args[2]) == L"--semantic-headings";
    if (args) LocalFree(args);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"CCLiveFixtureWindow";
    if (!RegisterClassW(&window_class)) return 1;
    WNDCLASSW semantic_class{};
    semantic_class.lpfnWndProc = DefWindowProcW;
    semantic_class.hInstance = instance;
    semantic_class.lpszClassName = L"assistant-message";
    if (!RegisterClassW(&semantic_class)) return 3;
    semantic_class.lpszClassName = L"thinking";
    if (!RegisterClassW(&semantic_class)) return 15;
    semantic_class.lpszClassName = L"user-message";
    if (!RegisterClassW(&semantic_class)) return 4;
    semantic_class.lpszClassName = L"article";
    if (!RegisterClassW(&semantic_class)) return 5;
    semantic_class.lpszClassName = L"font-claude-response";
    if (!RegisterClassW(&semantic_class)) return 6;
    semantic_class.lpszClassName = L"group/message-row";
    if (!RegisterClassW(&semantic_class)) return 7;
    semantic_class.lpszClassName = L"CCLiveFixtureFeed";
    semantic_class.lpfnWndProc = feed_proc;
    if (!RegisterClassW(&semantic_class)) return 8;
    semantic_class.lpszClassName = L"CCLiveFixtureArticle";
    if (!RegisterClassW(&semantic_class)) return 9;
    semantic_class.lpszClassName = L"CCLiveFixtureRichItem";
    if (!RegisterClassW(&semantic_class)) return 10;
    semantic_class.lpszClassName = L"group/copy";
    semantic_class.lpfnWndProc = DefWindowProcW;
    if (!RegisterClassW(&semantic_class)) return 11;
    semantic_class.lpszClassName = L"CCLiveFixtureBlock";
    if (!RegisterClassW(&semantic_class)) return 14;
    semantic_class.lpszClassName = L"CCLiveFixtureInline";
    if (!RegisterClassW(&semantic_class)) return 15;
    semantic_class.lpszClassName = L"CCLiveFixtureStatus";
    semantic_class.lpfnWndProc = feed_proc;
    if (!RegisterClassW(&semantic_class)) return 16;
    semantic_class.lpszClassName = L"CCLiveFixtureTool";
    if (!RegisterClassW(&semantic_class)) return 17;
    semantic_class.lpszClassName = L"CCLiveFixtureParagraph";
    if (!RegisterClassW(&semantic_class)) return 18;
    semantic_class.lpszClassName = L"CCLiveFixtureDocument";
    semantic_class.lpfnWndProc = feed_proc;
    if (!RegisterClassW(&semantic_class)) return 12;
    for (int level = 0; level <= 4; ++level) {
        const auto name = L"CCLiveFixtureHeading" + std::to_wstring(level);
        semantic_class.lpszClassName = name.c_str();
        if (!RegisterClassW(&semantic_class)) return 13;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, window_class.lpszClassName,
        L"CCLive synthetic accessibility fixture", WS_OVERLAPPEDWINDOW,
        -20000, -20000, 660, 400, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}










