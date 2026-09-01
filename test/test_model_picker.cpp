// Covers the pure functions ModelPicker's terminal loop drives --
// reduce_model_picker_cursor() (navigation), decode_plain_byte() (search-box
// text/Ctrl+S decoding), fuzzy_match() (search filtering), and
// sort_entries_for_display() (current/default-first ordering) -- see
// cli/model_picker.h's comments on why these are split out (testable
// without a TTY). run_model_picker() itself is not exercised here: it
// requires a real terminal (RawMode::enter() safely no-ops to a cancelled
// result otherwise, which is its own tested contract, not a picker behavior
// to characterize further).

#include "cli/model_picker.h"

#include "core/models.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <vector>

using pi::cli::decode_plain_byte;
using pi::cli::fuzzy_match;
using pi::cli::ModelPickerKey;
using pi::cli::reduce_model_picker_cursor;
using pi::cli::sort_entries_for_display;
using pi::core::ModelCatalogEntry;
using pi::core::ModelKey;

TEST(ModelPicker, MovesUpAndDownWithinBounds) {
  EXPECT_EQ(reduce_model_picker_cursor(1, ModelPickerKey::up, 3),
            std::size_t{0});
  EXPECT_EQ(reduce_model_picker_cursor(1, ModelPickerKey::down, 3),
            std::size_t{2});
}

TEST(ModelPicker, WrapsAtBothEnds) {
  EXPECT_EQ(reduce_model_picker_cursor(0, ModelPickerKey::up, 3),
            std::size_t{2});
  EXPECT_EQ(reduce_model_picker_cursor(2, ModelPickerKey::down, 3),
            std::size_t{0});
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

TEST(ModelPickerKeyDecode, EnterAndControlCharsDecode) {
  EXPECT_EQ(decode_plain_byte('\r').nav, ModelPickerKey::enter);
  EXPECT_EQ(decode_plain_byte('\n').nav, ModelPickerKey::enter);
  EXPECT_TRUE(decode_plain_byte(0x7f).is_backspace);
  EXPECT_TRUE(decode_plain_byte(0x08).is_backspace);
  EXPECT_TRUE(decode_plain_byte(0x13).is_ctrl_s);
}

TEST(ModelPickerKeyDecode, PrintableAsciiBecomesTextChar) {
  const auto event = decode_plain_byte('g');
  EXPECT_EQ(event.text_char, 'g');
  EXPECT_FALSE(event.is_backspace);
  EXPECT_FALSE(event.is_ctrl_s);
}

TEST(ModelPickerKeyDecode, LettersOnceReservedForVimNavAreNowSearchable) {
  // Search-box text input needs every printable character, including 'j'/
  // 'k'/'q' -- real model IDs contain them (e.g. "kimi-k2", "grok"). Only
  // real arrow keys navigate now; see model_picker.cpp's read_key_or_timeout.
  EXPECT_EQ(decode_plain_byte('j').text_char, 'j');
  EXPECT_EQ(decode_plain_byte('k').text_char, 'k');
  EXPECT_EQ(decode_plain_byte('q').text_char, 'q');
}

TEST(ModelPickerKeyDecode, OtherControlBytesAreNoOps) {
  const auto event = decode_plain_byte(0x01);
  EXPECT_EQ(event.nav, ModelPickerKey::other);
  EXPECT_EQ(event.text_char, 0);
  EXPECT_FALSE(event.is_backspace);
  EXPECT_FALSE(event.is_ctrl_s);
}

TEST(ModelPickerFuzzyMatch, EmptyQueryMatchesEverything) {
  EXPECT_TRUE(fuzzy_match("openai/gpt-4o", ""));
}

TEST(ModelPickerFuzzyMatch, SubsequenceMatchesCaseInsensitively) {
  EXPECT_TRUE(fuzzy_match("openai-codex/gpt-5.4", "oc54"));
  EXPECT_TRUE(fuzzy_match("openai-codex/gpt-5.4", "GPT54"));
}

TEST(ModelPickerFuzzyMatch, OutOfOrderCharactersDoNotMatch) {
  EXPECT_FALSE(fuzzy_match("groq/llama-3.3-70b", "b70"));
}

TEST(ModelPickerFuzzyMatch, MissingCharacterDoesNotMatch) {
  EXPECT_FALSE(fuzzy_match("openai/gpt-4o", "xyz"));
}

namespace {

ModelCatalogEntry make_entry(std::string provider,
                                       std::string model) {
  ModelCatalogEntry entry;
  entry.key = {.provider_id = std::move(provider), .model_id = std::move(model)};
  return entry;
}

} // namespace

TEST(ModelPickerSort, CurrentModelSortsFirst) {
  std::vector<ModelCatalogEntry> entries{
      make_entry("openai", "gpt-4o"),
      make_entry("openai-codex", "gpt-5.4"),
      make_entry("groq", "llama-3.3-70b"),
  };
  const ModelKey current{.provider_id = "groq",
                              .model_id = "llama-3.3-70b"};
  const auto sorted =
      sort_entries_for_display(entries, current, std::nullopt);
  ASSERT_EQ(sorted.size(), std::size_t{3});
  EXPECT_EQ(sorted[0].key, current);
}

TEST(ModelPickerSort, DefaultModelSortsSecondWhenNotCurrent) {
  std::vector<ModelCatalogEntry> entries{
      make_entry("openai", "gpt-4o"),
      make_entry("openai-codex", "gpt-5.4"),
      make_entry("groq", "llama-3.3-70b"),
  };
  const ModelKey current{.provider_id = "groq",
                              .model_id = "llama-3.3-70b"};
  const ModelKey default_model{.provider_id = "openai-codex",
                                    .model_id = "gpt-5.4"};
  const auto sorted =
      sort_entries_for_display(entries, current, default_model);
  ASSERT_EQ(sorted.size(), std::size_t{3});
  EXPECT_EQ(sorted[0].key, current);
  EXPECT_EQ(sorted[1].key, default_model);
}

TEST(ModelPickerSort, PreservesRelativeOrderOtherwise) {
  std::vector<ModelCatalogEntry> entries{
      make_entry("openai", "gpt-4o"),
      make_entry("groq", "llama-3.3-70b"),
  };
  const ModelKey current{.provider_id = "none", .model_id = "none"};
  const auto sorted =
      sort_entries_for_display(entries, current, std::nullopt);
  ASSERT_EQ(sorted.size(), std::size_t{2});
  EXPECT_EQ(sorted[0].key, entries[0].key);
  EXPECT_EQ(sorted[1].key, entries[1].key);
}
