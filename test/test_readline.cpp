#include "cli/readline.h"

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
        "ACTION:" + std::to_string(static_cast<int>(ControlAction::scroll_line_up));
    const auto down_marker =
        "ACTION:" +
        std::to_string(static_cast<int>(ControlAction::scroll_line_down));
    CHECK_EQ(count_occurrences(output, up_marker), 3U);
    CHECK_EQ(count_occurrences(output, down_marker), 3U);

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
  std::cout << "\nTests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
