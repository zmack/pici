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
  std::cout << "\nTests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
