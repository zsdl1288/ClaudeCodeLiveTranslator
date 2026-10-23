// Read-only output extraction adapted from yxue301603/claude-desktop-translator (MIT).
// Copyright (c) 2026 yxue301603. See third_party/desktop-translator-LICENSE.
// No focus, input, invocation, expansion, scroll, clipboard, screenshots or client writes.
#include "reader.hpp"
#include "task_identity.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <UIAutomation.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <map>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cclive {
namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

template<class T> class Ptr {
    T* p_ = nullptr;
public:
    Ptr() = default;
    Ptr(const Ptr& other) : p_(other.p_) { if (p_) p_->AddRef(); }
    Ptr(Ptr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ~Ptr() { if (p_) p_->Release(); }
    Ptr& operator=(Ptr other) noexcept { std::swap(p_, other.p_); return *this; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** out() { if (p_) { p_->Release(); p_ = nullptr; } return &p_; }
};
struct UiaError : std::runtime_error {
    HRESULT hr;
    explicit UiaError(HRESULT value) : std::runtime_error("UI Automation read failed"), hr(value) {}
};
void check(HRESULT hr) { if (FAILED(hr)) throw UiaError(hr); }
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
bool blank(const std::wstring& value) {
    return std::all_of(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(c) != 0; });
}
bool has_class(const std::wstring& value, const std::wstring& wanted) {
    const auto padded = L" " + value + L" ";
    return padded.find(L" " + wanted + L" ") != std::wstring::npos;
}
const auto flags = std::regex_constants::icase | std::regex_constants::optimize;
const std::wregex message_name(LR"(^(?:Message \d+(?: of \d+)?|消息\s*\d+(?:\s*(?:/|共)\s*\d+)?|Assistant message|Claude response|助手消息|助手回答)$)", flags);
const std::wregex assistant_marker(LR"(^claude\s*(?:responded|已回复|回复)\s*[:：])", flags);
const std::wregex user_marker(LR"(^(?:you said|您说|你说)\s*[:：])", flags);
const std::wregex strong_assistant_semantic(LR"(assistant[-_/ ](?:message|content|response))", flags);
const std::wregex assistant_name(LR"(assistant|助手|claude response)", flags);
const std::wregex thinking_title(LR"(^(?:Thought(?: for)?\b|Thinking\b|Reasoning\b|Show thinking\b|Hide thinking\b|思考|已思考))", flags);
const std::wregex plain_thinking_title(LR"(^(?:Thought\b|Thinking\b|Reasoning\b|思考|已思考))", flags);
const std::wregex auxiliary_class(LR"(\bsr-only\b|promotion|upsell)", flags);
const std::wregex thinking_semantic(LR"((?:^|[-_ /])(?:thinking|reasoning|thoughts?)(?:$|[-_ /]))", flags);
const std::wregex intermediate_semantic(LR"(tool[-_/](?:result|output|use)|intermediate|progress-message)", flags);
const std::wregex excluded_semantic(LR"(composer|navigation|sidebar|user[-_/ ]message|user[-_/ ]content)", flags);
const std::wregex assistant_semantic(LR"(font-claude-response|assistant[-_/ ](?:message|content|response))", flags);
const std::wregex mono_font(LR"(mono|consolas|courier)", flags);
const std::wregex promotional_heading(LR"(^(?:Claude Code is built into this app|Try Claude Code|Meet Claude Code|Upgrade to Pro|Upgrade to Max)$)", flags);
bool rx(const std::wstring& value, const std::wregex& expression) { return std::regex_search(value, expression); }
std::wstring window_title(HWND hwnd) {
    wchar_t text[512]{};
    GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
    return text;
}
DWORD check_window(HWND hwnd, bool fixture) {
    if (!IsWindow(hwnd)) throw std::runtime_error("Claude 窗口已关闭，请刷新窗口列表。");
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) throw std::runtime_error("暂时无法验证 Claude 窗口。");
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!ok) throw std::runtime_error("暂时无法验证 Claude 窗口。");
    std::wstring name(path, length);
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    name = lower(name);
    if (name != L"claude.exe" && !(fixture && name == L"cclivefixture.exe"))
        throw std::runtime_error("目标不是 Claude 客户端，已停止读取。");
    return pid;
}
bool write_line(const json& value) {
    const std::string line = value.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    while (offset < line.size()) {
        DWORD written = 0;
        if (!WriteFile(output, line.data() + offset, static_cast<DWORD>(line.size() - offset), &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}
std::wstring take_bstr(BSTR value) {
    std::wstring result;
    if (value) result.assign(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}
std::string task_digest(const std::string& input) {
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) throw std::runtime_error("Task identity hash unavailable");
    const bool created = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    std::string result;
    if (created && CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()), static_cast<DWORD>(input.size()), 0)) {
        BYTE bytes[32]{}; DWORD size = sizeof(bytes);
        if (CryptGetHashParam(hash, HP_HASHVAL, bytes, &size, 0)) {
            const char* hex = "0123456789abcdef";
            for (DWORD index = 0; index < size; ++index) { result += hex[bytes[index] >> 4]; result += hex[bytes[index] & 15]; }
        }
    }
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (result.empty()) throw std::runtime_error("Task identity hash unavailable");
    return result;
}
struct TaskMetadata { std::string id, title, quality, document_id, document_key, content_document_id; std::size_t url_candidates = 0; bool identity_pending = false; };
struct Node {
    Ptr<IUIAutomationElement> element;
    std::wstring name, class_name, automation_id, aria_role, localized_type;
    CONTROLTYPEID type = 0;
    int heading_level = 0;
    std::string id;
};
using N = std::shared_ptr<Node>;
bool is_assistant_marker(const N& node) { return rx(node->name, assistant_marker); }
bool is_user_marker(const N& node) { return rx(node->name, user_marker); }
bool is_status(const N& node) { return node->type == UIA_StatusBarControlTypeId || lower(node->aria_role) == L"status"; }
bool response_boundary(const N& node) {
    return node->type != UIA_TextControlTypeId &&
        (has_class(node->class_name, L"font-claude-response") || has_class(node->class_name, L"group/message-row"));
}
std::wstring summary_label(const std::wstring& value) {
    static const std::wregex spaces(L"\\s+");
    auto result = std::regex_replace(value, spaces, L" ");
    const auto first = result.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    return result.substr(first, result.find_last_not_of(L' ') - first + 1);
}
bool is_message(const N& node) {
    // Article is exposed by Chromium as Group, Pane or Custom depending on version.
    // Its exact localized message number remains mandatory; never read arbitrary panes.
    return rx(node->name, message_name) && (node->type == UIA_GroupControlTypeId ||
        node->type == UIA_CustomControlTypeId || node->type == UIA_PaneControlTypeId || lower(node->aria_role) == L"article");
}
std::string safe_role(const std::wstring& role) {
    const auto value = lower(role);
    for (const wchar_t* known : {L"article",L"feed",L"heading",L"paragraph",L"group",L"generic",L"list",L"listitem",L"document",L"textbox",L"button",L"link",L"navigation",L"main",L"region"})
        if (value == known) return utf8(value);
    return value.empty() ? "empty" : "other";
}
struct Block { std::string key, kind; std::wstring text; };
struct ResponseText {
    std::wstring text, code;
    std::string paragraph;
    bool previous_standalone = false;
    void flush_code() {
        if (code.empty()) return;
        if (code.find(L'\n') != std::wstring::npos || code.size() > 100) text += L"\n```\n" + code + L"\n```\n";
        else text += L"`" + code + L"`";
        code.clear();
    }
    void add(const std::wstring& value, bool monospace, const std::string& block, bool standalone = false) {
        if (standalone || previous_standalone || (!block.empty() && !paragraph.empty() && block != paragraph)) {
            flush_code();
            // These are verified structural paragraphs, not visual soft wraps.
            // Keep their boundary through sentence splitting for display grouping.
            if (!text.empty()) {
                if (text.back() != L'\n') text += L'\n';
                if (text.size() < 2 || text[text.size() - 2] != L'\n') text += L'\n';
            }
        }
        if (!block.empty()) paragraph = block;
        if (monospace) code += value;
        else { flush_code(); text += value; }
        previous_standalone = standalone;
    }
    std::wstring finish() { flush_code(); return text; }
};
class Reader {
    Ptr<IUIAutomation> uia_;
    Ptr<IUIAutomationCacheRequest> cache_;
    Ptr<IUIAutomationTreeWalker> walker_;
    std::unordered_map<std::string, N> nodes_;
    std::unordered_map<std::string, N> parents_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> view_identities_;
    std::unordered_map<std::string, std::string> view_first_users_;
    struct VerifiedTask { std::string id, title; };
    std::unordered_map<std::string, VerifiedTask> verified_document_tasks_;
    struct DocumentValue { Ptr<IUIAutomationValuePattern> value; Ptr<IUIAutomationLegacyIAccessiblePattern> legacy; PROPERTYID property = 0; };
    std::unordered_map<std::string, DocumentValue> document_values_;
    std::string identity(IUIAutomationElement* element) {
        SAFEARRAY* array = nullptr;
        check(element->GetRuntimeId(&array));
        if (!array) throw std::runtime_error("UI Automation runtime identity unavailable");
        LONG first = 0, last = -1;
        HRESULT hr = SafeArrayGetLBound(array, 1, &first);
        if (SUCCEEDED(hr)) hr = SafeArrayGetUBound(array, 1, &last);
        std::string result;
        if (SUCCEEDED(hr)) {
            for (LONG index = first; index <= last; ++index) {
                int value = 0;
                if (FAILED(SafeArrayGetElement(array, &index, &value))) { hr = E_FAIL; break; }
                if (!result.empty()) result += '-';
                result += std::to_string(value);
            }
        }
        SafeArrayDestroy(array);
        check(hr);
        return result;
    }
    N node(Ptr<IUIAutomationElement> element) {
        if (!element) return {};
        const auto key = identity(element.get());
        if (const auto found = nodes_.find(key); found != nodes_.end()) return found->second;
        auto result = std::make_shared<Node>();
        result->element = std::move(element); result->id = key;
        BSTR value = nullptr;
        check(result->element->get_CachedName(&value)); result->name = take_bstr(value); value = nullptr;
        check(result->element->get_CachedClassName(&value)); result->class_name = take_bstr(value); value = nullptr;
        check(result->element->get_CachedAutomationId(&value)); result->automation_id = take_bstr(value);
        check(result->element->get_CachedControlType(&result->type));
        value = nullptr;
        if (SUCCEEDED(result->element->get_CachedAriaRole(&value))) result->aria_role = take_bstr(value); else SysFreeString(value);
        value = nullptr;
        if (SUCCEEDED(result->element->get_CachedLocalizedControlType(&value))) result->localized_type = take_bstr(value); else SysFreeString(value);
        if (lower(result->aria_role) == L"heading") {
            VARIANT level; VariantInit(&level);
            // Standard UIA heading identifiers are 80051..80053. The property
            // is already cached: preserve semantic levels without per-text calls.
            if (SUCCEEDED(result->element->GetCachedPropertyValue(UIA_HeadingLevelPropertyId, &level)) &&
                level.vt == VT_I4 && level.lVal >= 80051 && level.lVal <= 80053)
                result->heading_level = static_cast<int>(level.lVal - 80050);
            VariantClear(&level);
        }
        nodes_.emplace(key, result);
        return result;
    }
    N parent(const N& child) {
        if (!child) return {};
        if (const auto found = parents_.find(child->id); found != parents_.end()) return found->second;
        Ptr<IUIAutomationElement> element;
        check(walker_->GetParentElementBuildCache(child->element.get(), cache_.get(), element.out()));
        auto result = node(std::move(element));
        parents_.emplace(child->id, result);
        return result;
    }
    std::string display_paragraph(const N& item, const N& boundary) {
        // RawView can retain an unnamed P group omitted from ControlView. The
        // response/message row is its boundary, never one global paragraph.
        N current = item, child = item, fallback = item;
        N response, response_child;
        for (int depth = 0; current && depth < 64; ++depth) {
            const auto role = lower(current->aria_role);
            const auto type_name = lower(current->localized_type);
            if (role == L"paragraph" || type_name == L"paragraph" || type_name == L"段落" ||
                current->heading_level || current->type == UIA_ListItemControlTypeId ||
                current->type == UIA_DataItemControlTypeId || current->type == UIA_HeaderItemControlTypeId) return current->id;
            if (!response && response_boundary(current)) { response = current; response_child = child; }
            if (current->id == boundary->id) break;
            fallback = current; child = current; current = parent(current);
        }
        if (response) {
            // Direct Text siblings may be fragments of one word or inline code.
            // Preserve their observed whitespace; do not guess extra boundaries.
            if (response_child && response_child->id != response->id &&
                response_child->type != UIA_TextControlTypeId &&
                response_child->type != UIA_HyperlinkControlTypeId &&
                lower(response_child->localized_type) != L"code" &&
                lower(response_child->aria_role) != L"code") return response_child->id;
            return response->id;
        }
        return fallback->id;
    }
    std::vector<N> descendants(const N& root, CONTROLTYPEID control = 0, std::size_t limit = 100000) {
        Ptr<IUIAutomationCondition> condition;
        if (control) {
            VARIANT value; VariantInit(&value); value.vt = VT_I4; value.lVal = control;
            check(uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, value, condition.out()));
        } else check(uia_->CreateTrueCondition(condition.out()));
        Ptr<IUIAutomationElementArray> array;
        check(root->element->FindAllBuildCache(TreeScope_Descendants, condition.get(), cache_.get(), array.out()));
        int count = 0; check(array->get_Length(&count));
        std::vector<N> result;
        const int capped = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(count), limit));
        result.reserve(capped);
        for (int index = 0; index < capped; ++index) {
            Ptr<IUIAutomationElement> element;
            if (FAILED(array->GetElement(index, element.out()))) continue;
            try { result.push_back(node(std::move(element))); } catch (const UiaError&) {}
        }
        return result;
    }
    bool inside(N current, const N& container) {
        for (int depth = 0; current && depth < 64; ++depth, current = parent(current))
            if (current->id == container->id) return true;
        return false;
    }
    std::string document_task_url(const N& document) {
        Ptr<IUIAutomationValuePattern> value;
        if (SUCCEEDED(document->element->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void**>(value.out()))) && value) {
            BSTR text = nullptr;
            if (SUCCEEDED(value->get_CurrentValue(&text))) {
                const auto url = canonical_task_url(utf8(take_bstr(text)));
                if (!url.empty()) { document_values_[document->id] = {value, {}, 0}; return url; }
            } else SysFreeString(text);
        }
        Ptr<IUIAutomationLegacyIAccessiblePattern> legacy;
        if (SUCCEEDED(document->element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, __uuidof(IUIAutomationLegacyIAccessiblePattern), reinterpret_cast<void**>(legacy.out()))) && legacy) {
            BSTR text = nullptr;
            if (SUCCEEDED(legacy->get_CurrentValue(&text))) {
                const auto url = canonical_task_url(utf8(take_bstr(text)));
                if (!url.empty()) { document_values_[document->id] = {{}, legacy, 0}; return url; }
            } else SysFreeString(text);
        }
        for (const PROPERTYID property : {UIA_ValueValuePropertyId, UIA_LegacyIAccessibleValuePropertyId}) {
            VARIANT value; VariantInit(&value);
            const HRESULT hr = document->element->GetCurrentPropertyValue(property, &value);
            std::string url;
            if (SUCCEEDED(hr) && value.vt == VT_BSTR && value.bstrVal) url = canonical_task_url(utf8(std::wstring(value.bstrVal, SysStringLen(value.bstrVal))));
            VariantClear(&value);
            if (!url.empty()) { document_values_[document->id] = {{}, {}, property}; return url; }
        }
        return {};
    }
    std::string verify_task_url(const TaskMetadata& task) {
        const auto found = document_values_.find(task.document_id);
        if (found == document_values_.end()) return {};
        BSTR text = nullptr; HRESULT hr = E_FAIL;
        if (found->second.value) hr = found->second.value->get_CurrentValue(&text);
        else if (found->second.legacy) hr = found->second.legacy->get_CurrentValue(&text);
        else if (found->second.property) {
            const auto node = nodes_.find(task.document_id);
            if (node == nodes_.end()) return {};
            VARIANT value; VariantInit(&value);
            hr = node->second->element->GetCurrentPropertyValue(found->second.property, &value);
            std::string url;
            if (SUCCEEDED(hr) && value.vt == VT_BSTR && value.bstrVal) url = canonical_task_url(utf8(std::wstring(value.bstrVal, SysStringLen(value.bstrVal))));
            VariantClear(&value); return url;
        }
        if (SUCCEEDED(hr)) return canonical_task_url(utf8(take_bstr(text)));
        SysFreeString(text); return {};
    }
    TaskMetadata task_metadata(HWND hwnd, DWORD pid, const N& window, const std::vector<N>& all) {
        TaskMetadata result;
        const auto window_name = clean_task_title(utf8(window_title(hwnd)));
        int best_title_score = -1;
        std::string selected_title, view_anchor;
        N selected_document;
        std::unordered_map<std::string, std::string> document_urls;
        const auto scope = std::to_string(pid) + ":" + std::to_string(reinterpret_cast<std::uintptr_t>(hwnd));
        for (const auto& document : all) if (document->type == UIA_DocumentControlTypeId) {
            const auto title = clean_task_title(utf8(document->name));
            int score = title.empty() ? 0 : 10;
            if (!title.empty() && title == window_name) score += 30;
            for (const auto& candidate : all) if (lower(candidate->aria_role) == L"feed") {
                auto ancestor = parent(candidate);
                for (int depth = 0; ancestor && ancestor->id != window->id && depth < 64; ++depth, ancestor = parent(ancestor)) {
                    if (ancestor->type == UIA_DocumentControlTypeId) { if (ancestor->id == document->id) score += 100; break; }
                }
            }
            if (score >= best_title_score) { best_title_score = score; selected_title = title; selected_document = document; }
            const auto url = document_task_url(document);
            if (!url.empty()) { ++result.url_candidates; document_urls[document->id] = url; }
            view_anchor += "|doc:" + document->id;
        }
        result.title = !selected_title.empty() ? selected_title : window_name;
        if (result.title.empty()) result.title = "未命名任务";
        if (selected_document) {
            result.content_document_id = selected_document->id;
            result.document_key = scope + "|doc:" + selected_document->id;
            // A nested content Document may expose its URL through an ancestor.
            // Never borrow the identity of an unrelated peer Document instead.
            int depth = 0;
            for (auto current = selected_document; current && current->id != window->id && depth < 64; ++depth, current = parent(current)) {
                if (const auto found = document_urls.find(current->id); found != document_urls.end()) {
                    result.id = found->second; result.document_id = current->id;
                    result.quality = "url";
                    return result;
                }
            }
            if (const auto known = verified_document_tasks_.find(result.document_key); known != verified_document_tasks_.end()) {
                // A temporarily missing URL is not evidence of a different task.
                // Keep its verified identity visible, but publish no unknown body.
                result.id = known->second.id; result.title = known->second.title;
                result.document_id = selected_document->id; result.quality = "url";
                result.identity_pending = true;
                return result;
            }
        }
        // Other Documents have URLs, but none belongs to the selected content.
        // Its unverified body must not be attributed to one of those other tasks.
        if (result.url_candidates) result.identity_pending = true;
        std::string feed_anchor;
        for (const auto& candidate : all) if (lower(candidate->aria_role) == L"feed") feed_anchor += "|feed:" + candidate->id;
        if (!feed_anchor.empty()) view_anchor = feed_anchor;
        if (view_anchor.empty()) view_anchor = "|window:" + window->id;
        const auto view = scope + view_anchor;
        const std::wregex first_user_name(LR"(^(?:Message\s+1(?: of \d+)?|消息\s*1)$)", flags);
        std::string first_user, first_user_node;
        for (const auto& article : all) if (rx(article->name, first_user_name)) {
            for (const auto& candidate : all) if (is_user_marker(candidate) && inside(candidate, article)) { first_user = utf8(candidate->name); first_user_node = article->id; break; }
            if (!first_user.empty()) break;
        }
        // The fallback is deliberately scoped to an exposed user-message node.
        // Identical first prompts in separate anonymous views must never merge.
        if (!first_user.empty()) {
            const auto fingerprint = task_digest(first_user);
            const auto known = view_identities_.find(view);
            const auto previous = view_first_users_.find(view);
            if (known != view_identities_.end() && previous != view_first_users_.end() && previous->second == fingerprint)
                result.id = known->second.first;
            else result.id = "uia:" + scope + ":first-user:" + task_digest(first_user + "|node:" + first_user_node);
            result.quality = "first_user_anchor";
            view_identities_[view] = {result.id, result.quality};
            view_first_users_[view] = fingerprint;
        } else if (const auto found = view_identities_.find(view); found != view_identities_.end()) {
            result.id = found->second.first; result.quality = found->second.second;
        } else {
            result.id = "uia:" + scope + ":view:" + task_digest(view_anchor);
            result.quality = "view_fallback";
            view_identities_[view] = {result.id, result.quality};
        }
        return result;
    }
    int expansion(const N& current) {
        Ptr<IUIAutomationExpandCollapsePattern> pattern;
        const auto hr = current->element->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
            __uuidof(IUIAutomationExpandCollapsePattern), reinterpret_cast<void**>(pattern.out()));
        if (FAILED(hr) || !pattern) return -1;
        ExpandCollapseState state;
        return SUCCEEDED(pattern->get_CurrentExpandCollapseState(&state)) ? static_cast<int>(state) : -1;
    }
    std::unordered_set<std::string> thinking_regions(const N& message, const std::vector<N>& all) {
        std::unordered_set<std::string> result;
        for (const auto& toggle : all) {
            if ((toggle->type != UIA_ButtonControlTypeId && toggle->type != UIA_GroupControlTypeId) ||
                !rx(toggle->name, thinking_title) || expansion(toggle) != ExpandCollapseState_Expanded) continue;
            auto current = toggle->type == UIA_GroupControlTypeId ? toggle : parent(toggle);
            for (int depth = 0; current && current->id != message->id && depth < 5; ++depth, current = parent(current)) {
                bool has_text = false;
                for (const auto& child : all) {
                    if (child->type == UIA_TextControlTypeId && !rx(child->name, plain_thinking_title) && inside(child, current) &&
                        (toggle->type != UIA_ButtonControlTypeId || !inside(child, toggle))) { has_text = true; break; }
                }
                if (has_text) { result.insert(current->id); break; }
            }
        }
        return result;
    }
    std::unordered_set<std::string> auxiliary_regions(const N& message, const std::vector<N>& all) {
        std::unordered_set<std::string> result;
        std::vector<N> buttons;
        std::unordered_set<std::wstring> status_labels;
        for (const auto& item : all) {
            if (item->type == UIA_ButtonControlTypeId) buttons.push_back(item);
            if (!is_status(item)) continue;
            result.insert(item->id);
            const auto label = summary_label(item->name);
            if (!label.empty()) status_labels.insert(label);
            for (const auto& child : all) if (child->type == UIA_TextControlTypeId && inside(child, item)) {
                const auto text = summary_label(child->name);
                if (!text.empty()) status_labels.insert(text);
            }
        }
        static const std::wregex tool_stat(LR"(^(?:(?:已)?运行\s*\d+\s*[个条]?\s*命令|(?:ran|running|executed)\s+\d+\s+commands?|新增\s*\d+\s*[,，]\s*删除\s*\d+|added\s+\d+\s*[,，]\s*(?:removed|deleted)\s+\d+)$)", flags);
        for (const auto& button : buttons) {
            const auto label = summary_label(button->name);
            if (label.empty()) continue;
            auto current = parent(button);
            for (int depth = 0; current && current->id != message->id && depth < 8; ++depth, current = parent(current)) {
                if (response_boundary(current)) break;
                const auto role = lower(current->aria_role);
                if (role == L"paragraph" || current->heading_level || current->type == UIA_ListItemControlTypeId) break;
                if (current->type != UIA_GroupControlTypeId && current->type != UIA_PaneControlTypeId && current->type != UIA_CustomControlTypeId) continue;
                bool duplicate = summary_label(current->name) == label, statistic = false;
                for (const auto& child : all) if (child->type == UIA_TextControlTypeId && inside(child, current)) {
                    const auto text = summary_label(child->name);
                    duplicate = duplicate || text == label;
                    statistic = statistic || std::regex_match(text, tool_stat);
                }
                // Tool controls expose the same task summary as a Button and
                // a live Status, or as a summary Button with exact run counters.
                // A prose sentence mentioning commands alone is never evidence.
                if (duplicate && (statistic || status_labels.count(label))) { result.insert(current->id); break; }
            }
        }
        bool dismiss = false;
        for (const auto& button : buttons) {
            if (button->name != L"Dismiss") continue;
            dismiss = true;
            auto current = parent(button);
            for (int depth = 0; current && current->id != message->id && depth < 6; ++depth, current = parent(current)) {
                if (has_class(current->class_name, L"group/message-row")) break;
                bool upgrade = false;
                for (const auto& other : buttons) if (other->name == L"Upgrade" && inside(other, current)) { upgrade = true; break; }
                if (upgrade) { result.insert(current->id); break; }
            }
        }
        if (dismiss) {
            N heading;
            for (const auto& item : all) {
                if (item->type == UIA_TextControlTypeId) heading = item;
                if (item->type != UIA_ButtonControlTypeId || item->name != L"Upgrade" || !heading || !rx(heading->name, promotional_heading)) continue;
                const auto current = parent(heading);
                if (current && current->id != message->id && !has_class(current->class_name, L"group/message-row")) result.insert(current->id);
            }
        }
        return result;
    }
    std::unordered_set<std::string> preferred_container_names(const std::vector<N>& all) {
        std::unordered_set<std::string> result;
        for (const auto& item : all) {
            if ((item->type != UIA_ListItemControlTypeId && !item->heading_level) || blank(item->name)) continue;
            bool has_text = false, covers_text = true, unsafe = false;
            std::size_t text_chars = 0;
            for (const auto& child : all) {
                if (child->id == item->id || !inside(child, item)) continue;
                if (child->type == UIA_TextControlTypeId && !blank(child->name)) {
                    has_text = true; text_chars += child->name.size();
                    if (item->name.find(child->name) == std::wstring::npos) covers_text = false;
                }
                const auto semantic = child->class_name + L" " + child->automation_id;
                if (child->type == UIA_ButtonControlTypeId || child->type == UIA_ToolBarControlTypeId || child->type == UIA_EditControlTypeId ||
                    is_status(child) || rx(semantic, auxiliary_class) || rx(semantic, excluded_semantic) || child->class_name.find(L"group/copy") != std::wstring::npos ||
                    has_class(child->class_name, L"font-mono") || expansion(child) == ExpandCollapseState_Collapsed) unsafe = true;
            }
            // Rich list items can expose their whole sentence as Name while only
            // bold labels are child Text nodes. Prefer the verified superset Name.
            // A named semantic heading can similarly own its child text once.
            // Aggregates containing code or auxiliary controls retain leaf filtering.
            if (has_text && covers_text && !unsafe && (item->heading_level || item->name.size() > text_chars)) result.insert(item->id);
        }
        return result;
    }
    bool covered_by_container_name(const N& item, const N& boundary, const std::unordered_set<std::string>& preferred) {
        auto ancestor = parent(item);
        for (int depth = 0; ancestor && ancestor->id != boundary->id && depth < 64; ++depth, ancestor = parent(ancestor))
            if (preferred.count(ancestor->id)) return true;
        return false;
    }
    std::vector<Block> output_blocks(const N& message, const std::string& key, const std::vector<N>* window_nodes = nullptr, bool identified_assistant = false) {
        std::vector<N> all;
        // Chromium can flatten FindAll(message) while preserving the RawView parent.
        // Enumerate from the window and classify by raw ancestry rather than losing body.
        if (window_nodes) {
            for (const auto& item : *window_nodes) if (item->id != message->id && inside(item, message)) all.push_back(item);
        } else all = descendants(message);
        bool assistant = identified_assistant || rx(message->name, assistant_name);
        // A localized accessibility heading identifies the speaker, never the body.
        for (const auto& item : all) if (is_user_marker(item)) return {};
        for (const auto& item : all) if (is_assistant_marker(item)) assistant = true;
        const auto preferred_names = preferred_container_names(all);
        const auto regions = thinking_regions(message, all);
        const auto auxiliary = auxiliary_regions(message, all);
        std::unordered_set<std::string> containers_with_text;
        for (const auto& leaf : all) {
            if (leaf->type != UIA_TextControlTypeId) continue;
            auto current = parent(leaf);
            for (int depth = 0; current && current->id != message->id && depth < 64; ++depth, current = parent(current))
                if (current->type == UIA_ListItemControlTypeId || current->type == UIA_DataItemControlTypeId || current->type == UIA_HeaderItemControlTypeId || current->heading_level)
                    containers_with_text.insert(current->id);
        }
        std::vector<Block> result;
        std::unordered_map<std::string, ResponseText> builders;
        for (const auto& item : all) {
            const auto type = item->type;
            if (type != UIA_TextControlTypeId && !((item->heading_level || type == UIA_ListItemControlTypeId || type == UIA_DataItemControlTypeId || type == UIA_HeaderItemControlTypeId) && (!containers_with_text.count(item->id) || preferred_names.count(item->id)))) continue;
            if (covered_by_container_name(item, message, preferred_names)) continue;
            if (item->class_name.find(L"sr-only") != std::wstring::npos || is_status(item) || is_assistant_marker(item) || is_user_marker(item)) continue;
            auto current = parent(item); auto child = item;
            bool excluded = false, code_block = false;
            std::string kind, region, paragraph;
            for (int depth = 0; current && current->id != message->id && depth < 64; ++depth) {
                if (is_status(current) || current->type == UIA_ToolBarControlTypeId || current->type == UIA_ButtonControlTypeId || current->type == UIA_EditControlTypeId || rx(current->class_name + L" " + current->automation_id, excluded_semantic) || rx(current->class_name, auxiliary_class) || expansion(current) == ExpandCollapseState_Collapsed || auxiliary.count(current->id)) { excluded = true; break; }
                const auto semantic = current->class_name + L" " + current->automation_id;
                if (regions.count(current->id) || rx(semantic, thinking_semantic)) { kind = "thinking"; region = current->id; }
                else if (kind != "thinking" && rx(semantic, intermediate_semantic)) { kind = "intermediate"; region = current->id; }
                else if (kind.empty() && has_class(current->class_name, L"font-claude-response")) { kind = "answer"; region = current->id; }
                else if (kind.empty() && assistant) { kind = "answer"; region = "message-body"; paragraph = child->id; }
                if (current->name.size() >= 5 && current->name.compare(current->name.size() - 5, 5, L" code") == 0 && current->class_name.find(L"group/copy") != std::wstring::npos) code_block = true;
                child = current; current = parent(current);
            }
            // A native GroupBox can directly contain Static children. Chromium normally adds
            // semantic containers, but the named assistant group itself is sufficient proof.
            if (!excluded && kind.empty() && assistant && current && current->id == message->id) { kind = "answer"; region = "message-body"; paragraph = child->id; }
            if (excluded || kind.empty()) continue;
            std::wstring text = item->name, font;
            Ptr<IUIAutomationTextPattern> pattern;
            if (!preferred_names.count(item->id) && SUCCEEDED(item->element->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern), reinterpret_cast<void**>(pattern.out()))) && pattern) {
                Ptr<IUIAutomationTextRange> range;
                if (SUCCEEDED(pattern->get_DocumentRange(range.out())) && range) {
                    BSTR value = nullptr;
                    if (SUCCEEDED(range->GetText(64000, &value))) text = take_bstr(value);
                    VARIANT attribute; VariantInit(&attribute);
                    if (SUCCEEDED(range->GetAttributeValue(UIA_FontNameAttributeId, &attribute)) && attribute.vt == VT_BSTR && attribute.bstrVal)
                        font.assign(attribute.bstrVal, SysStringLen(attribute.bstrVal));
                    VariantClear(&attribute);
                }
            }
            if (text.empty()) continue;
            paragraph = display_paragraph(item, message);
            if (type == UIA_ListItemControlTypeId) { paragraph = item->id; text = L"- " + text; }
            else if (type == UIA_DataItemControlTypeId || type == UIA_HeaderItemControlTypeId) paragraph = item->id;
            const bool monospace = rx(font, mono_font);
            if (code_block && !monospace) continue;
            const auto block_key = key + "|" + kind + "|" + (kind == "answer" ? "message-body" : region);
            if (!builders.count(block_key)) { builders.emplace(block_key, ResponseText{}); result.push_back({block_key, kind, {}}); }
            const bool heading = item->heading_level && !monospace;
            if (heading) { paragraph = item->id; text = std::wstring(item->heading_level, L'#') + L" " + text; }
            builders.at(block_key).add(text, monospace, paragraph, heading);
        }
        for (auto& block : result) block.text = builders.at(block.key).finish();
        return result;
    }
    N nearest_feed(N item, const N& window) {
        for (int depth = 0; item && item->id != window->id && depth < 64; ++depth, item = parent(item))
            if (lower(item->aria_role) == L"feed") return item;
        return {};
    }
    std::string message_key(N item, const N& boundary, DWORD pid) {
        const auto fallback = std::to_string(pid) + "|feed-message|" + item->id;
        for (int depth = 0; item && item->id != boundary->id && depth < 64; ++depth, item = parent(item))
            if (is_message(item) || lower(item->aria_role) == L"article") return std::to_string(pid) + "|message|" + item->id;
        return fallback;
    }
    std::vector<Block> streaming_article_blocks(const N& window, DWORD pid, const std::vector<N>& all) {
        std::unordered_map<std::string, std::vector<N>> articles;
        std::unordered_map<std::string, N> feeds;
        for (const auto& item : all) if (lower(item->aria_role) == L"article") {
            const auto feed = nearest_feed(item, window);
            if (feed) { articles[feed->id].push_back(item); feeds[feed->id] = feed; }
        }
        for (const auto& item : articles) {
            const auto& ordered = item.second;
            if (ordered.size() < 2) continue;
            const auto& previous = ordered[ordered.size() - 2];
            const auto& latest = ordered.back();
            bool preceding_user = false, latest_user = false, latest_assistant = false;
            std::vector<N> selected;
            for (const auto& node : all) {
                if (inside(node, previous) && is_user_marker(node)) preceding_user = true;
                if (!inside(node, latest)) continue;
                if (is_user_marker(node)) latest_user = true;
                if (is_assistant_marker(node)) latest_assistant = true;
                bool row = false;
                auto ancestor = parent(node);
                for (int depth = 0; ancestor && ancestor->id != latest->id && depth < 32; ++depth, ancestor = parent(ancestor))
                    if (has_class(ancestor->class_name, L"group/message-row")) { row = true; break; }
                if (row) selected.push_back(node);
            }
            // Code's streaming article omits its speaker heading until completion.
            // Infer only the newest article immediately after a proven user article,
            // and only exposed message-row descendants; input/sidebar are excluded.
            if (!preceding_user || latest_user || latest_assistant || selected.empty()) continue;
            auto result = output_blocks(latest, std::to_string(pid) + "|message|" + latest->id, &selected, true);
            result.erase(std::remove_if(result.begin(), result.end(), [](const Block& value) { return blank(value.text); }), result.end());
            if (!result.empty()) return result;
        }
        return {};
    }
    std::vector<Block> feed_blocks(const N& window, DWORD pid, const std::vector<N>& all) {
        struct Segment { N feed, marker; bool assistant; std::vector<N> nodes; };
        std::vector<Segment> segments;
        std::unordered_map<std::string, std::size_t> active;
        std::unordered_map<std::string, N> owners;
        auto owner = [&](N item) -> N {
            const auto found = owners.find(item->id);
            if (found != owners.end()) return found->second;
            const auto key = item->id;
            for (int depth = 0; item && item->id != window->id && depth < 64; ++depth, item = parent(item)) {
                if (lower(item->aria_role) == L"feed") { owners.emplace(key, item); return item; }
            }
            owners.emplace(key, N{}); return {};
        };
        for (const auto& item : all) {
            const auto feed = owner(item);
            if (!feed) continue;
            if (is_message(item)) active.erase(feed->id); // Wait for explicit speaker evidence.
            if (is_assistant_marker(item) || is_user_marker(item)) {
                segments.push_back({feed, item, !is_user_marker(item), {item}});
                active[feed->id] = segments.size() - 1;
                continue;
            }
            const auto current = active.find(feed->id);
            if (current != active.end()) segments[current->second].nodes.push_back(item);
        }
        // Code emits one response as several sibling message rows after one speaker
        // heading. Its article wrapper can contain only the first tool/title row.
        // Feed + explicit speaker boundaries provide a complete, narrow response scope.
        for (auto segment = segments.rbegin(); segment != segments.rend(); ++segment) {
            if (!segment->assistant) continue;
            auto result = output_blocks(segment->feed,
                message_key(segment->marker, segment->feed, pid), &segment->nodes);
            result.erase(std::remove_if(result.begin(), result.end(), [](const Block& value) { return blank(value.text); }), result.end());
            if (!result.empty()) return result;
        }
        return {};
    }
    std::vector<Block> semantic_blocks(const N& window, DWORD pid) {
        std::vector<Block> result;
        std::unordered_map<std::string, ResponseText> builders;
        std::unordered_map<std::string, bool> user_messages;
        const auto semantic_nodes = descendants(window, 0, 10000);
        const auto preferred_names = preferred_container_names(semantic_nodes);
        const auto auxiliary = auxiliary_regions(window, semantic_nodes);
        std::unordered_set<std::string> containers_with_text;
        for (const auto& leaf : semantic_nodes) if (leaf->type == UIA_TextControlTypeId) {
            auto ancestor = parent(leaf);
            for (int depth = 0; ancestor && ancestor->id != window->id && depth < 24; ++depth, ancestor = parent(ancestor))
                if (ancestor->type == UIA_ListItemControlTypeId || ancestor->type == UIA_DataItemControlTypeId || ancestor->type == UIA_HeaderItemControlTypeId || ancestor->heading_level) containers_with_text.insert(ancestor->id);
        }
        for (const auto& item : semantic_nodes) {
            if (item->type != UIA_TextControlTypeId && !((item->heading_level || item->type == UIA_ListItemControlTypeId || item->type == UIA_DataItemControlTypeId || item->type == UIA_HeaderItemControlTypeId) && (!containers_with_text.count(item->id) || preferred_names.count(item->id)))) continue;
            if (covered_by_container_name(item, window, preferred_names)) continue;
            if (item->name.empty() || item->name.size() > 64000 || item->class_name.find(L"sr-only") != std::wstring::npos || is_status(item) || is_assistant_marker(item) || is_user_marker(item)) continue;
            try {
                auto current = parent(item);
                std::string key, strong_key, message_key; N owner, strong_owner, message_owner; bool skip = false;
                for (int depth = 0; current && current->id != window->id && depth < 24; ++depth, current = parent(current)) {
                    const auto semantic = current->class_name + L" " + current->automation_id;
                    if (is_status(current) || auxiliary.count(current->id) || current->type == UIA_EditControlTypeId || current->type == UIA_ButtonControlTypeId || current->type == UIA_ToolBarControlTypeId || rx(semantic, excluded_semantic)) { skip = true; break; }
                    if (rx(semantic, assistant_semantic)) { key = std::to_string(pid) + "|semantic|" + current->id; owner = current; }
                    if (rx(semantic, strong_assistant_semantic)) { strong_key = std::to_string(pid) + "|semantic|" + current->id; strong_owner = current; }
                    if (is_message(current) || lower(current->aria_role) == L"article" || has_class(current->class_name, L"group/message-row")) {
                        auto found = user_messages.find(current->id);
                        if (found == user_messages.end()) {
                            bool user = false;
                            for (const auto& descendant : descendants(current)) if (is_user_marker(descendant)) { user = true; break; }
                            found = user_messages.emplace(current->id, user).first;
                        }
                        if (found->second) { skip = true; break; }
                        message_key = std::to_string(pid) + (lower(current->aria_role) == L"article" || is_message(current) ? "|message|" : "|row|") + current->id; message_owner = current;
                    }
                }
                if (skip || key.empty()) continue;
                // Styled paragraphs share one structural message/assistant output block.
                if (!message_key.empty()) { key = message_key; owner = message_owner; }
                else if (!strong_key.empty()) { key = strong_key; owner = strong_owner; }
                key += "|answer|message-body";
                if (!builders.count(key)) { builders.emplace(key, ResponseText{}); result.push_back({key, "answer", {}}); }
                auto& builder = builders.at(key);
                if (builder.text.size() + item->name.size() + 16 < 64000) {
                    auto value = (item->heading_level ? std::wstring(item->heading_level, L'#') + L" " : item->type == UIA_ListItemControlTypeId ? L"- " : L"") + item->name;
                    builder.add(value, false, display_paragraph(item, owner), item->heading_level != 0);
                }
            } catch (const UiaError&) {}
        }
        for (auto& block : result) block.text = builders.at(block.key).finish();
        // Semantic-only Code layouts: keep only the newest assistant container.
        if (result.size() > 1) return {std::move(result.back())};
        return result;
    }
public:
    Reader() {
        check(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void**>(uia_.out())));
        check(uia_->CreateCacheRequest(cache_.out()));
        check(cache_->put_TreeScope(TreeScope_Element));
        for (const PROPERTYID property : {UIA_NamePropertyId, UIA_ControlTypePropertyId, UIA_ClassNamePropertyId, UIA_AutomationIdPropertyId, UIA_AriaRolePropertyId, UIA_LocalizedControlTypePropertyId, UIA_HeadingLevelPropertyId}) check(cache_->AddProperty(property));
        check(uia_->get_RawViewWalker(walker_.out()));
    }
    json profile(HWND hwnd, bool fixture) {
        const auto started = Clock::now();
        check_window(hwnd, fixture);
        nodes_.clear(); parents_.clear(); document_values_.clear();
        Ptr<IUIAutomationElement> root;
        check(uia_->ElementFromHandleBuildCache(hwnd, cache_.get(), root.out()));
        const auto window = node(std::move(root));
        const auto all = descendants(window);
        json types = json::object(), roles = json::object(), marker_types = json::object();
        std::size_t messages = 0, assistant = 0, user = 0, semantic = 0, semantic_chars = 0;
        std::vector<N> candidates;
        for (const auto& item : all) {
            const auto type = std::to_string(item->type), role = safe_role(item->aria_role);
            types[type] = types.value(type, 0) + 1;
            roles[role] = roles.value(role, 0) + 1;
            if (is_message(item)) { ++messages; candidates.push_back(item); }
            if (is_assistant_marker(item)) { ++assistant; marker_types[type] = marker_types.value(type, 0) + 1; }
            if (is_user_marker(item)) ++user;
            if (rx(item->class_name + L" " + item->automation_id, assistant_semantic)) { ++semantic; semantic_chars += item->name.size(); }
        }
        std::size_t global_text_chars = 0;
        std::vector<std::pair<std::size_t, N>> texts, leaves;
        json marker_positions = json::array(), message_positions = json::array();
        for (std::size_t index = 0; index < all.size(); ++index) {
            const auto& item = all[index];
            if (item->type == UIA_TextControlTypeId) { global_text_chars += item->name.size(); texts.emplace_back(index, item); }
            if (item->type == UIA_TextControlTypeId || item->type == UIA_ListItemControlTypeId || item->type == UIA_DataItemControlTypeId || item->type == UIA_HeaderItemControlTypeId) leaves.emplace_back(index, item);
            if (is_assistant_marker(item) || is_user_marker(item)) marker_positions.push_back({{"index", index}, {"speaker", is_user_marker(item) ? "user" : "assistant"}, {"type", item->type}});
            if (is_message(item)) message_positions.push_back({{"index", index}, {"type", item->type}, {"role", safe_role(item->aria_role)}});
        }
        if (marker_positions.size() > 16) marker_positions.erase(marker_positions.begin(), marker_positions.end() - 16);
        if (message_positions.size() > 16) message_positions.erase(message_positions.begin(), message_positions.end() - 16);
        std::stable_sort(texts.begin(), texts.end(), [](const auto& left, const auto& right) { return left.second->name.size() > right.second->name.size(); });
        json longest = json::array();
        for (std::size_t index = 0; index < std::min<std::size_t>(8, texts.size()); ++index) {
            const auto& item = texts[index].second;
            json chain = json::array();
            auto ancestor = parent(item);
            for (int depth = 0; ancestor && ancestor->id != window->id && depth < 24; ++depth, ancestor = parent(ancestor))
                chain.push_back({{"type", ancestor->type}, {"role", safe_role(ancestor->aria_role)}, {"class", utf8(ancestor->class_name.substr(0, 192))}, {"message", is_message(ancestor)}});
            longest.push_back({{"index", texts[index].first}, {"length", item->name.size()}, {"type", item->type},
                {"role", safe_role(item->aria_role)}, {"class", utf8(item->class_name.substr(0, 192))},
                {"assistant_marker", is_assistant_marker(item)}, {"user_marker", is_user_marker(item)}, {"raw_ancestors", chain}});
        }
        std::stable_sort(leaves.begin(), leaves.end(), [](const auto& left, const auto& right) { return left.second->name.size() > right.second->name.size(); });
        json longest_leaves = json::array();
        for (std::size_t index = 0; index < std::min<std::size_t>(8, leaves.size()); ++index) {
            const auto& item = leaves[index].second;
            json chain = json::array();
            auto ancestor = parent(item);
            for (int depth = 0; ancestor && ancestor->id != window->id && depth < 24; ++depth, ancestor = parent(ancestor))
                chain.push_back({{"type", ancestor->type}, {"role", safe_role(ancestor->aria_role)}, {"class", utf8(ancestor->class_name.substr(0, 192))}, {"message", is_message(ancestor)}});
            longest_leaves.push_back({{"index", leaves[index].first}, {"length", item->name.size()}, {"type", item->type},
                {"role", safe_role(item->aria_role)}, {"class", utf8(item->class_name.substr(0, 192))},
                {"assistant_marker", is_assistant_marker(item)}, {"user_marker", is_user_marker(item)}, {"raw_ancestors", chain}});
        }
        json details = json::array();
        const auto begin = candidates.size() > 8 ? candidates.size() - 8 : 0;
        for (std::size_t index = begin; index < candidates.size(); ++index) {
            std::size_t texts = 0, chars = 0, assistant_count = 0, user_count = 0;
            for (const auto& item : descendants(candidates[index])) {
                if (is_assistant_marker(item)) ++assistant_count;
                if (is_user_marker(item)) ++user_count;
                if (item->type == UIA_TextControlTypeId && !is_assistant_marker(item) && !is_user_marker(item)) { ++texts; chars += item->name.size(); }
            }
            std::size_t raw_texts = 0, raw_chars = 0;
            for (const auto& item : all) if (item->type == UIA_TextControlTypeId && !is_assistant_marker(item) && !is_user_marker(item) && inside(item, candidates[index])) { ++raw_texts; raw_chars += item->name.size(); }
            details.push_back({{"raw_text_nodes", raw_texts}, {"raw_text_chars", raw_chars}, {"type", candidates[index]->type}, {"role", safe_role(candidates[index]->aria_role)},
                {"text_nodes", texts}, {"text_chars", chars}, {"assistant_markers", assistant_count}, {"user_markers", user_count}});
        }
        json list_items = json::array();
        for (const auto& item : all) if (item->type == UIA_ListItemControlTypeId && list_items.size() < 64) {
            std::size_t child_texts = 0, child_chars = 0; bool covers = true;
            for (const auto& child : all) if (child->type == UIA_TextControlTypeId && inside(child, item)) {
                ++child_texts; child_chars += child->name.size();
                if (!child->name.empty() && item->name.find(child->name) == std::wstring::npos) covers = false;
            }
            list_items.push_back({{"length", item->name.size()}, {"child_text_nodes", child_texts}, {"child_text_chars", child_chars}, {"name_covers_child_text", covers}});
        }
        const auto captured = snapshot(hwnd, fixture);
        std::size_t body_chars = 0;
        for (const auto& block : captured.at("blocks")) {
            const auto text = block.at("text").get<std::string>();
            body_chars += MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        }
        return {{"type", "reader-info"}, {"nodes", all.size()}, {"control_types", types}, {"aria_roles", roles},
            {"minimized", IsIconic(hwnd) != FALSE}, {"blocks_count", captured.at("blocks").size()}, {"chars_count", body_chars},
            {"capture_duration_ms", captured.at("duration_ms")},
            {"task_id_present", !captured.value("task_id", "").empty()}, {"task_title_present", !captured.value("task_title", "").empty()},
            {"identity_quality", captured.value("identity_quality", "")}, {"task_id_hash", task_digest(captured.value("task_id", "")).substr(0, 16)},
            {"list_items", list_items}, {"global_text_chars", global_text_chars}, {"longest_leaves", longest_leaves}, {"longest_texts", longest}, {"marker_positions", marker_positions}, {"message_positions", message_positions},
            {"message_count", messages}, {"assistant_markers", assistant}, {"assistant_marker_types", marker_types},
            {"user_markers", user}, {"semantic_containers", semantic}, {"semantic_name_chars", semantic_chars},
            {"latest_messages", details}, {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()},
            {"body_included", false}};
    }
    json snapshot(HWND hwnd, bool fixture) {
        const auto started = Clock::now();
        const DWORD pid = check_window(hwnd, fixture);
        nodes_.clear(); parents_.clear(); document_values_.clear();
        Ptr<IUIAutomationElement> root;
        check(uia_->ElementFromHandleBuildCache(hwnd, cache_.get(), root.out()));
        const auto window = node(std::move(root));
        const auto window_nodes = descendants(window);
        auto task = task_metadata(hwnd, pid, window, window_nodes);
        std::string context = task.id;
        const auto selected = nodes_.find(task.content_document_id);
        const auto content = selected == nodes_.end() ? window : selected->second;
        std::vector<N> content_nodes;
        for (const auto& item : window_nodes)
            if (content->id == window->id || inside(item, content)) content_nodes.push_back(item);
        std::vector<N> messages;
        for (const auto& item : content_nodes) if (is_message(item)) messages.push_back(item);
        std::vector<Block> blocks = streaming_article_blocks(content, pid, content_nodes);
        if (blocks.empty()) blocks = feed_blocks(content, pid, content_nodes);
        std::size_t tried = 0;
        for (auto message = messages.rbegin(); message != messages.rend() && tried < 4 && blocks.empty(); ++message, ++tried) {
            blocks = output_blocks(*message, std::to_string(pid) + "|message|" + (*message)->id, &content_nodes);
            blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const Block& block) { return blank(block.text); }), blocks.end());
            if (!blocks.empty()) break;
        }
        if (blocks.empty()) blocks = semantic_blocks(content, pid);
        json output = json::array();
        for (const auto& block : blocks) {
            if (output.size() == 128) break;
            if (!blank(block.text)) output.push_back({{"key", block.key}, {"text", utf8(block.text.substr(0, 64000))}, {"kind", block.kind}});
        }
        bool identity_pending = task.identity_pending;
        if (identity_pending) output.clear();
        else if (task.quality == "url") {
            // Re-read only the selected Document value after extraction. Navigation
            // must never attach text from one task to another task's cache.
            const auto verified = verify_task_url(task);
            if (verified != task.id) {
                output.clear(); identity_pending = true;
                if (!verified.empty()) { task.id = verified; context = task.id; }
                const auto visible_title = clean_task_title(utf8(window_title(hwnd)));
                if (!visible_title.empty()) task.title = visible_title;
            } else {
                // Commit only after the post-extraction URL check succeeds. An
                // observed navigation race must not replace a verified identity.
                verified_document_tasks_[task.document_key] = {task.id, task.title};
            }
        }
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        return {{"type", "snapshot"}, {"identity_pending", identity_pending}, {"context", context}, {"task_id", task.id}, {"task_title", task.title},
            {"source_hwnd", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd))}, {"source_pid", pid}, {"identity_quality", task.quality},
            {"blocks", output}, {"duration_ms", duration},
            {"status", output.empty() ? "已找到 Claude 窗口，当前没有可读取的助手正文。请保持有内容的会话打开；不会截图或自动点击。" : "已连接 Claude，正在只读采集助手文字"}};
    }
};
} // namespace

std::string enumerate_windows_json() {
    json windows = json::array();
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        try {
            const DWORD pid = check_window(hwnd, false);
            auto& result = *reinterpret_cast<json*>(parameter);
            result.push_back({{"hwnd", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd))}, {"pid", pid}, {"title", utf8(window_title(hwnd))}});
        } catch (...) {}
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows.dump(-1, ' ', false, json::error_handler_t::replace);
}
int reader_info_main(std::uint64_t handle, bool fixture) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 1;
    int result = 0;
    try { Reader reader; write_line(reader.profile(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle)), fixture)); }
    catch (...) { write_line({{"type", "error"}, {"text", "辅助功能结构诊断暂不可用。"}, {"body_included", false}}); result = 1; }
    CoUninitialize();
    return result;
}
int reader_main(std::uint64_t handle, bool once, bool fixture) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { write_line({{"type", "error"}, {"text", "辅助功能读取线程初始化失败。"}}); return 1; }
    int exit_code = 0;
    try {
        Reader reader;
        const auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
        do {
            const auto start = Clock::now();
            bool connected = true;
            try { connected = write_line(reader.snapshot(hwnd, fixture)); }
            catch (const UiaError& error) {
                connected = write_line({{"type", "error"}, {"text", error.hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) ? "Claude 页面正在切换，等待下一次读取。" : "辅助功能读取暂不可用，稍后自动重试。"}});
            } catch (const std::exception& error) { connected = write_line({{"type", "error"}, {"text", error.what()}}); }
            if (!connected || once) break;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            Sleep(static_cast<DWORD>(std::clamp<long long>(elapsed, 700, 3000)));
        } while (true);
    } catch (...) { write_line({{"type", "error"}, {"text", "Windows 辅助功能暂不可用。"}}); exit_code = 1; }
    CoUninitialize();
    return exit_code;
}
} // namespace cclive














