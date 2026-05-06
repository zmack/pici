#include <functional>
#include <fcntl.h>
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

static int count_ansi_sequences(std::string_view s) {
  int count = 0;
  for (std::size_t i = 0; i + 1 < s.size(); ++i) {
    if (s[i] == '\033' && s[i + 1] == '[') {
      ++count;
      i += 2;
      while (i < s.size() && s[i] < '@') {
        ++i;
      }
    }
  }
  return count;
}

static std::string visible_markdown_plain(std::string_view input) {
  auto plain = render_markdown_plain(input);
  if (!plain.empty() && plain.back() == '\n') {
    plain.pop_back();
  }
  std::size_t trailing = 0;
  for (std::size_t i = input.size(); i > 0 && input[i - 1] == '\n'; --i) {
    ++trailing;
  }
  plain.append(trailing, '\n');
  return plain;
}

struct TerminalState {
  std::vector<std::string> lines{1, ""};
  int row{0};
  int col{0};

  void ensure_row(int r) {
    while (static_cast<int>(lines.size()) <= r) {
      lines.push_back("");
    }
  }

  void put_char(char ch) {
    ensure_row(row);
    auto &line = lines[row];
    if (static_cast<int>(line.size()) < col) {
      line.resize(static_cast<std::size_t>(col), ' ');
    }
    if (static_cast<int>(line.size()) == col) {
      line.push_back(ch);
    } else {
      line[static_cast<std::size_t>(col)] = ch;
    }
    ++col;
  }

  void newline() {
    ++row;
    col = 0;
    ensure_row(row);
  }

  void carriage_return() { col = 0; }

  void cursor_up(int n) {
    row -= n;
    if (row < 0) {
      row = 0;
    }
  }

  void clear_to_end() {
    ensure_row(row);
    auto &line = lines[row];
    if (col < static_cast<int>(line.size())) {
      line.resize(static_cast<std::size_t>(col));
    }
    lines.resize(static_cast<std::size_t>(row + 1));
  }

  void apply(std::string_view stream) {
    for (std::size_t i = 0; i < stream.size(); ++i) {
      const char ch = stream[i];
      if (ch == '\033' && i + 1 < stream.size() && stream[i + 1] == '[') {
        i += 2;
        std::string params;
        while (i < stream.size()) {
          const char c = stream[i];
          if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
            params += c;
            ++i;
            continue;
          }
          break;
        }
        if (i >= stream.size()) {
          break;
        }
        int n = 0;
        for (char c : params) {
          if (c == ';' || c == '?') {
            break;
          }
          n = n * 10 + (c - '0');
        }
        if (stream[i] == 'A') {
          cursor_up(n > 0 ? n : 1);
        } else if (stream[i] == 'J') {
          clear_to_end();
        }
        continue;
      }
      if (ch == '\n') {
        newline();
      } else if (ch == '\r') {
        carriage_return();
      } else {
        put_char(ch);
      }
    }
  }

  std::string plain() const {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i > 0) {
        out += '\n';
      }
      out += lines[i];
    }
    return out;
  }
};

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

void test_code_block_syntax_highlighting() {
  tests::register_test("Markdown: supported fenced code block gets syntax highlighting", []() {
    const auto ansi =
        render_markdown_ansi("```cpp\nint main() { return 42; }\n```");
    const auto plain = strip_ansi(ansi);
    CHECK(plain.find("cpp") != std::string::npos);
    CHECK(plain.find("int main() { return 42; }") != std::string::npos);
    CHECK(has_ansi(ansi));
    CHECK(count_ansi_sequences(ansi) >= 4);
  });
}

void test_code_block_syntax_highlighting_with_info_suffix() {
  tests::register_test("Markdown: fenced language info ignores trailing metadata", []() {
    const auto ansi = render_markdown_ansi(
        "```cpp linenums title=demo\nint main() { return 42; }\n```");
    const auto plain = strip_ansi(ansi);
    CHECK(plain.find("cpp") != std::string::npos);
    CHECK(plain.find("int main() { return 42; }") != std::string::npos);
    CHECK(has_ansi(ansi));
    CHECK(count_ansi_sequences(ansi) >= 4);
  });
}

void test_python_code_block_syntax_highlighting() {
  tests::register_test("Markdown: python fenced code block gets syntax highlighting", []() {
    const auto ansi = render_markdown_ansi(
        "```python\ndef greet(name):\n    return f\"Hello, {name}\"\n```");
    const auto plain = strip_ansi(ansi);
    CHECK(plain.find("python") != std::string::npos);
    CHECK(plain.find("def greet(name):") != std::string::npos);
    CHECK(plain.find("return f\"Hello, {name}\"") != std::string::npos);
    CHECK(has_ansi(ansi));
    CHECK(count_ansi_sequences(ansi) >= 4);
  });
}

void test_requested_language_code_block_syntax_highlighting() {
  struct Case {
    std::string_view name;
    std::string_view markdown;
    std::string_view expected;
  };

  const Case cases[] = {
      {"javascript",
       "```javascript\nfunction greet(name) { return 'Hi ' + name; }\n```",
       "function greet(name)"},
      {"typescript",
       "```typescript\ntype User = { name: string };\n"
       "function greet(user: User) { return user.name; }\n```",
       "function greet(user: User)"},
      {"tsx",
       "```tsx\nexport function Card() { return <div>{\"Hi\"}</div>; }\n```",
       "export function Card()"},
      {"rust", "```rust\nfn main() { let answer = 42; }\n```",
       "fn main()"},
      {"go", "```go\npackage main\nfunc main() { println(\"hi\") }\n```",
       "func main()"},
      {"markdown", "```markdown\n# Title\n\n- item\n```", "# Title"},
      {"ruby", "```ruby\ndef greet(name)\n  puts \"Hi #{name}\"\nend\n```",
       "def greet(name)"},
      {"lua", "```lua\nlocal function greet(name)\n  return \"Hi \" .. name\nend\n```",
       "local function greet(name)"},
  };

  for (const auto &c : cases) {
    tests::register_test(
        "Markdown: " + std::string(c.name) +
            " fenced code block gets syntax highlighting",
        [&c]() {
          const auto ansi = render_markdown_ansi(c.markdown);
          const auto plain = strip_ansi(ansi);
          CHECK(plain.find(c.name) != std::string::npos);
          CHECK(plain.find(c.expected) != std::string::npos);
          CHECK(has_ansi(ansi));
          CHECK(count_ansi_sequences(ansi) >= 4);
        });
  }
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

static std::string read_fd_available(int fd) {
  std::string out;
  char buf[256];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    break;
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

void test_stream_renderer_trailing_newlines_visible_immediately() {
  tests::register_test("Stream renderer: trailing newlines are preserved between updates", []() {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    CHECK(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

    {
      auto renderer = make_diff_renderer(fds[1]);
      renderer->update("Hello");
      CHECK_EQ(read_fd_available(fds[0]), std::string("Hello"));

      renderer->update("\n\n");
      CHECK_EQ(read_fd_available(fds[0]), std::string("\n\n"));

      renderer->update("World");
      CHECK_EQ(read_fd_available(fds[0]), std::string("World"));

      renderer->finish();
      CHECK_EQ(read_fd_available(fds[0]), std::string("\n"));
    }

    ::close(fds[1]);
    auto out = read_fd_all(fds[0]);
    ::close(fds[0]);

    CHECK_EQ(out, std::string(""));
  });
}

void test_stream_renderer_single_newline_delta() {
  tests::register_test("Stream renderer: single newline delta stays visible", []() {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    CHECK(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

    {
      auto renderer = make_diff_renderer(fds[1]);
      renderer->update("Hello");
      CHECK_EQ(read_fd_available(fds[0]), std::string("Hello"));

      renderer->update("\n");
      CHECK_EQ(read_fd_available(fds[0]), std::string("\n"));

      renderer->update("World");
      auto next = read_fd_available(fds[0]);
      CHECK(!next.empty());
    }

    ::close(fds[1]);
    (void)read_fd_all(fds[0]);
    ::close(fds[0]);
  });
}

void test_stream_renderer_code_block_highlights_when_completed() {
  tests::register_test("Stream renderer: fenced code block gains syntax highlighting when closed", []() {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    CHECK(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

    TerminalState term;
    std::string raw_out;

    {
      auto renderer = make_diff_renderer(fds[1]);

      renderer->update("```python\n");
      auto chunk = read_fd_available(fds[0]);
      raw_out += chunk;
      term.apply(chunk);

      renderer->update("def greet(name):\n");
      chunk = read_fd_available(fds[0]);
      raw_out += chunk;
      term.apply(chunk);

      renderer->update("    return f\"Hello, {name}\"\n");
      chunk = read_fd_available(fds[0]);
      raw_out += chunk;
      term.apply(chunk);

      CHECK_EQ(term.plain(),
               visible_markdown_plain(
                   "```python\ndef greet(name):\n    return f\"Hello, {name}\"\n"));

      renderer->update("```");
      chunk = read_fd_available(fds[0]);
      raw_out += chunk;
      term.apply(chunk);

      const auto completed =
          "```python\ndef greet(name):\n    return f\"Hello, {name}\"\n```";
      CHECK_EQ(term.plain(), visible_markdown_plain(completed));
      CHECK(chunk.find("\033[35mdef\033[0m") != std::string::npos);
      CHECK(chunk.find("\033[1;36mgreet\033[0m") != std::string::npos);
      CHECK(chunk.find("\033[35mreturn\033[0m") != std::string::npos);

      renderer->finish();
      chunk = read_fd_available(fds[0]);
      raw_out += chunk;
      term.apply(chunk);
      CHECK_EQ(term.plain(), visible_markdown_plain(completed) + "\n");
    }

    ::close(fds[1]);
    (void)read_fd_all(fds[0]);
    ::close(fds[0]);
  });
}

void test_stream_renderer_matches_rendered_markdown_incrementally() {
  tests::register_test("Stream renderer: visible state matches markdown render after each delta", []() {
    const std::vector<std::vector<std::string>> cases = {
        {"Hello", "\n", "World"},
        {"# He", "ading", "\n\n", "Body"},
        {"- item", " one", "\n", "- item two"},
        {"```cpp\n", "int x = 1;\n", "```"},
        {"Para", "\n\n", "> quote", "\n", "tail"},
    };

    for (const auto &parts : cases) {
      std::size_t part_index = 0;
      int fds[2];
      CHECK_EQ(::pipe(fds), 0);
      CHECK(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

      TerminalState term;
      auto renderer = make_diff_renderer(fds[1]);
      std::string input;

      for (const auto &part : parts) {
        input += part;
        renderer->update(part);
        term.apply(read_fd_available(fds[0]));
        const auto expected = visible_markdown_plain(input);
        if (term.plain() != expected) {
          std::cerr << "  stream case mismatch at part " << part_index
                    << " input=[" << input << "]\n"
                    << "    actual=[" << term.plain() << "]\n"
                    << "    expect=[" << expected << "]\n";
        }
        CHECK_EQ(term.plain(), expected);
        ++part_index;
      }

      renderer->finish();
      term.apply(read_fd_available(fds[0]));
      const auto expected_final = visible_markdown_plain(input) + "\n";
      if (term.plain() != expected_final) {
        std::cerr << "  stream final mismatch input=[" << input << "]\n"
                  << "    actual=[" << term.plain() << "]\n"
                  << "    expect=[" << expected_final << "]\n";
      }
      CHECK_EQ(term.plain(), expected_final);

      ::close(fds[1]);
      (void)read_fd_all(fds[0]);
      ::close(fds[0]);
    }
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
  test_code_block_syntax_highlighting();
  test_code_block_syntax_highlighting_with_info_suffix();
  test_python_code_block_syntax_highlighting();
  test_requested_language_code_block_syntax_highlighting();
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
  test_stream_renderer_trailing_newlines_visible_immediately();
  test_stream_renderer_single_newline_delta();
  test_stream_renderer_code_block_highlights_when_completed();
  test_stream_renderer_matches_rendered_markdown_incrementally();

  tests::print_summary();
  return tests::failed == 0 ? 0 : 1;
}
