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

// Every path a patch touches: AddFile::path, DeleteFile::path,
// UpdateFile::path, and UpdateFile::move_path when present -- exactly the
// set apply_patch()'s own internal `get()` lambda visits. Callers that
// need to take a file-mutation lock before applying a patch (see
// builtin_tools.cpp's ApplyPatchTool::execute) should enumerate paths via
// this function rather than re-deriving the visitation rule ad hoc, so a
// future hunk type can't silently drift out of sync between the two.
std::vector<std::string> patch_paths(const Patch &patch);

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
