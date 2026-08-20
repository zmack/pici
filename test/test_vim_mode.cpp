// Unit tests for the vim-mode Normal-mode key layer (src/cli/vim_mode.{h,
// cpp}) -- pure, terminal-I/O-free tests of VimEngine::handle_normal_key
// operating directly on an in-memory buffer/cursor, mirroring how
// test_wrap.cpp exercises plan_word_wrap without a pty. See
// test/test_readline.cpp's vim-mode tests for the end-to-end equivalent
// (Escape entering Normal mode, i/a returning to Insert, vim_mode=false
// leaving M0-M4 behavior unchanged) against a real terminal.

#include "cli/vim_mode.h"

#include <cstddef>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace tests {

int passed{0};
int failed{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition)
    return true;
  ++failed;
  std::cerr << "FAIL " << location.file_name() << ":" << location.line()
            << " - " << expression << "\n";
  return false;
}

void run(std::string_view name, auto test) {
  const auto before = failed;
  test();
  if (failed == before) {
    ++passed;
    std::cout << "PASS " << name << "\n";
  }
}

} // namespace tests

#define CHECK(condition) ::tests::check((condition), #condition)
#define CHECK_EQ(left, right)                                                 \
  ::tests::check((left) == (right), #left " == " #right)

using pi::cli::VimEngine;
using pi::cli::VimMode;

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

void test_hl_move_by_codepoint() {
  tests::run("VimEngine: h/l move the cursor left/right by one codepoint",
             [] {
               Fixture f("abc", 1);
               f.send('l');
               CHECK_EQ(f.cursor, 2U);
               f.send('h');
               f.send('h');
               CHECK_EQ(f.cursor, 0U);
               // h at buffer start is a harmless no-op (clamped, not
               // wrapped or misinterpreted).
               const auto result = f.send('h');
               CHECK_EQ(f.cursor, 0U);
               CHECK(result.changed); // cursor "changed" to the same value;
                                      // redraw is idempotent either way.
             });
}

void test_jk_move_by_logical_line() {
  tests::run("VimEngine: j/k move the cursor by logical line, preserving "
             "column",
             [] {
               Fixture f("hello\nhi\nworld", 2); // cursor on the second 'l'
               f.send('j');
               // Line 2 ("hi") is shorter than column 2 -- clamps to its
               // end (offset 8, right after "hi").
               CHECK_EQ(f.cursor, 8U);
               f.send('j');
               // Line 3 ("world") has room for column 2 again.
               CHECK_EQ(f.cursor, 11U);
               f.send('k');
               CHECK_EQ(f.cursor, 8U);
             });
}

void test_0_and_dollar() {
  tests::run("VimEngine: 0/$ move to the start/end of the current line", [] {
    Fixture f("hello\nworld", 8); // cursor mid "world"
    f.send('0');
    CHECK_EQ(f.cursor, 6U); // start of "world"
    f.send('$');
    CHECK_EQ(f.cursor, 11U); // end of buffer (last line, no trailing '\n')
  });
}

void test_w_start_of_next_word() {
  tests::run("VimEngine: w moves to the start of the next word", [] {
    Fixture f("hello world foo", 0);
    f.send('w');
    CHECK_EQ(f.cursor, 6U); // 'w' of "world"
    f.send('w');
    CHECK_EQ(f.cursor, 12U); // 'f' of "foo"
    f.send('w');
    // No further word: lands at end of buffer.
    CHECK_EQ(f.cursor, 15U);
  });
}

void test_e_end_of_word_distinct_from_w() {
  tests::run(
      "VimEngine: e moves to the end of the current/next word, landing ON "
      "its last character -- distinct from w's start-of-next-word target",
      [] {
        Fixture f("hello world foo", 0);
        const auto w_target = [] {
          Fixture g("hello world foo", 0);
          g.send('w');
          return g.cursor;
        }();
        f.send('e');
        // e lands ON the 'o' ending "hello" (index 4); w lands at "world"'s
        // start (index 6) -- the two must not coincide, or this milestone
        // got the documented gotcha wrong.
        CHECK_EQ(f.cursor, 4U);
        CHECK(f.cursor != w_target);

        // Repeated e from the last character of the current word jumps to
        // the end of the *next* word instead of just one column further --
        // the classic "already on the last char" case.
        Fixture mid("hello world foo", 4); // already on the final 'o'
        mid.send('e');
        CHECK_EQ(mid.cursor, 10U); // 'd' ending "world"
      });
}

void test_b_start_of_previous_word() {
  tests::run("VimEngine: b moves to the start of the previous word", [] {
    Fixture f("hello world foo", 12); // on 'f' of "foo"
    f.send('b');
    CHECK_EQ(f.cursor, 6U); // start of "world"
    f.send('b');
    CHECK_EQ(f.cursor, 0U); // start of "hello"
  });
}

void test_uncovered_key_is_harmless_noop() {
  tests::run(
      "VimEngine: an uncovered Normal-mode key (e.g. 'x', not implemented "
      "in this milestone's scope) is a no-op, not eaten or misinterpreted",
      [] {
        Fixture f("hello", 2);
        const auto result = f.send('x');
        CHECK(!result.changed);
        CHECK_EQ(f.buf, "hello");
        CHECK_EQ(f.cursor, 2U);
        // A digit (no count-prefix support in this milestone's scope) is
        // likewise a no-op rather than starting some half-implemented
        // count.
        const auto digit_result = f.send('3');
        CHECK(!digit_result.changed);
        CHECK_EQ(f.buf, "hello");
      });
}

void test_dw_deletes_to_start_of_next_word() {
  tests::run("VimEngine: dw deletes from cursor to the start of the next "
             "word",
             [] {
               Fixture f("hello world foo", 0);
               f.send('d');
               const auto result = f.send('w');
               CHECK(result.changed);
               CHECK_EQ(f.buf, "world foo");
               CHECK_EQ(f.cursor, 0U);
               CHECK_EQ(f.kill_buffer, "hello ");
               // d/c stays in Normal mode.
               CHECK(f.engine.mode() == VimMode::Normal);
             });
}

void test_dl_deletes_char_under_cursor() {
  tests::run("VimEngine: dl deletes exactly the character under the cursor",
             [] {
               Fixture f("hello", 0);
               f.send('d');
               f.send('l');
               CHECK_EQ(f.buf, "ello");
               CHECK_EQ(f.cursor, 0U);
               CHECK_EQ(f.kill_buffer, "h");
             });
}

void test_dh_deletes_char_before_cursor() {
  tests::run(
      "VimEngine: dh deletes exactly the character before the cursor "
      "(like Backspace)",
      [] {
        Fixture f("hello world", 6); // cursor on 'w'
        f.send('d');
        f.send('h');
        CHECK_EQ(f.buf, "helloworld");
        CHECK_EQ(f.cursor, 5U);
        CHECK_EQ(f.kill_buffer, " ");
      });
}

void test_d0_deletes_to_line_start() {
  tests::run("VimEngine: d0 deletes from the line start up to (not "
             "including) the cursor",
             [] {
               Fixture f("hello world", 5); // cursor right after "hello"
               f.send('d');
               f.send('0');
               CHECK_EQ(f.buf, " world");
               CHECK_EQ(f.cursor, 0U);
               CHECK_EQ(f.kill_buffer, "hello");
             });
}

void test_ddollar_deletes_to_line_end() {
  tests::run("VimEngine: d$ deletes from the cursor to the end of the "
             "line, leaving the newline (if any) intact",
             [] {
               Fixture f("hello world\nsecond", 6); // on 'w' of "world"
               f.send('d');
               f.send('$');
               CHECK_EQ(f.buf, "hello \nsecond");
               CHECK_EQ(f.cursor, 6U);
               CHECK_EQ(f.kill_buffer, "world");
             });
}

void test_de_deletes_to_end_of_word() {
  tests::run("VimEngine: de deletes from cursor to the end of the current "
             "word",
             [] {
               Fixture f("hello world", 0);
               f.send('d');
               f.send('e');
               CHECK_EQ(f.buf, " world");
               CHECK_EQ(f.cursor, 0U);
               CHECK_EQ(f.kill_buffer, "hello");
             });
}

void test_db_deletes_to_start_of_previous_word() {
  tests::run("VimEngine: db deletes from the start of the previous word up "
             "to the cursor",
             [] {
               Fixture f("hello world", 11); // end of buffer
               f.send('d');
               f.send('b');
               CHECK_EQ(f.buf, "hello ");
               CHECK_EQ(f.cursor, 6U);
               CHECK_EQ(f.kill_buffer, "world");
             });
}

void test_operator_pending_invalid_motion_cancels_cleanly() {
  tests::run(
      "VimEngine: an operator followed by an unrecognized key cancels the "
      "pending operator without applying it or getting stuck",
      [] {
        Fixture f("hello world", 0);
        f.send('d');
        const auto cancel_result = f.send('x'); // not a motion
        CHECK(!cancel_result.changed);
        CHECK_EQ(f.buf, "hello world"); // nothing deleted
        // The pending operator must be fully cleared: a plain motion right
        // afterward moves the cursor instead of being swallowed as if it
        // were still completing "d".
        const auto motion_result = f.send('w');
        CHECK(motion_result.changed);
        CHECK_EQ(f.buf, "hello world"); // still just a motion, no deletion
        CHECK_EQ(f.cursor, 6U);
      });
}

void test_escape_cancels_pending_operator() {
  tests::run("VimEngine: cancel_pending() clears an in-progress operator "
             "(what readline.cpp calls on every Escape byte)",
             [] {
               Fixture f("hello world", 0);
               f.send('d');
               f.engine.cancel_pending();
               f.send('w');
               CHECK_EQ(f.buf, "hello world"); // no deletion: 'w' was a
                                               // plain motion, not part of
                                               // a stale "d"
               CHECK_EQ(f.cursor, 6U);
             });
}

void test_c_operator_deletes_and_enters_insert() {
  tests::run(
      "VimEngine: c<motion> deletes the span and drops into Insert mode",
      [] {
        Fixture f("hello world", 0);
        f.send('c');
        const auto result = f.send('w');
        CHECK(result.changed);
        CHECK_EQ(f.buf, "world");
        CHECK_EQ(f.cursor, 0U);
        CHECK(f.engine.mode() == VimMode::Insert);
      });
}

void test_dd_deletes_whole_line_and_joins() {
  tests::run("VimEngine: dd deletes the whole current line, including one "
             "adjacent newline, and lands on the line that takes its place",
             [] {
               Fixture f("aa\nbb\ncc", 4); // on the second 'b'
               f.send('d');
               const auto result = f.send('d');
               CHECK(result.changed);
               CHECK_EQ(f.buf, "aa\ncc");
               CHECK_EQ(f.cursor, 3U); // start of "cc"
               CHECK_EQ(f.kill_buffer, "bb\n");
               CHECK(f.engine.mode() == VimMode::Normal);
             });
}

void test_dd_on_last_line_absorbs_previous_newline() {
  tests::run(
      "VimEngine: dd on the last line (no trailing newline of its own) "
      "removes the previous line's newline instead, leaving no blank line",
      [] {
        Fixture f("aa\nbb\ncc", 6); // on the first 'c'
        f.send('d');
        f.send('d');
        CHECK_EQ(f.buf, "aa\nbb");
        CHECK_EQ(f.cursor, 3U); // start of the new last line, "bb"
      });
}

void test_dd_on_only_line_empties_buffer() {
  tests::run("VimEngine: dd on a single-line buffer empties it", [] {
    Fixture f("hello", 2);
    f.send('d');
    f.send('d');
    CHECK(f.buf.empty());
    CHECK_EQ(f.cursor, 0U);
  });
}

void test_cc_keeps_line_structure_and_enters_insert() {
  tests::run(
      "VimEngine: cc clears the current line's content but keeps the line "
      "itself (newlines untouched), landing in Insert mode at its start",
      [] {
        Fixture f("aa\nbb\ncc", 4); // on the second 'b'
        f.send('c');
        const auto result = f.send('c');
        CHECK(result.changed);
        CHECK_EQ(f.buf, "aa\n\ncc"); // "bb" removed, both newlines remain
        CHECK_EQ(f.cursor, 3U);
        CHECK_EQ(f.kill_buffer, "bb");
        CHECK(f.engine.mode() == VimMode::Insert);
      });
}

void test_i_a_A_I_enter_insert_mode() {
  tests::run("VimEngine: i/a/A/I all drop into Insert mode, positioning the "
             "cursor per their own convention",
             [] {
               {
                 Fixture f("abc", 1);
                 f.send('i'); // insert before cursor: no cursor change
                 CHECK_EQ(f.cursor, 1U);
                 CHECK(f.engine.mode() == VimMode::Insert);
               }
               {
                 Fixture f("abc", 1);
                 f.send('a'); // insert after cursor
                 CHECK_EQ(f.cursor, 2U);
                 CHECK(f.engine.mode() == VimMode::Insert);
               }
               {
                 Fixture f("hello\nworld", 8); // mid second line
                 f.send('A'); // insert at line end
                 CHECK_EQ(f.cursor, 11U);
                 CHECK(f.engine.mode() == VimMode::Insert);
               }
               {
                 Fixture f("hello\nworld", 8);
                 f.send('I'); // insert at line start
                 CHECK_EQ(f.cursor, 6U);
                 CHECK(f.engine.mode() == VimMode::Insert);
               }
             });
}

void test_starts_in_insert_mode_by_default() {
  tests::run("VimEngine: a fresh engine starts in Insert mode (empty "
             "composer behaves exactly like today until the user presses "
             "Escape)",
             [] {
               VimEngine engine;
               CHECK(engine.mode() == VimMode::Insert);
             });
}

int main() {
  test_starts_in_insert_mode_by_default();
  test_hl_move_by_codepoint();
  test_jk_move_by_logical_line();
  test_0_and_dollar();
  test_w_start_of_next_word();
  test_e_end_of_word_distinct_from_w();
  test_b_start_of_previous_word();
  test_uncovered_key_is_harmless_noop();
  test_dw_deletes_to_start_of_next_word();
  test_dl_deletes_char_under_cursor();
  test_dh_deletes_char_before_cursor();
  test_d0_deletes_to_line_start();
  test_ddollar_deletes_to_line_end();
  test_de_deletes_to_end_of_word();
  test_db_deletes_to_start_of_previous_word();
  test_operator_pending_invalid_motion_cancels_cleanly();
  test_escape_cancels_pending_operator();
  test_c_operator_deletes_and_enters_insert();
  test_dd_deletes_whole_line_and_joins();
  test_dd_on_last_line_absorbs_previous_newline();
  test_dd_on_only_line_empties_buffer();
  test_cc_keeps_line_structure_and_enters_insert();
  test_i_a_A_I_enter_insert_mode();
  std::cout << "\nTests: " << (tests::passed + tests::failed) << " total, "
            << tests::passed << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
