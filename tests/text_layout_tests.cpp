#include "text_layout.hpp"
#include <algorithm>
#include <richedit.h>
#include <cwchar>
#include <iostream>
#include <limits>
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
void highlight_mapping_checks() {
    auto marked = [&](const std::wstring& raw, LONG start, LONG length,
                      const std::wstring& expected, LONG displayed_start) {
        const auto plain = cclive::make_text_layout(raw);
        const auto layout = cclive::make_text_layout(raw, start, length);
        check(layout.text == plain.text, "latest highlight changed visible text");
        check(layout.paragraphs.size() == plain.paragraphs.size(), "latest highlight changed paragraph count");
        for (std::size_t n = 0; n < layout.paragraphs.size(); ++n) {
            const auto& before = plain.paragraphs[n]; const auto& after = layout.paragraphs[n];
            check(before.start == after.start && before.length == after.length &&
                  before.heading_level == after.heading_level && before.indent_level == after.indent_level &&
                  before.bullet == after.bullet && before.code == after.code && before.ordered == after.ordered &&
                  before.marker_length == after.marker_length, "latest highlight changed structural paragraph metadata");
        }
        check(styles(layout, cclive::TextStyle::Latest) == 1, "latest span did not produce exactly one style range");
        const auto found = std::find_if(layout.ranges.begin(), layout.ranges.end(), [](const auto& range) {
            return range.style == cclive::TextStyle::Latest;
        });
        check(found->start == displayed_start &&
              layout.text.substr(found->start, found->length) == expected,
              "raw UTF16 latest span mapped to the wrong displayed occurrence");
    };
    marked(L"**x** x", 6, 1, L"x", 2);
    marked(L"**x** x", 2, 1, L"x", 0);
    const std::wstring seam = L"**First. Second.** Final.";
    marked(seam, static_cast<LONG>(seam.find(L"Second.")), 7, L"Second.", 7);
    marked(L"A\r\nB", 3, 1, L"B", 2);
    marked(L"A\r\n\r\n\r\nB", 7, 1, L"B", 3);
    const std::wstring unicode_list = L"开头\r\n- **\U0001F600最新**\r\n10. `尾部`";
    marked(unicode_list, static_cast<LONG>(unicode_list.find(L"\U0001F600")),
           static_cast<LONG>(unicode_list.size()), L"\U0001F600最新\r10.\t尾部", 3);
    marked(L"前\U0001F600中文后", 1, 4, L"\U0001F600中文", 1);
    marked(L"# **标题**\r\n- `代码`", 0, 1000, L"标题\r代码", 0);
    marked(L"\\**literal**", 0, 3, L"\\**", 0);
    marked(L"```\ncode\n```", 0, 3, L"```", 0);
    marked(L"one two", 4, (std::numeric_limits<LONG>::max)(), L"two", 4);
    for (const auto& raw : {L"# title", L"- item", L"**bold**", L"`code`"}) {
        const auto layout = cclive::make_text_layout(raw, 0, raw[0] == L'`' ? 1 : 2);
        check(styles(layout, cclive::TextStyle::Latest) == 0, "removed marker-only span highlighted unrelated glyphs");
    }
    check(styles(cclive::make_text_layout(L"plain", -1, 3), cclive::TextStyle::Latest) == 0,
          "negative start enabled a latest span");
    check(styles(cclive::make_text_layout(L"plain", 1, -2), cclive::TextStyle::Latest) == 0,
          "negative length enabled a latest span");
    check(styles(cclive::make_text_layout(L"plain", 1, 0), cclive::TextStyle::Latest) == 0,
          "empty span enabled a latest span");
    check(styles(cclive::make_text_layout(L"plain", (std::numeric_limits<LONG>::max)(),
          (std::numeric_limits<LONG>::max)()), cclive::TextStyle::Latest) == 0,
          "out-of-input highlight range overflowed or selected unrelated text");
}
void native_highlight_checks() {
    const auto library = LoadLibraryW(L"Msftedit.dll");
    check(library != nullptr, "latest RichEdit library could not load");
    const HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE | ES_READONLY,
        -20000, -20000, 640, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(edit != nullptr, "latest RichEdit could not be created");
    const std::wstring raw = L"## **标题**\r\n正文 `code` 与 **加粗**。\r\n尾行";
    const auto plain = cclive::make_text_layout(raw);
    SetWindowTextW(edit, plain.text.c_str());
    auto read_text = [&] {
        std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
        GetWindowTextW(edit, text.data(), static_cast<int>(text.size()));
        text.resize(std::wcslen(text.c_str())); return text;
    };
    const auto copied_text = read_text();
    const LONG heading = static_cast<LONG>(plain.text.find(L"标题"));
    const LONG code = static_cast<LONG>(plain.text.find(L"code"));
    const LONG bold = static_cast<LONG>(plain.text.find(L"加粗"));
    auto read_style = [&](LONG position) {
        CHARRANGE range{position, position + 1}; SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        CHARFORMAT2W result{}; result.cbSize = sizeof(result);
        SendMessageW(edit, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&result)); return result;
    };
    auto apply = [&](LONG start, LONG length) {
        CHARRANGE selected{1, 4}; SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        cclive::apply_text_layout(edit, cclive::make_text_layout(raw, start, length), 12, 96);
        CHARRANGE restored{}; SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&restored));
        check(restored.cpMin == selected.cpMin && restored.cpMax == selected.cpMax,
              "latest formatting moved the caller selection");
        check(read_text() == copied_text, "latest formatting changed copied RichEdit text");
    };
    apply(static_cast<LONG>(raw.find(L"标题")), 2);
    const auto newest_heading = read_style(heading);
    check(newest_heading.crTextColor == cclive::text_layout_palette.latest &&
          newest_heading.crBackColor == cclive::text_layout_palette.latest_background &&
          newest_heading.yHeight == 280 && (newest_heading.dwEffects & CFE_BOLD),
          "latest colors removed heading size or weight");
    apply(static_cast<LONG>(raw.find(L"code")), 4);
    const auto old_heading = read_style(heading), newest_code = read_style(code);
    check(old_heading.crTextColor == cclive::text_layout_palette.heading &&
          old_heading.crBackColor == cclive::text_layout_palette.background,
          "moving latest highlight did not reset the old heading colors");
    check(newest_code.crTextColor == cclive::text_layout_palette.latest &&
          newest_code.crBackColor == cclive::text_layout_palette.latest_background &&
          std::wcscmp(newest_code.szFaceName, L"Consolas") == 0 && newest_code.yHeight == 240 &&
          !(newest_code.dwEffects & CFE_BOLD), "latest colors removed code face, size or weight");
    apply(static_cast<LONG>(raw.find(L"加粗")), 2);
    const auto old_code = read_style(code), newest_bold = read_style(bold);
    check(old_code.crTextColor == cclive::text_layout_palette.code &&
          old_code.crBackColor == cclive::text_layout_palette.code_background,
          "moving latest highlight did not reset the old code colors");
    check(newest_bold.crTextColor == cclive::text_layout_palette.latest &&
          newest_bold.crBackColor == cclive::text_layout_palette.latest_background &&
          (newest_bold.dwEffects & CFE_BOLD) && newest_bold.yHeight == 240,
          "latest colors removed inline bold styling");
    apply(-1, 0);
    const auto reset_bold = read_style(bold);
    check(reset_bold.crTextColor == cclive::text_layout_palette.body &&
          reset_bold.crBackColor == cclive::text_layout_palette.background && (reset_bold.dwEffects & CFE_BOLD),
          "disabling latest left stale colors or removed bold styling");
    check(GetForegroundWindow() != edit, "latest formatter activated its hidden RichEdit");
    DestroyWindow(edit); FreeLibrary(library);
}
void native_paragraph_checks() {
    const auto library = LoadLibraryW(L"Msftedit.dll");
    check(library != nullptr, "paragraph RichEdit library could not load");
    const HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE | ES_READONLY,
        -20000, -20000, 340, 1200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(edit != nullptr, "paragraph RichEdit could not be created");
    SendMessageW(edit, EM_SETTARGETDEVICE, 0, 0);
    RECT bounds{8, 4, 332, 1196};
    SendMessageW(edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&bounds));
    const std::wstring long_body = L"中文、标点与 English words 的长列表正文应在窄窗中自然换行，并让后续文字与第一行正文对齐。继续阅读这一条目，不能把回卷文字排到编号下方。";
    const auto layout = cclive::make_text_layout(L"# 首标题\n正文段落。\n## 后续标题\n1. " + long_body + L"\n10. " + long_body + L"\n100) " + long_body + L"\n  7. " + long_body + L"\n- " + long_body + L"\n  - " + long_body + L"\n普通收尾。");
    auto paragraph_style = [&](LONG position) {
        CHARRANGE range{position, position}; SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        PARAFORMAT2 value{}; value.cbSize = sizeof(value);
        SendMessageW(edit, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&value)); return value;
    };
    auto point_at = [&](LONG position) {
        POINTL point{}; SendMessageW(edit, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&point), position); return point;
    };
    for (const int points : {8, 10, 18, 40}) {
        SetWindowTextW(edit, layout.text.c_str());
        cclive::apply_text_layout(edit, layout, points, 96);
        const auto normal = paragraph_style(layout.paragraphs[1].start);
        check(normal.bLineSpacingRule == 5 && normal.dyLineSpacing == 25 && normal.dySpaceAfter == points * 6, "body line/paragraph rhythm did not scale with point size");
        check(normal.dxStartIndent == 0 && normal.dxOffset == 0 && normal.wNumbering == 0 && normal.cTabCount == 0, "normal paragraph inherited list indentation or tabs");
        check(paragraph_style(0).dySpaceBefore == 0 && paragraph_style(layout.paragraphs[2].start).dySpaceBefore == points * 12, "first and subsequent heading spacing incorrect");
        LONG ordered_x = -1;
        for (const auto& paragraph : layout.paragraphs) if (paragraph.ordered || paragraph.bullet) {
            const auto format = paragraph_style(paragraph.start);
            check(format.dySpaceAfter == points * 2 && format.dxOffset >= points * 36 && format.cTabCount == 1, "list spacing, hanging indent or tab column did not scale");
            check(format.wNumbering == (paragraph.bullet ? PFN_BULLET : 0), "source-owned ordered list was automatically renumbered");
            const LONG body = paragraph.start + (paragraph.ordered ? paragraph.marker_length + 1 : 0);
            CHARRANGE range{body, body}; SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
            SendMessageW(edit, EM_SCROLLCARET, 0, 0);
            const auto first = point_at(body);
            const auto line = SendMessageW(edit, EM_EXLINEFROMCHAR, 0, body);
            const LONG next = static_cast<LONG>(SendMessageW(edit, EM_LINEINDEX, line + 1, 0));
            check(next > body && next < paragraph.start + paragraph.length, "long list fixture did not wrap inside its paragraph");
            const auto wrapped = point_at(next);
            if (std::abs(first.x - wrapped.x) > 2)
                std::cerr << "wrap x: font=" << points << " ordered=" << paragraph.ordered << " nested=" << paragraph.indent_level << " first=" << first.x << " wrap=" << wrapped.x << '\n';
            check(std::abs(first.x - wrapped.x) <= 2 && wrapped.y > first.y, "wrapped list text did not align with first-line body column");
            if (paragraph.ordered && paragraph.indent_level == 0) {
                if (ordered_x < 0) ordered_x = first.x;
                check(std::abs(first.x - ordered_x) <= 2, "one, two and three digit source numbers have different body columns");
            }
        }
        const auto plain = cclive::make_text_layout(L"普通中文正文替换列表。");
        SetWindowTextW(edit, plain.text.c_str()); cclive::apply_text_layout(edit, plain, points, 96);
        const auto reset = paragraph_style(0);
        check(reset.dxStartIndent == 0 && reset.dxOffset == 0 && reset.cTabCount == 0 && reset.wNumbering == 0, "list to plain replacement left native numbering, tabs or hanging indent");
    }
    const auto long_number = cclive::make_text_layout(L"1. " + long_body + L"\n987654321. " + long_body);
    SetWindowTextW(edit, long_number.text.c_str()); cclive::apply_text_layout(edit, long_number, 10, 96);
    const auto first_body = point_at(long_number.paragraphs[0].start + 3);
    const auto large_body = point_at(long_number.paragraphs[1].start + 11);
    check(std::abs(first_body.x - large_body.x) <= 2, "long original source number exceeded the measured list tab column");
    check(GetForegroundWindow() != edit, "paragraph formatting activated hidden RichEdit");
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
        check(bullet.text == L"first\rnested\rthird\r1.\tOwn number\r2)\tOwn number", "list markers or content-owned numbers were altered incorrectly");
        check(bullet.paragraphs[0].bullet && bullet.paragraphs[1].bullet && bullet.paragraphs[2].bullet && !bullet.paragraphs[3].bullet, "unordered list scope incorrect");
        check(bullet.paragraphs[1].indent_level == 1, "nested list indentation was lost");
        check(bullet.paragraphs[3].ordered && bullet.paragraphs[3].marker_length == 2 && bullet.paragraphs[4].ordered, "source-owned ordered marker metadata missing");
        const auto numbered = cclive::make_text_layout(L"7. 中文\n10)   下一项\n  23. 嵌套\n1.正文\n3.14 decimal\n1.test");
        check(numbered.text == L"7.\t中文\r10)\t下一项\r23.\t嵌套\r1.\t正文\r3.14 decimal\r1.test", "original list numbers, CJK content or literal decimal/path changed");
        check(numbered.paragraphs[2].ordered && numbered.paragraphs[2].indent_level == 1 && !numbered.paragraphs[4].ordered && !numbered.paragraphs[5].ordered, "ordered list scope or nesting incorrect");
        const auto gaps = cclive::make_text_layout(L"段一\n\n \t\n\n段二\n\n\n");
        check(gaps.text == L"段一\r\r段二\r" && gaps.paragraphs.size() == 4, "repeated blank paragraphs were not collapsed to one");
        const auto literal_code = cclive::make_text_layout(L"```\nline\n\n\n  \nend\n```");
        check(literal_code.text == L"```\rline\r\r\r  \rend\r```", "blank-line cleanup altered fenced code");
        const auto inline_code = cclive::make_text_layout(L"路径 `a **b**` 与 **加粗 `x*y`**；\\**literal** \\`tick\\`。");
        check(inline_code.text == L"路径 a **b** 与 加粗 x*y；\\**literal** \\`tick\\`。", "inline code or escaped Markdown lost literal content");
        check(styles(inline_code, cclive::TextStyle::Code) == 2 && styles(inline_code, cclive::TextStyle::Bold) == 1, "inline code failed to protect literal emphasis delimiters");
        check(cclive::make_text_layout(L"before `unfinished **literal**").text == L"before `unfinished **literal**", "unfinished inline code was consumed");
        check(cclive::make_text_layout(L"``x`y``").text == L"x`y", "multi-backtick inline delimiter lost a literal backtick");
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
        highlight_mapping_checks();
        native_highlight_checks();
        native_format_checks();
        native_paragraph_checks();
        std::cout << checks << " text layout checks, 0 failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Text layout failed: " << error.what() << '\n';
        return 1;
    }
}
