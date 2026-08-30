#include "cli/readline.h"

#include <gtest/gtest.h>

#include "core/stream_renderer.h"
#include "core/terminal.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

using namespace pi::cli;

namespace {

template <typename F> void run_case(std::string_view, F &&test) {
  std::forward<F>(test)();
}

class ReadlineEnvironment : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    if (const char *value = ::getenv("PICI_DISABLE_KITTY_KEYBOARD"))
      previous_kitty_ = value;
    if (const char *value = ::getenv("COLORTERM"))
      previous_colorterm_ = value;
    ::setenv("PICI_DISABLE_KITTY_KEYBOARD", "1", 1);
    ::unsetenv("COLORTERM");
  }

  static void TearDownTestSuite() {
    if (previous_kitty_)
      ::setenv("PICI_DISABLE_KITTY_KEYBOARD", previous_kitty_->c_str(), 1);
    else
      ::unsetenv("PICI_DISABLE_KITTY_KEYBOARD");
    if (previous_colorterm_)
      ::setenv("COLORTERM", previous_colorterm_->c_str(), 1);
    else
      ::unsetenv("COLORTERM");
  }

private:
  static inline std::optional<std::string> previous_kitty_;
  static inline std::optional<std::string> previous_colorterm_;
};

} // namespace

std::string read_until(int fd, std::string output, std::string_view marker) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (output.find(marker) == std::string::npos &&
         std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{.fd = fd, .events = POLLIN};
    const auto ready = ::poll(&descriptor, 1, 100);
    if (ready <= 0)
      continue;
    char buffer[256];
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0)
      output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

// Like read_until, but only considers the marker found if it appears at or
// after byte offset `from` — used to wait for a *second* occurrence of a
// marker (e.g. a redraw's trailing escape sequence) that already exists
// earlier in `output` from a prior redraw.
std::string read_until_from(int fd, std::string output, std::size_t from,
                            std::string_view marker) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((output.size() < from ||
          output.find(marker, from) == std::string::npos) &&
         std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{.fd = fd, .events = POLLIN};
    const auto ready = ::poll(&descriptor, 1, 100);
    if (ready <= 0)
      continue;
    char buffer[256];
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0)
      output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

std::string read_new_output(int fd, std::string output) {
  const auto initial_size = output.size();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (output.size() == initial_size &&
         std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{.fd = fd, .events = POLLIN};
    if (::poll(&descriptor, 1, 100) <= 0)
      continue;
    char buffer[256];
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0)
      output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

int wait_for_child(pid_t child) {
  int status = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    const auto result = ::waitpid(child, &status, WNOHANG);
    if (result == child)
      break;
    if (result < 0)
      return -1;
    poll(nullptr, 0, 50);
    if (attempt == 19) {
      ::kill(child, SIGKILL);
      if (::waitpid(child, &status, 0) < 0)
        return -1;
    }
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

TEST_F(ReadlineEnvironment, test_wake_channel) {
  run_case("ReadlineWake: nonblocking CLOEXEC coalescing", [] {
    ReadlineWake wake;
    const auto descriptor_flags = ::fcntl(wake.read_fd(), F_GETFD);
    const auto status_flags = ::fcntl(wake.read_fd(), F_GETFL);
    EXPECT_TRUE((descriptor_flags & FD_CLOEXEC) != 0);
    EXPECT_TRUE((status_flags & O_NONBLOCK) != 0);
    EXPECT_TRUE(wake.notify());
    EXPECT_TRUE(wake.notify());
    EXPECT_TRUE(wake.notify());
    wake.drain();
    pollfd descriptor{.fd = wake.read_fd(), .events = POLLIN};
    EXPECT_EQ(::poll(&descriptor, 1, 0), 0);
  });
}

void test_non_tty_result(std::string input, ReadlineExit expected,
                         std::string expected_text) {
  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  EXPECT_EQ(::pipe(input_pipe), 0);
  EXPECT_EQ(::pipe(output_pipe), 0);
  const auto child = ::fork();
  EXPECT_TRUE(child >= 0);
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    const auto result = readline("> ");
    dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
            static_cast<int>(result.reason), result.cursor,
            result.text.c_str());
    _exit(0);
  }
  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  if (!input.empty())
    EXPECT_EQ(::write(input_pipe[1], input.data(), input.size()),
              static_cast<ssize_t>(input.size()));
  ::close(input_pipe[1]);
  auto output = read_until(output_pipe[0], {}, "RESULT:");
  ::close(output_pipe[0]);
  EXPECT_TRUE(output.find("RESULT:" + std::to_string(static_cast<int>(
                                          expected))) != std::string::npos);
  EXPECT_TRUE(output.find(":" + expected_text + "\n") != std::string::npos);
  EXPECT_EQ(wait_for_child(child), 0);
}

TEST_F(ReadlineEnvironment, test_non_tty_paths) {
  run_case("readline: non-TTY submit and EOF", [] {
    test_non_tty_result("scripted\n", ReadlineExit::submitted, "scripted");
    test_non_tty_result("", ReadlineExit::eof, "");
  });
}

TEST_F(ReadlineEnvironment, test_tty_wake_and_reentry) {
  run_case("readline: wake preserves middle cursor and re-entry", [] {
    ReadlineWake wake;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    EXPECT_TRUE(child >= 0);
    if (child == 0) {
      dprintf(STDOUT_FILENO, "READY\n");
      const auto first = readline("> ", {}, {}, {}, "héllo", 3, wake.read_fd());
      dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
              static_cast<int>(first.reason), first.cursor, first.text.c_str());
      dprintf(STDOUT_FILENO, "SECOND_READY\n");
      const auto second =
          readline("> ", {}, {}, {}, first.text, first.cursor, wake.read_fd());
      dprintf(STDOUT_FILENO, "\nRESULT2:%d:%zu:%s\n",
              static_cast<int>(second.reason), second.cursor,
              second.text.c_str());
      _exit(0);
    }

    auto output = read_until(master, {}, "READY");
    EXPECT_TRUE(output.find("READY") != std::string::npos);
    output = read_until(master, std::move(output), "héllo");
    EXPECT_EQ(::write(master, "\033[D", 3), 3);
    output = read_new_output(master, std::move(output));
    EXPECT_EQ(::write(master, "\033[C", 3), 3);
    output = read_new_output(master, std::move(output));
    EXPECT_TRUE(wake.notify());
    EXPECT_TRUE(wake.notify());
    EXPECT_TRUE(wake.notify());
    output = read_until(master, std::move(output), "RESULT:");
    const auto expected_result =
        "RESULT:" +
        std::to_string(static_cast<int>(ReadlineExit::mailbox_wake)) +
        ":3:héllo";
    EXPECT_TRUE(output.find(expected_result) != std::string::npos);
    output = read_until(master, std::move(output), "SECOND_READY");
    EXPECT_TRUE(output.find("SECOND_READY") != std::string::npos);

    output = read_new_output(master, std::move(output));
    EXPECT_EQ(::write(master, "X", 1), 1);
    output = read_new_output(master, std::move(output));
    EXPECT_TRUE(wake.notify());
    EXPECT_EQ(::write(master, "\r", 1), 1);
    output = read_until(master, std::move(output), "RESULT2:");
    const auto expected_second =
        "RESULT2:" + std::to_string(static_cast<int>(ReadlineExit::submitted)) +
        ":4:héXllo";
    EXPECT_TRUE(output.find(expected_second) != std::string::npos);
    EXPECT_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

TEST_F(ReadlineEnvironment, test_escape_wake) {
  run_case("readline: isolated escape remains wakeable", [] {
    ReadlineWake wake;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    EXPECT_TRUE(child >= 0);
    if (child == 0) {
      dprintf(STDOUT_FILENO, "READY\n");
      const auto result =
          readline("> ", {}, {}, {}, "draft", 2, wake.read_fd());
      dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
              static_cast<int>(result.reason), result.cursor,
              result.text.c_str());
      _exit(0);
    }

    auto output = read_until(master, {}, "READY");
    output = read_until(master, std::move(output), "draft");
    EXPECT_EQ(::write(master, "\033", 1), 1);
    EXPECT_TRUE(wake.notify());
    output = read_until(master, std::move(output), "RESULT:");
    const auto expected =
        "RESULT:" +
        std::to_string(static_cast<int>(ReadlineExit::mailbox_wake)) +
        ":2:draft";
    EXPECT_TRUE(output.find(expected) != std::string::npos);
    EXPECT_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

TEST_F(ReadlineEnvironment, test_eof_wake_race) {
  run_case("readline: EOF wins a wake boundary", [] {
    ReadlineWake wake;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    EXPECT_TRUE(child >= 0);
    if (child == 0) {
      dprintf(STDOUT_FILENO, "READY\n");
      const auto result =
          readline("> ", {}, {}, {}, "draft", 2, wake.read_fd());
      dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
              static_cast<int>(result.reason), result.cursor,
              result.text.c_str());
      _exit(0);
    }

    auto output = read_until(master, {}, "READY");
    output = read_until(master, std::move(output), "draft");
    EXPECT_TRUE(wake.notify());
    EXPECT_EQ(::write(master, "\004", 1), 1);
    output = read_until(master, std::move(output), "RESULT:");
    const auto expected =
        "RESULT:" + std::to_string(static_cast<int>(ReadlineExit::eof)) +
        ":2:draft";
    EXPECT_TRUE(output.find(expected) != std::string::npos);
    EXPECT_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

std::size_t count_occurrences(std::string_view haystack,
                              std::string_view needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

TEST_F(ReadlineEnvironment, test_mouse_wheel_scroll) {
  run_case("readline: SGR mouse wheel drives scroll actions", [] {
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    EXPECT_TRUE(child >= 0);
    if (child == 0) {
      dprintf(STDOUT_FILENO, "READY\n");
      ControlFn control_fn = [](ControlAction action) {
        dprintf(STDOUT_FILENO, "ACTION:%d\n", static_cast<int>(action));
      };
      const auto result = readline("> ", {}, control_fn, {}, "", 0);
      dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
              static_cast<int>(result.reason), result.cursor,
              result.text.c_str());
      _exit(0);
    }

    auto output = read_until(master, {}, "READY");

    // Wheel-up (Cb=64, no modifiers): three scroll_line_up actions per
    // notch, none of the down action.
    EXPECT_EQ(::write(master, "\033[<64;10;5M", 11), 11);
    output = read_new_output(master, std::move(output));

    // Wheel-down (Cb=65): three scroll_line_down actions.
    EXPECT_EQ(::write(master, "\033[<65;10;5M", 11), 11);
    output = read_new_output(master, std::move(output));

    // A plain left-click (Cb=0, no wheel bit) is not a scroll gesture and
    // must not drive any control action.
    EXPECT_EQ(::write(master, "\033[<0;10;5M", 10), 10);
    output = read_new_output(master, std::move(output));

    EXPECT_EQ(::write(master, "\r", 1), 1);
    output = read_until(master, std::move(output), "RESULT:");

    const auto up_marker =
        "ACTION:" +
        std::to_string(static_cast<int>(ControlAction::scroll_line_up));
    const auto down_marker =
        "ACTION:" +
        std::to_string(static_cast<int>(ControlAction::scroll_line_down));
    EXPECT_EQ(count_occurrences(output, up_marker), 3U);
    EXPECT_EQ(count_occurrences(output, down_marker), 3U);

    EXPECT_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

// True if `output` contains a cursor-forward escape ("\033[<n>C") whose
// argument reaches or exceeds `columns` — CSI n C clamps at the terminal's
// rightmost cell for such an n, so the cursor glyph silently lands on the
// last real character instead of where it was meant to go. See the M0 fix
// in InputRenderer::write_wrapped/redraw for the invariant this guards.
bool has_out_of_range_cursor_forward(std::string_view output, int columns) {
  std::size_t pos = 0;
  while ((pos = output.find("\033[", pos)) != std::string_view::npos) {
    const std::size_t digits_start = pos + 2;
    std::size_t i = digits_start;
    while (i < output.size() && output[i] >= '0' && output[i] <= '9')
      ++i;
    if (i < output.size() && output[i] == 'C' && i > digits_start) {
      const int n =
          std::stoi(std::string(output.substr(digits_start, i - digits_start)));
      if (n >= columns)
        return true;
    }
    pos = digits_start;
  }
  return false;
}

TEST_F(ReadlineEnvironment, test_wrap_boundary_cursor_placement) {
  constexpr int kColumns = 10;

  run_case("readline: cursor stays in range when text exactly fills a row", [] {
    struct winsize ws {};
    ws.ws_row = 24;
    ws.ws_col = kColumns;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, &ws);
    EXPECT_TRUE(child >= 0);
    if (child == 0) {
      dprintf(STDOUT_FILENO, "READY\n");
      // Ten digits exactly fill a 10-column row with the cursor left
      // at the end of the text — the end-of-text tail-capture case.
      const auto result = readline("", {}, {}, {}, "0123456789", 10);
      dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
              static_cast<int>(result.reason), result.cursor,
              result.text.c_str());
      _exit(0);
    }

    auto output = read_until(master, {}, "READY");
    output = read_until(master, std::move(output), "0123456789");
    EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));
    EXPECT_TRUE(output.find("\033[7m") != std::string::npos);

    EXPECT_EQ(::write(master, "\r", 1), 1);
    output = read_until(master, std::move(output), "RESULT:");
    EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));
    EXPECT_EQ(wait_for_child(child), 0);
    ::close(master);
  });

  run_case(
      "readline: cursor stays in range mid-text at a soft wrap boundary", [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = kColumns;
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          // 15 characters wrap once (row 1: "0123456789", row 2: "ABCDE").
          const auto result = readline("", {}, {}, {}, "0123456789ABCDE", 15);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        output = read_until(master, std::move(output), "ABCDE");
        // Walk the cursor back to offset 10 — the boundary between the two
        // wrapped rows, i.e. sitting right at the soft wrap.
        for (int i = 0; i < 5; ++i) {
          EXPECT_EQ(::write(master, "\033[D", 3), 3);
          output = read_new_output(master, std::move(output));
        }
        EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_alt_enter_inserts_newline) {
  run_case("readline: Alt+Enter inserts a newline, plain Enter still submits",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result = readline("> ", {}, {}, {}, "", 0);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             EXPECT_EQ(::write(master, "ab", 2), 2);
             output = read_new_output(master, std::move(output));
             // ESC immediately followed by \r — the legacy Alt+Enter
             // encoding.
             EXPECT_EQ(::write(master, "\033\r", 2), 2);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "cd", 2), 2);
             output = read_new_output(master, std::move(output));
             // Plain Enter still submits — it must not have been
             // reinterpreted.
             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             // The child's dprintf writes result.text (containing the real \n
             // Alt+Enter inserted) back through the pty's own *output*
             // processing, which still has ONLCR enabled (RawMode only
             // touches input flags) — so the embedded \n is observed here as
             // \r\n. The 5-byte cursor count below confirms the buffer itself
             // holds a single \n, not two bytes.
             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":5:ab\r\ncd";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment, test_bare_lf_inserts_newline) {
  run_case("readline: bare LF (Ctrl+J) inserts a newline, same as Alt+Enter -- "
           "many terminals translate Shift+Enter into a raw LF independent of "
           "the Kitty keyboard protocol or tmux's extended-keys forwarding",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result = readline("> ", {}, {}, {}, "", 0);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             EXPECT_EQ(::write(master, "ab", 2), 2);
             output = read_new_output(master, std::move(output));
             // Bare '\n' (Ctrl+J) -- must insert, not submit.
             EXPECT_EQ(::write(master, "\n", 1), 1);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "cd", 2), 2);
             output = read_new_output(master, std::move(output));
             // Plain Enter ('\r') still submits.
             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":5:ab\r\ncd";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment, test_multiline_navigation_and_backspace) {
  run_case("readline: Left/Right cross an embedded newline and Backspace joins "
           "lines",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result = readline("> ", {}, {}, {}, "", 0);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             // Build "ab\ncd" via Alt+Enter; cursor ends at offset 5 (the end).
             EXPECT_EQ(::write(master, "ab", 2), 2);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "\033\r", 2), 2);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "cd", 2), 2);
             output = read_new_output(master, std::move(output));

             // Left x3: 5->4->3->2. The third step crosses the embedded \n
             // backward (from right after it to right before it), landing right
             // after "ab".
             for (int i = 0; i < 3; ++i) {
               EXPECT_EQ(::write(master, "\033[D", 3), 3);
               output = read_new_output(master, std::move(output));
             }
             // Right x1: 2->3, crossing the same \n forward again, landing
             // right after it (right before "cd").
             EXPECT_EQ(::write(master, "\033[C", 3), 3);
             output = read_new_output(master, std::move(output));
             // Backspace at offset 3 erases the \n itself (offset 2), rejoining
             // "ab" and "cd" into "abcd" with the cursor left at offset 2.
             EXPECT_EQ(::write(master, "\x7f", 1), 1);
             output = read_new_output(master, std::move(output));

             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":2:abcd";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment, test_bracketed_paste_is_inert_block_insert) {
  run_case(
      "readline: bracketed paste lands intact and never submits, even with "
      "a trailing newline",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, "", 0);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        // A paste with an embedded newline AND a trailing newline: neither
        // must submit, and both must land in the buffer as literal bytes.
        const std::string paste = "\033[200~line1\nline2\n\033[201~";
        EXPECT_EQ(::write(master, paste.data(), paste.size()),
                  static_cast<ssize_t>(paste.size()));
        output = read_new_output(master, std::move(output));
        EXPECT_TRUE(output.find("RESULT:") == std::string::npos);

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        // See the Alt+Enter test above: the pty's own output processing
        // (ONLCR) renders the buffer's real embedded \n bytes as \r\n here.
        // The :12: cursor count confirms the buffer itself holds 12 bytes
        // (two single-byte \n, not \r\n pairs).
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":12:line1\r\nline2\r\n";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_embedded_newline_cursor_placement) {
  constexpr int kColumns = 10;

  run_case(
      "readline: cursor right before an embedded newline lands on the next "
      "row, not the previous one",
      [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = kColumns;
        int master = -1;
        // "0123456789" exactly fills the first row; cursor sits right at
        // the embedded \n that follows it (offset 10).
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("", {}, {}, {}, "0123456789\nABCDE", 10);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        output = read_until(master, std::move(output), "ABCDE");
        EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));
        // The final fill-and-toggle sequence must reach the reverse-video
        // toggle via a bare "\r" — no vertical move at all, since the
        // cursor's normalized row (start of the second row) matches where
        // painting actually stopped. Before the M1 fix, capturing the
        // pre-break {row 1, column 10} here produced an extra "\033[1A"
        // that painted the cursor block back over the first row's last
        // digit instead. M6's footer hint row (see readline.cpp's
        // InputRenderer::redraw) now sits between the composer's own
        // "\033[K" fill and this "\r\033[7m", so the two are no longer
        // byte-adjacent -- check for "\r\033[7m" and the *absence* of the
        // erroneous vertical move instead of exact adjacency to the fill.
        EXPECT_TRUE(output.find("\r\033[7m") != std::string::npos);
        EXPECT_TRUE(output.find("\033[1A\r\033[7m") == std::string::npos);
        EXPECT_TRUE(output.find("\033[1B\r\033[7m") == std::string::npos);

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });

  run_case("readline: cursor right after an embedded newline lands on the new "
           "row's start",
           [] {
             struct winsize ws {};
             ws.ws_row = 24;
             ws.ws_col = kColumns;
             int master = -1;
             // Cursor sits at offset 11 — right at 'A', the start of the second
             // row.
             const auto child = forkpty(&master, nullptr, nullptr, &ws);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result =
                   readline("", {}, {}, {}, "0123456789\nABCDE", 11);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             output = read_until(master, std::move(output), "ABCDE");
             EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));
             // See the comment on the identical check just above -- M6's footer
             // row now sits between the composer's fill and the toggle.
             EXPECT_TRUE(output.find("\r\033[7m") != std::string::npos);
             EXPECT_TRUE(output.find("\033[1A\r\033[7m") == std::string::npos);
             EXPECT_TRUE(output.find("\033[1B\r\033[7m") == std::string::npos);

             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment, test_composer_height_cap) {
  run_case("readline: composer height cap bounds rendered row bookkeeping "
           "regardless of draft length",
           [] {
             struct winsize ws {};
             ws.ws_row = 40;
             ws.ws_col = 80;
             int master = -1;
             std::string draft;
             constexpr int kLines = 20;
             for (int i = 0; i < kLines; ++i) {
               if (i > 0)
                 draft += '\n';
               draft += "line" + std::to_string(i);
             }
             const auto child = forkpty(&master, nullptr, nullptr, &ws);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result =
                   readline("> ", {}, {}, {}, draft, draft.size());
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             // Wait for the first redraw to complete in full (its trailing
             // "\033[?7h", not just the content partway through).
             output = read_until(master, std::move(output),
                                 "line" + std::to_string(kLines - 1));
             output = read_until(master, std::move(output), "\033[?7h");
             const auto before_size = output.size();

             // One more keystroke triggers a second redraw, whose
             // clear_previous() erases exactly rendered_rows_ rows (one
             // "\033[2K" per row) before repainting — direct evidence of what
             // InputRenderer's own row-count bookkeeping was set to by the
             // draft's (uncapped, 20-row) first render.
             EXPECT_EQ(::write(master, "X", 1), 1);
             output = read_until_from(master, std::move(output), before_size,
                                      "\033[?7h");
             const auto second_redraw = output.substr(before_size);
             EXPECT_EQ(count_occurrences(second_redraw, "\033[2K"),
                       pi::core::kMaxComposerRows);

             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment, test_word_wrap_boundary) {
  constexpr int kColumns = 10;

  run_case("readline: typing a long sentence wraps at word boundaries, not "
           "mid-word",
           [] {
             struct winsize ws {};
             ws.ws_row = 24;
             ws.ws_col = kColumns;
             int master = -1;
             // "hello world foo" at 10 columns: "hello" (5) fits row one; the
             // next word "world" doesn't fit alongside it (5+1+5=11 > 10) but
             // does fit a fresh row on its own, so the break falls at the space
             // between them (dropped, not carried over) rather than mid-word.
             // "world foo" (9 columns) then fits together on the second row.
             const auto child = forkpty(&master, nullptr, nullptr, &ws);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result =
                   readline("", {}, {}, {}, "hello world foo", 15);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             output = read_until(master, std::move(output), "foo");

             // The renderer emits "\033[K\r\n" for a row transition; ONLCR is
             // still enabled on the pty's output side (RawMode only touches
             // input flags, same as the embedded-\n tests above), so the raw
             // '\n' byte lands here as an *additional* "\r\n" on top of the
             // '\r' already written, observed as "\033[K\r\r\n".
             EXPECT_TRUE(output.find("hello\033[K\r\r\nworld foo") !=
                         std::string::npos);
             // Neither word is ever split across the row-clear/wrap sequence --
             // if it were, "hell" or "worl" would appear immediately followed
             // by it.
             EXPECT_TRUE(output.find("hell\033[K\r\r\no") == std::string::npos);
             EXPECT_TRUE(output.find("worl\033[K\r\r\nd") == std::string::npos);
             EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));

             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":15:hello world foo";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });

  run_case("readline: a single overlong token (longer than the row) still "
           "hard-wraps without overflowing",
           [] {
             struct winsize ws {};
             ws.ws_row = 24;
             ws.ws_col = kColumns;
             int master = -1;
             const std::string token(40, 'x'); // no whitespace anywhere in it
             const auto child = forkpty(&master, nullptr, nullptr, &ws);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result =
                   readline("", {}, {}, {}, token, token.size());
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             output =
                 read_until(master, std::move(output), std::string(4, 'x'));
             // Wait for the redraw to finish so all four wrapped rows (40
             // chars / 10 columns) have actually been emitted.
             output = read_until(master, std::move(output), "\033[?7h");
             EXPECT_TRUE(!has_out_of_range_cursor_forward(output, kColumns));
             // The row-clear/wrap marker (see the ONLCR note above for the
             // doubled \r) must appear (repeatedly) inside the unbroken token
             // -- confirming the hard-wrap fallback engaged instead of
             // overflowing a single row with all 40 characters.
             EXPECT_TRUE(count_occurrences(output, "\033[K\r\r\n") >= 3U);

             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

// Runs a readline() session (with the given initial draft/cursor) inside a
// forkpty child, feeds it `input` on the master side, then submits with a
// plain Enter and checks the resulting text/cursor. Used throughout the M3
// editing-binding tests below, which only care about the buffer/cursor state
// after a fixed key sequence, not about the intermediate rendered escapes.
void run_editing_case(std::string name, std::string_view initial_draft,
                      std::size_t initial_cursor, std::string_view input,
                      ReadlineExit expected_reason,
                      const std::string &expected_text,
                      std::size_t expected_cursor) {
  run_case(std::move(name), [=] {
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    EXPECT_TRUE(child >= 0);
    if (child == 0) {
      dprintf(STDOUT_FILENO, "READY\n");
      const auto result =
          readline("> ", {}, {}, {}, initial_draft, initial_cursor);
      dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
              static_cast<int>(result.reason), result.cursor,
              result.text.c_str());
      _exit(0);
    }

    auto output = read_until(master, {}, "READY");
    if (!input.empty()) {
      EXPECT_EQ(::write(master, input.data(), input.size()),
                static_cast<ssize_t>(input.size()));
      output = read_new_output(master, std::move(output));
    }
    EXPECT_EQ(::write(master, "\r", 1), 1);
    output = read_until(master, std::move(output), "RESULT:");
    // The trailing "\r\n" (the dprintf format's own '\n' after "%s", doubled
    // by ONLCR same as everywhere else in this file) anchors the end of the
    // text field -- without it, an expected text that happens to be a
    // prefix of a longer, unfixed actual buffer (e.g. "hello\r\nwo" against
    // an untouched "hello\r\nworld") would false-match via find().
    const auto expected =
        "RESULT:" + std::to_string(static_cast<int>(expected_reason)) + ":" +
        std::to_string(expected_cursor) + ":" + expected_text + "\r\n";
    EXPECT_TRUE(output.find(expected) != std::string::npos);
    EXPECT_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

TEST_F(ReadlineEnvironment, test_up_down_logical_line_navigation) {
  // Buffer: "ab\nc\ndefg" -- line0 "ab" (len 2), line1 "c" (len 1), line2
  // "defg" (len 4). Byte offsets: a0 b1 \n2 c3 \n4 d5 e6 f7 g8 (size 9).
  constexpr std::string_view kBuf = "ab\nc\ndefg";

  run_editing_case(
      "readline: Up from the last line lands on the shorter middle line, "
      "clamped to its length",
      kBuf, 9, "\033[A", ReadlineExit::submitted, "ab\r\nc\r\ndefg", 4);
  run_editing_case(
      "readline: Up twice preserves the original column once a line is "
      "long enough again",
      kBuf, 9, "\033[A\033[A", ReadlineExit::submitted, "ab\r\nc\r\ndefg", 1);
  run_editing_case("readline: Up on the first line is a no-op", kBuf, 9,
                   "\033[A\033[A\033[A", ReadlineExit::submitted,
                   "ab\r\nc\r\ndefg", 1);
  run_editing_case(
      "readline: Down from the first line preserves column onto a shorter "
      "line, clamped",
      kBuf, 0, "\033[B", ReadlineExit::submitted, "ab\r\nc\r\ndefg", 3);
  run_editing_case(
      "readline: Down twice reaches the last line at the preserved column",
      kBuf, 0, "\033[B\033[B", ReadlineExit::submitted, "ab\r\nc\r\ndefg", 5);
  run_editing_case("readline: Down on the last line is a no-op", kBuf, 0,
                   "\033[B\033[B\033[B", ReadlineExit::submitted,
                   "ab\r\nc\r\ndefg", 5);
}

TEST_F(ReadlineEnvironment, test_home_end_and_ctrl_a_e) {
  // "hello\nworld": hello=0-4, \n=5, world: w6 o7 r8 l9 d10 (size 11).
  constexpr std::string_view kBuf = "hello\nworld";

  run_editing_case("readline: Home moves to the current logical line's "
                   "start, not the buffer start",
                   kBuf, 9, "\033[H", ReadlineExit::submitted, "hello\r\nworld",
                   6);
  run_editing_case("readline: End moves to the current logical line's end, "
                   "not the buffer end",
                   kBuf, 6, "\033[F", ReadlineExit::submitted, "hello\r\nworld",
                   11);
  run_editing_case("readline: Home on the first line stops at offset 0", kBuf,
                   2, "\033[H", ReadlineExit::submitted, "hello\r\nworld", 0);
  run_editing_case("readline: End on the first line stops before the "
                   "embedded newline",
                   kBuf, 2, "\033[F", ReadlineExit::submitted, "hello\r\nworld",
                   5);
  run_editing_case(
      "readline: Ctrl+A behaves identically to Home on the second line", kBuf,
      9, "\x01", ReadlineExit::submitted, "hello\r\nworld", 6);
  run_editing_case(
      "readline: Ctrl+E behaves identically to End on the second line", kBuf, 6,
      "\x05", ReadlineExit::submitted, "hello\r\nworld", 11);
}

TEST_F(ReadlineEnvironment, test_ctrl_arrows_scroll_transcript_not_buffer) {
  run_case(
      "readline: Ctrl+Up/Down and Ctrl+Home/End fire transcript scroll "
      "actions and leave the buffer/cursor untouched",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          ControlFn control_fn = [](ControlAction action) {
            dprintf(STDOUT_FILENO, "ACTION:%d\n", static_cast<int>(action));
          };
          const auto result = readline("> ", {}, control_fn, {}, "ab\ncd", 2);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        // Ctrl+Up, Ctrl+Down, Ctrl+Home, Ctrl+End -- one of each, in the
        // xterm-compatible CSI "1;5<letter>" modifier encoding.
        EXPECT_EQ(::write(master, "\033[1;5A", 6), 6);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\033[1;5B", 6), 6);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\033[1;5H", 6), 6);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\033[1;5F", 6), 6);
        output = read_new_output(master, std::move(output));

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");

        const auto up =
            "ACTION:" +
            std::to_string(static_cast<int>(ControlAction::scroll_line_up));
        const auto down =
            "ACTION:" +
            std::to_string(static_cast<int>(ControlAction::scroll_line_down));
        const auto top =
            "ACTION:" +
            std::to_string(static_cast<int>(ControlAction::scroll_top));
        const auto bottom =
            "ACTION:" +
            std::to_string(static_cast<int>(ControlAction::scroll_bottom));
        EXPECT_EQ(count_occurrences(output, up), 1U);
        EXPECT_EQ(count_occurrences(output, down), 1U);
        EXPECT_EQ(count_occurrences(output, top), 1U);
        EXPECT_EQ(count_occurrences(output, bottom), 1U);

        // The buffer and cursor must be exactly as they started -- these
        // keys drove the transcript, not the composer.
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":2:ab\r\ncd";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_word_motion) {
  // "foo bar baz", single-word runs separated by single spaces.
  // f0 o1 o2 sp3 b4 a5 r6 sp7 b8 a9 z10 (size 11).
  constexpr std::string_view kBuf = "foo bar baz";

  run_editing_case("readline: Ctrl+Left steps back one word at a time", kBuf,
                   11, "\033[1;5D", ReadlineExit::submitted, "foo bar baz", 8);
  run_editing_case("readline: Ctrl+Left twice reaches the middle word", kBuf,
                   11, "\033[1;5D\033[1;5D", ReadlineExit::submitted,
                   "foo bar baz", 4);
  run_editing_case("readline: Ctrl+Left three times reaches the first word",
                   kBuf, 11, "\033[1;5D\033[1;5D\033[1;5D",
                   ReadlineExit::submitted, "foo bar baz", 0);
  run_editing_case("readline: Ctrl+Left at the buffer start is a no-op", kBuf,
                   11, "\033[1;5D\033[1;5D\033[1;5D\033[1;5D",
                   ReadlineExit::submitted, "foo bar baz", 0);
  run_editing_case("readline: Ctrl+Right steps forward one word at a time",
                   kBuf, 0, "\033[1;5C", ReadlineExit::submitted, "foo bar baz",
                   3);
  run_editing_case("readline: Ctrl+Right twice reaches the middle word's end",
                   kBuf, 0, "\033[1;5C\033[1;5C", ReadlineExit::submitted,
                   "foo bar baz", 7);
  run_editing_case("readline: Ctrl+Right three times reaches the buffer end",
                   kBuf, 0, "\033[1;5C\033[1;5C\033[1;5C",
                   ReadlineExit::submitted, "foo bar baz", 11);

  // Alt+B / Alt+F: the legacy meta encoding (ESC followed by a literal
  // 'b'/'f'), the same encoding family as Alt+Enter -- a second binding
  // for the same operations, for terminals that don't pass the CSI
  // modifier form through.
  run_editing_case("readline: Alt+B steps back one word, same as Ctrl+Left",
                   kBuf, 11, "\033b", ReadlineExit::submitted, "foo bar baz",
                   8);
  run_editing_case("readline: Alt+B three times reaches the first word", kBuf,
                   11, "\033b\033b\033b", ReadlineExit::submitted,
                   "foo bar baz", 0);
  run_editing_case("readline: Alt+F steps forward one word, same as "
                   "Ctrl+Right",
                   kBuf, 0, "\033f", ReadlineExit::submitted, "foo bar baz", 3);
  run_editing_case("readline: Alt+F three times reaches the buffer end", kBuf,
                   0, "\033f\033f\033f", ReadlineExit::submitted, "foo bar baz",
                   11);
}

TEST_F(ReadlineEnvironment, test_ctrl_w_deletes_word_and_leading_whitespace) {
  // "foo bar   baz": cursor placed right before "baz", after "bar" and its
  // three trailing spaces -- Ctrl+W must delete "bar   " (the word *and*
  // the whitespace run immediately before the cursor), not just "bar".
  run_editing_case(
      "readline: Ctrl+W deletes the word behind the cursor including the "
      "whitespace immediately before it",
      "foo bar   baz", 10, "\x17", ReadlineExit::submitted, "foo baz", 4);
}

TEST_F(ReadlineEnvironment, test_ctrl_u_and_ctrl_k_kill_to_line_boundaries) {
  // "hello\nworld": world starts at offset 6; cursor at 9 sits right after
  // "wor". Ctrl+U kills back to the line start ("wor"), leaving "ld".
  run_editing_case("readline: Ctrl+U kills from the cursor to the current "
                   "line's start",
                   "hello\nworld", 9, "\x15", ReadlineExit::submitted,
                   "hello\r\nld", 6);
  // Same buffer, cursor at 8 (after "wo"): Ctrl+K kills forward to the
  // line's end ("rld"), leaving "wo".
  run_editing_case(
      "readline: Ctrl+K kills from the cursor to the current line's end",
      "hello\nworld", 8, "\x0b", ReadlineExit::submitted, "hello\r\nwo", 8);
}

TEST_F(ReadlineEnvironment, test_ctrl_y_yanks_last_kill_only) {
  run_case(
      "readline: Ctrl+Y yanks only the most recent kill -- the single-slot "
      "buffer overwrites, it does not accumulate",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, "AAAA BBBB", 4);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        output = read_until(master, std::move(output), "AAAA BBBB");
        // Ctrl+W at offset 4 kills "AAAA" (kill buffer: "AAAA"), leaving
        // " BBBB" with the cursor at 0.
        EXPECT_EQ(::write(master, "\x17", 1), 1);
        output = read_new_output(master, std::move(output));
        // Ctrl+K at offset 0 kills the rest of the line, " BBBB" -- this
        // overwrites the kill buffer (now " BBBB", not "AAAA"), leaving an
        // empty buffer.
        EXPECT_EQ(::write(master, "\x0b", 1), 1);
        output = read_new_output(master, std::move(output));
        // Ctrl+Y yanks the kill buffer back. If the first kill had not
        // been overwritten, this would insert "AAAA" instead.
        EXPECT_EQ(::write(master, "\x19", 1), 1);
        output = read_new_output(master, std::move(output));

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":5: BBBB";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

// --- M4: Kitty keyboard protocol probe --------------------------------
//
// Every readline() call sent through a plain forkpty() pty behaves exactly
// like a terminal that never answers the Kitty-protocol query at all: the
// probe (see kitty_keyboard_enabled() in readline.cpp) sends
// "\033[?u\033[c" on the very first RawMode::enter() and, unless a test
// below scripts a reply, simply times out (kKittyProbeTimeout, 300ms)
// having received nothing. To keep the tests above this section fast and
// free of that latency/raciness -- they aren't testing M4 at all -- main()
// sets PICI_DISABLE_KITTY_KEYBOARD=1 for the whole process before running
// any test, which every forked child inherits and which skips the probe
// entirely (see kitty_keyboard_enabled()'s escape-hatch check). The tests
// below undo that in their own forked child, since they specifically want
// to exercise the probe.
//
// The query bytes themselves ("\033[?u\033[c") are only safe to write a
// reply after -- writing anything to the pty before the child's
// RawMode::enter() calls tcsetattr(TCSAFLUSH, ...) risks that call
// discarding it, since TCSAFLUSH flushes unread input. Waiting for the
// query to arrive at the master side guarantees raw mode (and thus
// TCSAFLUSH) has already happened, since RawMode::enter() writes the query
// immediately after tcsetattr succeeds.
constexpr std::string_view kKittyProbeQuery = "\033[?u\033[c";

// Waits for the probe's query bytes to appear in `output`, asserting they
// actually did -- rather than only relying on read_until's own internal
// timeout, which would let a test that scripts a reply to a query that
// never arrives pass vacuously (e.g. against a build that hasn't
// implemented the probe at all yet).
void await_kitty_probe_query(int master, std::string &output) {
  output = read_until(master, std::move(output), kKittyProbeQuery);
  EXPECT_TRUE(output.find(kKittyProbeQuery) != std::string::npos);
}

TEST_F(ReadlineEnvironment,
       test_kitty_probe_unsupported_replays_alt_enter_and_plain_submit) {
  run_case("readline: Kitty probe with only a DA1 reply concludes unsupported "
           "and behaves exactly like M1-M3 (Alt+Enter newline, plain Enter "
           "submits, no hang)",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               ::unsetenv("PICI_DISABLE_KITTY_KEYBOARD");
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result = readline("> ", {}, {}, {}, "", 0);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             await_kitty_probe_query(master, output);
             // Answer only the DA1 sentinel -- a plausible xterm-style DA1
             // reply -- never the Kitty flags query itself.
             static constexpr std::string_view kDa1Reply = "\033[?62;1;2;6c";
             EXPECT_EQ(::write(master, kDa1Reply.data(), kDa1Reply.size()),
                       static_cast<ssize_t>(kDa1Reply.size()));
             output = read_new_output(master, std::move(output));

             EXPECT_EQ(::write(master, "ab", 2), 2);
             output = read_new_output(master, std::move(output));
             // ESC immediately followed by \r -- the legacy Alt+Enter encoding,
             // still the only newline binding when the protocol isn't
             // supported.
             EXPECT_EQ(::write(master, "\033\r", 2), 2);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "cd", 2), 2);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");

             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":5:ab\r\ncd";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             // Unsupported means the "disambiguate escape codes" flag is never
             // pushed.
             EXPECT_TRUE(output.find("\033[>1u") == std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment,
       test_kitty_probe_supported_shift_enter_inserts_newline) {
  run_case(
      "readline: Kitty probe with a flags reply before DA1 concludes "
      "supported -- Shift+Enter inserts a newline, CSI-encoded plain Enter "
      "still submits",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          ::unsetenv("PICI_DISABLE_KITTY_KEYBOARD");
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, "", 0);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        await_kitty_probe_query(master, output);
        // Flags reply (progressive-enhancement flag 1, "disambiguate
        // escape codes") followed by the DA1 sentinel, in that order --
        // this is what makes the probe conclude "supported".
        static constexpr std::string_view kSupportedReply = "\033[?1u\033[?62c";
        EXPECT_EQ(
            ::write(master, kSupportedReply.data(), kSupportedReply.size()),
            static_cast<ssize_t>(kSupportedReply.size()));
        output = read_new_output(master, std::move(output));
        // Supported means the flag actually gets pushed on raw-mode entry.
        EXPECT_TRUE(output.find("\033[>1u") != std::string::npos);

        EXPECT_EQ(::write(master, "ab", 2), 2);
        output = read_new_output(master, std::move(output));
        // Shift+Enter: codepoint 13, modifier field 2 (Shift, 1-biased).
        static constexpr std::string_view kShiftEnter = "\033[13;2u";
        EXPECT_EQ(::write(master, kShiftEnter.data(), kShiftEnter.size()),
                  static_cast<ssize_t>(kShiftEnter.size()));
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "cd", 2), 2);
        output = read_new_output(master, std::move(output));
        // Plain Enter via the CSI-u encoding (no modifier section) --
        // exercises the same submit path a bare '\r' would, but through the
        // new decode.
        static constexpr std::string_view kPlainEnter = "\033[13u";
        EXPECT_EQ(::write(master, kPlainEnter.data(), kPlainEnter.size()),
                  static_cast<ssize_t>(kPlainEnter.size()));
        output = read_until(master, std::move(output), "RESULT:");

        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":5:ab\r\ncd";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        // The flag is popped again on the way out (raw.leave(), called from
        // finish()).
        EXPECT_TRUE(output.find("\033[<u") != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_kitty_probe_typeahead_survives_probe_window) {
  run_case("readline: type-ahead read during the Kitty probe's window is "
           "replayed, not dropped",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               ::unsetenv("PICI_DISABLE_KITTY_KEYBOARD");
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result = readline("> ", {}, {}, {}, "", 0);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             await_kitty_probe_query(master, output);
             // A fast typist's keystrokes, arriving while the probe is still
             // reading, before either reply is sent.
             EXPECT_EQ(::write(master, "hi", 2), 2);
             // Now let the probe conclude (unsupported is enough to prove
             // replay -- the DA1 reply on its own already exercises the "give
             // up waiting" path).
             static constexpr std::string_view kDa1Reply = "\033[?62c";
             EXPECT_EQ(::write(master, kDa1Reply.data(), kDa1Reply.size()),
                       static_cast<ssize_t>(kDa1Reply.size()));
             output = read_new_output(master, std::move(output));

             EXPECT_EQ(::write(master, "cd", 2), 2);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");

             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":4:hicd";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment,
       test_kitty_disable_env_var_skips_probe_and_forces_fallback) {
  run_case(
      "readline: PICI_DISABLE_KITTY_KEYBOARD forces the Alt+Enter fallback "
      "and skips the probe outright, even though nothing here proves the "
      "probe would otherwise have concluded supported",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          // Explicit for this test's own documentation, even though
          // main() already sets this process-wide by default.
          ::setenv("PICI_DISABLE_KITTY_KEYBOARD", "1", 1);
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, "", 0);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        EXPECT_EQ(::write(master, "ab", 2), 2);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\033\r", 2), 2);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "cd", 2), 2);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");

        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":5:ab\r\ncd";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        // The override skips the probe outright -- no query is ever sent --
        // and the flag is never pushed either.
        EXPECT_TRUE(output.find(kKittyProbeQuery) == std::string::npos);
        EXPECT_TRUE(output.find("\033[>1u") == std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_kitty_supported_existing_bindings_unaffected) {
  run_case(
      "readline: arrow keys, Ctrl+Left/Right, mouse wheel, and bracketed "
      "paste all still work identically once the Kitty flag is active",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          ::unsetenv("PICI_DISABLE_KITTY_KEYBOARD");
          dprintf(STDOUT_FILENO, "READY\n");
          ControlFn control_fn = [](ControlAction action) {
            dprintf(STDOUT_FILENO, "ACTION:%d\n", static_cast<int>(action));
          };
          const auto result = readline("> ", {}, control_fn, {}, "", 0);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        await_kitty_probe_query(master, output);
        static constexpr std::string_view kSupportedReply = "\033[?1u\033[?62c";
        EXPECT_EQ(
            ::write(master, kSupportedReply.data(), kSupportedReply.size()),
            static_cast<ssize_t>(kSupportedReply.size()));
        output = read_new_output(master, std::move(output));
        EXPECT_TRUE(output.find("\033[>1u") != std::string::npos);

        EXPECT_EQ(::write(master, "ab", 2), 2);
        output = read_new_output(master, std::move(output));
        // Plain Left, then Ctrl+Left: still character-left then word-left,
        // exactly as when the flag is inactive -- "ab" is cursor 1 then 0.
        EXPECT_EQ(::write(master, "\033[D", 3), 3);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\033[1;5D", 6), 6);
        output = read_new_output(master, std::move(output));
        // Plain Right, then Ctrl+Right: back to cursor 1 then 2.
        EXPECT_EQ(::write(master, "\033[C", 3), 3);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\033[1;5C", 6), 6);
        output = read_new_output(master, std::move(output));
        // SGR mouse wheel-up: three scroll_line_up actions, no buffer
        // change.
        EXPECT_EQ(::write(master, "\033[<64;10;5M", 11), 11);
        output = read_new_output(master, std::move(output));
        // Bracketed paste: still an inert block insert.
        static constexpr std::string_view kPaste = "\033[200~XY\033[201~";
        EXPECT_EQ(::write(master, kPaste.data(), kPaste.size()),
                  static_cast<ssize_t>(kPaste.size()));
        output = read_new_output(master, std::move(output));

        static constexpr std::string_view kPlainEnter = "\033[13u";
        EXPECT_EQ(::write(master, kPlainEnter.data(), kPlainEnter.size()),
                  static_cast<ssize_t>(kPlainEnter.size()));
        output = read_until(master, std::move(output), "RESULT:");

        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":4:abXY";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        const auto up_marker =
            "ACTION:" +
            std::to_string(static_cast<int>(ControlAction::scroll_line_up));
        EXPECT_EQ(count_occurrences(output, up_marker), 3U);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

// --- M5: vim mode ----------------------------------------------------------
//
// End-to-end coverage against a real terminal for the parts of vim mode
// that only make sense wired into readline()'s own dispatch loop: Escape
// entering Normal mode, i/a/A/I returning to Insert, an uncovered
// Normal-mode key being a safe no-op, and vim_mode=false leaving M0-M4
// behavior completely unchanged. Pure motion/operator logic (h j k l w b e
// 0 $, d/c, dd/cc, including the e-vs-w distinction) is covered directly
// against VimEngine, no pty needed, in test_vim_mode.cpp.

TEST_F(ReadlineEnvironment,
       test_vim_mode_escape_enters_normal_and_h_moves_without_inserting) {
  run_case(
      "vim mode: Escape enters Normal mode -- h moves the cursor instead of "
      "inserting the letter 'h'; i returns to Insert",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result =
              readline("> ", {}, {}, {}, "", 0, -1, false, {}, true);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        EXPECT_EQ(::write(master, "ab", 2), 2);
        output = read_new_output(master, std::move(output));
        // Bare Escape: enters Normal mode, no visible change.
        EXPECT_EQ(::write(master, "\x1b", 1), 1);
        // read_escape_sequence's own short poll window (25ms) has to expire
        // before this is recognized as a *bare* Escape rather than the
        // start of some other sequence -- give it a moment before sending
        // the next byte, matching test_escape_wake's same requirement.
        poll(nullptr, 0, 60);
        // 'h': a vim motion, not a literal character -- if this were
        // mistakenly inserted as text instead of intercepted, the
        // submitted buffer below would read "abh" instead of "aXb".
        EXPECT_EQ(::write(master, "h", 1), 1);
        output = read_new_output(master, std::move(output));
        // 'i': insert before cursor (now at offset 1, between "a" and
        // "b") -- returns to Insert mode without moving the cursor.
        EXPECT_EQ(::write(master, "i", 1), 1);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "X", 1), 1);
        output = read_new_output(master, std::move(output));

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":2:aXb";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_vim_mode_dd_deletes_whole_line_end_to_end) {
  run_case(
      "vim mode: dd deletes the whole current line against a real terminal, "
      "and plain Enter still submits from Normal mode",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          // Cursor starts at offset 6, the start of "line2".
          const auto result = readline("> ", {}, {}, {}, "line1\nline2\nline3",
                                       6, -1, false, {}, true);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "line3");
        EXPECT_EQ(::write(master, "\x1b", 1), 1);
        poll(nullptr, 0, 60);
        EXPECT_EQ(::write(master, "dd", 2), 2);
        output = read_new_output(master, std::move(output));

        // Plain Enter submits even while still in Normal mode -- a
        // deliberate scope decision (see composer-textarea-rewrite.md M5
        // and this test file's header comment): submit/EOF stay live in
        // every mode so the user is never trapped needing to press 'i'
        // first just to send a message.
        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":6:line1\r\nline3";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_vim_mode_c_operator_enters_insert_mode) {
  run_case(
      "vim mode: c<motion> deletes the span and drops straight into Insert "
      "mode -- no separate 'i' needed before typing",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, "hello world", 11, -1,
                                       false, {}, true);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "world");
        EXPECT_EQ(::write(master, "\x1b", 1), 1);
        poll(nullptr, 0, 60);
        EXPECT_EQ(::write(master, "0", 1), 1); // line start
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "cw", 2), 2); // change "hello " -> insert
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "X", 1), 1); // now in Insert mode
        output = read_new_output(master, std::move(output));

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":1:Xworld";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment, test_vim_mode_uncovered_key_is_safe_noop) {
  run_case(
      "vim mode: an uncovered Normal-mode key ('x', not implemented in this "
      "milestone's scope) neither edits the buffer nor gets inserted as "
      "text",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result =
              readline("> ", {}, {}, {}, "", 0, -1, false, {}, true);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        EXPECT_EQ(::write(master, "hi", 2), 2);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\x1b", 1), 1);
        poll(nullptr, 0, 60);
        EXPECT_EQ(::write(master, "x", 1), 1);
        output = read_new_output(master, std::move(output));

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        // If 'x' had fallen through to plain-insert instead of being
        // swallowed as an uncovered Normal-mode key, this would read
        // "hix" instead.
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) + ":2:hi";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment,
       test_vim_mode_arrow_keys_unaffected_in_normal_mode) {
  run_case("vim mode: escape-sequence bindings (plain Left/Right) keep working "
           "identically in Normal mode -- only single-byte keys are vim's to "
           "intercept",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result =
                   readline("> ", {}, {}, {}, "ab", 2, -1, false, {}, true);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             EXPECT_EQ(::write(master, "\x1b", 1), 1);
             poll(nullptr, 0, 60);
             // Plain Left (CSI "[D"), the ordinary arrow-key escape sequence --
             // not a vim single-byte key -- still moves the cursor exactly as
             // it would without vim mode.
             EXPECT_EQ(::write(master, "\033[D", 3), 3);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "i", 1),
                       1); // back to Insert at offset 1
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "X", 1), 1);
             output = read_new_output(master, std::move(output));

             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":2:aXb";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment, test_vim_mode_false_leaves_hjkl_as_literal_text) {
  run_case("vim mode: with vim_mode=false (the default), h/j/k/l are inserted "
           "as literal characters -- M0-M4 behavior is completely unaffected",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result =
                   readline("> ", {}, {}, {}, "", 0, -1, false, {}, false);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             EXPECT_EQ(::write(master, "hjkl", 4), 4);
             output = read_new_output(master, std::move(output));

             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             const auto expected =
                 "RESULT:" +
                 std::to_string(static_cast<int>(ReadlineExit::submitted)) +
                 ":4:hjkl";
             EXPECT_TRUE(output.find(expected) != std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

// --- M6: visual polish --------------------------------------------------
//
// Footer hint row tests exercise InputRenderer::redraw's new row painted
// below the composer (mirrors status_line_'s existing row above it) and
// specifically the acceptance criterion the plan calls out: it must not
// desync rendered_rows_/clear_previous()'s relative-motion erase
// bookkeeping. These don't touch COLORTERM (left unset by main() below),
// so the OSC 11 background-tint probe never fires here -- these are about
// the footer row, not the tint.

TEST_F(ReadlineEnvironment, test_footer_hint_shown_and_bookkept) {
  run_case(
      "readline: footer hint row is drawn below the composer and folded "
      "into rendered_rows_ -- a later redraw's clear_previous() erases "
      "exactly one row for the composer and one for the footer",
      [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = 80;
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, "", 0);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        // Wait for the first redraw to finish in full (its trailing
        // "\033[?7h", not just the content partway through).
        output = read_until(master, std::move(output), "\033[?7h");
        EXPECT_TRUE(output.find("Alt+Enter: newline") != std::string::npos);
        // rendered_rows_ was 0 before this first paint, so clear_previous()
        // was a no-op -- no erase yet.
        EXPECT_EQ(count_occurrences(output, "\033[2K"), 0U);
        const auto before_size = output.size();

        // A keystroke triggers a second redraw: clear_previous() now has to
        // erase both the composer's own row and the footer's row from the
        // first paint -- exactly 2 rows, one "\033[2K" each, if the footer
        // was correctly folded into rendered_rows_.
        EXPECT_EQ(::write(master, "a", 1), 1);
        output =
            read_until_from(master, std::move(output), before_size, "\033[?7h");
        const auto second_redraw = output.substr(before_size);
        EXPECT_EQ(count_occurrences(second_redraw, "\033[2K"), 2U);
        EXPECT_TRUE(second_redraw.find("Alt+Enter: newline") !=
                    std::string::npos);

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) + ":1:a";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

TEST_F(ReadlineEnvironment,
       test_footer_hint_hidden_when_composer_at_height_cap) {
  run_case(
      "readline: footer hint row is omitted once the composer is already "
      "using its full core::kMaxComposerRows budget, so it never grows the "
      "box past the cap or past region_renderer.cpp's fixed reservation",
      [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = 80;
        int master = -1;
        // Exactly core::kMaxComposerRows (6) lines, no wrapping at 80
        // columns -- the composer is already at its height cap with no
        // spare row for the footer.
        const std::string draft = "l0\nl1\nl2\nl3\nl4\nl5";
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, draft, draft.size());
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        output = read_until(master, std::move(output), "\033[?7h");
        EXPECT_TRUE(output.find("l5") != std::string::npos);
        EXPECT_TRUE(output.find("Alt+Enter: newline") == std::string::npos);

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        EXPECT_TRUE(output.find("RESULT:" + std::to_string(static_cast<int>(
                                                ReadlineExit::submitted))) !=
                    std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

// Minimal ANSI/VT100 terminal-state simulator -- just enough to track
// absolute cursor (row, column) and on-screen content through the escape
// sequences InputRenderer::redraw() actually emits: relative cursor motion
// (CSI A/B/C/D), line erase (CSI K), DECSC/DECRC ("\0337"/"\0338"), '\r',
// and '\n' -- including '\n' triggering a real scroll when the cursor is
// already on the terminal's last row, exactly like a real terminal. SGR,
// private-mode (?7h/?7l, ?2004h, etc.), and OSC sequences are recognized
// just enough to be skipped without disturbing cursor/content tracking.
//
// This exists to verify, independent of the C++ implementation's own
// internal row bookkeeping (rendered_rows_/cursor_row_), where the edit
// cursor and screen content actually end up after a real terminal
// interprets the bytes -- the same kind of check a real terminal emulator
// would let you make visually. See
// test_footer_scroll_at_terminal_bottom_does_not_desync_cursor below.
class MiniTerminal {
public:
  MiniTerminal(int width, int height)
      : width_(width),
        rows_(static_cast<std::size_t>(height),
              std::string(static_cast<std::size_t>(width), ' ')) {}

  void feed(std::string_view data) {
    for (std::size_t i = 0; i < data.size();) {
      const auto c = static_cast<unsigned char>(data[i]);
      if (c == 0x1b) {
        i += handle_escape(data.substr(i));
        continue;
      }
      if (c == '\r') {
        col_ = 0;
        ++i;
        continue;
      }
      if (c == '\n') {
        newline();
        ++i;
        continue;
      }
      // UTF-8 continuation bytes (0x80-0xBF) don't advance the column --
      // only each codepoint's lead byte does. Good enough for the one
      // multi-byte character (the footer's "\xc2\xb7") this ever needs to
      // track.
      if ((c & 0xc0) != 0x80) {
        if (row_ < static_cast<int>(rows_.size()) && col_ < width_)
          rows_[static_cast<std::size_t>(row_)]
               [static_cast<std::size_t>(col_)] = static_cast<char>(c);
        if (col_ + 1 < width_)
          ++col_;
      }
      ++i;
    }
  }

  int cursor_row() const { return row_; }
  const std::string &row(int r) const {
    return rows_[static_cast<std::size_t>(r)];
  }
  int height() const { return static_cast<int>(rows_.size()); }

  // Every row's content, joined for a substring/count search across the
  // whole visible screen (e.g. "does the footer text appear more than
  // once anywhere on screen").
  std::string screen_text() const {
    std::string joined;
    for (const auto &r : rows_) {
      joined += r;
      joined += '\n';
    }
    return joined;
  }

private:
  // 0-indexed, inclusive top/bottom of the current DECSTBM scroll region --
  // defaults to the whole screen, matching every test above this one (none
  // of them ever sets a narrower region, since plain readline() alone never
  // emits DECSTBM at all -- only RegionRenderer does). With the default,
  // every branch below that checks "is row_ inside the region" is trivially
  // true everywhere, so behavior for those tests is unchanged from before
  // this region-awareness was added.
  int scroll_top() const { return scroll_top_; }
  int scroll_bottom() const {
    return scroll_bottom_ < 0 ? static_cast<int>(rows_.size()) - 1
                              : scroll_bottom_;
  }

  void newline() {
    const int top = scroll_top();
    const int bottom = scroll_bottom();
    if (row_ >= top && row_ <= bottom) {
      // Cursor is inside the active scroll region (or the region is the
      // whole screen, the default -- see the member comment above).
      if (row_ == bottom) {
        scroll_region_up(top, bottom, 1);
        return;
      }
      ++row_;
      return;
    }
    // Cursor is outside the active scroll region entirely -- e.g. the
    // composer rows below RegionRenderer's DECSTBM-constrained transcript
    // region. A real terminal never scrolls anything on account of a
    // linefeed out here; the cursor just moves down, clamped at the
    // terminal's own physical last row (never at the region's boundary,
    // since it isn't in the region).
    if (row_ + 1 < static_cast<int>(rows_.size()))
      ++row_;
  }

  // Shifts rows [top, bottom] (0-indexed, inclusive) up by `count` lines,
  // filling the vacated rows at the bottom with blanks -- SU (CSI n S) and
  // an at-bottom linefeed inside a scroll region both funnel through this.
  void scroll_region_up(int top, int bottom, int count) {
    for (int step = 0; step < count; ++step) {
      for (int r = top; r < bottom; ++r)
        rows_[static_cast<std::size_t>(r)] =
            rows_[static_cast<std::size_t>(r + 1)];
      rows_[static_cast<std::size_t>(bottom)] =
          std::string(static_cast<std::size_t>(width_), ' ');
    }
  }

  // Shifts rows [top, bottom] (0-indexed, inclusive) down by `count` lines,
  // filling the vacated rows at the top with blanks -- SD (CSI n T).
  void scroll_region_down(int top, int bottom, int count) {
    for (int step = 0; step < count; ++step) {
      for (int r = bottom; r > top; --r)
        rows_[static_cast<std::size_t>(r)] =
            rows_[static_cast<std::size_t>(r - 1)];
      rows_[static_cast<std::size_t>(top)] =
          std::string(static_cast<std::size_t>(width_), ' ');
    }
  }

  // Returns the number of bytes consumed starting at data[0] == ESC.
  std::size_t handle_escape(std::string_view data) {
    if (data.size() < 2)
      return 1;
    const char kind = data[1];
    if (kind == '7') { // DECSC
      saved_row_ = row_;
      saved_col_ = col_;
      has_saved_ = true;
      return 2;
    }
    if (kind == '8') { // DECRC -- restores the ABSOLUTE saved position,
                       // same as a real terminal: if a scroll happened
                       // between the save and here, this does NOT track
                       // it (that's the bug under test).
      if (has_saved_) {
        row_ = saved_row_;
        col_ = saved_col_;
      }
      return 2;
    }
    if (kind == '[') {
      std::size_t end = 2;
      while (end < data.size() && !(data[end] >= '@' && data[end] <= '~'))
        ++end;
      if (end >= data.size())
        return data.size();
      const char final_byte = data[end];
      int n = 0;
      bool has_n = false;
      for (std::size_t p = 2; p < end; ++p) {
        if (data[p] >= '0' && data[p] <= '9') {
          n = n * 10 + (data[p] - '0');
          has_n = true;
        }
      }
      switch (final_byte) {
      case 'A':
        row_ = std::max(0, row_ - (has_n ? n : 1));
        break;
      case 'B':
        row_ = std::min(static_cast<int>(rows_.size()) - 1,
                        row_ + (has_n ? n : 1));
        break;
      case 'C':
        col_ = std::min(width_ - 1, col_ + (has_n ? n : 1));
        break;
      case 'D':
        col_ = std::max(0, col_ - (has_n ? n : 1));
        break;
      case 'K': {
        auto &r = rows_[static_cast<std::size_t>(row_)];
        if (n == 2)
          r.assign(static_cast<std::size_t>(width_), ' ');
        else
          for (int c = col_; c < width_; ++c)
            r[static_cast<std::size_t>(c)] = ' ';
        break;
      }
      case 'H':
      case 'f': {
        // CUP/HVP ("row;colH", either half optional, defaulting to 1) --
        // needed to track RegionRenderer's absolute-addressed writes
        // (position_prompt_cursor(), status/content row painting), unlike
        // the plain-readline()-only tests above that never emit this
        // sequence. Split on ';' explicitly instead of the single running
        // accumulator the other cases share -- that accumulator never
        // resets across a parameter boundary, which is harmless for every
        // other case here (all single-parameter) but would silently
        // concatenate "row;col" into one bogus number.
        int one = 0;
        int two = 0;
        bool have_one = false;
        bool have_two = false;
        bool second = false;
        for (std::size_t p = 2; p < end; ++p) {
          if (data[p] == ';') {
            second = true;
            continue;
          }
          if (data[p] < '0' || data[p] > '9')
            continue;
          if (second) {
            two = two * 10 + (data[p] - '0');
            have_two = true;
          } else {
            one = one * 10 + (data[p] - '0');
            have_one = true;
          }
        }
        const int target_row = have_one ? one : 1;
        const int target_col = have_two ? two : 1;
        row_ =
            std::clamp(target_row - 1, 0, static_cast<int>(rows_.size()) - 1);
        col_ = std::clamp(target_col - 1, 0, width_ - 1);
        break;
      }
      case 'r': {
        // DECSTBM ("top;bottomr", both optional -- defaults to 1 and the
        // terminal's own last row respectively) -- needed to confine
        // region_renderer.cpp's content-area repaints and diff_region_rows's
        // S/T scroll-shift optimization the same way a real terminal would,
        // so a test can tell "content painting stayed inside its region"
        // from "it reached rows outside it" (e.g. the composer's rows).
        // Same two-param split-on-';' parsing as 'H'/'f' above.
        int one = 0;
        int two = 0;
        bool have_one = false;
        bool have_two = false;
        bool second = false;
        for (std::size_t p = 2; p < end; ++p) {
          if (data[p] == ';') {
            second = true;
            continue;
          }
          if (data[p] < '0' || data[p] > '9')
            continue;
          if (second) {
            two = two * 10 + (data[p] - '0');
            have_two = true;
          } else {
            one = one * 10 + (data[p] - '0');
            have_one = true;
          }
        }
        const int last_row = static_cast<int>(rows_.size()) - 1;
        scroll_top_ = have_one ? std::clamp(one - 1, 0, last_row) : 0;
        scroll_bottom_ =
            have_two ? std::clamp(two - 1, scroll_top_, last_row) : last_row;
        break;
      }
      case 'S': // SU -- scroll the active region up by n (default 1).
        scroll_region_up(scroll_top(), scroll_bottom(), has_n ? n : 1);
        break;
      case 'T': // SD -- scroll the active region down by n (default 1).
        scroll_region_down(scroll_top(), scroll_bottom(), has_n ? n : 1);
        break;
      default:
        break; // SGR (m), private modes (h/l), etc. -- no cursor/content
               // effect this simulator needs to track.
      }
      return end + 1;
    }
    if (kind == ']') { // OSC -- skip to BEL or ST, neither ever appears in
                       // what redraw() itself emits, but the Kitty/OSC-11
                       // capability probe queries do, so tests that start
                       // from a bare "READY" (probe not yet disabled in
                       // the child) still parse cleanly.
      std::size_t end = 2;
      while (end < data.size() && data[end] != '\007') {
        if (data[end] == '\033' && end + 1 < data.size() &&
            data[end + 1] == '\\')
          return end + 2;
        ++end;
      }
      return end < data.size() ? end + 1 : data.size();
    }
    return 2; // Unrecognized single-char escape (e.g. a bare ESC) -- skip.
  }

  int width_;
  std::vector<std::string> rows_;
  int row_{0};
  int col_{0};
  bool has_saved_{false};
  int saved_row_{0};
  int saved_col_{0};
  // 0-indexed; scroll_bottom_ of -1 means "not yet set by DECSTBM", read as
  // the terminal's current last row via the scroll_bottom() accessor above.
  int scroll_top_{0};
  int scroll_bottom_{-1};
};

TEST_F(ReadlineEnvironment,
       test_footer_scroll_at_terminal_bottom_does_not_desync_cursor) {
  run_case(
      "readline: when the composer's own last row lands on the terminal's "
      "physical last row, painting the footer hint below it must not "
      "desync the edit cursor or leave stale content behind once the "
      "composer's row count changes -- regression test for the DECSC/DECRC "
      "vs. terminal-scroll bug (footer text appeared doubled when the row "
      "count contracted)",
      [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = 80;
        int master = -1;
        // 19 leading newlines in the prompt (on top of the "READY\n" the
        // child prints first) puts the prompt's own row at terminal row
        // 20 (0-indexed 19) of this 24-row pty. A single unbroken 228-char
        // draft line (no embedded '\n', so word-wrap owns every row break)
        // word-wraps to exactly 3 more rows below that -- empirically,
        // for a draft this long relative to the 80-column width, the
        // planner starts the draft on its own row rather than packing any
        // of it onto the prompt's "> " row, so the composer occupies rows
        // 19-22 (0-indexed) and the footer -- still under
        // core::kMaxComposerRows (6), so it shows -- lands on row 23: this
        // terminal's own last row. That's the exact geometry that used to
        // trigger the bug: the footer's "\r\n" transition has nowhere to
        // go there and scrolls the whole screen, which DECSC/DECRC (an
        // ABSOLUTE position save/restore) doesn't account for.
        const std::string prompt = std::string(19, '\n') + "> ";
        const std::string draft(228, 'x');
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline(prompt, {}, {}, "", draft, draft.size());
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        // Wait for the first redraw to finish in full.
        output = read_until(master, std::move(output), "\033[?7h");
        EXPECT_TRUE(output.find("Alt+Enter: newline") != std::string::npos);

        MiniTerminal term(80, 24);
        term.feed(output);
        // The edit cursor must land on the composer's own last content
        // row (which holds draft text -- 'x' characters), never on the
        // footer's own row, regardless of whether painting the footer
        // scrolled the screen out from under an absolute DECSC/DECRC
        // restore.
        EXPECT_TRUE(term.cursor_row() >= 0 &&
                    term.cursor_row() < term.height());
        const auto &cursor_row_text = term.row(term.cursor_row());
        EXPECT_TRUE(cursor_row_text.find('x') != std::string::npos);
        EXPECT_TRUE(cursor_row_text.find("Alt+Enter") == std::string::npos);
        // Exactly one copy of the footer text is visible on screen after
        // the first paint.
        EXPECT_EQ(count_occurrences(term.screen_text(), "Alt+Enter: newline"),
                  1U);

        // Now shrink the composer abruptly: Ctrl+U kills from the start of
        // the current logical line to the cursor -- with no embedded '\n'
        // in this draft, that's the whole 228-character buffer, collapsing
        // the composer from 3 content rows to 0 in a single redraw. This
        // is the "row count contracts" step from the bug report.
        const auto before_shrink = output.size();
        EXPECT_EQ(::write(master, "\x15", 1), 1);
        output = read_until_from(master, std::move(output), before_shrink,
                                 "\033[?7h");

        term.feed(output.substr(before_shrink));
        // After the shrink, the composer is back to just the empty
        // prompt row and the footer immediately below it -- nowhere near
        // the terminal's bottom, so no further scroll should be in play.
        // If the earlier scroll had desynced rendered_rows_/cursor_row_
        // from where the physical cursor really was, this redraw's
        // clear_previous() would erase the wrong rows and/or the footer
        // paint would land somewhere stale, leaving more than one visible
        // copy of the footer text on screen -- the reported "doubled"
        // symptom. There must still be exactly one.
        EXPECT_EQ(count_occurrences(term.screen_text(), "Alt+Enter: newline"),
                  1U);
        EXPECT_TRUE(term.cursor_row() >= 0 &&
                    term.cursor_row() < term.height());
        EXPECT_TRUE(term.row(term.cursor_row()).find("Alt+Enter") ==
                    std::string::npos);

        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) + ":0:";
        EXPECT_TRUE(output.find(expected) != std::string::npos);
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

// OSC 11 background-color probe tests. Written in one combined query with
// the same DA1 sentinel the Kitty probe uses (see
// compute_input_area_background() in readline.cpp), so these follow the
// exact same await-query/script-reply shape as the Kitty probe tests above
// -- the Kitty probe itself stays disabled throughout (main() below sets
// PICI_DISABLE_KITTY_KEYBOARD=1 process-wide and these tests don't undo
// it), so it never sends its own query to confuse these assertions.
constexpr std::string_view kOsc11ProbeQuery = "\033]11;?\007\033[c";

void await_osc11_probe_query(int master, std::string &output) {
  output = read_until(master, std::move(output), kOsc11ProbeQuery);
  EXPECT_TRUE(output.find(kOsc11ProbeQuery) != std::string::npos);
}

TEST_F(ReadlineEnvironment,
       test_osc11_probe_skipped_without_truecolor_colorterm) {
  run_case("readline: without COLORTERM=truecolor/24bit, the OSC 11 background "
           "query is never sent at all and the fixed \\033[100m tint is used",
           [] {
             int master = -1;
             const auto child = forkpty(&master, nullptr, nullptr, nullptr);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               ::unsetenv("COLORTERM");
               dprintf(STDOUT_FILENO, "READY\n");
               const auto result = readline("> ", {}, {}, {}, "", 0);
               dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                       static_cast<int>(result.reason), result.cursor,
                       result.text.c_str());
               _exit(0);
             }

             auto output = read_until(master, {}, "READY");
             output = read_until(master, std::move(output), "\033[?7h");
             EXPECT_TRUE(output.find("\033[100m") != std::string::npos);
             EXPECT_TRUE(output.find("\033]11;?") == std::string::npos);

             EXPECT_EQ(::write(master, "x", 1), 1);
             output = read_new_output(master, std::move(output));
             EXPECT_EQ(::write(master, "\r", 1), 1);
             output = read_until(master, std::move(output), "RESULT:");
             EXPECT_TRUE(
                 output.find("RESULT:" + std::to_string(static_cast<int>(
                                             ReadlineExit::submitted))) !=
                 std::string::npos);
             EXPECT_EQ(wait_for_child(child), 0);
             ::close(master);
           });
}

TEST_F(ReadlineEnvironment, test_osc11_probe_supported_blends_truecolor_tint) {
  run_case(
      "readline: COLORTERM=truecolor plus a scripted OSC 11 reply arriving "
      "before DA1 makes the composer's tint a blended truecolor "
      "\\033[48;2;r;g;bm sequence instead of the fixed fallback",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          ::setenv("COLORTERM", "truecolor", 1);
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("> ", {}, {}, {}, "", 0);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        await_osc11_probe_query(master, output);
        // A dark background (r=g=0x1e, b=0x22 -- decimal 30/30/34),
        // ST-terminated ("\033\\") rather than the BEL terminator the query
        // used -- exercises "accept either terminator in the reply" -- then
        // the DA1 sentinel immediately after.
        static constexpr std::string_view kBackgroundReply =
            "\033]11;rgb:1e1e/1e1e/2222\033\\\033[?62c";
        EXPECT_EQ(
            ::write(master, kBackgroundReply.data(), kBackgroundReply.size()),
            static_cast<ssize_t>(kBackgroundReply.size()));
        output = read_until(master, std::move(output), "\033[?7h");

        // Dark background, so each channel is nudged 12% toward white:
        // 30 + (255-30)*0.12 = 57 (r, g); 34 + (255-34)*0.12 = 60.52,
        // rounds to 61 (b).
        EXPECT_TRUE(output.find("\033[48;2;57;57;61m") != std::string::npos);
        EXPECT_TRUE(output.find("\033[100m") == std::string::npos);

        EXPECT_EQ(::write(master, "x", 1), 1);
        output = read_new_output(master, std::move(output));
        EXPECT_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        EXPECT_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

// Region-mode analogue of test_footer_scroll_at_terminal_bottom_does_not_
// desync_cursor above, but exercising a real RegionRenderer + readline()
// across a genuine turn boundary instead of driving readline() alone --
// see that test's own comment for why a bare forkpty of readline() by
// itself can never reach this bug (no RegionRenderer, no
// position_prompt_cursor(), no DECSTBM scroll region at all).
//
// Root cause (found via exactly this repro, replayed through a real
// terminal for exact row-by-row verification before being encoded as this
// permanent MiniTerminal-based test): main.cpp's "Print mode / initial
// message" handling -- reachable any time a session is launched with an
// initial prompt in non-print mode, e.g. `pici --render region "do X"`,
// an ordinary way to start an agentic CLI session -- runs that message's
// turn *before* the interactive readline() loop starts, then
// unconditionally writes a bare "\n" straight through std::cout with no
// check on renderer->owns_status_line(), completely bypassing both the
// Renderer and readline()'s own row bookkeeping. From cursor position
// (prompt_anchor, 1) -- exactly where position_prompt_cursor() had just
// left it -- that untracked newline lands the very first interactive
// InputRenderer's first_draw_ one row lower than position_prompt_cursor()'s
// own formula assumes, so its footer paints one row lower too. That's
// internally self-consistent for as long as that one InputRenderer
// instance lives (its own clear_previous() correctly erases whatever it
// itself painted), but the *next* turn boundary's position_prompt_cursor()
// call resets the cursor back to the correct anchor absolutely, with no
// memory of the drift -- so the next InputRenderer paints at the *correct*
// row, and the previous one's now-orphaned footer, one row below it, is
// never touched by anything again: two footers, visible from that point
// on -- exactly the reported "doubled starting with the second prompt"
// symptom, and it never self-heals since nothing owns that orphaned row.
//
// Fixed in region_renderer.cpp's position_prompt_cursor(): it now wipes
// every row in the composer's reserved area before repositioning, so
// whatever (if anything) desynced that area since the last turn boundary
// is gone before the next InputRenderer ever paints into it -- closing the
// gap regardless of what wrote there, not just this one specific main.cpp
// bug.
TEST_F(ReadlineEnvironment,
       test_region_turn_boundary_survives_untracked_cursor_drift) {
  run_case(
      "region renderer + readline: an untracked write that shifts the "
      "composer's on-screen position between turns (main.cpp's initial-CLI-"
      "argument-message path bypasses both renderers with a bare std::cout "
      "newline) must not leave a stale footer behind once "
      "position_prompt_cursor() resets to the next prompt's correct row -- "
      "regression test for 'doubled footer starting with the second "
      "prompt' in --render region mode",
      [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = 80;
        int master = -1;
        // Flush every prior test's buffered "PASS ..." output before
        // forking -- otherwise the child inherits an unflushed std::cout
        // buffer and its own first flush dumps that stale content into the
        // pty at whatever cursor position is active, corrupting the exact
        // screen geometry this test depends on.
        std::cout.flush();
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        EXPECT_TRUE(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          auto renderer = pi::core::make_region_renderer(STDOUT_FILENO);
          const std::string prompt = "\n\033[1;36m\xe2\x80\xba\033[22;39m ";
          // The initial CLI-argument message's turn, run before the
          // interactive loop -- see main.cpp lines ~2006-2020.
          renderer->on_turn_start();
          renderer->on_text_delta("Initial CLI-argument message's reply.");
          renderer->on_turn_end();
          std::cout << "\n"; // the untracked write itself
          std::cout.flush();

          renderer->set_status_line(std::nullopt);
          // Mirrors main.cpp's interactive loop, which calls this
          // immediately before every readline() call now -- see
          // prepare_for_prompt()'s own comment: on_turn_end() alone no
          // longer wipes/re-anchors the composer, since a single
          // user-visible exchange can span several on_turn_start()/
          // on_turn_end() pairs when the agent calls tools, and only the
          // *last* one is followed by readline().
          renderer->prepare_for_prompt();
          (void)readline(prompt, {}, {}, {}, "", 0, -1, true);
          renderer->on_turn_start();
          renderer->on_text_delta("Sure, here is a short reply.");
          renderer->on_turn_end();
          renderer->set_status_line(std::nullopt);
          renderer->prepare_for_prompt();
          (void)readline(prompt, {}, {}, {}, "", 0, -1, true);
          // Deliberately hang here (no _exit) -- the parent kills this
          // child once it has captured the second interactive prompt's
          // steady state; nothing past that point matters, and letting
          // this readline() call's own eventual EOF handling run would
          // repaint the screen again and contaminate the capture.
          for (;;)
            ::pause();
        }

        auto output = read_until(master, {}, "READY");
        // Each readline() round trip (first paint, then the
        // submit-triggered clear) emits exactly two "\033[?7h" markers;
        // three occurrences reach the second interactive prompt's first
        // paint -- the exact point the bug (and the fix) is about.
        for (int occurrence = 0; occurrence < 3; ++occurrence) {
          const auto from = output.size();
          output = read_until_from(master, std::move(output), from, "\033[?7h");
          if (occurrence == 0) // first prompt just finished painting once
            EXPECT_EQ(::write(master, "\r", 1), 1); // submit it
        }

        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        ::close(master);

        MiniTerminal term(80, 24);
        term.feed(output);
        // Exactly one copy of the footer text must be visible -- before the
        // fix, the first (drift-shifted) prompt's orphaned footer survived
        // one row below the second prompt's correctly-positioned one.
        EXPECT_EQ(count_occurrences(term.screen_text(), "Alt+Enter: newline"),
                  1U);
      });
}

// Regression test for "the input line disappears when the agent generates
// output" in --render region mode: a real terminal resize observed *while a
// turn is active* -- distinct from the drift test just above, whose two
// turns are both already finished by on_turn_end() before either readline()
// call runs, so it can never reach a resize mid-turn.
//
// Root cause (found via a real forkpty + tmux repro, replayed row-by-row
// through a real terminal before being encoded here): RegionRenderer::
// on_resize() is documented to only ever run between turns -- it's wired up
// exclusively through readline()'s own on_resize callback in
// cli/readline.cpp, which by construction only fires while readline()
// itself is polling for keys, never while a turn is active. But paint_loop()
// deliberately keeps polling for a resize while a turn is active anyway (see
// its own comment: a resize must still reflow the transcript at a new width
// without waiting for the turn to end), and render_frame() recomputed
// content_rows from the terminal's *live* term_height(fd_) on every single
// frame -- so a real mid-turn resize changed content_rows on the very next
// frame, in violation of on_resize()'s own "only between turns" contract.
// Growing the terminal grows content_rows to cover physical rows that still
// hold readline()'s already-painted "> " prompt and footer hint (left on
// screen, untouched, for the whole turn -- see position_prompt_cursor()'s
// own comment); diff_region_rows's unconditional full repaint (triggered by
// the resulting layout_changed) then overwrites them with transcript
// content. Confirmed empirically: resizing a real pty 24 rows -> 30 rows a
// few hundred ms into on_text_delta() streaming wiped the prompt and footer
// off the visible screen immediately in a real terminal (tmux), and they
// didn't return until on_turn_end() -> position_prompt_cursor() -> the next
// readline() call repainted them at the (by-then-current) correct rows.
//
// Fixed in region_renderer.cpp: State::turn_layout_height pins the height
// render_frame() uses for row math (content_rows, the DECSTBM scroll
// region, the status row) to whatever it was at on_turn_start(), for the
// rest of that turn, so a resize's row-count change genuinely cannot reach
// render_frame() until on_turn_end() resets it back to 0 ("use the live
// height").
TEST_F(ReadlineEnvironment, test_composer_survives_mid_turn_resize) {
  run_case("region renderer: a terminal resize observed while a turn is active "
           "(paint_loop's own resize polling, not readline()'s on_resize "
           "callback -- see RegionRenderer::on_resize()'s own comment that it "
           "must only run between turns) must not let the transcript/status "
           "repaint reclaim rows the composer's prompt and footer are still "
           "painted on -- regression test for 'the input line disappears when "
           "the agent generates output'",
           [] {
             struct winsize ws {};
             ws.ws_row = 24;
             ws.ws_col = 80;
             int master = -1;
             std::cout.flush();
             const auto child = forkpty(&master, nullptr, nullptr, &ws);
             EXPECT_TRUE(child >= 0);
             if (child == 0) {
               dprintf(STDOUT_FILENO, "READY\n");
               auto renderer = pi::core::make_region_renderer(STDOUT_FILENO);
               const std::string prompt = "\n> ";
               // A non-empty initial draft submits on a bare "\r" with no
               // typing needed (an empty buffer's Enter is a no-op) -- same
               // trick test_footer_scroll_at_terminal_bottom... above uses.
               (void)readline(prompt, {}, {}, {}, "hi", 2, -1,
                              /*clear_on_submit=*/true);
               renderer->on_turn_start();
               // 24 chunks * 25ms ~= 600ms of simulated token-by-token
               // streaming -- long enough for the parent to resize reliably
               // partway through, with plenty of turn left afterward for more
               // frames to paint post-resize (the bug needs the composer to
               // still be gone right up to on_turn_end(), not just for one
               // transient frame).
               for (int i = 0; i < 24; ++i) {
                 renderer->on_text_delta("chunk ");
                 std::this_thread::sleep_for(std::chrono::milliseconds(25));
               }
               renderer->on_turn_end();
               // Deliberately hang -- the parent kills this child once it has
               // captured everything it needs; letting readline()'s next call
               // run (it would block on stdin forever anyway with no more
               // scripted input) adds nothing and risks a stray extra repaint
               // contaminating the capture, same rationale as the drift test
               // above.
               for (;;)
                 ::pause();
             }

             auto output = read_until(master, {}, "READY");
             output = read_until(master, std::move(output), "\033[?7h");
             EXPECT_EQ(::write(master, "\r", 1), 1); // submit "hi"
             // The clear_on_submit redraw: the composer's static "empty box +
             // footer" that's left on screen, untouched, for the rest of the
             // turn. Captures the size before the move -- passing std::move
             // (output) and output.size() as sibling arguments would read the
             // size in unspecified order relative to the move.
             const auto before_submit = output.size();
             output = read_until_from(master, std::move(output), before_submit,
                                      "\033[?7h");

             constexpr int kComposerRows =
                 static_cast<int>(pi::core::kMaxComposerRows);
             constexpr int kOldContentRows = 24 - 1 - kComposerRows; // == 17
             MiniTerminal before(80, 24);
             before.feed(output);
             // Composer's own first row (content_rows rows, then the status
             // row, then the composer starts) holds the prompt; the footer sits
             // one row below it -- see region_renderer.cpp's row-layout
             // comments.
             EXPECT_TRUE(before.row(kOldContentRows + 1).find("> ") !=
                         std::string::npos);
             EXPECT_TRUE(before.row(kOldContentRows + 2).find("Alt+Enter") !=
                         std::string::npos);

             // Let a couple of streaming frames land, then resize mid-turn --
             // the exact condition under test.
             std::this_thread::sleep_for(std::chrono::milliseconds(150));
             struct winsize grown {};
             grown.ws_row = 30;
             grown.ws_col = 80;
             // ioctl declarations vary by platform -- same rationale as
             // readline.cpp's terminal_columns().
             // NOLINTNEXTLINE(misc-include-cleaner)
             EXPECT_EQ(::ioctl(master, TIOCSWINSZ, &grown), 0);
             ::kill(child, SIGWINCH);

             // Let streaming continue well past the resize -- 200ms is ~8 more
             // on_text_delta() chunks at 25ms each, comfortably inside the
             // 600ms (24 chunks) the child's loop runs for, so the turn is
             // still active. This is the exact window the bug needs: not just
             // the one frame immediately after the resize, but the composer
             // staying gone for a real, sustained stretch of continued
             // generation -- matching the user's report ("disappears when the
             // agent generates output"), and distinguishing it from a resize
             // that's noticed but only transiently mishandled. Drains
             // everything the pty produces over that whole window (unlike
             // read_new_output, which returns after the *first* non-empty read
             // -- too early to capture a 200ms backlog) via the same poll+read
             // loop read_until uses internally, just bounded by wall-clock time
             // instead of a marker.
             {
               const auto deadline = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(200);
               while (std::chrono::steady_clock::now() < deadline) {
                 pollfd descriptor{.fd = master, .events = POLLIN};
                 if (::poll(&descriptor, 1, 25) <= 0)
                   continue;
                 char buffer[256];
                 const auto count = ::read(master, buffer, sizeof(buffer));
                 if (count > 0)
                   output.append(buffer, static_cast<std::size_t>(count));
               }
             }

             // Confirm this snapshot really is still mid-turn, not a race where
             // the turn happened to finish before the sleep above elapsed --
             // "tokens: 0  done" only ever appears once, written by
             // on_turn_end(), never before.
             EXPECT_TRUE(output.find("tokens: 0  done") == std::string::npos);

             // Replay everything captured so far on an 80x30 canvas -- a strict
             // superset of the pre-resize 80x24 screen, so this changes nothing
             // about the composer's original rows unless render_frame() itself
             // started addressing rows outside its old content_rows range,
             // which is exactly what's under test. The composer's prompt and
             // footer must still be sitting at their original rows
             // (kOldContentRows + 1 and + 2), untouched, well after the resize
             // and with the turn still actively streaming.
             MiniTerminal mid(80, 30);
             mid.feed(output);
             EXPECT_TRUE(mid.row(kOldContentRows + 1).find("> ") !=
                         std::string::npos);
             EXPECT_TRUE(mid.row(kOldContentRows + 2).find("Alt+Enter") !=
                         std::string::npos);

             // Let the turn actually finish and confirm recovery: the next
             // resync (triggered by on_turn_end() resetting turn_layout_height
             // back to 0) picks up the terminal's now-current 30-row size, so
             // the DECSTBM sequence for its content_rows (23 == 30 - 1 -
             // kMaxComposerRows) must appear by the time the turn ends --
             // proving the resize was deferred, not silently dropped
             // altogether.
             output = read_until(master, std::move(output), "tokens: 0  done");
             EXPECT_TRUE(output.find("tokens: 0  done") != std::string::npos);
             const std::string grown_scroll_region =
                 "\033[1;" + std::to_string(30 - 1 - kComposerRows) + "r";
             EXPECT_TRUE(output.find(grown_scroll_region) != std::string::npos);

             ::kill(child, SIGKILL);
             int status = 0;
             ::waitpid(child, &status, 0);
             ::close(master);
           });
}
