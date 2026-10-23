#include "text_layout.hpp"
#include <richedit.h>
#include <algorithm>
#include <array>
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
bool space(wchar_t value) { return value == L' ' || value == L'\t'; }
std::size_t backtick_end(const std::wstring& value, std::size_t start, std::size_t count) {
    for (std::size_t index = start; index < value.size();) {
        if (value[index] != L'`') { ++index; continue; }
        const auto first = index;
        while (index < value.size() && value[index] == L'`') ++index;
        if (index - first == count) return first;
    }
    return std::wstring::npos;
}
std::size_t bold_end(const std::wstring& value, std::size_t start) {
    for (std::size_t index = start; index < value.size();) {
        if (value[index] == L'\\' && index + 1 < value.size()) { index += 2; continue; }
        if (value[index] == L'`') {
            auto end = index;
            while (end < value.size() && value[end] == L'`') ++end;
            const auto closing = backtick_end(value, end, end - index);
            if (closing != std::wstring::npos) { index = closing + end - index; continue; }
        }
        if (value.compare(index, 2, L"**") == 0) return index;
        ++index;
    }
    return std::wstring::npos;
}
void append_inline(TextLayout& layout, const std::wstring& value, bool allow_bold = true) {
    for (std::size_t index = 0; index < value.size();) {
        // Preserve literal escapes and paths; escaped delimiters are not markup.
        if (value[index] == L'\\' && index + 1 < value.size()) {
            layout.text.append(value, index, 2); index += 2; continue;
        }
        if (value[index] == L'`') {
            auto end = index;
            while (end < value.size() && value[end] == L'`') ++end;
            const auto closing = backtick_end(value, end, end - index);
            if (closing == std::wstring::npos) { layout.text.append(value, index, std::wstring::npos); break; }
            const LONG start = static_cast<LONG>(layout.text.size());
            layout.text.append(value, end, closing - end);
            if (closing > end) layout.ranges.push_back({start, static_cast<LONG>(closing - end), TextStyle::Code, 0});
            index = closing + end - index; continue;
        }
        if (allow_bold && value.compare(index, 2, L"**") == 0) {
            const auto end = bold_end(value, index + 2);
            if (end != std::wstring::npos && end > index + 2) {
                const LONG start = static_cast<LONG>(layout.text.size());
                append_inline(layout, value.substr(index + 2, end - index - 2), false);
                layout.ranges.push_back({start, static_cast<LONG>(layout.text.size() - start), TextStyle::Bold, 0});
                index = end + 2; continue;
            }
        }
        layout.text += value[index++];
    }
}
PARAFORMAT2 body_paragraph(int fontPoints) {
    PARAFORMAT2 format{};
    format.cbSize = sizeof(format);
    format.dwMask = PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_LINESPACING |
                    PFM_NUMBERING | PFM_STARTINDENT | PFM_OFFSET |
                    PFM_NUMBERINGTAB | PFM_TABSTOPS | PFM_ALIGNMENT;
    format.wAlignment = PFA_LEFT;
    format.dySpaceAfter = fontPoints * 6; // 0.3 em, in twips.
    format.bLineSpacingRule = 5;
    format.dyLineSpacing = 25; // 25/20 = 1.25 lines, independent of DPI.
    return format;
}
void select_range(HWND edit, LONG start, LONG length) {
    CHARRANGE selection{start, start + length};
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
}
std::array<LONG, 9> list_columns(HWND edit, const TextLayout& layout, int fontPoints) {
    std::array<LONG, 9> columns;
    columns.fill(fontPoints * 36);
    HDC dc = GetDC(edit);
    const int dpi_x = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    const int dpi_y = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    HFONT font = dc ? CreateFontW(-MulDiv(fontPoints, dpi_y, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI") : nullptr;
    const auto old = font ? SelectObject(dc, font) : nullptr;
    for (const auto& paragraph : layout.paragraphs) if (paragraph.ordered) {
        LONG width = paragraph.marker_length * fontPoints * 14;
        SIZE size{};
        if (font && GetTextExtentPoint32W(dc, layout.text.data() + paragraph.start, paragraph.marker_length, &size))
            width = MulDiv(size.cx, 1440, dpi_x);
        auto& column = columns[std::clamp(paragraph.indent_level, 0, 8)];
        column = std::max(column, width + fontPoints * 12);
    }
    if (old) SelectObject(dc, old);
    if (font) DeleteObject(font);
    if (dc) ReleaseDC(edit, dc);
    return columns;
}
}
TextLayout make_text_layout(const std::wstring& input) {
    TextLayout layout;
    const auto lines = split_lines(input);
    bool in_code = false, previous_blank = false;
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
        const bool blank = !in_code && !fence && first == line.size();
        if (blank && previous_blank) continue;
        previous_blank = blank;
        if (!layout.paragraphs.empty()) layout.text += L'\r';
        TextParagraphStyle paragraph;
        paragraph.start = static_cast<LONG>(layout.text.size());
        if (in_code || fence) {
            paragraph.code = true;
            layout.text += line;
            if (!line.empty()) layout.ranges.push_back({paragraph.start, static_cast<LONG>(line.size()), TextStyle::Code, 0});
            if (closes) { in_code = false; fence_character = 0; fence_length = 0; }
            else if (!in_code && fence) { in_code = true; fence_character = line[first]; fence_length = fence_run - first; }
        } else if (!blank) {
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
                while (content < line.size() && space(line[content])) ++content;
            } else {
                auto digits = first;
                while (digits < line.size() && line[digits] >= L'0' && line[digits] <= L'9') ++digits;
                if (digits > first && digits - first <= 9 && digits + 1 < line.size() &&
                    (line[digits] == L'.' || line[digits] == L')') &&
                    (space(line[digits + 1]) || line[digits + 1] >= 0x2e80)) {
                    paragraph.ordered = true;
                    paragraph.indent_level = std::min(columns / 2, 8);
                    paragraph.marker_length = static_cast<int>(digits + 1 - first);
                    layout.text.append(line, first, paragraph.marker_length);
                    layout.text += L'\t';
                    content = digits + 1;
                    while (content < line.size() && space(line[content])) ++content;
                }
            }
            append_inline(layout, line.substr(content));
            if (paragraph.heading_level && layout.text.size() > static_cast<std::size_t>(paragraph.start))
                layout.ranges.push_back({paragraph.start, static_cast<LONG>(layout.text.size() - paragraph.start), TextStyle::Heading, paragraph.heading_level});
        }
        paragraph.length = static_cast<LONG>(layout.text.size() - paragraph.start);
        layout.paragraphs.push_back(paragraph);

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
    const auto columns = list_columns(edit, layout, fontPoints);
    for (const auto& paragraph : layout.paragraphs) {
        select_range(edit, paragraph.start, paragraph.length);
        auto format = body_paragraph(fontPoints);
        if (paragraph.bullet || paragraph.ordered) {
            const auto column = columns[std::clamp(paragraph.indent_level, 0, 8)];
            const auto nesting = paragraph.indent_level * fontPoints * 20;
            format.dxStartIndent = nesting;
            format.dxOffset = column;
            format.cTabCount = 1;
            format.rgxTabs[0] = nesting + column;
            format.dySpaceAfter = fontPoints * 2;
            if (paragraph.bullet) { format.wNumbering = PFN_BULLET; format.wNumberingTab = static_cast<WORD>(column); }
        }
        if (paragraph.heading_level) {
            format.dySpaceBefore = paragraph.start == 0 ? 0 : fontPoints * 12;
            format.dySpaceAfter = fontPoints * 6;
        }
        if (!paragraph.length) format.dySpaceAfter = 0;
        if (paragraph.code) {
            format.bLineSpacingRule = 0; format.dyLineSpacing = 0;
            format.dySpaceAfter = 0;
            format.dxStartIndent = fontPoints * 10;
        }
        SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format));
    }
    // Structural headings first. Bold inherits their size; literal code wins.
    for (const auto style : {TextStyle::Heading, TextStyle::Bold, TextStyle::Code})
    for (const auto& range : layout.ranges) {
        if (range.length <= 0 || range.style != style) continue;
        select_range(edit, range.start, range.length);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        if (range.style == TextStyle::Bold) {
            format.dwMask = CFM_BOLD;
            format.dwEffects = CFE_BOLD;
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
