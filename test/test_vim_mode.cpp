// Unit tests for the vim-mode Normal-mode key layer (src/cli/vim_mode.{h,
// cpp}) -- pure, terminal-I/O-free tests of VimEngine::handle_normal_key
// operating directly on an in-memory buffer/cursor, mirroring how
// test_wrap.cpp exercises plan_word_wrap without a pty. See
// test/test_readline.cpp's vim-mode tests for the end-to-end equivalent
// (Escape entering Normal mode, i/a returning to Insert, vim_mode=false
// leaving M0-M4 behavior unchanged) against a real terminal.

#include "cli/vim_mode.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

using pi::cli::VimEngine;
using pi::cli::VimMode;

template <typename F> void run_case(std::string_view, F &&test) {
  std::forward<F>(test)();
}

// Sends one key through a fresh VimEngine already in Normal mode (the
// production entry point, readline.cpp, only ever calls
// handle_normal_key while mode() == Normal -- see its hook-point comment).
struct Fixture {
  VimEngine engine;
  std::string buf;
  std::size_t cursor{0};
  std::string kill_buffer;

  explicit Fixture(std::string text, std::size_t start_cursor = 0)
      : buf(std::move(text)), cursor(start_cursor) {
    engine.set_mode(VimMode::Normal);
  }

  VimEngine::Result send(char key) {
    return engine.handle_normal_key(static_cast<unsigned char>(key), buf,
                                    cursor, kill_buffer);
  }
};

TEST(VimMode, test_hl_move_by_codepoint) {
  run_case("VimEngine: h/l move the cursor left/right by one codepoint", [] {
    Fixture f("abc", 1);
    f.send('l');
    EXPECT_EQ(f.cursor, 2U);
    f.send('h');
    f.send('h');
    EXPECT_EQ(f.cursor, 0U);
    // h at buffer start is a harmless no-op (clamped, not
    // wrapped or misinterpreted).
    const auto result = f.send('h');
    EXPECT_EQ(f.cursor, 0U);
    EXPECT_TRUE(result.changed); // cursor "changed" to the same value;
                                 // redraw is idempotent either way.
  });
}

TEST(VimMode, test_jk_move_by_logical_line) {
  run_case("VimEngine: j/k move the cursor by logical line, preserving "
           "column",
           [] {
             Fixture f("hello\nhi\nworld", 2); // cursor on the second 'l'
             f.send('j');
             // Line 2 ("hi") is shorter than column 2 -- clamps to its
             // end (offset 8, right after "hi").
             EXPECT_EQ(f.cursor, 8U);
             f.send('j');
             // Line 3 ("world") has room for column 2 again.
             EXPECT_EQ(f.cursor, 11U);
             f.send('k');
             EXPECT_EQ(f.cursor, 8U);
           });
}

TEST(VimMode, test_0_and_dollar) {
  run_case("VimEngine: 0/$ move to the start/end of the current line", [] {
    Fixture f("hello\nworld", 8); // cursor mid "world"
    f.send('0');
    EXPECT_EQ(f.cursor, 6U); // start of "world"
    f.send('$');
    EXPECT_EQ(f.cursor, 11U); // end of buffer (last line, no trailing '\n')
  });
}

TEST(VimMode, test_w_start_of_next_word) {
  run_case("VimEngine: w moves to the start of the next word", [] {
    Fixture f("hello world foo", 0);
    f.send('w');
    EXPECT_EQ(f.cursor, 6U); // 'w' of "world"
    f.send('w');
    EXPECT_EQ(f.cursor, 12U); // 'f' of "foo"
    f.send('w');
    // No further word: lands at end of buffer.
    EXPECT_EQ(f.cursor, 15U);
  });
}

TEST(VimMode, test_e_end_of_word_distinct_from_w) {
  run_case("VimEngine: e moves to the end of the current/next word, landing ON "
           "its last character -- distinct from w's start-of-next-word target",
           [] {
             Fixture f("hello world foo", 0);
             const auto w_target = [] {
               Fixture g("hello world foo", 0);
               g.send('w');
               return g.cursor;
             }();
             f.send('e');
             // e lands ON the 'o' ending "hello" (index 4); w lands at
             // "world"'s start (index 6) -- the two must not coincide, or this
             // milestone got the documented gotcha wrong.
             EXPECT_EQ(f.cursor, 4U);
             EXPECT_TRUE(f.cursor != w_target);

             // Repeated e from the last character of the current word jumps to
             // the end of the *next* word instead of just one column further --
             // the classic "already on the last char" case.
             Fixture mid("hello world foo", 4); // already on the final 'o'
             mid.send('e');
             EXPECT_EQ(mid.cursor, 10U); // 'd' ending "world"
           });
}

TEST(VimMode, test_b_start_of_previous_word) {
  run_case("VimEngine: b moves to the start of the previous word", [] {
    Fixture f("hello world foo", 12); // on 'f' of "foo"
    f.send('b');
    EXPECT_EQ(f.cursor, 6U); // start of "world"
    f.send('b');
    EXPECT_EQ(f.cursor, 0U); // start of "hello"
  });
}

TEST(VimMode, test_uncovered_key_is_harmless_noop) {
  run_case("VimEngine: an uncovered Normal-mode key (e.g. 'x', not implemented "
           "in this milestone's scope) is a no-op, not eaten or misinterpreted",
           [] {
             Fixture f("hello", 2);
             const auto result = f.send('x');
             EXPECT_TRUE(!result.changed);
             EXPECT_EQ(f.buf, "hello");
             EXPECT_EQ(f.cursor, 2U);
             // A digit (no count-prefix support in this milestone's scope) is
             // likewise a no-op rather than starting some half-implemented
             // count.
             const auto digit_result = f.send('3');
             EXPECT_TRUE(!digit_result.changed);
             EXPECT_EQ(f.buf, "hello");
           });
}

TEST(VimMode, test_dw_deletes_to_start_of_next_word) {
  run_case("VimEngine: dw deletes from cursor to the start of the next "
           "word",
           [] {
             Fixture f("hello world foo", 0);
             f.send('d');
             const auto result = f.send('w');
             EXPECT_TRUE(result.changed);
             EXPECT_EQ(f.buf, "world foo");
             EXPECT_EQ(f.cursor, 0U);
             EXPECT_EQ(f.kill_buffer, "hello ");
             // d/c stays in Normal mode.
             EXPECT_TRUE(f.engine.mode() == VimMode::Normal);
           });
}

TEST(VimMode, test_dl_deletes_char_under_cursor) {
  run_case("VimEngine: dl deletes exactly the character under the cursor", [] {
    Fixture f("hello", 0);
    f.send('d');
    f.send('l');
    EXPECT_EQ(f.buf, "ello");
    EXPECT_EQ(f.cursor, 0U);
    EXPECT_EQ(f.kill_buffer, "h");
  });
}

TEST(VimMode, test_dh_deletes_char_before_cursor) {
  run_case("VimEngine: dh deletes exactly the character before the cursor "
           "(like Backspace)",
           [] {
             Fixture f("hello world", 6); // cursor on 'w'
             f.send('d');
             f.send('h');
             EXPECT_EQ(f.buf, "helloworld");
             EXPECT_EQ(f.cursor, 5U);
             EXPECT_EQ(f.kill_buffer, " ");
           });
}

TEST(VimMode, test_d0_deletes_to_line_start) {
  run_case("VimEngine: d0 deletes from the line start up to (not "
           "including) the cursor",
           [] {
             Fixture f("hello world", 5); // cursor right after "hello"
             f.send('d');
             f.send('0');
             EXPECT_EQ(f.buf, " world");
             EXPECT_EQ(f.cursor, 0U);
             EXPECT_EQ(f.kill_buffer, "hello");
           });
}

TEST(VimMode, test_ddollar_deletes_to_line_end) {
  run_case("VimEngine: d$ deletes from the cursor to the end of the "
           "line, leaving the newline (if any) intact",
           [] {
             Fixture f("hello world\nsecond", 6); // on 'w' of "world"
             f.send('d');
             f.send('$');
             EXPECT_EQ(f.buf, "hello \nsecond");
             EXPECT_EQ(f.cursor, 6U);
             EXPECT_EQ(f.kill_buffer, "world");
           });
}

TEST(VimMode, test_de_deletes_to_end_of_word) {
  run_case("VimEngine: de deletes from cursor to the end of the current "
           "word",
           [] {
             Fixture f("hello world", 0);
             f.send('d');
             f.send('e');
             EXPECT_EQ(f.buf, " world");
             EXPECT_EQ(f.cursor, 0U);
             EXPECT_EQ(f.kill_buffer, "hello");
           });
}

TEST(VimMode, test_db_deletes_to_start_of_previous_word) {
  run_case("VimEngine: db deletes from the start of the previous word up "
           "to the cursor",
           [] {
             Fixture f("hello world", 11); // end of buffer
             f.send('d');
             f.send('b');
             EXPECT_EQ(f.buf, "hello ");
             EXPECT_EQ(f.cursor, 6U);
             EXPECT_EQ(f.kill_buffer, "world");
           });
}

TEST(VimMode, test_operator_pending_invalid_motion_cancels_cleanly) {
  run_case("VimEngine: an operator followed by an unrecognized key cancels the "
           "pending operator without applying it or getting stuck",
           [] {
             Fixture f("hello world", 0);
             f.send('d');
             const auto cancel_result = f.send('x'); // not a motion
             EXPECT_TRUE(!cancel_result.changed);
             EXPECT_EQ(f.buf, "hello world"); // nothing deleted
             // The pending operator must be fully cleared: a plain motion right
             // afterward moves the cursor instead of being swallowed as if it
             // were still completing "d".
             const auto motion_result = f.send('w');
             EXPECT_TRUE(motion_result.changed);
             EXPECT_EQ(f.buf,
                       "hello world"); // still just a motion, no deletion
             EXPECT_EQ(f.cursor, 6U);
           });
}

TEST(VimMode, test_escape_cancels_pending_operator) {
  run_case("VimEngine: cancel_pending() clears an in-progress operator "
           "(what readline.cpp calls on every Escape byte)",
           [] {
             Fixture f("hello world", 0);
             f.send('d');
             f.engine.cancel_pending();
             f.send('w');
             EXPECT_EQ(f.buf, "hello world"); // no deletion: 'w' was a
                                              // plain motion, not part of
                                              // a stale "d"
             EXPECT_EQ(f.cursor, 6U);
           });
}

TEST(VimMode, test_c_operator_deletes_and_enters_insert) {
  run_case("VimEngine: c<motion> deletes the span and drops into Insert mode",
           [] {
             Fixture f("hello world", 0);
             f.send('c');
             const auto result = f.send('w');
             EXPECT_TRUE(result.changed);
             EXPECT_EQ(f.buf, "world");
             EXPECT_EQ(f.cursor, 0U);
             EXPECT_TRUE(f.engine.mode() == VimMode::Insert);
           });
}

TEST(VimMode, test_dd_deletes_whole_line_and_joins) {
  run_case("VimEngine: dd deletes the whole current line, including one "
           "adjacent newline, and lands on the line that takes its place",
           [] {
             Fixture f("aa\nbb\ncc", 4); // on the second 'b'
             f.send('d');
             const auto result = f.send('d');
             EXPECT_TRUE(result.changed);
             EXPECT_EQ(f.buf, "aa\ncc");
             EXPECT_EQ(f.cursor, 3U); // start of "cc"
             EXPECT_EQ(f.kill_buffer, "bb\n");
             EXPECT_TRUE(f.engine.mode() == VimMode::Normal);
           });
}

TEST(VimMode, test_dd_on_last_line_absorbs_previous_newline) {
  run_case("VimEngine: dd on the last line (no trailing newline of its own) "
           "removes the previous line's newline instead, leaving no blank line",
           [] {
             Fixture f("aa\nbb\ncc", 6); // on the first 'c'
             f.send('d');
             f.send('d');
             EXPECT_EQ(f.buf, "aa\nbb");
             EXPECT_EQ(f.cursor, 3U); // start of the new last line, "bb"
           });
}

TEST(VimMode, test_dd_on_only_line_empties_buffer) {
  run_case("VimEngine: dd on a single-line buffer empties it", [] {
    Fixture f("hello", 2);
    f.send('d');
    f.send('d');
    EXPECT_TRUE(f.buf.empty());
    EXPECT_EQ(f.cursor, 0U);
  });
}

TEST(VimMode, test_cc_keeps_line_structure_and_enters_insert) {
  run_case("VimEngine: cc clears the current line's content but keeps the line "
           "itself (newlines untouched), landing in Insert mode at its start",
           [] {
             Fixture f("aa\nbb\ncc", 4); // on the second 'b'
             f.send('c');
             const auto result = f.send('c');
             EXPECT_TRUE(result.changed);
             EXPECT_EQ(f.buf, "aa\n\ncc"); // "bb" removed, both newlines remain
             EXPECT_EQ(f.cursor, 3U);
             EXPECT_EQ(f.kill_buffer, "bb");
             EXPECT_TRUE(f.engine.mode() == VimMode::Insert);
           });
}

TEST(VimMode, test_i_a_A_I_enter_insert_mode) {
  run_case("VimEngine: i/a/A/I all drop into Insert mode, positioning the "
           "cursor per their own convention",
           [] {
             {
               Fixture f("abc", 1);
               f.send('i'); // insert before cursor: no cursor change
               EXPECT_EQ(f.cursor, 1U);
               EXPECT_TRUE(f.engine.mode() == VimMode::Insert);
             }
             {
               Fixture f("abc", 1);
               f.send('a'); // insert after cursor
               EXPECT_EQ(f.cursor, 2U);
               EXPECT_TRUE(f.engine.mode() == VimMode::Insert);
             }
             {
               Fixture f("hello\nworld", 8); // mid second line
               f.send('A');                  // insert at line end
               EXPECT_EQ(f.cursor, 11U);
               EXPECT_TRUE(f.engine.mode() == VimMode::Insert);
             }
             {
               Fixture f("hello\nworld", 8);
               f.send('I'); // insert at line start
               EXPECT_EQ(f.cursor, 6U);
               EXPECT_TRUE(f.engine.mode() == VimMode::Insert);
             }
           });
}

TEST(VimMode, test_starts_in_insert_mode_by_default) {
  run_case("VimEngine: a fresh engine starts in Insert mode (empty "
           "composer behaves exactly like today until the user presses "
           "Escape)",
           [] {
             VimEngine engine;
             EXPECT_TRUE(engine.mode() == VimMode::Insert);
           });
}
