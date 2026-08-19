#include "cli/readline.h"

#include "core/terminal.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

using namespace pi::cli;

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition)
    return true;
  ++current_failed;
  std::cerr << "  FAIL " << location.file_name() << ":" << location.line()
            << " - " << expression << "\n";
  return false;
}

#define CHECK(expression) tests::check((expression), #expression)
#define CHECK_EQ(left, right)                                                  \
  tests::check((left) == (right), #left " == " #right)

void run(std::string name, const std::function<void()> &test) {
  ++total;
  current_failed = 0;
  test();
  if (current_failed == 0)
    ++passed;
  else
    ++failed;
  std::cout << (current_failed == 0 ? "  PASS " : "  FAIL ") << name << "\n";
}

} // namespace tests

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

void test_wake_channel() {
  tests::run("ReadlineWake: nonblocking CLOEXEC coalescing", [] {
    ReadlineWake wake;
    const auto descriptor_flags = ::fcntl(wake.read_fd(), F_GETFD);
    const auto status_flags = ::fcntl(wake.read_fd(), F_GETFL);
    CHECK((descriptor_flags & FD_CLOEXEC) != 0);
    CHECK((status_flags & O_NONBLOCK) != 0);
    CHECK(wake.notify());
    CHECK(wake.notify());
    CHECK(wake.notify());
    wake.drain();
    pollfd descriptor{.fd = wake.read_fd(), .events = POLLIN};
    CHECK_EQ(::poll(&descriptor, 1, 0), 0);
  });
}

void test_non_tty_result(std::string input, ReadlineExit expected,
                         std::string expected_text) {
  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  CHECK_EQ(::pipe(input_pipe), 0);
  CHECK_EQ(::pipe(output_pipe), 0);
  const auto child = ::fork();
  CHECK(child >= 0);
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
    CHECK_EQ(::write(input_pipe[1], input.data(), input.size()),
             static_cast<ssize_t>(input.size()));
  ::close(input_pipe[1]);
  auto output = read_until(output_pipe[0], {}, "RESULT:");
  ::close(output_pipe[0]);
  CHECK(output.find("RESULT:" + std::to_string(static_cast<int>(expected))) !=
        std::string::npos);
  CHECK(output.find(":" + expected_text + "\n") != std::string::npos);
  CHECK_EQ(wait_for_child(child), 0);
}

void test_non_tty_paths() {
  tests::run("readline: non-TTY submit and EOF", [] {
    test_non_tty_result("scripted\n", ReadlineExit::submitted, "scripted");
    test_non_tty_result("", ReadlineExit::eof, "");
  });
}

void test_tty_wake_and_reentry() {
  tests::run("readline: wake preserves middle cursor and re-entry", [] {
    ReadlineWake wake;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    CHECK(child >= 0);
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
    CHECK(output.find("READY") != std::string::npos);
    output = read_until(master, std::move(output), "héllo");
    CHECK_EQ(::write(master, "\033[D", 3), 3);
    output = read_new_output(master, std::move(output));
    CHECK_EQ(::write(master, "\033[C", 3), 3);
    output = read_new_output(master, std::move(output));
    CHECK(wake.notify());
    CHECK(wake.notify());
    CHECK(wake.notify());
    output = read_until(master, std::move(output), "RESULT:");
    const auto expected_result =
        "RESULT:" +
        std::to_string(static_cast<int>(ReadlineExit::mailbox_wake)) +
        ":3:héllo";
    CHECK(output.find(expected_result) != std::string::npos);
    output = read_until(master, std::move(output), "SECOND_READY");
    CHECK(output.find("SECOND_READY") != std::string::npos);

    output = read_new_output(master, std::move(output));
    CHECK_EQ(::write(master, "X", 1), 1);
    output = read_new_output(master, std::move(output));
    CHECK(wake.notify());
    CHECK_EQ(::write(master, "\n", 1), 1);
    output = read_until(master, std::move(output), "RESULT2:");
    const auto expected_second =
        "RESULT2:" + std::to_string(static_cast<int>(ReadlineExit::submitted)) +
        ":4:héXllo";
    CHECK(output.find(expected_second) != std::string::npos);
    CHECK_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

void test_escape_wake() {
  tests::run("readline: isolated escape remains wakeable", [] {
    ReadlineWake wake;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    CHECK(child >= 0);
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
    CHECK_EQ(::write(master, "\033", 1), 1);
    CHECK(wake.notify());
    output = read_until(master, std::move(output), "RESULT:");
    const auto expected =
        "RESULT:" +
        std::to_string(static_cast<int>(ReadlineExit::mailbox_wake)) +
        ":2:draft";
    CHECK(output.find(expected) != std::string::npos);
    CHECK_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

void test_eof_wake_race() {
  tests::run("readline: EOF wins a wake boundary", [] {
    ReadlineWake wake;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    CHECK(child >= 0);
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
    CHECK(wake.notify());
    CHECK_EQ(::write(master, "\004", 1), 1);
    output = read_until(master, std::move(output), "RESULT:");
    const auto expected =
        "RESULT:" + std::to_string(static_cast<int>(ReadlineExit::eof)) +
        ":2:draft";
    CHECK(output.find(expected) != std::string::npos);
    CHECK_EQ(wait_for_child(child), 0);
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

void test_mouse_wheel_scroll() {
  tests::run("readline: SGR mouse wheel drives scroll actions", [] {
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    CHECK(child >= 0);
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
    CHECK_EQ(::write(master, "\033[<64;10;5M", 11), 11);
    output = read_new_output(master, std::move(output));

    // Wheel-down (Cb=65): three scroll_line_down actions.
    CHECK_EQ(::write(master, "\033[<65;10;5M", 11), 11);
    output = read_new_output(master, std::move(output));

    // A plain left-click (Cb=0, no wheel bit) is not a scroll gesture and
    // must not drive any control action.
    CHECK_EQ(::write(master, "\033[<0;10;5M", 10), 10);
    output = read_new_output(master, std::move(output));

    CHECK_EQ(::write(master, "\n", 1), 1);
    output = read_until(master, std::move(output), "RESULT:");

    const auto up_marker =
        "ACTION:" +
        std::to_string(static_cast<int>(ControlAction::scroll_line_up));
    const auto down_marker =
        "ACTION:" +
        std::to_string(static_cast<int>(ControlAction::scroll_line_down));
    CHECK_EQ(count_occurrences(output, up_marker), 3U);
    CHECK_EQ(count_occurrences(output, down_marker), 3U);

    CHECK_EQ(wait_for_child(child), 0);
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

void test_wrap_boundary_cursor_placement() {
  constexpr int kColumns = 10;

  tests::run("readline: cursor stays in range when text exactly fills a row",
             [] {
               struct winsize ws {};
               ws.ws_row = 24;
               ws.ws_col = kColumns;
               int master = -1;
               const auto child = forkpty(&master, nullptr, nullptr, &ws);
               CHECK(child >= 0);
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
               CHECK(!has_out_of_range_cursor_forward(output, kColumns));
               CHECK(output.find("\033[7m") != std::string::npos);

               CHECK_EQ(::write(master, "\n", 1), 1);
               output = read_until(master, std::move(output), "RESULT:");
               CHECK(!has_out_of_range_cursor_forward(output, kColumns));
               CHECK_EQ(wait_for_child(child), 0);
               ::close(master);
             });

  tests::run(
      "readline: cursor stays in range mid-text at a soft wrap boundary", [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = kColumns;
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        CHECK(child >= 0);
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
          CHECK_EQ(::write(master, "\033[D", 3), 3);
          output = read_new_output(master, std::move(output));
        }
        CHECK(!has_out_of_range_cursor_forward(output, kColumns));

        CHECK_EQ(::write(master, "\n", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        CHECK(!has_out_of_range_cursor_forward(output, kColumns));
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

void test_alt_enter_inserts_newline() {
  tests::run("readline: Alt+Enter inserts a newline, plain Enter still submits",
             [] {
               int master = -1;
               const auto child = forkpty(&master, nullptr, nullptr, nullptr);
               CHECK(child >= 0);
               if (child == 0) {
                 dprintf(STDOUT_FILENO, "READY\n");
                 const auto result = readline("> ", {}, {}, {}, "", 0);
                 dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                         static_cast<int>(result.reason), result.cursor,
                         result.text.c_str());
                 _exit(0);
               }

               auto output = read_until(master, {}, "READY");
               CHECK_EQ(::write(master, "ab", 2), 2);
               output = read_new_output(master, std::move(output));
               // ESC immediately followed by \r — the legacy Alt+Enter
               // encoding.
               CHECK_EQ(::write(master, "\033\r", 2), 2);
               output = read_new_output(master, std::move(output));
               CHECK_EQ(::write(master, "cd", 2), 2);
               output = read_new_output(master, std::move(output));
               // Plain Enter still submits — it must not have been
               // reinterpreted.
               CHECK_EQ(::write(master, "\r", 1), 1);
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
               CHECK(output.find(expected) != std::string::npos);
               CHECK_EQ(wait_for_child(child), 0);
               ::close(master);
             });
}

void test_multiline_navigation_and_backspace() {
  tests::run(
      "readline: Left/Right cross an embedded newline and Backspace joins "
      "lines",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        CHECK(child >= 0);
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
        CHECK_EQ(::write(master, "ab", 2), 2);
        output = read_new_output(master, std::move(output));
        CHECK_EQ(::write(master, "\033\r", 2), 2);
        output = read_new_output(master, std::move(output));
        CHECK_EQ(::write(master, "cd", 2), 2);
        output = read_new_output(master, std::move(output));

        // Left x3: 5->4->3->2. The third step crosses the embedded \n
        // backward (from right after it to right before it), landing right
        // after "ab".
        for (int i = 0; i < 3; ++i) {
          CHECK_EQ(::write(master, "\033[D", 3), 3);
          output = read_new_output(master, std::move(output));
        }
        // Right x1: 2->3, crossing the same \n forward again, landing right
        // after it (right before "cd").
        CHECK_EQ(::write(master, "\033[C", 3), 3);
        output = read_new_output(master, std::move(output));
        // Backspace at offset 3 erases the \n itself (offset 2), rejoining
        // "ab" and "cd" into "abcd" with the cursor left at offset 2.
        CHECK_EQ(::write(master, "\x7f", 1), 1);
        output = read_new_output(master, std::move(output));

        CHECK_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":2:abcd";
        CHECK(output.find(expected) != std::string::npos);
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

void test_bracketed_paste_is_inert_block_insert() {
  tests::run(
      "readline: bracketed paste lands intact and never submits, even with "
      "a trailing newline",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        CHECK(child >= 0);
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
        CHECK_EQ(::write(master, paste.data(), paste.size()),
                 static_cast<ssize_t>(paste.size()));
        output = read_new_output(master, std::move(output));
        CHECK(output.find("RESULT:") == std::string::npos);

        CHECK_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        // See the Alt+Enter test above: the pty's own output processing
        // (ONLCR) renders the buffer's real embedded \n bytes as \r\n here.
        // The :12: cursor count confirms the buffer itself holds 12 bytes
        // (two single-byte \n, not \r\n pairs).
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":12:line1\r\nline2\r\n";
        CHECK(output.find(expected) != std::string::npos);
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

void test_embedded_newline_cursor_placement() {
  constexpr int kColumns = 10;

  tests::run(
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
        CHECK(child >= 0);
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
        CHECK(!has_out_of_range_cursor_forward(output, kColumns));
        // The final fill-and-toggle sequence must be a bare "\r" straight
        // into the reverse-video toggle — no vertical move at all, since
        // the cursor's normalized row (start of the second row) matches
        // where painting actually stopped. Before the M1 fix, capturing
        // the pre-break {row 1, column 10} here produced an extra
        // "\033[1A" that painted the cursor block back over the first
        // row's last digit instead.
        CHECK(output.find("\033[K\r\033[7m") != std::string::npos);

        CHECK_EQ(::write(master, "\n", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });

  tests::run(
      "readline: cursor right after an embedded newline lands on the new "
      "row's start",
      [] {
        struct winsize ws {};
        ws.ws_row = 24;
        ws.ws_col = kColumns;
        int master = -1;
        // Cursor sits at offset 11 — right at 'A', the start of the second
        // row.
        const auto child = forkpty(&master, nullptr, nullptr, &ws);
        CHECK(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("", {}, {}, {}, "0123456789\nABCDE", 11);
          dprintf(STDOUT_FILENO, "\nRESULT:%d:%zu:%s\n",
                  static_cast<int>(result.reason), result.cursor,
                  result.text.c_str());
          _exit(0);
        }

        auto output = read_until(master, {}, "READY");
        output = read_until(master, std::move(output), "ABCDE");
        CHECK(!has_out_of_range_cursor_forward(output, kColumns));
        CHECK(output.find("\033[K\r\033[7m") != std::string::npos);

        CHECK_EQ(::write(master, "\n", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

void test_composer_height_cap() {
  tests::run("readline: composer height cap bounds rendered row bookkeeping "
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
               CHECK(child >= 0);
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
               CHECK_EQ(::write(master, "X", 1), 1);
               output = read_until_from(master, std::move(output), before_size,
                                        "\033[?7h");
               const auto second_redraw = output.substr(before_size);
               CHECK_EQ(count_occurrences(second_redraw, "\033[2K"),
                        pi::core::kMaxComposerRows);

               CHECK_EQ(::write(master, "\r", 1), 1);
               output = read_until(master, std::move(output), "RESULT:");
               CHECK_EQ(wait_for_child(child), 0);
               ::close(master);
             });
}

void test_word_wrap_boundary() {
  constexpr int kColumns = 10;

  tests::run(
      "readline: typing a long sentence wraps at word boundaries, not "
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
        CHECK(child >= 0);
        if (child == 0) {
          dprintf(STDOUT_FILENO, "READY\n");
          const auto result = readline("", {}, {}, {}, "hello world foo", 15);
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
        CHECK(output.find("hello\033[K\r\r\nworld foo") != std::string::npos);
        // Neither word is ever split across the row-clear/wrap sequence --
        // if it were, "hell" or "worl" would appear immediately followed
        // by it.
        CHECK(output.find("hell\033[K\r\r\no") == std::string::npos);
        CHECK(output.find("worl\033[K\r\r\nd") == std::string::npos);
        CHECK(!has_out_of_range_cursor_forward(output, kColumns));

        CHECK_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":15:hello world foo";
        CHECK(output.find(expected) != std::string::npos);
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });

  tests::run("readline: a single overlong token (longer than the row) still "
             "hard-wraps without overflowing",
             [] {
               struct winsize ws {};
               ws.ws_row = 24;
               ws.ws_col = kColumns;
               int master = -1;
               const std::string token(40, 'x'); // no whitespace anywhere in it
               const auto child = forkpty(&master, nullptr, nullptr, &ws);
               CHECK(child >= 0);
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
               CHECK(!has_out_of_range_cursor_forward(output, kColumns));
               // The row-clear/wrap marker (see the ONLCR note above for the
               // doubled \r) must appear (repeatedly) inside the unbroken token
               // -- confirming the hard-wrap fallback engaged instead of
               // overflowing a single row with all 40 characters.
               CHECK(count_occurrences(output, "\033[K\r\r\n") >= 3U);

               CHECK_EQ(::write(master, "\r", 1), 1);
               output = read_until(master, std::move(output), "RESULT:");
               CHECK_EQ(wait_for_child(child), 0);
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
  tests::run(std::move(name), [=] {
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, nullptr);
    CHECK(child >= 0);
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
      CHECK_EQ(::write(master, input.data(), input.size()),
               static_cast<ssize_t>(input.size()));
      output = read_new_output(master, std::move(output));
    }
    CHECK_EQ(::write(master, "\r", 1), 1);
    output = read_until(master, std::move(output), "RESULT:");
    // The trailing "\r\n" (the dprintf format's own '\n' after "%s", doubled
    // by ONLCR same as everywhere else in this file) anchors the end of the
    // text field -- without it, an expected text that happens to be a
    // prefix of a longer, unfixed actual buffer (e.g. "hello\r\nwo" against
    // an untouched "hello\r\nworld") would false-match via find().
    const auto expected =
        "RESULT:" + std::to_string(static_cast<int>(expected_reason)) + ":" +
        std::to_string(expected_cursor) + ":" + expected_text + "\r\n";
    CHECK(output.find(expected) != std::string::npos);
    CHECK_EQ(wait_for_child(child), 0);
    ::close(master);
  });
}

void test_up_down_logical_line_navigation() {
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

void test_home_end_and_ctrl_a_e() {
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

void test_ctrl_arrows_scroll_transcript_not_buffer() {
  tests::run(
      "readline: Ctrl+Up/Down and Ctrl+Home/End fire transcript scroll "
      "actions and leave the buffer/cursor untouched",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        CHECK(child >= 0);
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
        CHECK_EQ(::write(master, "\033[1;5A", 6), 6);
        output = read_new_output(master, std::move(output));
        CHECK_EQ(::write(master, "\033[1;5B", 6), 6);
        output = read_new_output(master, std::move(output));
        CHECK_EQ(::write(master, "\033[1;5H", 6), 6);
        output = read_new_output(master, std::move(output));
        CHECK_EQ(::write(master, "\033[1;5F", 6), 6);
        output = read_new_output(master, std::move(output));

        CHECK_EQ(::write(master, "\r", 1), 1);
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
        CHECK_EQ(count_occurrences(output, up), 1U);
        CHECK_EQ(count_occurrences(output, down), 1U);
        CHECK_EQ(count_occurrences(output, top), 1U);
        CHECK_EQ(count_occurrences(output, bottom), 1U);

        // The buffer and cursor must be exactly as they started -- these
        // keys drove the transcript, not the composer.
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":2:ab\r\ncd";
        CHECK(output.find(expected) != std::string::npos);
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

void test_word_motion() {
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

void test_ctrl_w_deletes_word_and_leading_whitespace() {
  // "foo bar   baz": cursor placed right before "baz", after "bar" and its
  // three trailing spaces -- Ctrl+W must delete "bar   " (the word *and*
  // the whitespace run immediately before the cursor), not just "bar".
  run_editing_case(
      "readline: Ctrl+W deletes the word behind the cursor including the "
      "whitespace immediately before it",
      "foo bar   baz", 10, "\x17", ReadlineExit::submitted, "foo baz", 4);
}

void test_ctrl_u_and_ctrl_k_kill_to_line_boundaries() {
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

void test_ctrl_y_yanks_last_kill_only() {
  tests::run(
      "readline: Ctrl+Y yanks only the most recent kill -- the single-slot "
      "buffer overwrites, it does not accumulate",
      [] {
        int master = -1;
        const auto child = forkpty(&master, nullptr, nullptr, nullptr);
        CHECK(child >= 0);
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
        CHECK_EQ(::write(master, "\x17", 1), 1);
        output = read_new_output(master, std::move(output));
        // Ctrl+K at offset 0 kills the rest of the line, " BBBB" -- this
        // overwrites the kill buffer (now " BBBB", not "AAAA"), leaving an
        // empty buffer.
        CHECK_EQ(::write(master, "\x0b", 1), 1);
        output = read_new_output(master, std::move(output));
        // Ctrl+Y yanks the kill buffer back. If the first kill had not
        // been overwritten, this would insert "AAAA" instead.
        CHECK_EQ(::write(master, "\x19", 1), 1);
        output = read_new_output(master, std::move(output));

        CHECK_EQ(::write(master, "\r", 1), 1);
        output = read_until(master, std::move(output), "RESULT:");
        const auto expected =
            "RESULT:" +
            std::to_string(static_cast<int>(ReadlineExit::submitted)) +
            ":5: BBBB";
        CHECK(output.find(expected) != std::string::npos);
        CHECK_EQ(wait_for_child(child), 0);
        ::close(master);
      });
}

int main() {
  test_wake_channel();
  test_non_tty_paths();
  test_tty_wake_and_reentry();
  test_escape_wake();
  test_eof_wake_race();
  test_mouse_wheel_scroll();
  test_wrap_boundary_cursor_placement();
  test_alt_enter_inserts_newline();
  test_multiline_navigation_and_backspace();
  test_bracketed_paste_is_inert_block_insert();
  test_embedded_newline_cursor_placement();
  test_composer_height_cap();
  test_word_wrap_boundary();
  test_up_down_logical_line_navigation();
  test_home_end_and_ctrl_a_e();
  test_ctrl_arrows_scroll_transcript_not_buffer();
  test_word_motion();
  test_ctrl_w_deletes_word_and_leading_whitespace();
  test_ctrl_u_and_ctrl_k_kill_to_line_boundaries();
  test_ctrl_y_yanks_last_kill_only();
  std::cout << "\nTests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
