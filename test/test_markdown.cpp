#include <functional>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <unistd.h>

#include "core/markdown.h"
#include "core/stream_renderer.h"

using namespace pi::core;

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool CHECK_impl(bool cond, bool expected, std::string_view expr,
                std::source_location loc = std::source_location::current()) {
  if (cond != expected) {
    current_failed++;
    std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line() << " - "
              << expr << "\n";
    return false;
  }
  return true;
}

#define CHECK(cond)                                                            \
  (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond,                  \
                       std::source_location::current()))
#define CHECK_EQ(a, b)                                                         \
  (::tests::CHECK_impl((a) == (b), true, #a " == " #b,                        \
                       std::source_location::current()))

void register_test(std::string name, std::function<void()> fn) {
  total++;
  current_failed = 0;
  fn();
  if (current_failed == 0) {
    passed++;
    std::cout << "  PASS " << name << "\n";
  } else {
    failed++;
    std::cout << "  FAIL " << name << "\n";
  }
}

void print_summary() {
  std::cout << "\n========================================\n";
  std::cout << "  Tests: " << total << " total, " << passed << " passed, "
            << failed << " failed\n";
  std::cout << "========================================\n";
}

} // namespace tests

// Strip ANSI escapes so we can assert on content independently of color codes.
static std::string strip_ansi(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
      i += 2;
      while (i < s.size() && s[i] != 'm') {
        ++i;
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

static bool has_ansi(std::string_view s) {
  for (std::size_t i = 0; i + 1 < s.size(); ++i) {
    if (s[i] == '\033' && s[i + 1] == '[') {
      return true;
    }
  }
  return false;
}

void test_plain_text() {
  tests::register_test("Markdown: plain text passes through", []() {
    auto out = render_markdown_plain("Hello, world.");
    CHECK(out.find("Hello, world.") != std::string::npos);
  });
}

void test_bold() {
  tests::register_test("Markdown: bold has ANSI and plain content", []() {
    auto ansi = render_markdown_ansi("**bold text**");
    CHECK(has_ansi(ansi));
    auto plain = strip_ansi(ansi);
    CHECK(plain.find("bold text") != std::string::npos);
  });
}

void test_italic() {
  tests::register_test("Markdown: italic has ANSI and plain content", []() {
    auto ansi = render_markdown_ansi("*italic text*");
    CHECK(has_ansi(ansi));
    auto plain = strip_ansi(ansi);
    CHECK(plain.find("italic text") != std::string::npos);
  });
}

void test_inline_code() {
  tests::register_test("Markdown: inline code has ANSI and plain content", []() {
    auto ansi = render_markdown_ansi("Use `foo()` here.");
    CHECK(has_ansi(ansi));
    auto plain = strip_ansi(ansi);
    CHECK(plain.find("foo()") != std::string::npos);
  });
}

void test_heading_prefix() {
  tests::register_test("Markdown: headings have # prefix and ANSI", []() {
    auto h1 = render_markdown_ansi("# Title");
    CHECK(has_ansi(h1));
    CHECK(strip_ansi(h1).find("# Title") != std::string::npos);

    auto h2 = render_markdown_ansi("## Section");
    CHECK(strip_ansi(h2).find("## Section") != std::string::npos);

    auto h3 = render_markdown_ansi("### Sub");
    CHECK(strip_ansi(h3).find("### Sub") != std::string::npos);
  });
}

void test_code_block() {
  tests::register_test("Markdown: fenced code block has ANSI and content", []() {
    auto ansi = render_markdown_ansi("```\nfn main() {}\n```");
    CHECK(has_ansi(ansi));
    auto plain = strip_ansi(ansi);
    CHECK(plain.find("fn main()") != std::string::npos);
  });
}

void test_code_block_language() {
  tests::register_test("Markdown: code block language shown", []() {
    auto plain = strip_ansi(render_markdown_ansi("```rust\nlet x = 1;\n```"));
    CHECK(plain.find("rust") != std::string::npos);
    CHECK(plain.find("let x = 1;") != std::string::npos);
  });
}

void test_unordered_list() {
  tests::register_test("Markdown: unordered list has bullet and content", []() {
    auto plain = strip_ansi(render_markdown_ansi("- alpha\n- beta\n- gamma"));
    CHECK(plain.find("alpha") != std::string::npos);
    CHECK(plain.find("beta") != std::string::npos);
    CHECK(plain.find("gamma") != std::string::npos);
    // Each item should be on its own line
    CHECK(plain.find('\n') != std::string::npos);
  });
}

void test_ordered_list() {
  tests::register_test("Markdown: ordered list has numbers and content", []() {
    auto plain =
        strip_ansi(render_markdown_ansi("1. first\n2. second\n3. third"));
    CHECK(plain.find("1.") != std::string::npos);
    CHECK(plain.find("first") != std::string::npos);
    CHECK(plain.find("2.") != std::string::npos);
    CHECK(plain.find("second") != std::string::npos);
  });
}

void test_strikethrough() {
  tests::register_test("Markdown: strikethrough has ANSI and content", []() {
    auto ansi = render_markdown_ansi("~~deleted~~");
    CHECK(has_ansi(ansi));
    auto plain = strip_ansi(ansi);
    CHECK(plain.find("deleted") != std::string::npos);
  });
}

void test_link() {
  tests::register_test("Markdown: link shows text and URL", []() {
    auto plain =
        strip_ansi(render_markdown_ansi("[click here](https://example.com)"));
    CHECK(plain.find("click here") != std::string::npos);
    CHECK(plain.find("https://example.com") != std::string::npos);
  });
}

void test_hr() {
  tests::register_test("Markdown: horizontal rule renders", []() {
    auto plain = strip_ansi(render_markdown_ansi("---"));
    CHECK(plain.find('\n') != std::string::npos);
    // Should contain some kind of line character
    CHECK(plain.size() > 2);
  });
}

void test_render_markdown_plain_strips_ansi() {
  tests::register_test("render_markdown_plain: no ANSI codes", []() {
    auto plain = render_markdown_plain("**bold** and *italic*");
    CHECK(!has_ansi(plain));
    CHECK(plain.find("bold") != std::string::npos);
    CHECK(plain.find("italic") != std::string::npos);
  });
}

void test_mixed_formatting() {
  tests::register_test("Markdown: mixed formatting renders correctly", []() {
    const char *src = "# Hello\n\n"
                      "Some **bold** and *italic* text.\n\n"
                      "- item one\n"
                      "- item two\n\n"
                      "```cpp\nint x = 42;\n```\n";
    auto ansi = render_markdown_ansi(src);
    CHECK(has_ansi(ansi));
    auto plain = strip_ansi(ansi);
    CHECK(plain.find("# Hello") != std::string::npos);
    CHECK(plain.find("bold") != std::string::npos);
    CHECK(plain.find("italic") != std::string::npos);
    CHECK(plain.find("item one") != std::string::npos);
    CHECK(plain.find("int x = 42;") != std::string::npos);
  });
}

void test_empty_input() {
  tests::register_test("Markdown: empty input produces single newline", []() {
    auto out = render_markdown_ansi("");
    CHECK_EQ(out, std::string("\n"));
  });
}

static std::string read_fd_all(int fd) {
  std::string out;
  char buf[256];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

void test_stream_renderer_plain_append() {
  tests::register_test("Stream renderer: plain text appends without redraw", []() {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);

    {
      auto renderer = make_diff_renderer(fds[1]);
      renderer->update("Hel");
      renderer->update("lo");
      renderer->finish();
    }

    ::close(fds[1]);
    auto out = read_fd_all(fds[0]);
    ::close(fds[0]);

    CHECK_EQ(out, std::string("Hello\n"));
  });
}

void test_stream_renderer_newline_append() {
  tests::register_test("Stream renderer: paragraph growth appends without redraw", []() {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);

    {
      auto renderer = make_diff_renderer(fds[1]);
      renderer->update("Hello");
      renderer->update("\n\nWorld");
      renderer->finish();
    }

    ::close(fds[1]);
    auto out = read_fd_all(fds[0]);
    ::close(fds[0]);

    CHECK_EQ(out, std::string("Hello\n\nWorld\n"));
  });
}

int main() {
  std::cout << "=== pi-cpp markdown tests ===\n\n";

  test_plain_text();
  test_bold();
  test_italic();
  test_inline_code();
  test_heading_prefix();
  test_code_block();
  test_code_block_language();
  test_unordered_list();
  test_ordered_list();
  test_strikethrough();
  test_link();
  test_hr();
  test_render_markdown_plain_strips_ansi();
  test_mixed_formatting();
  test_empty_input();
  test_stream_renderer_plain_append();
  test_stream_renderer_newline_append();

  tests::print_summary();
  return tests::failed == 0 ? 0 : 1;
}
