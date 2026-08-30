// Unit tests for the word-wrap planner (src/cli/wrap.{h,cpp}) — pure,
// terminal-I/O-free tests of plan_word_wrap's break-point output. See
// test/test_readline.cpp's test_wrap_boundary_cursor_placement and
// test_word_wrap_boundary (forkpty-based) for the end-to-end equivalent
// against real terminal output.

#include "cli/wrap.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using pi::cli::plan_word_wrap;

template <typename F> void run_case(std::string_view, F &&test) {
  std::forward<F>(test)();
}

// Replays a plan against the same text it was computed from, reconstructing
// the rows exactly as InputRenderer::write_wrapped's replay loop would
// paint them (bytes are copied verbatim between breaks -- including any
// ANSI escapes, which the real replay loop always emits regardless of
// pending breaks -- and dropped whitespace in [content_end, resume_offset)
// is skipped). This exercises the planner's output the same way the
// renderer consumes it, without needing a pty.
std::vector<std::string> wrapped_rows(std::string_view text,
                                      std::size_t columns,
                                      std::size_t start_column) {
  const auto breaks = plan_word_wrap(text, columns, start_column);
  std::vector<std::string> rows(1);
  std::size_t offset = 0;
  std::size_t index = 0;
  while (offset < text.size()) {
    if (index < breaks.size() && offset == breaks[index].content_end) {
      rows.emplace_back();
      offset = breaks[index].resume_offset;
      ++index;
      continue;
    }
    rows.back() += text[offset];
    ++offset;
  }
  return rows;
}

struct Position {
  std::size_t row{1};
  std::size_t column{0};
};

bool operator==(const Position &a, const Position &b) {
  return a.row == b.row && a.column == b.column;
}

// Avoids the brace-init comma being misread as a second macro argument by
// EXPECT_TRUE() at call sites below.
Position pos(std::size_t row, std::size_t column) {
  return {.row = row, .column = column};
}

// Mirrors write_wrapped's cursor-capture invariant (capture happens after
// the wrap decision for the character at cursor_offset, and any offset
// that falls in a dropped-whitespace range normalizes to the row it just
// moved onto) for plain ASCII text driven purely off plan_word_wrap's
// output, independent of terminal I/O.
Position position_for(std::string_view text, std::size_t columns,
                      std::size_t start_column, std::size_t cursor_offset) {
  const auto breaks = plan_word_wrap(text, columns, start_column);
  Position result{.row = 1, .column = start_column};
  std::size_t row = 1;
  std::size_t column = start_column;
  std::size_t offset = 0;
  std::size_t index = 0;
  const auto capture = [&] { result = {.row = row, .column = column}; };
  while (offset < text.size()) {
    if (index < breaks.size() && offset == breaks[index].content_end) {
      const auto resume = breaks[index].resume_offset;
      ++row;
      column = 0;
      if (cursor_offset >= offset && cursor_offset < resume)
        capture();
      ++index;
      offset = resume;
      continue;
    }
    if (offset == cursor_offset)
      capture();
    ++column; // ASCII-only helper: every byte here is one display column.
    ++offset;
  }
  if (cursor_offset == text.size()) {
    result = {.row = row, .column = column};
    if (result.column == columns) {
      ++result.row;
      result.column = 0;
    }
  }
  return result;
}

TEST(Wrap, test_wraps_at_spaces_not_mid_word) {
  run_case("plan_word_wrap: wraps at spaces, never splits a word", [] {
    const auto rows = wrapped_rows("hello world foo", 8, 0);
    EXPECT_EQ(rows.size(), 3U);
    if (rows.size() == 3) {
      EXPECT_EQ(rows[0], "hello");
      EXPECT_EQ(rows[1], "world");
      EXPECT_EQ(rows[2], "foo");
    }
    for (const auto &row : rows)
      EXPECT_TRUE(row.size() <= 8);
  });
}

TEST(Wrap, test_overlong_token_hard_wraps) {
  run_case(
      "plan_word_wrap: a single token wider than the row hard-wraps without "
      "overflowing",
      [] {
        const std::string token(40, 'x');
        const auto rows = wrapped_rows(token, 20, 0);
        EXPECT_EQ(rows.size(), 2U);
        for (const auto &row : rows)
          EXPECT_TRUE(row.size() <= 20);
        std::string rebuilt;
        for (const auto &row : rows)
          rebuilt += row;
        EXPECT_EQ(rebuilt, token);
      });

  run_case(
      "plan_word_wrap: an overlong token resumes word-wrapping right after "
      "it",
      [] {
        const std::string text = std::string(25, 'x') + " ok";
        const auto rows = wrapped_rows(text, 10, 0);
        // "xxxxxxxxxxxxxxxxxxxxxxxxx" (25) hard-wraps into two full 10-wide
        // rows and a 5-wide remainder; word-wrapping then resumes right
        // where the token left off, so " ok" (6 columns free on that last
        // row) lands on the *same* row as the remainder rather than being
        // forced onto a fresh one -- greedy word wrap doesn't leave room
        // unused just because a hard-wrap fallback happened first.
        EXPECT_EQ(rows.size(), 3U);
        if (rows.size() == 3)
          EXPECT_EQ(rows[2], "xxxxx ok");
        for (const auto &row : rows)
          EXPECT_TRUE(row.size() <= 10);
      });
}

TEST(Wrap, test_ansi_escapes_are_zero_width_and_not_break_points) {
  run_case("plan_word_wrap: ANSI escapes don't count toward width or become "
           "break points",
           [] {
             const std::string text = "\033[31mhello\033[0m world";
             const auto rows = wrapped_rows(text, 8, 0);
             EXPECT_EQ(rows.size(), 2U);
             if (rows.size() == 2) {
               EXPECT_EQ(rows[0], "\033[31mhello\033[0m");
               EXPECT_EQ(rows[1], "world");
             }
             // Without the escapes contributing to width, plain "helloworld"
             // colored text still wraps exactly like the plain-text case
             // above.
             const auto plain_rows = wrapped_rows("hello world", 8, 0);
             EXPECT_EQ(plain_rows.size(), rows.size());
           });
}

TEST(Wrap, test_prompt_carried_start_column) {
  run_case("plan_word_wrap: a nonzero start_column (prompt-carried) wraps "
           "correctly and continuation rows start at column 0",
           [] {
             // Simulates a 6-column-wide prompt already having painted the
             // row: "hello" (5 wide) doesn't fit in the remaining 4 columns,
             // but does fit a fresh row, so it moves there entirely rather
             // than being split; "world" then can't fit after it (5+1+5 > 10)
             // and gets its own row too -- landing at column 0, not
             // hang-indented under the prompt's 6-column start.
             const auto rows = wrapped_rows("hello world", 10, 6);
             EXPECT_EQ(rows.size(), 3U);
             if (rows.size() == 3) {
               // Nothing from this text fits alongside the prompt's own
               // 6 columns.
               EXPECT_TRUE(rows[0].empty());
               EXPECT_EQ(rows[1], "hello");
               EXPECT_EQ(rows[2], "world");
             }
           });
}

TEST(Wrap, test_cursor_position_at_word_wrap_boundary) {
  run_case("plan_word_wrap: cursor position right before/at/after a word-wrap "
           "break lands correctly",
           [] {
             // "hello world" at columns=8: "hello" (5) fits, then " world"
             // (1+5) doesn't (5+1+5=11 > 8) but "world" alone fits a fresh row,
             // so the break drops the space between them: content_end=5,
             // resume=6.
             const auto last_char_before_break =
                 position_for("hello world", 8, 0, 4);
             EXPECT_TRUE(last_char_before_break == pos(1, 4));

             // Offset 5 is the dropped space itself -- normalizes to the new
             // row's start, mirroring the M0/M1 invariant for a cursor sitting
             // exactly on a soft-wrap point.
             const auto on_dropped_space = position_for("hello world", 8, 0, 5);
             EXPECT_TRUE(on_dropped_space == pos(2, 0));

             // Offset 6 is 'w', the first character resumed on the new row.
             const auto first_char_after_break =
                 position_for("hello world", 8, 0, 6);
             EXPECT_TRUE(first_char_after_break == pos(2, 0));
           });

  run_case("plan_word_wrap: cursor position at a hard-wrap fallback boundary "
           "inside an overlong token",
           [] {
             const std::string token(20, 'x');
             // columns=8: token hard-wraps into rows of 8, 8, 4. Offset 8 is
             // the first character of the second row.
             const auto at_fallback_boundary = position_for(token, 8, 0, 8);
             EXPECT_TRUE(at_fallback_boundary == pos(2, 0));
             const auto last_of_first_row = position_for(token, 8, 0, 7);
             EXPECT_TRUE(last_of_first_row == pos(1, 7));
           });
}

TEST(Wrap, test_fits_without_wrapping) {
  run_case("plan_word_wrap: text that fits produces no breaks", [] {
    const auto breaks = plan_word_wrap("short", 80, 0);
    EXPECT_TRUE(breaks.empty());
  });
}

TEST(Wrap, test_end_of_text_full_row_normalizes) {
  run_case("plan_word_wrap: cursor at end of text on an exactly-full row "
           "normalizes to the next row's start",
           [] {
             // "12345678" (8 chars) exactly fills an 8-column row.
             const auto position = position_for("12345678", 8, 0, 8);
             EXPECT_TRUE(position == pos(2, 0));
           });
}
