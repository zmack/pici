#include "cli/model_picker_settings.h"
#include "nlohmann/json_fwd.hpp"

#include "core/models.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace pi::cli {

namespace {

using json = nlohmann::json;

} // namespace

std::filesystem::path default_picker_settings_path() {
  // getenv() is only called here during startup/on-demand path resolution;
  // no worker thread concurrently calls setenv()/putenv() in this process.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char *override_path = std::getenv("PICI_PICKER_SETTINGS_FILE");
      override_path != nullptr && *override_path != '\0') {
    return override_path;
  }
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME");
      xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "pici" / "picker_settings.json";
  }
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char *home = std::getenv("HOME");
      home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".config" / "pici" /
           "picker_settings.json";
  }
  throw std::runtime_error("cannot determine picker settings file path; set "
                           "PICI_PICKER_SETTINGS_FILE");
}

std::optional<core::ModelKey> load_default_model() {
  std::filesystem::path path;
  try {
    path = default_picker_settings_path();
  } catch (const std::runtime_error &) {
    return std::nullopt;
  }
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  json doc;
  try {
    in >> doc;
  } catch (const json::exception &) {
    return std::nullopt;
  }
  const auto it = doc.find("default_model");
  if (it == doc.end() || !it->is_object())
    return std::nullopt;
  const auto provider = it->find("provider");
  const auto model_id = it->find("model_id");
  if (provider == it->end() || !provider->is_string() ||
      model_id == it->end() || !model_id->is_string())
    return std::nullopt;
  return core::ModelKey{.provider_id = provider->get<std::string>(),
                        .model_id = model_id->get<std::string>()};
}

void save_default_model(const core::ModelKey &key) {
  const auto path = default_picker_settings_path();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  json doc;
  doc["default_model"] = {{"provider", key.provider_id},
                          {"model_id", key.model_id}};
  std::ofstream out(path, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot write picker settings file: " +
                             path.string());
  out << doc.dump(2) << "\n";
}

} // namespace pi::cli
