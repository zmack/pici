#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
#include <unistd.h>
#include <utility>
#include <vector>

#include "core/stream_renderer.h"
#include "core/terminal.h"

using namespace pi::core;

// ── skip_ansi_sequence
// ────────────────────────────────────────────────────────

TEST(Terminal, SkipAnsiCsi) {
  {
    // \033[1;94m — bold blue
    std::string_view s = "\033[1;94m";
    EXPECT_EQ(skip_ansi_sequence(s, 0), s.size()); // skips all 7 bytes
  }

  {
    std::string_view s = "\033[J";
    EXPECT_EQ(skip_ansi_sequence(s, 0), s.size()); // 3 bytes
  }

  {
    std::string_view s = "\033[H";
    EXPECT_EQ(skip_ansi_sequence(s, 0), s.size()); // 3 bytes
  }

  {
    std::string_view s = "\033[?25l";
    EXPECT_EQ(skip_ansi_sequence(s, 0), s.size()); // 6 bytes
  }

  {
    // "abc\033[1mdef" — CSI starts at index 3
    std::string_view s = "abc\033[1mdef";
    EXPECT_EQ(skip_ansi_sequence(s, 3), 7u); // ends after 'm' at index 6
  }
}

TEST(Terminal, SkipAnsiOsc) {
  {
    // OSC 8 hyperlink: \033]8;;url\007
    std::string s = "\033]8;;https://example.com\007";
    EXPECT_EQ(skip_ansi_sequence(s, 0), s.size());
  }

  {
    // OSC terminated by ST (ESC + backslash)
    std::string s = "\033]8;;https://example.com\033\\";
    EXPECT_EQ(skip_ansi_sequence(s, 0), s.size());
  }

  {
    std::string_view s = "\033]\007";
    EXPECT_EQ(skip_ansi_sequence(s, 0), s.size());
  }
}

TEST(Terminal, SkipAnsiTwoByte) {
  {
    // \033M = reverse linefeed (RI)
    std::string_view s = "\033M";
    EXPECT_EQ(skip_ansi_sequence(s, 0), 2u);
  }

  {
    std::string_view s = "\033E"; // NEL (next line)
    EXPECT_EQ(skip_ansi_sequence(s, 0), 2u);
  }
}

TEST(Terminal, SkipAnsiNonEscape) {
  {
    std::string_view s = "hello";
    EXPECT_EQ(skip_ansi_sequence(s, 0), 0u);
    EXPECT_EQ(skip_ansi_sequence(s, 2), 2u);
  }

  {
    std::string_view s = "abc\033";
    EXPECT_EQ(skip_ansi_sequence(s, 3), 3u); // no following byte, don't skip
  }
}

// ── match_osc_color_reply (M6: OSC 11 background-tint probe) ─────────────────

TEST(Terminal, MatchOscColorReply) {
  {
    std::string_view s = "\033]11;rgb:1e1e/1e1e/2222\007";
    EXPECT_EQ(match_osc_color_reply(s, 0), s.size());
  }

  {
    std::string_view s = "\033]11;rgb:1e1e/1e1e/2222\033\\";
    EXPECT_EQ(match_osc_color_reply(s, 0), s.size());
  }

  {
    std::string_view s = "\033]11;rgb:1e/1e/22\007";
    EXPECT_EQ(match_osc_color_reply(s, 0), s.size());
  }

  {
    std::string_view s = "garbage\033]11;rgb:00/00/00\007";
    EXPECT_EQ(match_osc_color_reply(s, 7), s.size() - 7);
  }

  {
    std::string_view s = "\033[?1u";
    EXPECT_EQ(match_osc_color_reply(s, 0), 0u);
  }

  {
    std::string_view s = "hello";
    EXPECT_EQ(match_osc_color_reply(s, 0), 0u);
  }

  {
    std::string_view s = "\033]0;my title\007";
    EXPECT_EQ(match_osc_color_reply(s, 0), 0u);
  }

  {
    std::string_view s = "\033]11;rgb:1e1e/1e1e/2222";
    EXPECT_EQ(match_osc_color_reply(s, 0), 0u);
  }

  {
    std::string_view s = "\033]11;rgb:1e1e/1e1e/2222\033";
    EXPECT_EQ(match_osc_color_reply(s, 0), 0u);
  }
}

// ── advance_utf8 ─────────────────────────────────────────────────────────────

TEST(Terminal, AdvanceUtf8) {
  {
    EXPECT_EQ(advance_utf8("hello", 0), 1u);
    EXPECT_EQ(advance_utf8("hello", 4), 5u);
  }

  {
    // é = \xc3\xa9
    std::string_view s = "\xc3\xa9";
    EXPECT_EQ(advance_utf8(s, 0), 2u);
  }

  {
    // 你 = \xe4\xbd\xa0
    std::string_view s = "\xe4\xbd\xa0";
    EXPECT_EQ(advance_utf8(s, 0), 3u);
  }

  {
    // 😀 = \xf0\x9f\x98\x80
    std::string_view s = "\xf0\x9f\x98\x80";
    EXPECT_EQ(advance_utf8(s, 0), 4u);
  }

  {
    // Only first 2 bytes of 你 present
    std::string_view s = "\xe4\xbd";
    EXPECT_EQ(advance_utf8(s, 0), 2u); // min(0+3, 2)
  }

  {
    std::string_view s = "a";
    EXPECT_EQ(advance_utf8(s, 1), 1u); // i >= size → return i
  }

  { EXPECT_EQ(advance_utf8("", 0), 0u); }
}

// ── codepoint_width
// ───────────────────────────────────────────────────────────

TEST(Terminal, CodepointWidth) {
  {
    EXPECT_EQ(codepoint_width("A", 0), 1);
    EXPECT_EQ(codepoint_width("z", 0), 1);
  }

  { EXPECT_EQ(codepoint_width(" ", 0), 1); }

  {
    EXPECT_EQ(codepoint_width("\x01", 0), 0);
    EXPECT_EQ(codepoint_width("\x1f", 0), 0);
  }

  {
    // \xe4\xbd\xa0
    EXPECT_EQ(codepoint_width("\xe4\xbd\xa0", 0), 2);
  }

  {
    // \xe5\xa5\xbd
    EXPECT_EQ(codepoint_width("\xe5\xa5\xbd", 0), 2);
  }

  {
    // \xed\x95\x9c
    EXPECT_EQ(codepoint_width("\xed\x95\x9c", 0), 2);
  }

  {
    // \xef\xbc\xa1
    EXPECT_EQ(codepoint_width("\xef\xbc\xa1", 0), 2);
  }

  {
    // \xf0\x9f\x98\x80
    EXPECT_EQ(codepoint_width("\xf0\x9f\x98\x80", 0), 2);
  }

  {
    // \xcc\x80
    EXPECT_EQ(codepoint_width("\xcc\x80", 0), 0);
  }

  {
    // \xc3\xa9 — narrow
    EXPECT_EQ(codepoint_width("\xc3\xa9", 0), 1);
  }
}

TEST(Terminal, TerminalUiHelpers) {
  {
    EXPECT_EQ(display_columns("\033[31mred\033[0m"), 3);
    EXPECT_EQ(display_columns("你"), 2);
  }

  {
    EXPECT_EQ(truncate_ansi_line("\033[31mabcdef\033[0m", 3),
              "\033[31mabc\033[0m");
    EXPECT_EQ(truncate_ansi_line("hello\nworld", 80), "hello");
  }
}

TEST(Terminal, SplitLines) {
  {
    const auto lines = split_lines("\033[31mabcdef\033[0m", 3);
    EXPECT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "\033[31mabc");
    EXPECT_EQ(lines[1], "def");
    EXPECT_EQ(lines[2], "\033[0m");
  }

  {
    const auto lines = split_lines("one\n\ntwo", 80);
    EXPECT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "one");
    EXPECT_EQ(lines[1], "");
    EXPECT_EQ(lines[2], "two");
  }
}

TEST(Terminal, DispatchToolUpdate) {
  {
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
    EXPECT_EQ(renderer.seen_call_id, "call-1");
    EXPECT_EQ(renderer.seen_tool_name, "bash");
    EXPECT_EQ(renderer.seen_result, "out");
  }

  {
    class RequestRenderer final : public Renderer {
    public:
      void on_text_delta(std::string_view) override {}
      void on_request(const RendererRequest &request) override {
        ++calls;
        seen = request;
      }

      int calls{0};
      RendererRequest seen;
    } renderer;

    UserMessage user;
    user.content.emplace_back(TextContent{.text = "hello"});
    user.content.emplace_back(
        ImageContent{.data = "base64", .mime_type = "image/png"});
    user.content.emplace_back(ThinkingContent{.thinking = "not user text"});
    InputProvenance presentation{.source = InputProvenance::Source::mailbox,
                                 .message_id = "message-1",
                                 .message_kind = "request",
                                 .sender_agent_id = "agent-1",
                                 .sender_session_id = "session-1"};

    dispatch_event(
        MessageStartEvent{Message{std::move(user)}, std::move(presentation)},
        renderer);
    EXPECT_EQ(renderer.calls, 1);
    EXPECT_EQ(renderer.seen.text, "hello");
    EXPECT_EQ(renderer.seen.non_text_attachments, std::size_t{2});
    EXPECT_EQ(renderer.seen.presentation.source,
              InputProvenance::Source::mailbox);
    ASSERT_TRUE(renderer.seen.presentation.message_id.has_value());
    EXPECT_EQ(*renderer.seen.presentation.message_id, "message-1");
    ASSERT_TRUE(renderer.seen.presentation.sender_agent_id.has_value());
    EXPECT_EQ(*renderer.seen.presentation.sender_agent_id, "agent-1");

    UserMessage ordinary;
    ordinary.content.emplace_back(TextContent{.text = "ordinary"});
    dispatch_event(MessageStartEvent{Message{std::move(ordinary)}}, renderer);
    EXPECT_EQ(renderer.calls, 2);
    EXPECT_EQ(renderer.seen.presentation.source,
              InputProvenance::Source::ordinary);
    EXPECT_FALSE(renderer.seen.presentation.message_id.has_value());

    AssistantMessage assistant;
    assistant.content.emplace_back(TextContent{.text = "assistant"});
    dispatch_event(MessageStartEvent{Message{std::move(assistant)}}, renderer);
    EXPECT_EQ(renderer.calls, 2);
  }

  {
    class EndRenderer final : public Renderer {
    public:
      void on_text_delta(std::string_view) override {}
      void
      on_message_end_presentation(const MessageEndPresentation &end) override {
        seen = end;
      }

      MessageEndPresentation seen;
    } renderer;

    AssistantMessage assistant;
    assistant.stop_reason = StopReason::length;
    assistant.usage.output = 17;
    dispatch_event(MessageEndEvent{Message{std::move(assistant)}}, renderer);
    EXPECT_EQ(renderer.seen.stop_reason, StopReason::length);
    EXPECT_EQ(renderer.seen.usage.output, std::uint64_t{17});
  }
}

// ── cursor_rows_for_rendered
// ──────────────────────────────────────────────────

TEST(Terminal, CursorRows) {
  { EXPECT_EQ(cursor_rows_for_rendered("Hello", 80), 1); }

  { EXPECT_EQ(cursor_rows_for_rendered("Hello\n", 80), 2); }

  { EXPECT_EQ(cursor_rows_for_rendered("Hello\n\n", 80), 3); }

  { EXPECT_EQ(cursor_rows_for_rendered("Hello\nWorld", 80), 2); }

  {
    // 10 chars at width 5 → wraps once, cursor at start of row 3
    EXPECT_EQ(cursor_rows_for_rendered("0123456789", 5), 3);
  }

  {
    // 5 chars at width 5 — cursor wraps to row 2
    EXPECT_EQ(cursor_rows_for_rendered("12345", 5), 2);
  }

  {
    // \033[1;94m## Heading\033[0m\n\n — should be same as "## Heading\n\n"
    EXPECT_EQ(cursor_rows_for_rendered("\033[1;94m## Heading\033[0m\n\n", 80),
              3);
    EXPECT_EQ(cursor_rows_for_rendered("## Heading\n\n", 80), 3);
  }

  {
    // The hyperlink escape itself contributes 0 columns
    std::string s = "\033]8;;https://example.com\007link text\033]8;;\007";
    // "link text" is 9 chars, fits on 1 row at width 80
    EXPECT_EQ(cursor_rows_for_rendered(s, 80), 1);
  }

  {
    // 你好 = 2+2 = 4 visual cols at width 5 → fits on 1 row (no wrap)
    EXPECT_EQ(cursor_rows_for_rendered("\xe4\xbd\xa0\xe5\xa5\xbd", 5), 1);
    // at width 4 → exactly 4 cols → cursor wraps to row 2
    EXPECT_EQ(cursor_rows_for_rendered("\xe4\xbd\xa0\xe5\xa5\xbd", 4), 2);
    // at width 3 → 你 (2) fits, 好 (2) would exceed → wraps → 2 rows
    EXPECT_EQ(cursor_rows_for_rendered("\xe4\xbd\xa0\xe5\xa5\xbd", 3), 2);
  }

  {
    // "A你B" at width 4 → A(1) + 你(2) + B(1) = 4 cols → wraps → 2 rows
    EXPECT_EQ(cursor_rows_for_rendered("A\xe4\xbd\xa0"
                                       "B",
                                       4),
              2);
    // at width 5 → 4 cols fits → 1 row
    EXPECT_EQ(cursor_rows_for_rendered("A\xe4\xbd\xa0"
                                       "B",
                                       5),
              1);
  }

  { EXPECT_EQ(cursor_rows_for_rendered("", 80), 1); }
}

// ── rows_for_line
// ─────────────────────────────────────────────────────────────

TEST(Terminal, RowsForLine) {
  { EXPECT_EQ(rows_for_line("Hello", 80), 1); }

  { EXPECT_EQ(rows_for_line("12345", 5), 2); }

  { EXPECT_EQ(rows_for_line("0123456789", 5), 3); }

  {
    EXPECT_EQ(rows_for_line("\033[1mHello\033[0m", 80), 1);
    EXPECT_EQ(rows_for_line("\033[1mHello\033[0m", 3), 2);
  }

  {
    // 你好 = 4 visual cols at width 5 → 1 row
    EXPECT_EQ(rows_for_line("\xe4\xbd\xa0\xe5\xa5\xbd", 5), 1);
    // at width 3 → 你(2)=2, 好(2) would push to 4 > 3 → wrap → 2 rows
    EXPECT_EQ(rows_for_line("\xe4\xbd\xa0\xe5\xa5\xbd", 3), 2);
  }

  { EXPECT_EQ(rows_for_line("", 80), 1); }
}

// ── BlockBoundaryScanner
// ──────────────────────────────────────────────────────

TEST(Terminal, BlockBoundaryScanner) {
  {
    BlockBoundaryScanner sc;
    sc.advance("para\n\nmore");
    EXPECT_EQ(sc.last_stable, 6u);
  }

  {
    // Boundary IS committed even with nothing after it — empty suffix is fine.
    BlockBoundaryScanner sc;
    sc.advance("para\n\n");
    EXPECT_EQ(sc.last_stable, 6u);
  }

  {
    // Line before blank has indent 2; last_nonblank_col0 == false.
    BlockBoundaryScanner sc;
    sc.advance("  item\n\nmore");
    EXPECT_EQ(sc.last_stable, 0u);
  }

  {
    // "p1\n\n" → boundary at 4; "p2\n\n" → boundary at 8.
    BlockBoundaryScanner sc;
    sc.advance("p1\n\np2\n\nmore");
    EXPECT_EQ(sc.last_stable, 8u);
  }

  {
    BlockBoundaryScanner sc;
    sc.advance("");
    EXPECT_EQ(sc.last_stable, 0u);
  }

  {
    // Initial last_nonblank_col0=true → \n\n at start commits.
    BlockBoundaryScanner sc;
    sc.advance("\n\npara");
    EXPECT_EQ(sc.last_stable, 2u);
  }

  {
    // ```py\ncode\n```\n\nmore — fence opens, closes, then boundary fires.
    BlockBoundaryScanner sc;
    sc.advance("```py\ncode\n```\n\nmore");
    // "```py\n" = 6, "code\n" = 5, "```\n" = 4, "\n" = 1 → boundary at 16
    EXPECT_EQ(sc.last_stable, 16u);
  }

  {
    // Blank line inside the fence must not become a boundary.
    BlockBoundaryScanner sc;
    sc.advance("```py\n\nstill\n```\n\nmore");
    // "```py\n" = 6, "\n" = 1, "still\n" = 6, "```\n" = 4, "\n" = 1 → boundary
    // at 18
    EXPECT_EQ(sc.last_stable, 18u);
    // Verify it's not the in-fence \n\n (which would be at 7).
    // last_stable must be >= 18 (after the closing fence).
  }

  {
    BlockBoundaryScanner sc;
    sc.advance("~~~\ncode\n~~~\n\nmore");
    // "~~~\n" = 4, "code\n" = 5, "~~~\n" = 4, "\n" = 1 → boundary at 14
    EXPECT_EQ(sc.last_stable, 14u);
  }

  {
    // "```   \n" is a valid closer; line_only_fence stays true through spaces.
    BlockBoundaryScanner sc;
    sc.advance("```\ncode\n```   \n\nmore");
    // "```\n" = 4, "code\n" = 5, "```   \n" = 7, "\n" = 1 → boundary at 17
    EXPECT_EQ(sc.last_stable, 17u);
  }

  {
    // Scanning "para\n" then the full "para\n\nmore" must give the same
    // last_stable as scanning "para\n\nmore" in one shot.
    // Second advance() starts from scan_pos=5 so only processes "\nmore".
    BlockBoundaryScanner sc_inc;
    sc_inc.advance("para\n");
    sc_inc.advance("para\n\nmore");
    BlockBoundaryScanner sc_one;
    sc_one.advance("para\n\nmore");
    EXPECT_EQ(sc_inc.last_stable, sc_one.last_stable);
    EXPECT_EQ(sc_inc.last_stable, 6u);
  }

  {
    // Opening ````py needs closing ```` (4+), not just ```.
    BlockBoundaryScanner sc;
    sc.advance("````py\ncode\n```\nstill_in_fence\n````\n\nout");
    // After "```\n": fence_run=3 < fence_len=4, so fence stays open.
    // After "````\n": fence_run=4 >= fence_len=4 → close.
    // Then "\n" → boundary.
    // "````py\n"=7 + "code\n"=5 + "```\n"=4 + "still_in_fence\n"=15 +
    // "````\n"=5 + "\n"=1 = 37
    EXPECT_EQ(sc.last_stable, 37u);
  }
}

TEST(Terminal, TerminalTitleHelpers) {
  {
    auto s = sanitize_terminal_title(
        "  Project\t|\nWorking\x1b\x07\x9d\x9c |  Thread  ");
    EXPECT_EQ(s, "Project | Working | Thread");
  }
  {
    auto s = sanitize_terminal_title(
        "Pro\u202Ej\u2066e\u200Fc\u061Ct\u200B \uFEFFT\u2060itle");
    EXPECT_EQ(s, "Project Title");
  }
  {
    std::string in = "a\x1b"
                     "b\x07"
                     "c\n"
                     "d\r"
                     "e\t"
                     "f\x1f"
                     "g";
    EXPECT_EQ(sanitize_terminal_title(in), "a b c d e f g");
  }
  {
    EXPECT_EQ(sanitize_terminal_title("  hello   world  "), "hello world");
    EXPECT_EQ(sanitize_terminal_title("\t\n hello \n\t world \n"),
              "hello world");
  }
  {
    EXPECT_EQ(sanitize_terminal_title("a\u202Db\u202Ec"), "abc");
    EXPECT_EQ(sanitize_terminal_title("a\u200Bb\u200Cc"), "abc");
    EXPECT_EQ(sanitize_terminal_title("a\uFEFFb"), "ab");
  }
  {
    EXPECT_EQ(sanitize_terminal_title("caf\xc3\xa9 \xf0\x9f\x98\x80"),
              "caf\xc3\xa9 \xf0\x9f\x98\x80");
    EXPECT_EQ(sanitize_terminal_title("\xe4\xbd\xa0\xe5\xa5\xbd"),
              "\xe4\xbd\xa0\xe5\xa5\xbd");
  }
  {
    std::string in(kMaxTerminalTitleChars + 10, 'a');
    auto s = sanitize_terminal_title(in);
    EXPECT_EQ(s.size(), kMaxTerminalTitleChars);
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
      ++count;
      i = advance_utf8(s, i);
    }
    EXPECT_EQ(count, kMaxTerminalTitleChars);
  }
  {
    std::string in(kMaxTerminalTitleChars - 1, 'a');
    in += " b";
    auto s = sanitize_terminal_title(in);
    ASSERT_THAT(s, testing::Not(testing::IsEmpty()));
    EXPECT_EQ(s.size(), kMaxTerminalTitleChars);
    EXPECT_EQ(s.back(), 'b');
  }
  {
    EXPECT_EQ(sanitize_terminal_title("\x1b\x07 \n\t"), "");
    EXPECT_EQ(sanitize_terminal_title("\u200B\uFEFF"), "");
  }
  {
    std::string in;
    for (std::size_t i = 0; i < kMaxTerminalTitleChars - 1; ++i)
      in += 'a';
    in += "\xf0\x9f\x98\x80";
    in += "extra";
    auto s = sanitize_terminal_title(in);
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
      ++count;
      i = advance_utf8(s, i);
    }
    EXPECT_EQ(count, kMaxTerminalTitleChars);
  }
  { EXPECT_EQ(format_active_terminal_title("pici", "\u280B"), "\u280B pici"); }
  { EXPECT_EQ(format_active_terminal_title("", "\u280B"), "\u280B"); }
  {
    auto s = format_active_terminal_title("", "\u280B");
    EXPECT_THAT(s, testing::Not(testing::HasSubstr(" ")));
    EXPECT_EQ(format_active_terminal_title("x", "\u280B"), "\u280B x");
  }
  {
    EXPECT_EQ(terminal_title_sequence("hello"), "\033]0;hello\007");
    EXPECT_EQ(terminal_title_sequence(""), "\033]0;\007");
  }
  {
    EXPECT_EQ(kTerminalTitleSpinnerFrames.size(), 10u);
    const std::array<std::string_view, 10> expected = {
        "\u280B", "\u2819", "\u2839", "\u2838", "\u283C",
        "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"};
    for (std::size_t i = 0; i < 10; ++i) {
      SCOPED_TRACE("frame " + std::to_string(i));
      EXPECT_EQ(kTerminalTitleSpinnerFrames[i], expected[i]);
    }
  }
  {
    std::filesystem::path p = "/tmp/some-unique-pici-test-dir";
    EXPECT_EQ(terminal_project_label(p), "some-unique-pici-test-dir");
  }
  { EXPECT_EQ(terminal_project_label(std::filesystem::path{}), "pici"); }
}

TEST(Terminal, TerminalTitleController) {
  {
    std::vector<std::string> writes;
    {
      TerminalTitleController c(
          -1, "pici",
          [&](std::string_view s) {
            writes.emplace_back(s);
            return TerminalTitleResult::Applied;
          },
          std::chrono::milliseconds(10), false);
      c.start_activity();
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      c.stop_activity();
    }
    EXPECT_THAT(writes, testing::IsEmpty());
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "myproj",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(100), true);
    ASSERT_THAT(writes, testing::SizeIs(1));
    EXPECT_EQ(writes[0], "myproj");
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "pici",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(100), true);
    writes.clear();
    c.start_activity();
    ASSERT_THAT(writes, testing::SizeIs(1));
    EXPECT_EQ(writes[0], std::string("\u280B") + " pici");
    c.stop_activity();
  }
  {
    std::vector<std::string> writes;
    std::mutex m;
    TerminalTitleController c(
        -1, "p",
        [&](std::string_view s) {
          std::lock_guard<std::mutex> lk(m);
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    c.stop_activity();
    std::lock_guard<std::mutex> lk(m);
    bool saw_second = false, saw_wrap = false;
    for (auto &w : writes)
      if (w == std::string("\u2819") + " p")
        saw_second = true;
    std::size_t active_count = 0;
    for (auto &w : writes) {
      for (auto f : kTerminalTitleSpinnerFrames)
        if (w.rfind(f, 0) == 0) {
          ++active_count;
          break;
        }
    }
    if (active_count >= 10)
      saw_wrap = true;
    EXPECT_EQ(saw_second, true);
    (void)saw_wrap;
    EXPECT_EQ(active_count >= 3, true);
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "base",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    c.stop_activity();
    ASSERT_THAT(writes, testing::Not(testing::IsEmpty()));
    EXPECT_EQ(writes.back(), "base");
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "base",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    c.stop_activity();
    std::size_t n = writes.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(writes.size(), n);
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "pici",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(50), true);
    writes.clear();
    c.start_activity();
    c.start_activity();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    c.stop_activity();
    std::size_t active_frames = 0;
    for (auto &w : writes) {
      for (auto f : kTerminalTitleSpinnerFrames)
        if (w.rfind(f, 0) == 0) {
          ++active_frames;
          break;
        }
    }
    EXPECT_EQ(active_frames >= 1, true);
    EXPECT_EQ(active_frames <= 2, true);
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "old",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(10), true);
    writes.clear();
    c.start_activity();
    c.set_base_title("new");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    c.stop_activity();
    ASSERT_THAT(writes, testing::Not(testing::IsEmpty()));
    EXPECT_EQ(writes.back(), "new");
    bool saw_new_active = false;
    for (auto &w : writes)
      if (w.find("new") != std::string::npos)
        saw_new_active = true;
    EXPECT_THAT(saw_new_active, testing::IsTrue());
  }
  {
    std::vector<std::string> writes;
    {
      TerminalTitleController c(
          -1, "pici",
          [&](std::string_view s) {
            writes.emplace_back(s);
            return TerminalTitleResult::Applied;
          },
          std::chrono::milliseconds(10), true);
      c.start_activity();
    }
    ASSERT_THAT(writes, testing::Not(testing::IsEmpty()));
    EXPECT_EQ(writes.back(), "");
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "same",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(100), true);
    std::size_t n = writes.size();
    c.set_base_title("same");
    EXPECT_EQ(writes.size(), n);
    c.set_base_title("same ");
    EXPECT_EQ(writes.size(), n);
  }
  {
    std::vector<std::string> writes;
    TerminalTitleController c(
        -1, "",
        [&](std::string_view s) {
          writes.emplace_back(s);
          return TerminalTitleResult::Applied;
        },
        std::chrono::milliseconds(100), true);
    writes.clear();
    c.start_activity();
    ASSERT_THAT(writes, testing::SizeIs(1));
    EXPECT_EQ(writes[0], "\u280B");
    c.stop_activity();
  }
}

TEST(Terminal, AltScreenSession) {
  {
    int fds[2]{};
    if (::pipe(fds) != 0) {
      FAIL() << "pipe(fds) failed";
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
    EXPECT_EQ(output, "\033[?1049h\033[H\033[2J\033[?1000h\033[?1006h"
                      "\033[?1006l\033[?1000l\033[r\033[?25h\033[?1049l");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  {
    notify_sigint();
    EXPECT_EQ(consume_sigint(), true);
    EXPECT_EQ(consume_sigint(), false);
  }
}

// ── main ─────────────────────────────────────────────────────────────────────
