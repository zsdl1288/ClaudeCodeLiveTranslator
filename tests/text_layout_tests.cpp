#include "text_layout.hpp"
#include <algorithm>
#include <richedit.h>
#include <cwchar>
#include <iostream>
#include <stdexcept>
namespace {
int checks = 0;
void check(bool condition, const char* text) { ++checks; if (!condition) throw std::runtime_error(text); }
std::size_t styles(const cclive::TextLayout& layout, cclive::TextStyle style) {
    return std::count_if(layout.ranges.begin(), layout.ranges.end(), [&](const auto& range) { return range.style == style; });
}
}
void native_format_checks() {
    const HMODULE library = LoadLibraryW(L"Msftedit.dll");
    check(library != nullptr, "native RichEdit library could not load");
    const HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE | ES_READONLY,
        0, 0, 640, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(edit != nullptr, "hidden native RichEdit could not be created");
    const auto layout = cclive::make_text_layout(L"# Heading\nNormal **emphasis**.\n- bullet\n```\ncode line\n```");
    SetWindowTextW(edit, layout.text.c_str());
    CHARFORMAT2W stale{};
    stale.cbSize = sizeof(stale); stale.dwMask = CFM_SIZE | CFM_ITALIC | CFM_UNDERLINE | CFM_COLOR;
    stale.dwEffects = CFE_ITALIC | CFE_UNDERLINE; stale.yHeight = 700; stale.crTextColor = RGB(0, 0, 0);
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&stale));
    SendMessageW(edit, EM_SETZOOM, 3, 2);
    CHARRANGE selected{2, 7};
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
    cclive::apply_text_layout(edit, layout, 10, 96);
    CHARRANGE restored{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&restored));
    check(restored.cpMin == selected.cpMin && restored.cpMax == selected.cpMax, "formatting did not restore caller selection");
    auto read_style = [&](LONG position) {
        CHARRANGE selection{position, position + 1};
        SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        CHARFORMAT2W style{}; style.cbSize = sizeof(style);
        SendMessageW(edit, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&style));
        return style;
    };
    const auto body_position = static_cast<LONG>(layout.text.find(L"Normal"));
    const auto bold_position = static_cast<LONG>(layout.text.find(L"emphasis"));
    const auto code_position = static_cast<LONG>(layout.text.find(L"code line"));
    const auto normal10 = read_style(body_position);
    check(normal10.yHeight == 200 && (normal10.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_UNDERLINE)) == 0, "body retained old size or inherited effects");
    check(normal10.crTextColor == cclive::text_layout_palette.body && normal10.crBackColor == cclive::text_layout_palette.background, "body did not use warm foreground/background palette");
    const auto heading10 = read_style(0);
    check(heading10.yHeight == 260 && (heading10.dwEffects & CFE_BOLD) != 0 && heading10.crTextColor == cclive::text_layout_palette.heading, "heading hierarchy did not match selected base size and palette");
    const auto bold10 = read_style(bold_position);
    check(bold10.yHeight == 200 && (bold10.dwEffects & CFE_BOLD) != 0, "inline bold size was inherited from stale formatting");
    const auto code10 = read_style(code_position);
    check(code10.yHeight == 200 && std::wcscmp(code10.szFaceName, L"Consolas") == 0 && (code10.dwEffects & CFE_BOLD) == 0, "code did not reset face, size and weight");
    check(code10.crTextColor == cclive::text_layout_palette.code && code10.crBackColor == cclive::text_layout_palette.code_background, "code hierarchy palette incorrect");
    int numerator = 0, denominator = 0;
    SendMessageW(edit, EM_GETZOOM, reinterpret_cast<WPARAM>(&numerator), reinterpret_cast<LPARAM>(&denominator));
    check(numerator == 1 && denominator == 1, "old RichEdit zoom overrode point-size setting");
    cclive::apply_text_layout(edit, layout, 18, 144);
    check(read_style(body_position).yHeight == 360 && read_style(bold_position).yHeight == 360 && read_style(code_position).yHeight == 360, "10 to 18 point change did not update all body/code styles");
    check(read_style(0).yHeight == 420, "heading did not follow 18 point base size");
    CHARFORMAT2W defaults{}; defaults.cbSize = sizeof(defaults);
    SendMessageW(edit, EM_GETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&defaults));
    check(defaults.yHeight == 360 && defaults.crTextColor == cclive::text_layout_palette.body, "insertion defaults did not follow selected point size and color");
    cclive::apply_text_layout(edit, layout, 10, 96);
    check(read_style(body_position).yHeight == 200 && read_style(0).yHeight == 260 && read_style(code_position).yHeight == 200, "18 to 10 point change left stale special style sizes");
    const auto plain = cclive::make_text_layout(L"Plain text replacing heading and code.");
    SetWindowTextW(edit, plain.text.c_str());
    cclive::apply_text_layout(edit, plain, 16, 96);
    const auto changed = read_style(0);
    check(changed.yHeight == 320 && changed.crTextColor == cclive::text_layout_palette.body && std::wcscmp(changed.szFaceName, L"Microsoft YaHei UI") == 0, "replaced plain body retained prior heading/code format");
    check((changed.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_UNDERLINE)) == 0 && changed.crBackColor == cclive::text_layout_palette.background, "plain replacement retained prior effects or code background");
    check(GetForegroundWindow() != edit, "formatter activated hidden caller-owned RichEdit");
    DestroyWindow(edit); FreeLibrary(library);
}
int main() {
    try {
        const auto plain = cclive::make_text_layout(L"你好\nsecond\r\nthird\r");
        check(plain.text == L"你好\rsecond\rthird\r", "paragraph separators do not match RichEdit character positions");
        check(plain.paragraphs.size() == 4 && plain.ranges.empty(), "plain Unicode paragraphs were guessed as formatted text");
        const auto headings = cclive::make_text_layout(L"# One\n## Two\n### Three\n#### Literal\n    # Indented");
        check(headings.text == L"One\rTwo\rThree\r#### Literal\r    # Indented", "heading markers removed outside supported literal Markdown");
        check(styles(headings, cclive::TextStyle::Heading) == 3, "heading levels were not parsed");
        check(headings.paragraphs[0].heading_level == 1 && headings.paragraphs[1].heading_level == 2 && headings.paragraphs[2].heading_level == 3, "heading paragraph levels incorrect");
        const auto bold = cclive::make_text_layout(L"Text **bold** end **two**.");
        check(bold.text == L"Text bold end two.", "bold delimiters were not removed");
        check(styles(bold, cclive::TextStyle::Bold) == 2 && bold.ranges[0].start == 5 && bold.ranges[0].length == 4, "bold range does not match visible UTF16 text");
        check(cclive::make_text_layout(L"**unfinished").text == L"**unfinished", "incomplete streamed bold text was destroyed");
        check(cclive::make_text_layout(L"****").text == L"****", "empty unmatched delimiters were consumed");
        const auto unicode = cclive::make_text_layout(L"\U0001F600 **文本**");
        check(unicode.ranges.size() == 1 && static_cast<std::size_t>(unicode.ranges[0].start) == unicode.text.find(L"文本") && unicode.ranges[0].length == 2, "Unicode range offsets are not UTF16 positions");
        const auto bullet = cclive::make_text_layout(L"- first\n  * nested\n+ third\n1. Own number\n2) Own number");
        check(bullet.text == L"first\rnested\rthird\r1. Own number\r2) Own number", "list markers or content-owned numbers were altered incorrectly");
        check(bullet.paragraphs[0].bullet && bullet.paragraphs[1].bullet && bullet.paragraphs[2].bullet && !bullet.paragraphs[3].bullet, "unordered list scope incorrect");
        check(bullet.paragraphs[1].indent_level == 1, "nested list indentation was lost");
        const auto code = cclive::make_text_layout(L"```cpp\n# Not heading **literal**\n- literal\n```\n**outside**");
        check(code.text == L"```cpp\r# Not heading **literal**\r- literal\r```\routside", "fenced code was parsed as prose or modified");
        check(styles(code, cclive::TextStyle::Code) == 4 && styles(code, cclive::TextStyle::Heading) == 0 && styles(code, cclive::TextStyle::Bold) == 1, "fenced code formatting boundaries incorrect");
        check(code.paragraphs[0].code && code.paragraphs[3].code && !code.paragraphs[4].code && !code.paragraphs[2].bullet, "code paragraph boundary incorrect");
        const auto open_code = cclive::make_text_layout(L"~~~text\n**still literal**");
        check(open_code.text == L"~~~text\r**still literal**" && styles(open_code, cclive::TextStyle::Code) == 2, "unfinished code fence was not preserved");
        const auto combined = cclive::make_text_layout(L"## **Bold heading**\n- **Bold item**");
        check(combined.text == L"Bold heading\rBold item" && styles(combined, cclive::TextStyle::Heading) == 1 && styles(combined, cclive::TextStyle::Bold) == 2, "heading/list inline bold composition failed");
        const auto unmarked = cclive::make_text_layout(L"PROGRESS\nNext steps\nA short title");
        check(unmarked.ranges.empty(), "unmarked prose was guessed as headings");
        const auto literal = cclive::make_text_layout(L"<script>alert(1)</script>\nC:\\project\\file.cpp");
        check(literal.text == L"<script>alert(1)</script>\rC:\\project\\file.cpp", "ordinary code-like/path content was interpreted or modified");
        check(cclive::make_text_layout(L"").text.empty(), "empty layout inserted content");
        for (const auto* layout : {&plain, &headings, &bold, &unicode, &bullet, &code, &open_code, &combined}) {
            for (const auto& range : layout->ranges)
                check(range.start >= 0 && range.length > 0 && static_cast<std::size_t>(range.start + range.length) <= layout->text.size(), "style range extends outside rendered text");
            for (const auto& paragraph : layout->paragraphs)
                check(paragraph.start >= 0 && paragraph.length >= 0 && static_cast<std::size_t>(paragraph.start + paragraph.length) <= layout->text.size(), "paragraph range extends outside rendered text");
        }
        native_format_checks();
        std::cout << checks << " text layout checks, 0 failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Text layout failed: " << error.what() << '\n';
        return 1;
    }
}
