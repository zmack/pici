#include "core/apply_patch.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {
namespace {
std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> out;
  std::string input(text);
  std::istringstream stream(input);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    out.push_back(std::move(line));
  }
  if (!text.empty() && text.back() == '\n' && !out.empty() &&
      out.back().empty())
    out.pop_back();
  return out;
}
std::string trim(const std::string &s) {
  const auto first = s.find_first_not_of(" \t");
  if (first == std::string::npos)
    return {};
  const auto last = s.find_last_not_of(" \t");
  return s.substr(first, last - first + 1);
}
std::string rstrip(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  return s;
}
std::string normalized(std::string s) {
  s = trim(s);
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\xE2' && i + 2 < s.size()) { // common UTF-8 punctuation
      const auto b = static_cast<unsigned char>(s[i + 1]);
      const auto c = static_cast<unsigned char>(s[i + 2]);
      if (b == 0x80 && (c == 0x98 || c == 0x99)) {
        out += '\'';
        i += 2;
        continue;
      }
      if (b == 0x80 && (c == 0x9C || c == 0x9D)) {
        out += '"';
        i += 2;
        continue;
      }
      if (b == 0x80 && (c == 0x93 || c == 0x94)) {
        out += '-';
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}
std::string marker_path(std::string_view line, std::string_view marker) {
  return trim(std::string(line.substr(marker.size())));
}
void fail(ParseDiagnostic &e, std::size_t line, std::string msg) {
  e.line = line;
  e.message = std::move(msg);
}

bool seek(const std::vector<std::string> &lines,
          const std::vector<std::string> &pattern, std::size_t start,
          std::size_t &found) {
  if (pattern.empty()) {
    found = std::min(start, lines.size());
    return true;
  }
  if (pattern.size() > lines.size())
    return false;
  for (int rung = 0; rung != 4; ++rung) {
    for (std::size_t i = start; i + pattern.size() <= lines.size(); ++i) {
      bool ok = true;
      for (std::size_t j = 0; j < pattern.size(); ++j) {
        auto a = lines[i + j];
        auto b = pattern[j];
        if (rung == 1) {
          a = rstrip(std::move(a));
          b = rstrip(std::move(b));
        } else if (rung == 2) {
          a = trim(a);
          b = trim(b);
        } else if (rung == 3) {
          a = normalized(std::move(a));
          b = normalized(std::move(b));
        }
        if (a != b) {
          ok = false;
          break;
        }
      }
      if (ok) {
        found = i;
        return true;
      }
    }
  }
  return false;
}
std::filesystem::path safe_path(const std::filesystem::path &cwd,
                                const std::string &name) {
  auto root = std::filesystem::weakly_canonical(cwd);
  auto p = std::filesystem::path(name);
  if (p.is_absolute())
    throw std::runtime_error("Path is outside the workspace: " + name);
  p = (cwd / p).lexically_normal();
  auto parent = p.parent_path();
  std::error_code ec;
  auto canon_parent = std::filesystem::weakly_canonical(parent, ec);
  if (ec)
    canon_parent = parent.lexically_normal();
  auto rel = std::filesystem::relative(canon_parent, root, ec);
  if (ec || rel.native().starts_with(".."))
    throw std::runtime_error("Path is outside the workspace: " + name);
  return p;
}
std::string read_file(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    throw std::runtime_error("Cannot read file: " + p.string());
  return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace

std::optional<Patch> parse_patch(std::string_view text, bool lenient,
                                 ParseDiagnostic &error) {
  const auto lines = split_lines(text);
  Patch patch;
  bool begun = false;
  bool ended = false;
  UpdateFile *current_update = nullptr;
  UpdateFileChunk *chunk = nullptr;
  AddFile *current_add = nullptr;
  for (std::size_t n = 0; n < lines.size(); ++n) {
    const auto &raw = lines[n];
    // Lenient mode only relaxes matching of directive markers ("*** ..."
    // and "@@" lines some models pad with stray whitespace). Content lines
    // (+/-/space-prefixed) must keep their raw form: the leading character
    // is the diff-line sentinel, not incidental formatting, so trimming it
    // away would turn a valid context line into an unrecognized one.
    const auto line = lenient ? trim(raw) : raw;
    if (!begun) {
      if (line == "*** Begin Patch" || line.starts_with("*** Begin Patch"))
        begun = true;
      if (begun)
        continue;
      if (!lenient || line.starts_with("***")) {
        fail(error, n + 1, "expected *** Begin Patch");
        return std::nullopt;
      }
      continue;
    }
    if (line == "*** End Patch") {
      ended = true;
      break;
    }
    if (line.starts_with("*** Add File:")) {
      patch.hunks.emplace_back(AddFile{marker_path(line, "*** Add File:"), {}});
      current_add = &std::get<AddFile>(patch.hunks.back());
      current_update = nullptr;
      chunk = nullptr;
      continue;
    }
    if (line.starts_with("*** Delete File:")) {
      patch.hunks.emplace_back(
          DeleteFile{marker_path(line, "*** Delete File:")});
      current_add = nullptr;
      current_update = nullptr;
      chunk = nullptr;
      continue;
    }
    if (line.starts_with("*** Update File:")) {
      patch.hunks.emplace_back(
          UpdateFile{marker_path(line, "*** Update File:"), {}, {}});
      current_update = &std::get<UpdateFile>(patch.hunks.back());
      current_add = nullptr;
      chunk = nullptr;
      continue;
    }
    if (line.starts_with("*** Move to:") && (current_update != nullptr)) {
      current_update->move_path = marker_path(line, "*** Move to:");
      continue;
    }
    if (line == "*** End of File" && (chunk != nullptr)) {
      chunk->anchored_to_eof = true;
      continue;
    }
    if ((current_add != nullptr) && !raw.empty() && raw[0] == '+') {
      current_add->lines.push_back(raw.substr(1));
      continue;
    }
    if ((current_update != nullptr) && line.starts_with("@@")) {
      current_update->chunks.push_back({});
      chunk = &current_update->chunks.back();
      auto ctx = trim(line.substr(2));
      if (!ctx.empty())
        chunk->change_context = std::move(ctx);
      continue;
    }
    if ((current_update != nullptr) && (chunk != nullptr) && !raw.empty() &&
        (raw[0] == '+' || raw[0] == '-' || raw[0] == ' ')) {
      if (raw[0] == '-')
        chunk->old_lines.push_back(raw.substr(1));
      else if (raw[0] == '+')
        chunk->new_lines.push_back(raw.substr(1));
      else {
        chunk->old_lines.push_back(raw.substr(1));
        chunk->new_lines.push_back(raw.substr(1));
      }
      continue;
    }
    if (!line.empty()) {
      fail(error, n + 1, "unknown or misplaced patch directive: " + raw);
      return std::nullopt;
    }
  }
  if (!begun || !ended) {
    fail(error, lines.size(), "unterminated patch");
    return std::nullopt;
  }
  for (const auto &h : patch.hunks)
    if (const auto *a = std::get_if<AddFile>(&h);
        (a != nullptr) && a->lines.empty()) {
      fail(error, 0, "Add File has no content");
      return std::nullopt;
    }
  return patch;
}

AppliedPatch apply_patch(const std::filesystem::path &cwd, const Patch &patch,
                         const ApplyPatchOptions &options) {
  if (options.line_endings == LineEndings::preserve)
    throw std::runtime_error("preserve line endings is not yet implemented");
  struct Entry {
    bool loaded = false, exists = false, trailing_newline = true, bom = false;
    std::vector<std::string> lines;
  };
  std::map<std::string, Entry> overlay;
  std::map<std::string, std::filesystem::path> paths;
  AppliedPatch result;
  auto get = [&](const std::string &name) -> Entry & {
    auto p = safe_path(cwd, name);
    auto key = p.string();
    paths[key] = p;
    auto &e = overlay[key];
    if (!e.loaded) {
      e.loaded = true;
      e.exists = std::filesystem::exists(p);
      if (e.exists) {
        auto raw = read_file(p);
        e.bom = raw.starts_with("\xEF\xBB\xBF");
        if (e.bom)
          raw.erase(0, 3);
        e.trailing_newline = !raw.empty() && raw.back() == '\n';
        e.lines = split_lines(raw);
      }
    }
    return e;
  };
  for (std::size_t hi = 0; hi < patch.hunks.size(); ++hi) {
    const auto &h = patch.hunks[hi];
    if (const auto *a = std::get_if<AddFile>(&h)) {
      auto &e = get(a->path);
      if (e.exists)
        throw std::runtime_error("hunk " + std::to_string(hi) +
                                 ": Add File already exists: " + a->path);
      e.exists = true;
      e.lines = a->lines;
      result.added.push_back(a->path);
      continue;
    }
    if (const auto *d = std::get_if<DeleteFile>(&h)) {
      auto &e = get(d->path);
      if (!e.exists)
        throw std::runtime_error("hunk " + std::to_string(hi) +
                                 ": Delete File does not exist: " + d->path);
      e.exists = false;
      result.deleted.push_back(d->path);
      continue;
    }
    const auto &u = std::get<UpdateFile>(h);
    auto &src = get(u.path);
    if (!src.exists)
      throw std::runtime_error("hunk " + std::to_string(hi) +
                               ": file does not exist: " + u.path);
    auto lines = src.lines;
    std::size_t start = 0;
    for (std::size_t ci = 0; ci < u.chunks.size(); ++ci) {
      const auto &c = u.chunks[ci];
      std::size_t at = 0;
      if (c.anchored_to_eof && lines.size() >= c.old_lines.size())
        at = lines.size() - c.old_lines.size();
      else if (!seek(lines, c.old_lines, start, at))
        throw std::runtime_error(
            "hunk " + std::to_string(hi) + ", chunk " + std::to_string(ci) +
            ": could not find context in " + u.path +
            " (search started at line " + std::to_string(start + 1) + ")");
      lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(at),
                  lines.begin() +
                      static_cast<std::ptrdiff_t>(at + c.old_lines.size()));
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                   c.new_lines.begin(), c.new_lines.end());
      start = at + c.new_lines.size();
    }
    if (u.move_path) {
      auto &dst = get(*u.move_path);
      if (dst.exists)
        throw std::runtime_error("hunk " + std::to_string(hi) +
                                 ": move destination exists: " + *u.move_path);
      dst.exists = true;
      dst.lines = std::move(lines);
      dst.trailing_newline = src.trailing_newline;
      dst.bom = src.bom;
      src.exists = false;
      result.moved.push_back(u.path + " -> " + *u.move_path);
    } else {
      src.lines = std::move(lines);
      result.updated.push_back(u.path);
    }
  }
  for (auto &[key, e] : overlay) {
    auto p = paths[key];
    if (e.exists) {
      if (options.create_parents)
        std::filesystem::create_directories(p.parent_path());
      std::ofstream out(p, std::ios::binary | std::ios::trunc);
      if (!out)
        throw std::runtime_error("Cannot write file: " + p.string());
      if (e.bom)
        out << "\xEF\xBB\xBF";
      for (std::size_t i = 0; i < e.lines.size(); ++i) {
        if (i != 0)
          out << '\n';
        out << e.lines[i];
      }
      if (e.trailing_newline && !e.lines.empty())
        out << '\n';
    } else if (std::filesystem::exists(p))
      std::filesystem::remove(p);
  }
  std::ostringstream diff;
  for (const auto &p : result.added)
    diff << "--- /dev/null\n+++ b/" << p << "\n";
  for (const auto &p : result.updated)
    diff << "--- a/" << p << "\n+++ b/" << p << "\n";
  for (const auto &p : result.deleted)
    diff << "--- a/" << p << "\n+++ /dev/null\n";
  result.unified_diff = diff.str();
  return result;
}
} // namespace pi::core
