#include "text_layout.hpp"
#include "markdown_table.hpp"
#include <richedit.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <string_view>
#include <cstring>
namespace cclive {
namespace {
struct SourceLine {
    std::wstring text;
    std::size_t start;
};
std::vector<SourceLine> split_lines(const std::wstring& input) {
    std::vector<SourceLine> lines;
    std::size_t begin = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != L'\r' && input[index] != L'\n') continue;
        lines.push_back({input.substr(begin, index - begin), begin});
        if (input[index] == L'\r' && index + 1 < input.size() && input[index + 1] == L'\n') ++index;
        begin = index + 1;
    }
    lines.push_back({input.substr(begin), begin});
    return lines;
}
// Map only the requested source span, keeping layout output and structural
// parsing unchanged. Source runs remain UTF-16; generated CRs/tabs between
// surviving glyphs are naturally inside the single resulting style range.
struct HighlightMap {
    std::size_t first = 0, last = 0;
    LONG displayed_first = -1, displayed_last = -1;
    HighlightMap(std::size_t input_size, LONG start, LONG length) {
        if (start < 0 || length <= 0) return;
        first = std::min(input_size, static_cast<std::size_t>(start));
        last = first + std::min(input_size - first, static_cast<std::size_t>(length));
    }
    void append(TextLayout& layout, std::wstring_view run, std::size_t source_start) {
        if (run.empty()) return;
        const auto displayed_start = layout.text.size();
        layout.text.append(run.data(), run.size());
        if (first >= last) return;
        const auto overlap_first = std::max(first, source_start);
        const auto overlap_last = std::min(last, source_start + run.size());
        if (overlap_first >= overlap_last) return;
        const auto begin = static_cast<LONG>(displayed_start + overlap_first - source_start);
        const auto end = static_cast<LONG>(displayed_start + overlap_last - source_start);
        if (displayed_first < 0) displayed_first = begin;
        displayed_last = end;
    }
    void finish(TextLayout& layout) const {
        if (displayed_first >= 0 && displayed_last > displayed_first)
            layout.ranges.push_back({displayed_first, displayed_last - displayed_first, TextStyle::Latest, 0});
    }
};
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
void append_run(TextLayout& layout, std::wstring_view value, std::size_t source_start, HighlightMap& highlight, bool table) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (table && value[index] == L'\\' && index + 1 < value.size() && (value[index + 1] == L'|' || value[index + 1] == L'\\')) {
            highlight.append(layout, value.substr(index + 1, 1), source_start + index + 1); ++index;
        } else if (table && value.substr(index, 4) == L"<br>") {
            highlight.append(layout, L"\v", source_start + index); index += 3;
        } else if (table && value.substr(index, 5) == L"&amp;") {
            highlight.append(layout, L"&", source_start + index); index += 4;
        } else if (table && value.substr(index, 4) == L"&lt;") {
            highlight.append(layout, L"<", source_start + index); index += 3;
        } else if (table && value.substr(index, 4) == L"&gt;") {
            highlight.append(layout, L">", source_start + index); index += 3;
        } else highlight.append(layout, value.substr(index, 1), source_start + index);
    }
}
void append_inline(TextLayout& layout, const std::wstring& value, std::size_t source_start, HighlightMap& highlight, bool allow_bold = true, bool table = false) {
    for (std::size_t index = 0; index < value.size();) {
        if (table && ((value[index] == L'\\' && index + 1 < value.size() && (value[index + 1] == L'|' || value[index + 1] == L'\\')) || value.compare(index, 4, L"<br>") == 0)) {
            const auto count = value[index] == L'\\' ? 2 : 4;
            append_run(layout, std::wstring_view(value).substr(index, count), source_start + index, highlight, true); index += count; continue;
        }
        if (table && (value.compare(index, 5, L"&amp;") == 0 || value.compare(index, 4, L"&lt;") == 0 || value.compare(index, 4, L"&gt;") == 0)) {
            const auto count = value.compare(index, 5, L"&amp;") == 0 ? 5 : 4;
            append_run(layout, std::wstring_view(value).substr(index, count), source_start + index, highlight, true); index += count; continue;
        }
        // Preserve literal escapes and paths; escaped delimiters are not markup.
        if (value[index] == L'\\' && index + 1 < value.size()) {
            highlight.append(layout, std::wstring_view(value).substr(index, 2), source_start + index); index += 2; continue;
        }
        if (value[index] == L'`') {
            auto end = index;
            while (end < value.size() && value[end] == L'`') ++end;
            const auto closing = backtick_end(value, end, end - index);
            if (closing == std::wstring::npos) { highlight.append(layout, std::wstring_view(value).substr(index), source_start + index); break; }
            const LONG start = static_cast<LONG>(layout.text.size());
            append_run(layout, std::wstring_view(value).substr(end, closing - end), source_start + end, highlight, table);
            if (closing > end) layout.ranges.push_back({start, static_cast<LONG>(layout.text.size() - start), TextStyle::Code, 0});
            index = closing + end - index; continue;
        }
        if (allow_bold && value.compare(index, 2, L"**") == 0) {
            const auto end = bold_end(value, index + 2);
            if (end != std::wstring::npos && end > index + 2) {
                const LONG start = static_cast<LONG>(layout.text.size());
                append_inline(layout, value.substr(index + 2, end - index - 2), source_start + index + 2, highlight, false, table);
                layout.ranges.push_back({start, static_cast<LONG>(layout.text.size() - start), TextStyle::Bold, 0});
                index = end + 2; continue;
            }
        }
        highlight.append(layout, std::wstring_view(value).substr(index, 1), source_start + index); ++index;
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
TextLayout make_text_layout(const std::wstring& input, LONG highlightStart, LONG highlightLength) {
    TextLayout layout;
    HighlightMap highlight(input.size(), highlightStart, highlightLength);
    const auto lines = split_lines(input);
    bool in_code = false, previous_blank = false;
    wchar_t fence_character = 0;
    std::size_t fence_length = 0;
    std::size_t table_columns = 0;
    std::vector<int> table_alignments;
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const auto& line = lines[line_index].text;
        const auto source_start = lines[line_index].start;
        std::vector<markdown::Cell> cells, separator_cells;
        const bool table_row = !in_code && markdown::row<wchar_t>(line, cells) && cells.size() <= 63;
        bool table_header = false;
        if (table_row && line_index + 1 < lines.size()) {
            const auto& separator = lines[line_index + 1].text;
            table_header = markdown::row<wchar_t>(separator, separator_cells) &&
                separator_cells.size() == cells.size() && markdown::separator<wchar_t>(separator, separator_cells);
            if (table_header) {
                table_columns = cells.size(); table_alignments.clear();
                for (const auto& cell : separator_cells) {
                    const auto alignment = std::wstring_view(separator).substr(cell.start, cell.length);
                    table_alignments.push_back(alignment.back() == L':' ? (alignment.front() == L':' ? 1 : 2) : 0);
                }
            }
        }
        if (table_row && (table_header || table_columns == cells.size())) {
            layout.has_tables = true;
            if (!layout.paragraphs.empty()) layout.text += L'\r';
            TextParagraphStyle paragraph;
            paragraph.start = static_cast<LONG>(layout.text.size());
            paragraph.table = true; paragraph.table_header = table_header;
            paragraph.cell_alignments = table_alignments;
            for (std::size_t column = 0; column < cells.size(); ++column) {
                if (column) layout.text += L'\t';
                const auto& cell = cells[column];
                append_inline(layout, line.substr(cell.start, cell.length), source_start + cell.start, highlight, true, true);
                paragraph.cell_ends.push_back(static_cast<LONG>(layout.text.size()) - paragraph.start);
            }
            paragraph.length = static_cast<LONG>(layout.text.size()) - paragraph.start;
            if (table_header && paragraph.length)
                layout.ranges.push_back({paragraph.start, paragraph.length, TextStyle::Bold, 0});
            layout.paragraphs.push_back(std::move(paragraph));
            previous_blank = false;
            if (table_header) ++line_index;
            continue;
        }
        table_columns = 0;
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
            highlight.append(layout, line, source_start);
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
                    highlight.append(layout, std::wstring_view(line).substr(first, paragraph.marker_length), source_start + first);
                    layout.text += L'\t';
                    content = digits + 1;
                    while (content < line.size() && space(line[content])) ++content;
                }
            }
            append_inline(layout, line.substr(content), source_start + content, highlight);
            if (paragraph.heading_level && layout.text.size() > static_cast<std::size_t>(paragraph.start))
                layout.ranges.push_back({paragraph.start, static_cast<LONG>(layout.text.size() - paragraph.start), TextStyle::Heading, paragraph.heading_level});
        }
        paragraph.length = static_cast<LONG>(layout.text.size() - paragraph.start);
        layout.paragraphs.push_back(paragraph);

    }
    // Native table rows always terminate in a paragraph, even at document end.
    if (!layout.paragraphs.empty() && layout.paragraphs.back().table) {
        layout.text += L'\r';
        TextParagraphStyle tail; tail.start = static_cast<LONG>(layout.text.size());
        layout.paragraphs.push_back(tail);
    }
    if (layout.has_tables) {
        layout.native_positions.resize(layout.text.size() + 1);
        LONG native = 0;
        for (std::size_t n = 0; n < layout.paragraphs.size(); ++n) {
            const auto& paragraph = layout.paragraphs[n];
            if (paragraph.table) native += 2; // Native row opener U+FFF9 and CR.
            for (LONG p = 0; p < paragraph.length; ++p)
                layout.native_positions[paragraph.start + p] = native++;
            layout.native_positions[paragraph.start + paragraph.length] = native;
            if (paragraph.table) native += 2; // Final cell delimiter and U+FFFB.
            if (n + 1 < layout.paragraphs.size()) ++native;
        }
    }
    highlight.finish(layout);
    return layout;
}
LONG native_text_position(const TextLayout& layout, LONG displayed) {
    if (displayed < 0 || layout.native_positions.empty()) return displayed;
    const auto limit = static_cast<LONG>(layout.native_positions.size() - 1);
    return layout.native_positions[std::min(displayed, limit)];
}
namespace {
void rtf_text(std::string& out, std::wstring_view text) {
    for (const auto ch : text) {
        if (ch == L'\\' || ch == L'{' || ch == L'}') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch == L'\t') out += "\\tab ";
        else if (ch == L'\v') out += "\\line ";
        else if (ch >= 32 && ch < 127) out += static_cast<char>(ch);
        else out += "\\u" + std::to_string(static_cast<short>(ch)) + "?";
    }
}
struct RtfInput { const std::string& text; std::size_t offset = 0; };
DWORD CALLBACK rtf_read(DWORD_PTR cookie, LPBYTE target, LONG count, LONG* read) {
    auto& input = *reinterpret_cast<RtfInput*>(cookie);
    *read = static_cast<LONG>(std::min(static_cast<std::size_t>(count), input.text.size() - input.offset));
    std::memcpy(target, input.text.data() + input.offset, *read); input.offset += *read; return 0;
}
}
bool replace_text_layout(HWND edit, const TextLayout& layout) {
    if (!layout.has_tables) return SetWindowTextW(edit, layout.text.c_str()) != FALSE;
    RECT rect{}; SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&rect));
    HDC dc = GetDC(edit); const auto dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(edit, dc);
    const LONG width = std::max<LONG>(720, MulDiv(std::max<LONG>(48, rect.right - rect.left - 8), 1440, dpi));
    std::string rtf = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Microsoft YaHei UI;}}{\\colortbl;\\red83\\green81\\blue76;\\red49\\green49\\blue46;\\red42\\green42\\blue40;}\\uc1\\fs24 ";
    for (std::size_t n = 0; n < layout.paragraphs.size(); ++n) {
        const auto& paragraph = layout.paragraphs[n];
        if (paragraph.table) {
            rtf += "\\trowd\\trgaph90\\trleft0";
            const auto count = paragraph.cell_ends.size();
            for (std::size_t cell = 0; cell < count; ++cell) {
                rtf += "\\clvertalt\\clbrdrt\\brdrs\\brdrw10\\brdrcf1\\clbrdrl\\brdrs\\brdrw10\\brdrcf1\\clbrdrb\\brdrs\\brdrw10\\brdrcf1\\clbrdrr\\brdrs\\brdrw10\\brdrcf1";
                rtf += paragraph.table_header ? "\\clcbpat2" : "\\clcbpat3";
                rtf += "\\cellx" + std::to_string(width * static_cast<LONG>(cell + 1) / static_cast<LONG>(count));
            }
            LONG begin = 0;
            for (std::size_t cell = 0; cell < count; ++cell) {
                const auto alignment = cell < paragraph.cell_alignments.size() ? paragraph.cell_alignments[cell] : 0;
                rtf += alignment == 1 ? "\\pard\\intbl\\qc " : alignment == 2 ? "\\pard\\intbl\\qr " : "\\pard\\intbl\\ql ";
                rtf_text(rtf, std::wstring_view(layout.text).substr(paragraph.start + begin, paragraph.cell_ends[cell] - begin));
                rtf += "\\cell "; begin = paragraph.cell_ends[cell] + 1;
            }
            rtf += "\\row ";
        } else {
            rtf += "\\pard "; rtf_text(rtf, std::wstring_view(layout.text).substr(paragraph.start, paragraph.length));
            if (n + 1 < layout.paragraphs.size()) rtf += "\\par ";
        }
    }
    rtf += '}'; RtfInput input{rtf}; EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&input), 0, rtf_read};
    SendMessageW(edit, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
    return stream.dwError == 0;
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
        select_range(edit, native_text_position(layout, paragraph.start), native_text_position(layout, paragraph.start + paragraph.length) - native_text_position(layout, paragraph.start));
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
        if (paragraph.table) { format.dySpaceAfter = 0; format.dySpaceBefore = 0; }
        if (paragraph.code) {
            format.bLineSpacingRule = 0; format.dyLineSpacing = 0;
            format.dySpaceAfter = 0;
            format.dxStartIndent = fontPoints * 10;
        }
        SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format));
        if (paragraph.table) {
            LONG begin = 0;
            for (std::size_t cell = 0; cell < paragraph.cell_ends.size(); ++cell) {
                const auto start = native_text_position(layout, paragraph.start + begin);
                const auto end = native_text_position(layout, paragraph.start + paragraph.cell_ends[cell]);
                select_range(edit, start, std::max<LONG>(0, end - start));
                PARAFORMAT2 alignment{}; alignment.cbSize = sizeof(alignment); alignment.dwMask = PFM_ALIGNMENT;
                const auto value = cell < paragraph.cell_alignments.size() ? paragraph.cell_alignments[cell] : 0;
                alignment.wAlignment = value == 1 ? PFA_CENTER : value == 2 ? PFA_RIGHT : PFA_LEFT;
                SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&alignment));
                begin = paragraph.cell_ends[cell] + 1;
            }
        }
    }
    // Structural headings first. Bold inherits their size; literal code wins.
    for (const auto style : {TextStyle::Heading, TextStyle::Bold, TextStyle::Code, TextStyle::Latest})
    for (const auto& range : layout.ranges) {
        if (range.length <= 0 || range.style != style) continue;
        select_range(edit, native_text_position(layout, range.start), native_text_position(layout, range.start + range.length) - native_text_position(layout, range.start));
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
        } else if (range.style == TextStyle::Latest) {
            format.dwMask = CFM_COLOR | CFM_BACKCOLOR;
            format.crTextColor = text_layout_palette.latest;
            format.crBackColor = text_layout_palette.latest_background;
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
