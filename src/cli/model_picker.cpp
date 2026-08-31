#include "cli/model_picker.h"

#include "core/models.h"
#include "core/terminal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/time.h> // NOLINT(misc-include-cleaner): the portable public
                      // header for struct timeval, not the glibc-private one
                      // the tool would otherwise suggest.
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace pi::cli {
namespace {

struct RawMode {
  int fd{-1};
  struct termios saved {};
  bool active{false};

  RawMode() = default;
  RawMode(const RawMode &) = delete;
  RawMode &operator=(const RawMode &) = delete;
  RawMode(RawMode &&) = delete;
  RawMode &operator=(RawMode &&) = delete;
  ~RawMode() { leave(); }

  bool enter(int fdesc) {
    if (isatty(fdesc) == 0 || tcgetattr(fdesc, &saved) != 0)
      return false;
    auto raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fdesc, TCSAFLUSH, &raw) != 0)
      return false;
    fd = fdesc;
    active = true;
    return true;
  }

  void leave() {
    if (active) {
      tcsetattr(fd, TCSAFLUSH, &saved);
      active = false;
    }
  }
};

// Entering `1049h` while a caller already owns a persistent alt-screen
// session (the region renderer) clears that screen without saving it, and
// the matching `1049l` on exit drops back to the primary buffer the owning
// renderer never painted, corrupting the whole display. When
// `caller_owns_alt_screen` is true, this draws straight into the existing
// screen instead and the caller repaints once control returns.
struct AlternateScreen {
  bool active{false};
  bool owned{false};
  explicit AlternateScreen(bool caller_owns_alt_screen)
      : owned(!caller_owns_alt_screen) {
    if (!owned)
      return;
    constexpr std::string_view sequence = "\033[?1049h";
    active = ::write(STDOUT_FILENO, sequence.data(), sequence.size()) >= 0;
  }
  AlternateScreen(const AlternateScreen &) = delete;
  AlternateScreen &operator=(const AlternateScreen &) = delete;
  ~AlternateScreen() {
    if (active && owned) {
      constexpr std::string_view sequence = "\033[?1049l";
      core::write_best_effort(STDOUT_FILENO, sequence.data(), sequence.size());
    }
  }
};

ModelPickerKey read_key() {
  unsigned char c = 0;
  if (::read(STDIN_FILENO, &c, 1) <= 0)
    return ModelPickerKey::escape;
  if (c == 'k')
    return ModelPickerKey::up;
  if (c == 'j')
    return ModelPickerKey::down;
  if (c == 'q' || c == 27) {
    if (c == 'q')
      return ModelPickerKey::escape;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval timeout{}; // NOLINT(misc-include-cleaner): from <sys/time.h> above
    timeout.tv_usec = 50'000;
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &timeout) <= 0)
      return ModelPickerKey::escape;
    unsigned char next = 0;
    if (::read(STDIN_FILENO, &next, 1) <= 0 || next != '[')
      return ModelPickerKey::escape;
    unsigned char arrow = 0;
    if (::read(STDIN_FILENO, &arrow, 1) <= 0)
      return ModelPickerKey::escape;
    if (arrow == 'A')
      return ModelPickerKey::up;
    if (arrow == 'B')
      return ModelPickerKey::down;
  }
  if (c == '\r' || c == '\n')
    return ModelPickerKey::enter;
  return ModelPickerKey::other;
}

void write_text(std::string_view text) {
  core::write_best_effort(STDOUT_FILENO, text.data(), text.size());
}

std::string display(
    const core::ModelCatalogEntry &entry,
    const std::function<std::string(const core::ModelKey &)> &availability) {
  const bool images = std::ranges::find(entry.input_capabilities, "image") !=
                      entry.input_capabilities.end();
  auto result = entry.key.provider_id + "/" + entry.key.model_id +
                "  context=" + std::to_string(entry.context_window) +
                "  reasoning=" + (entry.reasoning ? "yes" : "no") +
                "  images=" + (images ? "yes" : "no");
  if (availability)
    result += "  auth=" + availability(entry.key);
  return result;
}

void render(
    const std::vector<core::ModelCatalogEntry> &entries, std::size_t cursor,
    std::size_t view_top, int visible_rows,
    const std::function<std::string(const core::ModelKey &)> &availability) {
  write_text("\033[H\033[J  Model selector\n\n");
  for (int row = 0; row < visible_rows; ++row) {
    const auto index = view_top + static_cast<std::size_t>(row);
    if (index >= entries.size())
      break;
    if (index == cursor)
      write_text("\033[7m");
    write_text("  " + display(entries[index], availability));
    if (index == cursor)
      write_text("\033[0m");
    write_text("\033[K\n");
  }
  write_text("\n  \033[2m↑↓ navigate   Enter switch   Esc/q cancel\033[0m\n");
}

} // namespace

std::size_t reduce_model_picker_cursor(std::size_t cursor, ModelPickerKey key,
                                       std::size_t count) {
  if (count == 0)
    return cursor;
  switch (key) {
  case ModelPickerKey::up:
    return cursor > 0 ? cursor - 1 : cursor;
  case ModelPickerKey::down:
    return cursor + 1 < count ? cursor + 1 : cursor;
  case ModelPickerKey::enter:
  case ModelPickerKey::escape:
  case ModelPickerKey::other:
    return cursor;
  }
  return cursor;
}

ModelPickerResult run_model_picker(const ModelPickerInput &input,
                                   bool caller_owns_alt_screen) {
  const auto &entries = input.catalog_view.entries;
  if (entries.empty())
    return {};
  RawMode raw;
  if (!raw.enter(STDIN_FILENO))
    return {};
  AlternateScreen screen(caller_owns_alt_screen);
  const int visible_rows = std::max(1, core::term_height(STDOUT_FILENO) - 4);
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].key == input.current) {
      cursor = i;
      break;
    }
  }
  std::size_t view_top = 0;
  render(entries, cursor, view_top, visible_rows, input.availability);
  while (true) {
    const auto key = read_key();
    if (key == ModelPickerKey::escape)
      return {};
    if (key == ModelPickerKey::enter)
      return {.cancelled = false, .selected = entries[cursor].key};
    cursor = reduce_model_picker_cursor(cursor, key, entries.size());
    if (cursor < view_top)
      view_top = cursor;
    if (cursor >= view_top + static_cast<std::size_t>(visible_rows))
      view_top = cursor - static_cast<std::size_t>(visible_rows) + 1;
    render(entries, cursor, view_top, visible_rows, input.availability);
  }
}

} // namespace pi::cli
