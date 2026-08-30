// Unit tests for the vim-mode Normal-mode key layer (src/cli/vim_mode.{h,
// cpp}) -- pure, terminal-I/O-free tests of VimEngine::handle_normal_key
// operating directly on an in-memory buffer/cursor, mirroring how
// test_wrap.cpp exercises plan_word_wrap without a pty. See
// test/test_readline.cpp's vim-mode tests for the end-to-end equivalent
// (Escape entering Normal mode, i/a returning to Insert, vim_mode=false
// leaving M0-M4 behavior unchanged) against a real terminal.

#include "cli/vim_mode.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

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

TEST(VimMode, HlMoveByCodepoint) {

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
  EXPECT_THAT(result.changed,
              testing::IsTrue()); // cursor "changed" to the same value;
                                  // redraw is idempotent either way.
}

TEST(VimMode, JkMoveByLogicalLine) {

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
}

TEST(VimMode, ZeroAndDollar) {

  Fixture f("hello\nworld", 8); // cursor mid "world"
  f.send('0');
  EXPECT_EQ(f.cursor, 6U); // start of "world"
  f.send('$');
  EXPECT_EQ(f.cursor, 11U); // end of buffer (last line, no trailing '\n')
}

TEST(VimMode, WStartOfNextWord) {

  Fixture f("hello world foo", 0);
  f.send('w');
  EXPECT_EQ(f.cursor, 6U); // 'w' of "world"
  f.send('w');
  EXPECT_EQ(f.cursor, 12U); // 'f' of "foo"
  f.send('w');
  // No further word: lands at end of buffer.
  EXPECT_EQ(f.cursor, 15U);
}

TEST(VimMode, EEndOfWordDistinctFromW) {

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
  EXPECT_NE(f.cursor, w_target);

  // Repeated e from the last character of the current word jumps to
  // the end of the *next* word instead of just one column further --
  // the classic "already on the last char" case.
  Fixture mid("hello world foo", 4); // already on the final 'o'
  mid.send('e');
  EXPECT_EQ(mid.cursor, 10U); // 'd' ending "world"
}

TEST(VimMode, BStartOfPreviousWord) {

  Fixture f("hello world foo", 12); // on 'f' of "foo"
  f.send('b');
  EXPECT_EQ(f.cursor, 6U); // start of "world"
  f.send('b');
  EXPECT_EQ(f.cursor, 0U); // start of "hello"
}

TEST(VimMode, UncoveredKeyIsHarmlessNoop) {

  Fixture f("hello", 2);
  const auto result = f.send('x');
  EXPECT_THAT(result.changed, testing::IsFalse());
  EXPECT_EQ(f.buf, "hello");
  EXPECT_EQ(f.cursor, 2U);
  // A digit (no count-prefix support in this milestone's scope) is
  // likewise a no-op rather than starting some half-implemented
  // count.
  const auto digit_result = f.send('3');
  EXPECT_THAT(digit_result.changed, testing::IsFalse());
  EXPECT_EQ(f.buf, "hello");
}

TEST(VimMode, DwDeletesToStartOfNextWord) {

  Fixture f("hello world foo", 0);
  f.send('d');
  const auto result = f.send('w');
  EXPECT_THAT(result.changed, testing::IsTrue());
  EXPECT_EQ(f.buf, "world foo");
  EXPECT_EQ(f.cursor, 0U);
  EXPECT_EQ(f.kill_buffer, "hello ");
  // d/c stays in Normal mode.
  EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Normal));
}

TEST(VimMode, DlDeletesCharUnderCursor) {

  Fixture f("hello", 0);
  f.send('d');
  f.send('l');
  EXPECT_EQ(f.buf, "ello");
  EXPECT_EQ(f.cursor, 0U);
  EXPECT_EQ(f.kill_buffer, "h");
}

TEST(VimMode, DhDeletesCharBeforeCursor) {

  Fixture f("hello world", 6); // cursor on 'w'
  f.send('d');
  f.send('h');
  EXPECT_EQ(f.buf, "helloworld");
  EXPECT_EQ(f.cursor, 5U);
  EXPECT_EQ(f.kill_buffer, " ");
}

TEST(VimMode, DzeroDeletesToLineStart) {

  Fixture f("hello world", 5); // cursor right after "hello"
  f.send('d');
  f.send('0');
  EXPECT_EQ(f.buf, " world");
  EXPECT_EQ(f.cursor, 0U);
  EXPECT_EQ(f.kill_buffer, "hello");
}

TEST(VimMode, DdollarDeletesToLineEnd) {

  Fixture f("hello world\nsecond", 6); // on 'w' of "world"
  f.send('d');
  f.send('$');
  EXPECT_EQ(f.buf, "hello \nsecond");
  EXPECT_EQ(f.cursor, 6U);
  EXPECT_EQ(f.kill_buffer, "world");
}

TEST(VimMode, DeDeletesToEndOfWord) {

  Fixture f("hello world", 0);
  f.send('d');
  f.send('e');
  EXPECT_EQ(f.buf, " world");
  EXPECT_EQ(f.cursor, 0U);
  EXPECT_EQ(f.kill_buffer, "hello");
}

TEST(VimMode, DbDeletesToStartOfPreviousWord) {

  Fixture f("hello world", 11); // end of buffer
  f.send('d');
  f.send('b');
  EXPECT_EQ(f.buf, "hello ");
  EXPECT_EQ(f.cursor, 6U);
  EXPECT_EQ(f.kill_buffer, "world");
}

TEST(VimMode, InvalidMotionCancelsPendingOperator) {

  Fixture f("hello world", 0);
  f.send('d');
  const auto cancel_result = f.send('x'); // not a motion
  EXPECT_THAT(cancel_result.changed, testing::IsFalse());
  EXPECT_EQ(f.buf, "hello world"); // nothing deleted
  // The pending operator must be fully cleared: a plain motion right
  // afterward moves the cursor instead of being swallowed as if it
  // were still completing "d".
  const auto motion_result = f.send('w');
  EXPECT_THAT(motion_result.changed, testing::IsTrue());
  EXPECT_EQ(f.buf,
            "hello world"); // still just a motion, no deletion
  EXPECT_EQ(f.cursor, 6U);
}

TEST(VimMode, EscapeCancelsPendingOperator) {

  Fixture f("hello world", 0);
  f.send('d');
  f.engine.cancel_pending();
  f.send('w');
  EXPECT_EQ(f.buf, "hello world"); // no deletion: 'w' was a
                                   // plain motion, not part of
                                   // a stale "d"
  EXPECT_EQ(f.cursor, 6U);
}

TEST(VimMode, COperatorDeletesAndEntersInsert) {

  Fixture f("hello world", 0);
  f.send('c');
  const auto result = f.send('w');
  EXPECT_THAT(result.changed, testing::IsTrue());
  EXPECT_EQ(f.buf, "world");
  EXPECT_EQ(f.cursor, 0U);
  EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Insert));
}

TEST(VimMode, DdDeletesWholeLineAndJoins) {

  Fixture f("aa\nbb\ncc", 4); // on the second 'b'
  f.send('d');
  const auto result = f.send('d');
  EXPECT_THAT(result.changed, testing::IsTrue());
  EXPECT_EQ(f.buf, "aa\ncc");
  EXPECT_EQ(f.cursor, 3U); // start of "cc"
  EXPECT_EQ(f.kill_buffer, "bb\n");
  EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Normal));
}

TEST(VimMode, DdOnLastLineAbsorbsPreviousNewline) {

  Fixture f("aa\nbb\ncc", 6); // on the first 'c'
  f.send('d');
  f.send('d');
  EXPECT_EQ(f.buf, "aa\nbb");
  EXPECT_EQ(f.cursor, 3U); // start of the new last line, "bb"
}

TEST(VimMode, DdOnOnlyLineEmptiesBuffer) {

  Fixture f("hello", 2);
  f.send('d');
  f.send('d');
  EXPECT_THAT(f.buf, testing::IsEmpty());
  EXPECT_EQ(f.cursor, 0U);
}

TEST(VimMode, CcKeepsLineStructureAndEntersInsert) {

  Fixture f("aa\nbb\ncc", 4); // on the second 'b'
  f.send('c');
  const auto result = f.send('c');
  EXPECT_THAT(result.changed, testing::IsTrue());
  EXPECT_EQ(f.buf, "aa\n\ncc"); // "bb" removed, both newlines remain
  EXPECT_EQ(f.cursor, 3U);
  EXPECT_EQ(f.kill_buffer, "bb");
  EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Insert));
}

TEST(VimMode, InsertCommandsEnterInsertMode) {

  {
    Fixture f("abc", 1);
    f.send('i'); // insert before cursor: no cursor change
    EXPECT_EQ(f.cursor, 1U);
    EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Insert));
  }
  {
    Fixture f("abc", 1);
    f.send('a'); // insert after cursor
    EXPECT_EQ(f.cursor, 2U);
    EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Insert));
  }
  {
    Fixture f("hello\nworld", 8); // mid second line
    f.send('A');                  // insert at line end
    EXPECT_EQ(f.cursor, 11U);
    EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Insert));
  }
  {
    Fixture f("hello\nworld", 8);
    f.send('I'); // insert at line start
    EXPECT_EQ(f.cursor, 6U);
    EXPECT_THAT(f.engine.mode(), testing::Eq(VimMode::Insert));
  }
}

TEST(VimMode, StartsInInsertModeByDefault) {

  VimEngine engine;
  EXPECT_THAT(engine.mode(), testing::Eq(VimMode::Insert));
}
