#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cclive::markdown {
struct Cell { std::size_t start = 0, length = 0; };
template<class C> bool blank(C c) { return c == C(' ') || c == C('\t'); }
// Cell positions refer to the original line, including escaped/inline code text.
template<class C> bool row(std::basic_string_view<C> text, std::vector<Cell>& cells) {
    cells.clear();
    std::size_t first = 0, last = text.size();
    while (first < last && blank(text[first])) ++first;
    while (last > first && blank(text[last - 1])) --last;
    if (first == last) return false;
    std::vector<std::size_t> pipes;
    std::size_t code = 0;
    for (std::size_t i = first; i < last; ++i) {
        if (text[i] == C('\\') && i + 1 < last) { ++i; continue; }
        if (text[i] == C('`')) {
            auto end = i + 1;
            while (end < last && text[end] == C('`')) ++end;
            const auto count = end - i;
            if (!code) code = count;
            else if (code == count) code = 0;
            i = end - 1;
        } else if (!code && text[i] == C('|')) pipes.push_back(i);
    }
    if (pipes.empty()) return false;
    auto append = [&](std::size_t begin, std::size_t end) {
        while (begin < end && blank(text[begin])) ++begin;
        while (end > begin && blank(text[end - 1])) --end;
        cells.push_back({begin, end - begin});
    };
    auto begin = first;
    std::size_t n = 0;
    if (pipes.front() == first) { begin = first + 1; ++n; }
    for (; n < pipes.size(); ++n) {
        append(begin, pipes[n]);
        begin = pipes[n] + 1;
    }
    if (pipes.back() + 1 != last) append(begin, last);
    // At most 63 columns keeps native cell widths and hostile input bounded.
    return !cells.empty() && cells.size() <= 63;
}
template<class C> bool separator(std::basic_string_view<C> text, const std::vector<Cell>& cells) {
    if (cells.empty()) return false;
    for (const auto& cell : cells) {
        auto first = cell.start, last = first + cell.length;
        if (first < last && text[first] == C(':')) ++first;
        if (first < last && text[last - 1] == C(':')) --last;
        if (last - first < 3) return false;
        for (auto i = first; i < last; ++i) if (text[i] != C('-')) return false;
    }
    return true;
}
struct Line { std::size_t start = 0, length = 0; std::vector<Cell> cells; };
template<class C> std::vector<Line> table(std::basic_string_view<C> text) {
    std::vector<Line> result;
    std::size_t begin = 0;
    while (begin < text.size()) {
        auto end = text.find_first_of(std::basic_string<C>{C('\r'), C('\n')}, begin);
        if (end == std::basic_string_view<C>::npos) end = text.size();
        auto line = text.substr(begin, end - begin);
        Line parsed{begin, end - begin, {}};
        if (!row(line, parsed.cells)) return {};
        if (!result.empty() && parsed.cells.size() != result.front().cells.size()) return {};
        result.push_back(std::move(parsed));
        if (end == text.size()) break;
        begin = end + 1;
        if (text[end] == C('\r') && begin < text.size() && text[begin] == C('\n')) ++begin;
    }
    if (result.size() < 2 || !separator(text.substr(result[1].start, result[1].length), result[1].cells)) return {};
    return result;
}
// Retain a large table's header plus its newest data rows within the same budget.
inline std::optional<std::string> table_tail(std::string_view text, std::size_t maximum) {
    if (maximum < 3) return std::nullopt;
    const auto rows = table(text);
    if (rows.empty() || rows.size() <= maximum) return std::nullopt;
    std::string result(text.substr(rows[0].start, rows[1].start + rows[1].length - rows[0].start));
    result += '\n';
    result += text.substr(rows[rows.size() - maximum + 2].start);
    return result;
}
inline std::string decode_cell(std::string_view text) {
    std::string result;
    for (std::size_t n = 0; n < text.size(); ++n) {
        if (text[n] == '\\' && n + 1 < text.size() && (text[n + 1] == '|' || text[n + 1] == '\\')) { result += text[++n]; }
        else if (text.substr(n, 5) == "&amp;") { result += '&'; n += 4; }
        else if (text.substr(n, 4) == "&lt;") { result += '<'; n += 3; }
        else if (text.substr(n, 4) == "&gt;") { result += '>'; n += 3; }
        else if (text.substr(n, 4) == "<br>") { result += '\n'; n += 3; }
        else result += text[n];
    }
    return result;
}
inline std::string encode_cell(std::string_view text) {
    std::string result;
    for (std::size_t n = 0; n < text.size(); ++n) {
        if (text[n] == '\\') result += "\\\\";
        else if (text[n] == '&') result += "&amp;";
        else if (text[n] == '<') result += "&lt;";
        else if (text[n] == '>') result += "&gt;";
        else if (text[n] == '|') result += "\\|";
        else if (text[n] == '\r' || text[n] == '\n') {
            if (text[n] == '\r' && n + 1 < text.size() && text[n + 1] == '\n') ++n;
            result += "<br>";
        } else result += text[n];
    }
    return result;
}
} // namespace cclive::markdown
