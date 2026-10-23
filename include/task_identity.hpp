#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
namespace cclive {
inline std::string trim_task_text(std::string value) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && space(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && space(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}
inline std::string task_ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
inline std::string canonical_task_url(std::string value) {
    value = trim_task_text(std::move(value));
    if (value.size() > 8192) return {};
    for (unsigned char c : value) if (c <= 0x20 || c == 0x7f) return {};
    const auto end = value.find_first_of("?#");
    if (end != std::string::npos) value.resize(end);
    auto lowered = task_ascii_lower(value);
    if (lowered.rfind("https://", 0) == 0) value.erase(0, 8);
    else if (lowered.rfind("http://", 0) == 0) value.erase(0, 7);
    else if (value.find("://") != std::string::npos) return {};
    const auto slash = value.find('/');
    if (slash == std::string::npos) return {};
    auto host = task_ascii_lower(value.substr(0, slash));
    if (host == "claude.ai:443" || host == "www.claude.ai:443") host.resize(host.size() - 4);
    if (host != "claude.ai" && host != "www.claude.ai") return {};
    std::vector<std::string> parts;
    for (std::size_t start = slash + 1; start <= value.size();) {
        const auto next = value.find('/', start);
        const auto length = next == std::string::npos ? value.size() - start : next - start;
        if (length) parts.push_back(value.substr(start, length));
        if (next == std::string::npos) break;
        start = next + 1;
    }
    std::string route, id;
    if (parts.size() >= 2 && (parts[0] == "chat" || parts[0] == "epitaxy")) { route = parts[0]; id = parts[1]; }
    else if (parts.size() >= 4 && parts[0] == "project" && parts[2] == "chat") { route = "chat"; id = parts[3]; }
    if (id.size() < 4 || id.size() > 200 || (route == "epitaxy" && (id.rfind("local_", 0) != 0 || id.size() <= 6))) return {};
    const auto reserved = task_ascii_lower(id);
    for (const char* blank : {"new", "new-chat", "new_chat", "new-conversation", "new_conversation", "local_new", "local_new_chat", "local_new-chat"}) if (reserved == blank) return {};
    for (unsigned char c : id) if (!std::isalnum(c) && c != '-' && c != '_') return {};
    return "https://claude.ai/" + route + "/" + id;
}
inline std::string clean_task_title(std::string value) {
    value = trim_task_text(std::move(value));
    bool removed = true;
    while (removed) {
        removed = false;
        const auto lowered = task_ascii_lower(value);
        for (const char* suffix : {" - Claude Code", " — Claude Code", " – Claude Code", " | Claude Code", " - Claude", " — Claude", " – Claude", " | Claude"}) {
            const std::string ending = task_ascii_lower(suffix);
            if (lowered.size() >= ending.size() && lowered.compare(lowered.size() - ending.size(), ending.size(), ending) == 0) {
                value = trim_task_text(value.substr(0, value.size() - ending.size())); removed = true; break;
            }
        }
    }
    const auto lowered = task_ascii_lower(value);
    if (lowered == "claude" || lowered == "claude code") return {};
    return value;
}
}

