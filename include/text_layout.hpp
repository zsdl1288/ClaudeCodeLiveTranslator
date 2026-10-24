#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
namespace cclive {
enum class TextStyle { Bold, Heading, Code, Latest };
struct TextLayoutPalette {
    COLORREF background, body, heading, code, code_background, latest, latest_background;
};
inline constexpr TextLayoutPalette text_layout_palette{
    RGB(42, 42, 40), RGB(218, 216, 210), RGB(240, 238, 232),
    RGB(209, 205, 194), RGB(34, 34, 32), RGB(126, 232, 208), RGB(43, 66, 61)};
struct TextStyleRange {
    LONG start = 0;
    LONG length = 0;
    TextStyle style = TextStyle::Bold;
    int heading_level = 0;
};
struct TextParagraphStyle {
    LONG start = 0;
    LONG length = 0;
    int heading_level = 0;
    int indent_level = 0;
    bool bullet = false;
    bool code = false;
    bool ordered = false;
    bool table = false, table_header = false;
    std::vector<LONG> cell_ends; // Relative UTF-16 cell ends, excluding separator tabs.
    std::vector<int> cell_alignments; // 0 left, 1 center, 2 right.
    int marker_length = 0; // Source-owned number and separator before the display tab.
};
struct TextLayout {
    // Single CR paragraph separators match RichEdit's UTF-16 character positions.
    std::wstring text;
    std::vector<TextStyleRange> ranges;
    std::vector<TextParagraphStyle> paragraphs;
    bool has_tables = false;
    std::vector<LONG> native_positions; // Display UTF-16 boundaries to RichEdit native cp.
};
// The optional highlight span uses raw input UTF-16 positions, before Markdown removal.
TextLayout make_text_layout(const std::wstring& input, LONG highlightStart = -1, LONG highlightLength = 0);
LONG native_text_position(const TextLayout& layout, LONG displayed);
// Replaces caller-owned text using native RTF table rows when needed.
bool replace_text_layout(HWND edit, const TextLayout& layout);
// Formats only this caller-owned RichEdit, resetting zoom and all inherited styles.
// Does not replace text or focus; the caller preserves its viewport around reflow.
void apply_text_layout(HWND edit, const TextLayout& layout, int fontPoints, int dpi);
}
