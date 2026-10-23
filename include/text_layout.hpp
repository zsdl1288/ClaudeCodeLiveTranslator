#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
namespace cclive {
enum class TextStyle { Bold, Heading, Code };
struct TextLayoutPalette {
    COLORREF background, body, heading, code, code_background;
};
inline constexpr TextLayoutPalette text_layout_palette{
    RGB(42, 42, 40), RGB(218, 216, 210), RGB(240, 238, 232),
    RGB(209, 205, 194), RGB(34, 34, 32)};
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
};
struct TextLayout {
    // Single CR paragraph separators match RichEdit's UTF-16 character positions.
    std::wstring text;
    std::vector<TextStyleRange> ranges;
    std::vector<TextParagraphStyle> paragraphs;
};
TextLayout make_text_layout(const std::wstring& input);
// Formats only this caller-owned RichEdit, resetting zoom and all inherited styles.
// Does not replace text or focus; the caller preserves its viewport around reflow.
void apply_text_layout(HWND edit, const TextLayout& layout, int fontPoints, int dpi);
}
