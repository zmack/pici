#include "cli/model_picker_settings.h"

#include "core/models.h"
#include "support/gtest_helpers.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

using pi::test::ScopedEnvironmentVariable;
using pi::test::TemporaryDirectory;

namespace {

std::string settings_path_for(const TemporaryDirectory &dir) {
  return (dir.path() / "picker_settings.json").string();
}

} // namespace

TEST(ModelPickerSettings, MissingFileReturnsNullopt) {
  TemporaryDirectory dir{"pici-picker-settings"};
  ScopedEnvironmentVariable env("PICI_PICKER_SETTINGS_FILE",
                                settings_path_for(dir));
  EXPECT_FALSE(pi::cli::load_default_model().has_value());
}

TEST(ModelPickerSettings, CorruptFileReturnsNullopt) {
  TemporaryDirectory dir{"pici-picker-settings"};
  const auto path = settings_path_for(dir);
  ScopedEnvironmentVariable env("PICI_PICKER_SETTINGS_FILE", path);
  {
    std::ofstream out(path);
    out << "not json";
  }
  EXPECT_FALSE(pi::cli::load_default_model().has_value());
}

TEST(ModelPickerSettings, SaveThenLoadRoundTrips) {
  TemporaryDirectory dir{"pici-picker-settings"};
  ScopedEnvironmentVariable env("PICI_PICKER_SETTINGS_FILE",
                                settings_path_for(dir));
  const pi::core::ModelKey key{.provider_id = "openai-codex",
                              .model_id = "gpt-5.4"};
  pi::cli::save_default_model(key);
  const auto loaded = pi::cli::load_default_model();
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->provider_id, "openai-codex");
  EXPECT_EQ(loaded->model_id, "gpt-5.4");
}

TEST(ModelPickerSettings, SaveCreatesParentDirectory) {
  TemporaryDirectory dir{"pici-picker-settings"};
  const auto nested = (dir.path() / "nested" / "picker_settings.json").string();
  ScopedEnvironmentVariable env("PICI_PICKER_SETTINGS_FILE", nested);
  pi::cli::save_default_model(
      {.provider_id = "openai", .model_id = "gpt-4o"});
  EXPECT_TRUE(std::filesystem::exists(nested));
}
