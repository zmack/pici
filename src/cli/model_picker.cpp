#include "cli/model_picker.h"

#include "core/models.h"
#include "core/terminal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/time.h> // NOLINT(misc-include-cleaner): the portable public
                      // header for struct timeval, not the glibc-private one
                      // the tool would otherwise suggest.
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
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

struct ReadResult {
  bool timed_out{false};
  ModelPickerKeyEvent event{};
};

// Disambiguates a bare Esc from an arrow key the user already started
// typing. Split out of read_key_or_timeout() to keep that function's
// cognitive complexity down; this one always uses its own short fixed
// lookahead timeout regardless of the caller's own timeout_ms.
ModelPickerKeyEvent decode_escape_sequence() {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  timeval esc_timeout{}; // NOLINT(misc-include-cleaner): from <sys/time.h>
  esc_timeout.tv_usec = 50'000;
  if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &esc_timeout) <= 0)
    return {.nav = ModelPickerKey::escape};
  unsigned char next = 0;
  if (::read(STDIN_FILENO, &next, 1) <= 0 || next != '[')
    return {.nav = ModelPickerKey::escape};
  unsigned char arrow = 0;
  if (::read(STDIN_FILENO, &arrow, 1) <= 0)
    return {.nav = ModelPickerKey::escape};
  if (arrow == 'A')
    return {.nav = ModelPickerKey::up};
  if (arrow == 'B')
    return {.nav = ModelPickerKey::down};
  return {.nav = ModelPickerKey::escape};
}

// `timeout_ms < 0` blocks indefinitely on the outer read (used when no
// background refresh is running and there's nothing to poll for).
ReadResult read_key_or_timeout(int timeout_ms) {
  if (timeout_ms >= 0) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval timeout{}; // NOLINT(misc-include-cleaner): from <sys/time.h> above
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec =
        static_cast<decltype(timeout.tv_usec)>(timeout_ms % 1000) * 1000;
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &timeout) <= 0)
      return {.timed_out = true};
  }
  unsigned char c = 0;
  if (::read(STDIN_FILENO, &c, 1) <= 0)
    return {.event = {.nav = ModelPickerKey::escape}};
  if (c == 27)
    return {.event = decode_escape_sequence()};
  return {.event = decode_plain_byte(c)};
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
    std::size_t view_top, int visible_rows, const core::ModelKey &current,
    std::string_view query, std::string_view status_line,
    bool save_as_default_available,
    const std::function<std::string(const core::ModelKey &)> &availability) {
  write_text("\033[H\033[J  Model selector\n");
  write_text("  Search: " + std::string(query) + "\033[K\n\n");
  for (int row = 0; row < visible_rows; ++row) {
    const auto index = view_top + static_cast<std::size_t>(row);
    if (index >= entries.size())
      break;
    const bool is_current = entries[index].key == current;
    const std::string marker = is_current ? "✓ " : "  ";
    if (index == cursor)
      write_text("\033[7m");
    write_text(marker + display(entries[index], availability));
    if (index == cursor)
      write_text("\033[0m");
    write_text("\033[K\n");
  }
  if (entries.empty()) {
    write_text("\033[K\n  No matching models\033[K\n");
  } else if (view_top > 0 || view_top + static_cast<std::size_t>(visible_rows) <
                                 entries.size()) {
    write_text("  (" + std::to_string(cursor + 1) + "/" +
               std::to_string(entries.size()) + ")\033[K\n");
  }
  if (!status_line.empty())
    write_text("\n  " + std::string(status_line) + "\033[K\n");
  std::string hint = "\n  \033[2m↑↓ navigate   Enter switch";
  if (save_as_default_available)
    hint += "   Ctrl+S set default";
  hint += "   Esc cancel\033[0m\n";
  write_text(hint);
}

// A background refresh's completed result, or its error, handed off between
// the detached refresh thread and the picker's own loop.
struct RefreshState {
  std::mutex mutex;
  bool closed{false};
  bool done{false};
  std::optional<core::ModelCatalogView> result;
  std::optional<std::string> error;
};

// Detached (never joined) because ModelDiscoveryAdapter::discover() is a
// real network call that can hang well past when the user closes the
// picker; a joining thread would block the picker's return on a stuck
// provider. `state->closed` (set by the picker before it returns) is what
// actually stops the thread from touching anything afterward; `stop_token`
// is only a best-effort hint to refresh() itself.
void launch_refresh(
    const std::shared_ptr<RefreshState> &state,
    std::function<core::ModelCatalogView(std::stop_token)> refresh_catalog,
    std::stop_token stop_token) {
  std::thread([state, refresh_catalog = std::move(refresh_catalog),
               stop_token = std::move(stop_token)]() mutable {
    std::optional<core::ModelCatalogView> result;
    std::optional<std::string> error;
    try {
      result = refresh_catalog(std::move(stop_token));
    } catch (const std::exception &e) {
      error = e.what();
    }
    std::scoped_lock lock(state->mutex);
    if (state->closed)
      return;
    state->done = true;
    state->result = std::move(result);
    state->error = std::move(error);
  }).detach();
}

} // namespace

std::size_t reduce_model_picker_cursor(std::size_t cursor, ModelPickerKey key,
                                       std::size_t count) {
  if (count == 0)
    return cursor;
  switch (key) {
  case ModelPickerKey::up:
    return cursor == 0 ? count - 1 : cursor - 1;
  case ModelPickerKey::down:
    return cursor + 1 == count ? 0 : cursor + 1;
  case ModelPickerKey::enter:
  case ModelPickerKey::escape:
  case ModelPickerKey::other:
    return cursor;
  }
  return cursor;
}

ModelPickerKeyEvent decode_plain_byte(unsigned char c) {
  if (c == '\r' || c == '\n')
    return {.nav = ModelPickerKey::enter};
  if (c == 0x7f || c == 0x08)
    return {.is_backspace = true};
  if (c == 0x13) // Ctrl+S
    return {.is_ctrl_s = true};
  if (c >= 0x20 && c < 0x7f)
    return {.text_char = static_cast<char>(c)};
  return {};
}

bool fuzzy_match(std::string_view haystack, std::string_view query) {
  if (query.empty())
    return true;
  std::size_t hi = 0;
  for (const char qc : query) {
    const auto qlow =
        static_cast<char>(std::tolower(static_cast<unsigned char>(qc)));
    bool found = false;
    while (hi < haystack.size()) {
      const auto hlow = static_cast<char>(
          std::tolower(static_cast<unsigned char>(haystack[hi])));
      ++hi;
      if (hlow == qlow) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

std::vector<core::ModelCatalogEntry>
sort_entries_for_display(std::vector<core::ModelCatalogEntry> entries,
                         const core::ModelKey &current,
                         const std::optional<core::ModelKey> &default_model) {
  const auto rank = [&](const core::ModelCatalogEntry &entry) {
    if (entry.key == current)
      return 0;
    if (default_model && entry.key == *default_model)
      return 1;
    return 2;
  };
  std::ranges::stable_sort(
      entries, {}, [&](const core::ModelCatalogEntry &e) { return rank(e); });
  return entries;
}

namespace {

// All of run_model_picker()'s mutable display state, gathered so the
// per-event and per-refresh handling below can be free functions instead of
// closures -- that's what keeps run_model_picker() itself under the
// cognitive-complexity threshold.
struct PickerState {
  std::vector<core::ModelCatalogEntry> entries;
  std::vector<core::ModelCatalogEntry> filtered;
  std::string query;
  std::size_t cursor{0};
  std::size_t view_top{0};
  std::string status_line;

  void refilter() {
    if (query.empty()) {
      filtered = entries;
    } else {
      filtered.clear();
      for (const auto &entry : entries) {
        const std::string haystack = entry.key.provider_id + "/" +
                                     entry.key.model_id + " " +
                                     entry.display_name;
        if (fuzzy_match(haystack, query))
          filtered.push_back(entry);
      }
    }
    cursor = 0;
    view_top = 0;
  }
};

// Consumes a completed background refresh (if any) from `state` and folds
// it into `picker`. Returns true when something changed and the caller
// should refilter/re-render.
bool apply_refresh_completion(RefreshState &state,
                              const ModelPickerInput &input,
                              PickerState &picker) {
  bool completed = false;
  std::optional<core::ModelCatalogView> result;
  std::optional<std::string> error;
  {
    std::scoped_lock lock(state.mutex);
    if (state.done) {
      completed = true;
      result = std::move(state.result);
      error = std::move(state.error);
      state.done = false;
    }
  }
  if (!completed)
    return false;
  if (result) {
    picker.entries = sort_entries_for_display(result->entries, input.current,
                                              input.default_model);
    picker.status_line = "Model catalog refreshed.";
  } else {
    picker.status_line =
        "Could not refresh model catalog: " + error.value_or("unknown error");
  }
  return true;
}

enum class LoopAction { keep_going, redraw, finish };

// Applies one decoded key event to `picker`, filling `result` only when
// returning finish. Kept as a free function (not a lambda) for the same
// cognitive-complexity reason as PickerState above.
LoopAction handle_key_event(const ModelPickerKeyEvent &event,
                            PickerState &picker, const ModelPickerInput &input,
                            int visible_rows, ModelPickerResult &result) {
  if (event.nav == ModelPickerKey::escape) {
    result = {};
    return LoopAction::finish;
  }
  if (event.nav == ModelPickerKey::enter) {
    if (picker.filtered.empty())
      return LoopAction::keep_going;
    result = {.cancelled = false,
              .selected = picker.filtered[picker.cursor].key};
    return LoopAction::finish;
  }
  if (event.is_ctrl_s) {
    if (!input.save_as_default || picker.filtered.empty())
      return LoopAction::keep_going;
    const auto key = picker.filtered[picker.cursor].key;
    input.save_as_default(key);
    result = {.cancelled = false, .selected = key, .saved_as_default = true};
    return LoopAction::finish;
  }
  if (event.is_backspace) {
    if (picker.query.empty())
      return LoopAction::keep_going;
    picker.query.pop_back();
    picker.refilter();
    return LoopAction::redraw;
  }
  if (event.text_char != 0) {
    picker.query.push_back(event.text_char);
    picker.refilter();
    return LoopAction::redraw;
  }
  if (event.nav != ModelPickerKey::up && event.nav != ModelPickerKey::down)
    return LoopAction::keep_going;
  if (picker.filtered.empty())
    return LoopAction::keep_going;
  picker.cursor = reduce_model_picker_cursor(picker.cursor, event.nav,
                                             picker.filtered.size());
  if (picker.cursor < picker.view_top)
    picker.view_top = picker.cursor;
  if (picker.cursor >= picker.view_top + static_cast<std::size_t>(visible_rows))
    picker.view_top =
        picker.cursor - static_cast<std::size_t>(visible_rows) + 1;
  return LoopAction::redraw;
}

} // namespace

ModelPickerResult run_model_picker(const ModelPickerInput &input,
                                   bool caller_owns_alt_screen) {
  PickerState picker;
  picker.entries = sort_entries_for_display(input.catalog_view.entries,
                                            input.current, input.default_model);
  if (picker.entries.empty())
    return {};
  RawMode raw;
  if (!raw.enter(STDIN_FILENO))
    return {};
  AlternateScreen screen(caller_owns_alt_screen);
  const int visible_rows = std::max(1, core::term_height(STDOUT_FILENO) - 6);

  picker.filtered = picker.entries;
  for (std::size_t i = 0; i < picker.filtered.size(); ++i) {
    if (picker.filtered[i].key == input.current) {
      picker.cursor = i;
      break;
    }
  }

  const auto refresh_state = std::make_shared<RefreshState>();
  std::stop_source refresh_stop_source;
  const bool refresh_launched = static_cast<bool>(input.refresh_catalog);
  if (refresh_launched) {
    picker.status_line = "Refreshing model catalog…";
    launch_refresh(refresh_state, input.refresh_catalog,
                   refresh_stop_source.get_token());
  }

  const auto do_render = [&] {
    render(picker.filtered, picker.cursor, picker.view_top, visible_rows,
           input.current, picker.query, picker.status_line,
           static_cast<bool>(input.save_as_default), input.availability);
  };
  do_render();

  const auto close = [&] {
    if (!refresh_launched)
      return;
    refresh_stop_source.request_stop();
    std::scoped_lock lock(refresh_state->mutex);
    refresh_state->closed = true;
  };

  ModelPickerResult result;
  while (true) {
    const auto polled = read_key_or_timeout(refresh_launched ? 100 : -1);
    if (polled.timed_out) {
      if (apply_refresh_completion(*refresh_state, input, picker)) {
        picker.refilter();
        do_render();
      }
      continue;
    }
    const auto action =
        handle_key_event(polled.event, picker, input, visible_rows, result);
    if (action == LoopAction::finish) {
      close();
      return result;
    }
    if (action == LoopAction::redraw)
      do_render();
  }
}

} // namespace pi::cli
