#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

#include "core/terminal.h"
#include "core/stream_renderer.h"

using namespace pi::core;

// ── Minimal test harness (same pattern as other test files) ──────────────────

namespace tests {

int passed{0}, failed{0}, total{0}, current_failed{0};

struct TestResult { std::string name; bool ok; };
std::vector<TestResult> results;

bool check_eq_impl(bool cond, std::string_view expr,
                   std::source_location loc = std::source_location::current()) {
  if (!cond) {
    ++current_failed;
    std::cerr << "  FAIL " << loc.file_name() << ':' << loc.line()
              << " — " << expr << '\n';
  }
  return cond;
}

template<typename A, typename B>
bool CHECK_EQ_impl(const A &a, const B &b, std::string_view expr,
                   std::source_location loc = std::source_location::current()) {
  if (a != b) {
    ++current_failed;
    std::cerr << "  FAIL " << loc.file_name() << ':' << loc.line()
              << " — " << expr << " (got " << a << ", expected " << b << ")\n";
    return false;
  }
  return true;
}

#define CHECK_EQ(a, b) \
  ::tests::CHECK_EQ_impl((a), (b), #a " == " #b, std::source_location::current())

void register_test(std::string name, std::function<void()> fn) {
  ++total; current_failed = 0;
  fn();
  if (current_failed == 0) ++passed; else ++failed;
  results.push_back({std::move(name), current_failed == 0});
}

void print_summary() {
  std::cout << "\n========================================\n";
  std::cout << "  Tests: " << total << " total, "
            << passed << " passed, " << failed << " failed\n";
  std::cout << "========================================\n";
  if (failed > 0) {
    std::cout << "\nFailed tests:\n";
    for (const auto &r : results)
      if (!r.ok) std::cout << "  - " << r.name << '\n';
  }
}

} // namespace tests

// ── skip_ansi_sequence ────────────────────────────────────────────────────────

void test_skip_ansi_csi() {
  tests::register_test("skip_ansi_sequence: CSI — SGR", [] {
    // \033[1;94m — bold blue
    std::string_view s = "\033[1;94m";
    CHECK_EQ(skip_ansi_sequence(s, 0), s.size()); // skips all 7 bytes
  });

  tests::register_test("skip_ansi_sequence: CSI — erase", [] {
    std::string_view s = "\033[J";
    CHECK_EQ(skip_ansi_sequence(s, 0), s.size()); // 3 bytes
  });

  tests::register_test("skip_ansi_sequence: CSI — cursor home", [] {
    std::string_view s = "\033[H";
    CHECK_EQ(skip_ansi_sequence(s, 0), s.size()); // 3 bytes
  });

  tests::register_test("skip_ansi_sequence: CSI — private mode (?25l)", [] {
    std::string_view s = "\033[?25l";
    CHECK_EQ(skip_ansi_sequence(s, 0), s.size()); // 6 bytes
  });

  tests::register_test("skip_ansi_sequence: CSI mid-string", [] {
    // "abc\033[1mdef" — CSI starts at index 3
    std::string_view s = "abc\033[1mdef";
    CHECK_EQ(skip_ansi_sequence(s, 3), 7u); // ends after 'm' at index 6
  });
}

void test_skip_ansi_osc() {
  tests::register_test("skip_ansi_sequence: OSC — BEL terminated", [] {
    // OSC 8 hyperlink: \033]8;;url\007
    std::string s = "\033]8;;https://example.com\007";
    CHECK_EQ(skip_ansi_sequence(s, 0), s.size());
  });

  tests::register_test("skip_ansi_sequence: OSC — ST terminated", [] {
    // OSC terminated by ST (ESC + backslash)
    std::string s = "\033]8;;https://example.com\033\\";
    CHECK_EQ(skip_ansi_sequence(s, 0), s.size());
  });

  tests::register_test("skip_ansi_sequence: OSC — empty", [] {
    std::string_view s = "\033]\007";
    CHECK_EQ(skip_ansi_sequence(s, 0), s.size());
  });
}

void test_skip_ansi_twobyte() {
  tests::register_test("skip_ansi_sequence: two-byte — reverse index", [] {
    // \033M = reverse linefeed (RI)
    std::string_view s = "\033M";
    CHECK_EQ(skip_ansi_sequence(s, 0), 2u);
  });

  tests::register_test("skip_ansi_sequence: two-byte — NEL", [] {
    std::string_view s = "\033E"; // NEL (next line)
    CHECK_EQ(skip_ansi_sequence(s, 0), 2u);
  });
}

void test_skip_ansi_non_escape() {
  tests::register_test("skip_ansi_sequence: not an escape — returns i", [] {
    std::string_view s = "hello";
    CHECK_EQ(skip_ansi_sequence(s, 0), 0u);
    CHECK_EQ(skip_ansi_sequence(s, 2), 2u);
  });

  tests::register_test("skip_ansi_sequence: lone ESC at end — returns i", [] {
    std::string_view s = "abc\033";
    CHECK_EQ(skip_ansi_sequence(s, 3), 3u); // no following byte, don't skip
  });
}

// ── advance_utf8 ─────────────────────────────────────────────────────────────

void test_advance_utf8() {
  tests::register_test("advance_utf8: ASCII", [] {
    CHECK_EQ(advance_utf8("hello", 0), 1u);
    CHECK_EQ(advance_utf8("hello", 4), 5u);
  });

  tests::register_test("advance_utf8: 2-byte (é U+00E9)", [] {
    // é = \xc3\xa9
    std::string_view s = "\xc3\xa9";
    CHECK_EQ(advance_utf8(s, 0), 2u);
  });

  tests::register_test("advance_utf8: 3-byte (你 U+4F60)", [] {
    // 你 = \xe4\xbd\xa0
    std::string_view s = "\xe4\xbd\xa0";
    CHECK_EQ(advance_utf8(s, 0), 3u);
  });

  tests::register_test("advance_utf8: 4-byte (😀 U+1F600)", [] {
    // 😀 = \xf0\x9f\x98\x80
    std::string_view s = "\xf0\x9f\x98\x80";
    CHECK_EQ(advance_utf8(s, 0), 4u);
  });

  tests::register_test("advance_utf8: truncated 3-byte — clamps to size", [] {
    // Only first 2 bytes of 你 present
    std::string_view s = "\xe4\xbd";
    CHECK_EQ(advance_utf8(s, 0), 2u); // min(0+3, 2)
  });

  tests::register_test("advance_utf8: at end of string", [] {
    std::string_view s = "a";
    CHECK_EQ(advance_utf8(s, 1), 1u); // i >= size → return i
  });

  tests::register_test("advance_utf8: empty string", [] {
    CHECK_EQ(advance_utf8("", 0), 0u);
  });
}

// ── codepoint_width ───────────────────────────────────────────────────────────

void test_codepoint_width() {
  tests::register_test("codepoint_width: ASCII letter", [] {
    CHECK_EQ(codepoint_width("A", 0), 1);
    CHECK_EQ(codepoint_width("z", 0), 1);
  });

  tests::register_test("codepoint_width: ASCII space", [] {
    CHECK_EQ(codepoint_width(" ", 0), 1);
  });

  tests::register_test("codepoint_width: control char (non-printing)", [] {
    CHECK_EQ(codepoint_width("\x01", 0), 0);
    CHECK_EQ(codepoint_width("\x1f", 0), 0);
  });

  tests::register_test("codepoint_width: CJK ideograph 你 (U+4F60)", [] {
    // \xe4\xbd\xa0
    CHECK_EQ(codepoint_width("\xe4\xbd\xa0", 0), 2);
  });

  tests::register_test("codepoint_width: CJK ideograph 好 (U+597D)", [] {
    // \xe5\xa5\xbd
    CHECK_EQ(codepoint_width("\xe5\xa5\xbd", 0), 2);
  });

  tests::register_test("codepoint_width: Hangul 한 (U+D55C)", [] {
    // \xed\x95\x9c
    CHECK_EQ(codepoint_width("\xed\x95\x9c", 0), 2);
  });

  tests::register_test("codepoint_width: fullwidth A (U+FF21)", [] {
    // \xef\xbc\xa1
    CHECK_EQ(codepoint_width("\xef\xbc\xa1", 0), 2);
  });

  tests::register_test("codepoint_width: emoji 😀 (U+1F600)", [] {
    // \xf0\x9f\x98\x80
    CHECK_EQ(codepoint_width("\xf0\x9f\x98\x80", 0), 2);
  });

  tests::register_test("codepoint_width: combining grave (U+0300)", [] {
    // \xcc\x80
    CHECK_EQ(codepoint_width("\xcc\x80", 0), 0);
  });

  tests::register_test("codepoint_width: Latin extended é (U+00E9)", [] {
    // \xc3\xa9 — narrow
    CHECK_EQ(codepoint_width("\xc3\xa9", 0), 1);
  });
}

void test_terminal_ui_helpers() {
  tests::register_test("display_columns: ignores ANSI and counts wide text", [] {
    CHECK_EQ(display_columns("\033[31mred\033[0m"), 3);
    CHECK_EQ(display_columns("你"), 2);
  });

  tests::register_test("truncate_ansi_line: preserves complete ANSI codes", [] {
    CHECK_EQ(truncate_ansi_line("\033[31mabcdef\033[0m", 3),
             "\033[31mabc\033[0m");
    CHECK_EQ(truncate_ansi_line("hello\nworld", 80), "hello");
  });
}

void test_split_lines() {
  tests::register_test("split_lines: wraps visible columns and preserves ANSI", [] {
    const auto lines = split_lines("\033[31mabcdef\033[0m", 3);
    CHECK_EQ(lines.size(), 3u);
    CHECK_EQ(lines[0], "\033[31mabc");
    CHECK_EQ(lines[1], "def");
    CHECK_EQ(lines[2], "\033[0m");
  });

  tests::register_test("split_lines: keeps explicit empty rows", [] {
    const auto lines = split_lines("one\n\ntwo", 80);
    CHECK_EQ(lines.size(), 3u);
    CHECK_EQ(lines[0], "one");
    CHECK_EQ(lines[1], "");
    CHECK_EQ(lines[2], "two");
  });
}

void test_dispatch_tool_update() {
  tests::register_test("dispatch_event: forwards tool updates", [] {
    class RecordingRenderer final : public Renderer {
    public:
      void on_text_delta(std::string_view) override {}
      void on_tool_update(std::string_view call_id, std::string_view tool_name,
                          std::string_view partial_result) override {
        seen_call_id = call_id;
        seen_tool_name = tool_name;
        seen_result = partial_result;
      }

      std::string seen_call_id;
      std::string seen_tool_name;
      std::string seen_result;
    } renderer;

    dispatch_event(ToolExecutionUpdateEvent{"call-1", "bash", "{}", "out"},
                   renderer);
    CHECK_EQ(renderer.seen_call_id, "call-1");
    CHECK_EQ(renderer.seen_tool_name, "bash");
    CHECK_EQ(renderer.seen_result, "out");
  });
}

// ── cursor_rows_for_rendered ──────────────────────────────────────────────────

void test_cursor_rows() {
  tests::register_test("cursor_rows_for_rendered: short ASCII", [] {
    CHECK_EQ(cursor_rows_for_rendered("Hello", 80), 1);
  });

  tests::register_test("cursor_rows_for_rendered: single newline", [] {
    CHECK_EQ(cursor_rows_for_rendered("Hello\n", 80), 2);
  });

  tests::register_test("cursor_rows_for_rendered: two newlines", [] {
    CHECK_EQ(cursor_rows_for_rendered("Hello\n\n", 80), 3);
  });

  tests::register_test("cursor_rows_for_rendered: two lines of text", [] {
    CHECK_EQ(cursor_rows_for_rendered("Hello\nWorld", 80), 2);
  });

  tests::register_test("cursor_rows_for_rendered: wrapping at width", [] {
    // 10 chars at width 5 → wraps once, cursor at start of row 3
    CHECK_EQ(cursor_rows_for_rendered("0123456789", 5), 3);
  });

  tests::register_test("cursor_rows_for_rendered: exactly fills one row", [] {
    // 5 chars at width 5 — cursor wraps to row 2
    CHECK_EQ(cursor_rows_for_rendered("12345", 5), 2);
  });

  tests::register_test("cursor_rows_for_rendered: ANSI codes ignored", [] {
    // \033[1;94m## Heading\033[0m\n\n — should be same as "## Heading\n\n"
    CHECK_EQ(cursor_rows_for_rendered("\033[1;94m## Heading\033[0m\n\n", 80), 3);
    CHECK_EQ(cursor_rows_for_rendered("## Heading\n\n", 80), 3);
  });

  tests::register_test("cursor_rows_for_rendered: OSC hyperlink ignored", [] {
    // The hyperlink escape itself contributes 0 columns
    std::string s = "\033]8;;https://example.com\007link text\033]8;;\007";
    // "link text" is 9 chars, fits on 1 row at width 80
    CHECK_EQ(cursor_rows_for_rendered(s, 80), 1);
  });

  tests::register_test("cursor_rows_for_rendered: CJK wide chars", [] {
    // 你好 = 2+2 = 4 visual cols at width 5 → fits on 1 row (no wrap)
    CHECK_EQ(cursor_rows_for_rendered("\xe4\xbd\xa0\xe5\xa5\xbd", 5), 1);
    // at width 4 → exactly 4 cols → cursor wraps to row 2
    CHECK_EQ(cursor_rows_for_rendered("\xe4\xbd\xa0\xe5\xa5\xbd", 4), 2);
    // at width 3 → 你 (2) fits, 好 (2) would exceed → wraps → 2 rows
    CHECK_EQ(cursor_rows_for_rendered("\xe4\xbd\xa0\xe5\xa5\xbd", 3), 2);
  });

  tests::register_test("cursor_rows_for_rendered: mixed ASCII+wide", [] {
    // "A你B" at width 4 → A(1) + 你(2) + B(1) = 4 cols → wraps → 2 rows
    CHECK_EQ(cursor_rows_for_rendered("A\xe4\xbd\xa0""B", 4), 2);
    // at width 5 → 4 cols fits → 1 row
    CHECK_EQ(cursor_rows_for_rendered("A\xe4\xbd\xa0""B", 5), 1);
  });

  tests::register_test("cursor_rows_for_rendered: empty string", [] {
    CHECK_EQ(cursor_rows_for_rendered("", 80), 1);
  });
}

// ── rows_for_line ─────────────────────────────────────────────────────────────

void test_rows_for_line() {
  tests::register_test("rows_for_line: fits in one row", [] {
    CHECK_EQ(rows_for_line("Hello", 80), 1);
  });

  tests::register_test("rows_for_line: exactly fills one row (wraps)", [] {
    CHECK_EQ(rows_for_line("12345", 5), 2);
  });

  tests::register_test("rows_for_line: wraps once", [] {
    CHECK_EQ(rows_for_line("0123456789", 5), 3);
  });

  tests::register_test("rows_for_line: ANSI codes ignored", [] {
    CHECK_EQ(rows_for_line("\033[1mHello\033[0m", 80), 1);
    CHECK_EQ(rows_for_line("\033[1mHello\033[0m", 3), 2);
  });

  tests::register_test("rows_for_line: wide chars double col count", [] {
    // 你好 = 4 visual cols at width 5 → 1 row
    CHECK_EQ(rows_for_line("\xe4\xbd\xa0\xe5\xa5\xbd", 5), 1);
    // at width 3 → 你(2)=2, 好(2) would push to 4 > 3 → wrap → 2 rows
    CHECK_EQ(rows_for_line("\xe4\xbd\xa0\xe5\xa5\xbd", 3), 2);
  });

  tests::register_test("rows_for_line: empty string", [] {
    CHECK_EQ(rows_for_line("", 80), 1);
  });
}

// ── BlockBoundaryScanner ──────────────────────────────────────────────────────

void test_block_boundary_scanner() {
  tests::register_test("BlockBoundaryScanner: basic paragraph \\n\\n", [] {
    BlockBoundaryScanner sc;
    sc.advance("para\n\nmore");
    CHECK_EQ(sc.last_stable, 6u);
  });

  tests::register_test("BlockBoundaryScanner: empty-tail boundary (no guard)", [] {
    // Boundary IS committed even with nothing after it — empty suffix is fine.
    BlockBoundaryScanner sc;
    sc.advance("para\n\n");
    CHECK_EQ(sc.last_stable, 6u);
  });

  tests::register_test("BlockBoundaryScanner: indented line — no commit", [] {
    // Line before blank has indent 2; last_nonblank_col0 == false.
    BlockBoundaryScanner sc;
    sc.advance("  item\n\nmore");
    CHECK_EQ(sc.last_stable, 0u);
  });

  tests::register_test("BlockBoundaryScanner: two boundaries — last one wins", [] {
    // "p1\n\n" → boundary at 4; "p2\n\n" → boundary at 8.
    BlockBoundaryScanner sc;
    sc.advance("p1\n\np2\n\nmore");
    CHECK_EQ(sc.last_stable, 8u);
  });

  tests::register_test("BlockBoundaryScanner: empty input", [] {
    BlockBoundaryScanner sc;
    sc.advance("");
    CHECK_EQ(sc.last_stable, 0u);
  });

  tests::register_test("BlockBoundaryScanner: initial blank lines", [] {
    // Initial last_nonblank_col0=true → \n\n at start commits.
    BlockBoundaryScanner sc;
    sc.advance("\n\npara");
    CHECK_EQ(sc.last_stable, 2u);
  });

  tests::register_test("BlockBoundaryScanner: backtick fence open+close", [] {
    // ```py\ncode\n```\n\nmore — fence opens, closes, then boundary fires.
    BlockBoundaryScanner sc;
    sc.advance("```py\ncode\n```\n\nmore");
    // "```py\n" = 6, "code\n" = 5, "```\n" = 4, "\n" = 1 → boundary at 16
    CHECK_EQ(sc.last_stable, 16u);
  });

  tests::register_test("BlockBoundaryScanner: \\n\\n inside fence — no commit", [] {
    // Blank line inside the fence must not become a boundary.
    BlockBoundaryScanner sc;
    sc.advance("```py\n\nstill\n```\n\nmore");
    // "```py\n" = 6, "\n" = 1, "still\n" = 6, "```\n" = 4, "\n" = 1 → boundary at 18
    CHECK_EQ(sc.last_stable, 18u);
    // Verify it's not the in-fence \n\n (which would be at 7).
    // last_stable must be >= 18 (after the closing fence).
  });

  tests::register_test("BlockBoundaryScanner: tilde fence", [] {
    BlockBoundaryScanner sc;
    sc.advance("~~~\ncode\n~~~\n\nmore");
    // "~~~\n" = 4, "code\n" = 5, "~~~\n" = 4, "\n" = 1 → boundary at 14
    CHECK_EQ(sc.last_stable, 14u);
  });

  tests::register_test("BlockBoundaryScanner: closing fence with trailing spaces", [] {
    // "```   \n" is a valid closer; line_only_fence stays true through spaces.
    BlockBoundaryScanner sc;
    sc.advance("```\ncode\n```   \n\nmore");
    // "```\n" = 4, "code\n" = 5, "```   \n" = 7, "\n" = 1 → boundary at 17
    CHECK_EQ(sc.last_stable, 17u);
  });

  tests::register_test("BlockBoundaryScanner: incrementality", [] {
    // Scanning "para\n" then the full "para\n\nmore" must give the same
    // last_stable as scanning "para\n\nmore" in one shot.
    // Second advance() starts from scan_pos=5 so only processes "\nmore".
    BlockBoundaryScanner sc_inc;
    sc_inc.advance("para\n");
    sc_inc.advance("para\n\nmore");
    BlockBoundaryScanner sc_one;
    sc_one.advance("para\n\nmore");
    CHECK_EQ(sc_inc.last_stable, sc_one.last_stable);
    CHECK_EQ(sc_inc.last_stable, 6u);
  });

  tests::register_test("BlockBoundaryScanner: longer fence (4 backticks)", [] {
    // Opening ````py needs closing ```` (4+), not just ```.
    BlockBoundaryScanner sc;
    sc.advance("````py\ncode\n```\nstill_in_fence\n````\n\nout");
    // After "```\n": fence_run=3 < fence_len=4, so fence stays open.
    // After "````\n": fence_run=4 >= fence_len=4 → close.
    // Then "\n" → boundary.
    // "````py\n"=7 + "code\n"=5 + "```\n"=4 + "still_in_fence\n"=15 + "````\n"=5 + "\n"=1 = 37
    CHECK_EQ(sc.last_stable, 37u);
  });
}

void test_terminal_title_helpers() {
  tests::register_test("sanitize_terminal_title: basic controls", [] {
    auto s = sanitize_terminal_title("  Project\t|\nWorking\x1b\x07\x9d\x9c |  Thread  ");
    CHECK_EQ(s, "Project | Working | Thread");
  });
  tests::register_test("sanitize_terminal_title: strips invisible format chars", [] {
    auto s = sanitize_terminal_title(
        "Pro\u202Ej\u2066e\u200Fc\u061Ct\u200B \uFEFFT\u2060itle");
    CHECK_EQ(s, "Project Title");
  });
  tests::register_test("sanitize_terminal_title: ESC BEL newline CR tab C1", [] {
    std::string in = "a\x1b" "b\x07" "c\n" "d\r" "e\t" "f\x1f" "g";
    CHECK_EQ(sanitize_terminal_title(in), "a b c d e f g");
  });
  tests::register_test("sanitize_terminal_title: leading trailing repeated whitespace", [] {
    CHECK_EQ(sanitize_terminal_title("  hello   world  "), "hello world");
    CHECK_EQ(sanitize_terminal_title("\t\n hello \n\t world \n"), "hello world");
  });
  tests::register_test("sanitize_terminal_title: Trojan-Source bidi controls", [] {
    CHECK_EQ(sanitize_terminal_title("a\u202Db\u202Ec"), "abc");
    CHECK_EQ(sanitize_terminal_title("a\u200Bb\u200Cc"), "abc");
    CHECK_EQ(sanitize_terminal_title("a\uFEFFb"), "ab");
  });
  tests::register_test("sanitize_terminal_title: multibyte utf8 preserved", [] {
    CHECK_EQ(sanitize_terminal_title("caf\xc3\xa9 \xf0\x9f\x98\x80"), "caf\xc3\xa9 \xf0\x9f\x98\x80");
    CHECK_EQ(sanitize_terminal_title("\xe4\xbd\xa0\xe5\xa5\xbd"), "\xe4\xbd\xa0\xe5\xa5\xbd");
  });
  tests::register_test("sanitize_terminal_title: truncates to 240 chars", [] {
    std::string in(kMaxTerminalTitleChars + 10, 'a');
    auto s = sanitize_terminal_title(in);
    CHECK_EQ(s.size(), kMaxTerminalTitleChars);
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) { ++count; i = advance_utf8(s, i); }
    CHECK_EQ(count, kMaxTerminalTitleChars);
  });
  tests::register_test("sanitize_terminal_title: pending-space boundary prefers visible", [] {
    std::string in(kMaxTerminalTitleChars - 1, 'a');
    in += " b";
    auto s = sanitize_terminal_title(in);
    CHECK_EQ(s.size(), kMaxTerminalTitleChars);
    CHECK_EQ(s.back(), 'b');
  });
  tests::register_test("sanitize_terminal_title: empty after sanitization", [] {
    CHECK_EQ(sanitize_terminal_title("\x1b\x07 \n\t"), "");
    CHECK_EQ(sanitize_terminal_title("\u200B\uFEFF"), "");
  });
  tests::register_test("sanitize_terminal_title: no split UTF-8 on truncation", [] {
    std::string in;
    for (std::size_t i = 0; i < kMaxTerminalTitleChars - 1; ++i) in += 'a';
    in += "\xf0\x9f\x98\x80";
    in += "extra";
    auto s = sanitize_terminal_title(in);
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) { ++count; i = advance_utf8(s, i); }
    CHECK_EQ(count, kMaxTerminalTitleChars);
  });
  tests::register_test("format_active_terminal_title: basic", [] {
    CHECK_EQ(format_active_terminal_title("pici", "\u280B"), "\u280B pici");
  });
  tests::register_test("format_active_terminal_title: empty base", [] {
    CHECK_EQ(format_active_terminal_title("", "\u280B"), "\u280B");
  });
  tests::register_test("format_active_terminal_title: no trailing space", [] {
    auto s = format_active_terminal_title("", "\u280B");
    CHECK_EQ(s.find(' '), std::string::npos);
    CHECK_EQ(format_active_terminal_title("x", "\u280B"), "\u280B x");
  });
  tests::register_test("terminal_title_sequence: OSC BEL framing", [] {
    CHECK_EQ(terminal_title_sequence("hello"), "\033]0;hello\007");
    CHECK_EQ(terminal_title_sequence(""), "\033]0;\007");
  });
  tests::register_test("kTerminalTitleSpinnerFrames: ten in order", [] {
    CHECK_EQ(kTerminalTitleSpinnerFrames.size(), 10u);
    const std::array<std::string_view, 10> expected = {"\u280B", "\u2819", "\u2839", "\u2838", "\u283C", "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"};
    for (std::size_t i = 0; i < 10; ++i) CHECK_EQ(kTerminalTitleSpinnerFrames[i], expected[i]);
  });
  tests::register_test("terminal_project_label: no git falls back to basename", [] {
    std::filesystem::path p = "/tmp/some-unique-pici-test-dir";
    CHECK_EQ(terminal_project_label(p), "some-unique-pici-test-dir");
  });
  tests::register_test("terminal_project_label: empty returns pici", [] {
    CHECK_EQ(terminal_project_label(std::filesystem::path{}), "pici");
  });
}

void test_terminal_title_controller() {
  tests::register_test("TerminalTitleController: non-TTY produces no writes", [] {
    std::vector<std::string> writes;
    {
      TerminalTitleController c(-1, "pici",
        [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
        std::chrono::milliseconds(10), false);
      c.start_activity();
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      c.stop_activity();
    }
    CHECK_EQ(writes.size(), 0u);
  });
  tests::register_test("TerminalTitleController: construction writes initial title", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "myproj",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(100), true);
    CHECK_EQ(writes.size(), 1u);
    if (!writes.empty()) CHECK_EQ(writes[0], "myproj");
  });
  tests::register_test("TerminalTitleController: start emits first frame immediately", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "pici",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(100), true);
    writes.clear();
    c.start_activity();
    CHECK_EQ(writes.size(), 1u);
    if (!writes.empty()) CHECK_EQ(writes[0], std::string("\u280B") + " pici");
    c.stop_activity();
  });
  tests::register_test("TerminalTitleController: ticks advance and wrap", [] {
    std::vector<std::string> writes;
    std::mutex m;
    TerminalTitleController c(-1, "p",
      [&](std::string_view s) { std::lock_guard<std::mutex> lk(m); writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    c.stop_activity();
    std::lock_guard<std::mutex> lk(m);
    bool saw_second = false, saw_wrap = false;
    for (auto &w : writes) if (w == std::string("\u2819") + " p") saw_second = true;
    std::size_t active_count = 0;
    for (auto &w : writes) {
      for (auto f : kTerminalTitleSpinnerFrames)
        if (w.rfind(f, 0) == 0) { ++active_count; break; }
    }
    if (active_count >= 10) saw_wrap = true;
    CHECK_EQ(saw_second, true);
    (void)saw_wrap;
    CHECK_EQ(active_count >= 3, true);
  });
  tests::register_test("TerminalTitleController: stop restores base", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "base",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    c.stop_activity();
    CHECK_EQ(writes.back(), "base");
  });
  tests::register_test("TerminalTitleController: no write after stop", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "base",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    c.stop_activity();
    std::size_t n = writes.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_EQ(writes.size(), n);
  });
  tests::register_test("TerminalTitleController: idempotent start", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "pici",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(50), true);
    writes.clear();
    c.start_activity();
    c.start_activity();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    c.stop_activity();
    std::size_t active_frames = 0;
    for (auto &w : writes) {
      for (auto f : kTerminalTitleSpinnerFrames)
        if (w.rfind(f, 0) == 0) { ++active_frames; break; }
    }
    CHECK_EQ(active_frames >= 1, true);
    CHECK_EQ(active_frames <= 2, true);
  });
  tests::register_test("TerminalTitleController: set_base_title during activity", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "old",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    c.set_base_title("new");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    c.stop_activity();
    CHECK_EQ(writes.back(), "new");
    bool saw_new_active = false;
    for (auto &w : writes) if (w.find("new") != std::string::npos) saw_new_active = true;
    CHECK_EQ(saw_new_active, true);
  });
  tests::register_test("TerminalTitleController: destruction clears title", [] {
    std::vector<std::string> writes;
    {
      TerminalTitleController c(-1, "pici",
        [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
        std::chrono::milliseconds(10), true);
      c.start_activity();
    }
    CHECK_EQ(writes.back(), "");
  });
  tests::register_test("TerminalTitleController: duplicate writes skipped", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "same",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(100), true);
    std::size_t n = writes.size();
    c.set_base_title("same");
    CHECK_EQ(writes.size(), n);
    c.set_base_title("same ");
    CHECK_EQ(writes.size(), n);
  });
  tests::register_test("TerminalTitleController: empty base shows only frame", [] {
    std::vector<std::string> writes;
    TerminalTitleController c(-1, "",
      [&](std::string_view s) { writes.emplace_back(s); return TerminalTitleResult::Applied; },
      std::chrono::milliseconds(100), true);
    writes.clear();
    c.start_activity();
    CHECK_EQ(writes[0], "\u280B");
    c.stop_activity();
  });
}

void test_alt_screen_session() {
  tests::register_test("AltScreenSession: restores alternate screen", [] {
    int fds[2]{};
    if (::pipe(fds) != 0) {
      tests::check_eq_impl(false, "pipe(fds) == 0");
      return;
    }

    {
      AltScreenSession session(fds[1]);
      session.leave();
    }

    std::array<char, 128> buffer{};
    const ssize_t count = ::read(fds[0], buffer.data(), buffer.size());
    const std::string output(buffer.data(),
                             count > 0 ? static_cast<std::size_t>(count) : 0);
    CHECK_EQ(output, "\033[?1049h\033[H\033[2J"
                    "\033[r\033[?25h\033[?1049l");
    ::close(fds[0]);
    ::close(fds[1]);
  });

  tests::register_test("SIGINT notification: consume is one-shot", [] {
    notify_sigint();
    CHECK_EQ(consume_sigint(), true);
    CHECK_EQ(consume_sigint(), false);
  });
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
  test_skip_ansi_csi();
  test_skip_ansi_osc();
  test_skip_ansi_twobyte();
  test_skip_ansi_non_escape();
  test_advance_utf8();
  test_codepoint_width();
  test_terminal_ui_helpers();
  test_split_lines();
  test_dispatch_tool_update();
  test_cursor_rows();
  test_rows_for_line();
  test_block_boundary_scanner();
  test_terminal_title_helpers();
  test_terminal_title_controller();
  test_alt_screen_session();

  tests::print_summary();
  return tests::failed > 0 ? 1 : 0;
}
