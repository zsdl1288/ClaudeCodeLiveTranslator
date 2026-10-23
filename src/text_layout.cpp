#include "text_layout.hpp"
#include <richedit.h>
#include <algorithm>
#include <cwchar>
namespace cclive {
namespace {
std::vector<std::wstring> split_lines(const std::wstring& input) {
    std::vector<std::wstring> lines;
    std::size_t begin = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != L'\r' && input[index] != L'\n') continue;
        lines.push_back(input.substr(begin, index - begin));
        if (input[index] == L'\r' && index + 1 < input.size() && input[index + 1] == L'\n') ++index;
        begin = index + 1;
    }
    lines.push_back(input.substr(begin));
    return lines;
}
void append_inline(TextLayout& layout, const std::wstring& value) {
    for (std::size_t index = 0; index < value.size();) {
        if (value.compare(index, 2, L"**") == 0) {
            const auto end = value.find(L"**", index + 2);
            if (end != std::wstring::npos && end > index + 2) {
                const LONG start = static_cast<LONG>(layout.text.size());
                layout.text.append(value, index + 2, end - index - 2);
                layout.ranges.push_back({start, static_cast<LONG>(end - index - 2), TextStyle::Bold, 0});
                index = end + 2;
                continue;
            }
        }
        layout.text += value[index++];
    }
}
PARAFORMAT2 body_paragraph(int fontPoints) {
    PARAFORMAT2 format{};
    format.cbSize = sizeof(format);
    format.dwMask = PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_LINESPACING |
                    PFM_NUMBERING | PFM_STARTINDENT | PFM_OFFSET | PFM_NUMBERINGTAB;
    format.dySpaceAfter = std::max(12, fontPoints * 2);
    format.bLineSpacingRule = 0;
    return format;
}
void select_range(HWND edit, LONG start, LONG length) {
    CHARRANGE selection{start, start + length};
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
}
}
TextLayout make_text_layout(const std::wstring& input) {
    TextLayout layout;
    const auto lines = split_lines(input);
    bool in_code = false;
    wchar_t fence_character = 0;
    std::size_t fence_length = 0;
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const auto& line = lines[line_index];
        std::size_t first = 0;
        int columns = 0;
        while (first < line.size() && (line[first] == L' ' || line[first] == L'\t')) {
            columns += line[first] == L'\t' ? 4 : 1; ++first;
        }
        std::size_t fence_run = first;
        if (first < line.size() && (line[first] == L'`' || line[first] == L'~'))
            while (fence_run < line.size() && line[fence_run] == line[first]) ++fence_run;
        const bool fence = fence_run - first >= 3;
        const bool closes = in_code && fence && line[first] == fence_character && fence_run - first >= fence_length;
        TextParagraphStyle paragraph;
        paragraph.start = static_cast<LONG>(layout.text.size());
        if (in_code || fence) {
            paragraph.code = true;
            layout.text += line;
            if (!line.empty()) layout.ranges.push_back({paragraph.start, static_cast<LONG>(line.size()), TextStyle::Code, 0});
            if (closes) { in_code = false; fence_character = 0; fence_length = 0; }
            else if (!in_code && fence) { in_code = true; fence_character = line[first]; fence_length = fence_run - first; }
        } else {
            std::size_t content = 0;
            std::size_t hashes = first;
            while (hashes < line.size() && line[hashes] == L'#') ++hashes;
            if (columns <= 3 && hashes > first && hashes - first <= 3 && hashes < line.size() && (line[hashes] == L' ' || line[hashes] == L'\t')) {
                paragraph.heading_level = static_cast<int>(hashes - first);
                content = hashes;
                while (content < line.size() && (line[content] == L' ' || line[content] == L'\t')) ++content;
            } else if (first + 1 < line.size() && (line[first] == L'-' || line[first] == L'*' || line[first] == L'+') && (line[first + 1] == L' ' || line[first + 1] == L'\t')) {
                paragraph.bullet = true;
                paragraph.indent_level = std::min(columns / 2, 8);
                content = first + 2;
            }
            append_inline(layout, line.substr(content));
            if (paragraph.heading_level && layout.text.size() > static_cast<std::size_t>(paragraph.start))
                layout.ranges.push_back({paragraph.start, static_cast<LONG>(layout.text.size() - paragraph.start), TextStyle::Heading, paragraph.heading_level});
        }
        paragraph.length = static_cast<LONG>(layout.text.size() - paragraph.start);
        layout.paragraphs.push_back(paragraph);
        if (line_index + 1 < lines.size()) layout.text += L'\r';
    }
    return layout;
}
void apply_text_layout(HWND edit, const TextLayout& layout, int fontPoints, int dpi) {
    (void)dpi; // RichEdit character sizes and paragraph distances are in twips.
    if (!edit || !IsWindow(edit)) return;
    fontPoints = std::clamp(fontPoints, 6, 96);
    CHARRANGE saved{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    CHARRANGE all{0, -1};
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&all));
    // A prior RichEdit zoom must not silently override a new point-size setting.
    SendMessageW(edit, EM_SETZOOM, 1, 1);
    SendMessageW(edit, EM_SETBKGNDCOLOR, 0, text_layout_palette.background);
    CHARFORMAT2W body{};
    body.cbSize = sizeof(body);
    body.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE |
                  CFM_STRIKEOUT | CFM_COLOR | CFM_BACKCOLOR;
    body.crTextColor = text_layout_palette.body;
    body.crBackColor = text_layout_palette.background;
    body.yHeight = fontPoints * 20;
    wcsncpy(body.szFaceName, L"Microsoft YaHei UI", LF_FACESIZE - 1);
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&body));
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&body));
    auto base = body_paragraph(fontPoints);
    SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&base));
    for (const auto& paragraph : layout.paragraphs) {
        select_range(edit, paragraph.start, paragraph.length);
        auto format = body_paragraph(fontPoints);
        if (paragraph.bullet) {
            format.wNumbering = PFN_BULLET;
            format.dxStartIndent = 240 * (paragraph.indent_level + 1);
            format.dxOffset = 120;
            format.wNumberingTab = 240;
        }
        if (paragraph.heading_level) {
            format.dySpaceBefore = fontPoints * 4;
            format.dySpaceAfter = fontPoints * 2;
        }
        if (paragraph.code) {
            format.dySpaceAfter = 0;
            format.dxStartIndent = 120;
        }
        SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format));
    }
    for (const auto& range : layout.ranges) {
        if (range.length <= 0) continue;
        select_range(edit, range.start, range.length);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        if (range.style == TextStyle::Bold) {
            format.dwMask = CFM_BOLD | CFM_SIZE | CFM_COLOR;
            format.dwEffects = CFE_BOLD;
            format.yHeight = fontPoints * 20;
            format.crTextColor = text_layout_palette.body;
        }
        else if (range.style == TextStyle::Heading) {
            format.dwMask = CFM_BOLD | CFM_SIZE | CFM_COLOR; format.dwEffects = CFE_BOLD;
            format.crTextColor = text_layout_palette.heading;
            format.yHeight = (fontPoints + 4 - std::clamp(range.heading_level, 1, 3)) * 20;
        } else {
            format.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_COLOR | CFM_BACKCOLOR;
            format.crTextColor = text_layout_palette.code;
            format.crBackColor = text_layout_palette.code_background;
            format.yHeight = fontPoints * 20;
            wcsncpy(format.szFaceName, L"Consolas", LF_FACESIZE - 1);
        }
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    }
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
}
}
