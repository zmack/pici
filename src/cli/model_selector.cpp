#include "cli/model_selector.h"

#include "core/message_types.h"
#include "core/terminal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
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

struct AlternateScreen {
  bool active{false};
  AlternateScreen() {
    constexpr std::string_view sequence = "\033[?1049h";
    active = ::write(STDOUT_FILENO, sequence.data(), sequence.size()) >= 0;
  }
  AlternateScreen(const AlternateScreen &) = delete;
  AlternateScreen &operator=(const AlternateScreen &) = delete;
  ~AlternateScreen() {
    if (active) {
      constexpr std::string_view sequence = "\033[?1049l";
      ::write(STDOUT_FILENO, sequence.data(), sequence.size());
    }
  }
};

enum class Key { up, down, enter, escape, other };

Key read_key() {
  unsigned char c = 0;
  if (::read(STDIN_FILENO, &c, 1) <= 0)
    return Key::escape;
  if (c == 'k')
    return Key::up;
  if (c == 'j')
    return Key::down;
  if (c == 'q' || c == 27) {
    if (c == 'q')
      return Key::escape;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval timeout{}; // NOLINT(misc-include-cleaner): from <sys/time.h> above
    timeout.tv_usec = 50'000;
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &timeout) <= 0)
      return Key::escape;
    unsigned char next = 0;
    if (::read(STDIN_FILENO, &next, 1) <= 0 || next != '[')
      return Key::escape;
    unsigned char arrow = 0;
    if (::read(STDIN_FILENO, &arrow, 1) <= 0)
      return Key::escape;
    if (arrow == 'A')
      return Key::up;
    if (arrow == 'B')
      return Key::down;
  }
  if (c == '\r' || c == '\n')
    return Key::enter;
  return Key::other;
}

void write_text(std::string_view text) {
  ::write(STDOUT_FILENO, text.data(), text.size());
}

std::string display(const core::Model &model,
                    const ModelAvailability &availability) {
  const bool images = std::ranges::find(model.input_capabilities, "image") !=
                      model.input_capabilities.end();
  auto result = model.provider + "/" + model.id +
                "  context=" + std::to_string(model.context_window) +
                "  reasoning=" + (model.reasoning ? "yes" : "no") +
                "  images=" + (images ? "yes" : "no");
  if (availability)
    result += "  auth=" + availability(model);
  return result;
}

void render(const std::vector<const core::Model *> &models, std::size_t cursor,
            std::size_t view_top, int visible_rows,
            const ModelAvailability &availability) {
  write_text("\033[H\033[J  Model selector\n\n");
  for (int row = 0; row < visible_rows; ++row) {
    const auto index = view_top + static_cast<std::size_t>(row);
    if (index >= models.size())
      break;
    if (index == cursor)
      write_text("\033[7m");
    write_text("  " + display(*models[index], availability));
    if (index == cursor)
      write_text("\033[0m");
    write_text("\033[K\n");
  }
  write_text("\n  \033[2m↑↓ navigate   Enter switch   Esc/q cancel\033[0m\n");
}

} // namespace

ModelSelectorResult
run_model_selector(const std::vector<const core::Model *> &models,
                   std::string_view current_provider,
                   std::string_view current_model,
                   const ModelAvailability &availability) {
  if (models.empty())
    return {};
  RawMode raw;
  if (!raw.enter(STDIN_FILENO))
    return {};
  AlternateScreen screen;
  const int visible_rows = std::max(1, core::term_height(STDOUT_FILENO) - 4);
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < models.size(); ++i) {
    if (models[i]->provider == current_provider &&
        models[i]->id == current_model) {
      cursor = i;
      break;
    }
  }
  std::size_t view_top = 0;
  render(models, cursor, view_top, visible_rows, availability);
  while (true) {
    switch (read_key()) {
    case Key::escape:
      return {};
    case Key::enter:
      return {.cancelled = false, .model = *models[cursor]};
    case Key::up:
      if (cursor > 0)
        --cursor;
      break;
    case Key::down:
      if (cursor + 1 < models.size())
        ++cursor;
      break;
    case Key::other:
      break;
    }
    if (cursor < view_top)
      view_top = cursor;
    if (cursor >= view_top + static_cast<std::size_t>(visible_rows))
      view_top = cursor - static_cast<std::size_t>(visible_rows) + 1;
    render(models, cursor, view_top, visible_rows, availability);
  }
}

} // namespace pi::cli
