// Covers reduce_model_picker_cursor(), the pure navigation state machine
// ModelPicker's terminal loop drives -- see cli/model_picker.h's comment on
// why this is split out (testable without a TTY). run_model_picker() itself
// is not exercised here: it requires a real terminal (RawMode::enter()
// safely no-ops to a cancelled result otherwise, which is its own tested
// contract, not a picker behavior to characterize further).

#include "cli/model_picker.h"

#include <gtest/gtest.h>

#include <cstddef>

using pi::cli::ModelPickerKey;
using pi::cli::reduce_model_picker_cursor;

TEST(ModelPicker, MovesUpAndDownWithinBounds) {
  EXPECT_EQ(reduce_model_picker_cursor(1, ModelPickerKey::up, 3),
            std::size_t{0});
  EXPECT_EQ(reduce_model_picker_cursor(1, ModelPickerKey::down, 3),
            std::size_t{2});
}

TEST(ModelPicker, ClampsAtBothEnds) {
  EXPECT_EQ(reduce_model_picker_cursor(0, ModelPickerKey::up, 3),
            std::size_t{0});
  EXPECT_EQ(reduce_model_picker_cursor(2, ModelPickerKey::down, 3),
            std::size_t{2});
}

TEST(ModelPicker, EnterEscapeAndOtherLeaveCursorUnchanged) {
  EXPECT_EQ(reduce_model_picker_cursor(1, ModelPickerKey::enter, 3),
            std::size_t{1});
  EXPECT_EQ(reduce_model_picker_cursor(1, ModelPickerKey::escape, 3),
            std::size_t{1});
  EXPECT_EQ(reduce_model_picker_cursor(1, ModelPickerKey::other, 3),
            std::size_t{1});
}

TEST(ModelPicker, EmptyCandidateListNeverMoves) {
  EXPECT_EQ(reduce_model_picker_cursor(0, ModelPickerKey::down, 0),
            std::size_t{0});
  EXPECT_EQ(reduce_model_picker_cursor(0, ModelPickerKey::up, 0),
            std::size_t{0});
}
