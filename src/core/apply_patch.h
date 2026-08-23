#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pi::core {

struct UpdateFileChunk {
  std::optional<std::string> change_context;
  std::vector<std::string> old_lines;
  std::vector<std::string> new_lines;
  bool anchored_to_eof{false};
};

struct AddFile {
  std::string path;
  std::vector<std::string> lines;
};
struct DeleteFile {
  std::string path;
};
struct UpdateFile {
  std::string path;
  std::optional<std::string> move_path;
  std::vector<UpdateFileChunk> chunks;
};
using Hunk = std::variant<AddFile, DeleteFile, UpdateFile>;
struct Patch {
  std::vector<Hunk> hunks;
};
struct ParseDiagnostic {
  std::size_t line{};
  std::string message;
};

std::optional<Patch> parse_patch(std::string_view text, bool lenient,
                                 ParseDiagnostic &error);

enum class LineEndings { normalize_lf, preserve };
struct ApplyPatchOptions {
  bool create_parents{true};
  LineEndings line_endings{LineEndings::normalize_lf};
};
struct AppliedPatch {
  std::vector<std::string> added, updated, deleted, moved;
  std::string unified_diff;
};

AppliedPatch apply_patch(const std::filesystem::path &cwd, const Patch &patch,
                         const ApplyPatchOptions &options = {});

} // namespace pi::core
