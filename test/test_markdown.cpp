#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

#include "core/markdown.h"
#include "core/stream_renderer.h"

#include <gtest/gtest.h>

using namespace pi::core;

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

TEST(Markdown, PlainTextPassesThrough) {
  auto out = render_markdown_plain("Hello, world.");
  EXPECT_TRUE(out.find("Hello, world.") != std::string::npos);
}

TEST(Markdown, BoldHasANSIAndPlainContent) {
  auto ansi = render_markdown_ansi("**bold text**");
  EXPECT_TRUE(has_ansi(ansi));
  auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("bold text") != std::string::npos);
}

TEST(Markdown, ItalicHasANSIAndPlainContent) {
  auto ansi = render_markdown_ansi("*italic text*");
  EXPECT_TRUE(has_ansi(ansi));
  auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("italic text") != std::string::npos);
}

TEST(Markdown, InlineCodeHasANSIAndPlainContent) {
  auto ansi = render_markdown_ansi("Use `foo()` here.");
  EXPECT_TRUE(has_ansi(ansi));
  auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("foo()") != std::string::npos);
}

TEST(Markdown, HeadingsHavePrefixAndANSI) {
  auto h1 = render_markdown_ansi("# Title");
  EXPECT_TRUE(has_ansi(h1));
  EXPECT_TRUE(strip_ansi(h1).find("# Title") != std::string::npos);

  auto h2 = render_markdown_ansi("## Section");
  EXPECT_TRUE(strip_ansi(h2).find("## Section") != std::string::npos);

  auto h3 = render_markdown_ansi("### Sub");
  EXPECT_TRUE(strip_ansi(h3).find("### Sub") != std::string::npos);
}

TEST(Markdown, FencedCodeBlockHasANSIAndContent) {
  auto ansi = render_markdown_ansi("```\nfn main() {}\n```");
  EXPECT_TRUE(has_ansi(ansi));
  auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("fn main()") != std::string::npos);
}

TEST(Markdown, CodeBlockLanguageShown) {
  auto plain = strip_ansi(render_markdown_ansi("```rust\nlet x = 1;\n```"));
  EXPECT_TRUE(plain.find("rust") != std::string::npos);
  EXPECT_TRUE(plain.find("let x = 1;") != std::string::npos);
}

TEST(Markdown, SupportedFencedCodeBlockGetsSyntaxHighlighting) {
  const auto ansi =
      render_markdown_ansi("```cpp\nint main() { return 42; }\n```");
  const auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("cpp") != std::string::npos);
  EXPECT_TRUE(plain.find("int main() { return 42; }") != std::string::npos);
  EXPECT_TRUE(has_ansi(ansi));
  EXPECT_TRUE(count_ansi_sequences(ansi) >= 4);
}

TEST(Markdown, FencedLanguageInfoIgnoresTrailingMetadata) {
  const auto ansi = render_markdown_ansi(
      "```cpp linenums title=demo\nint main() { return 42; }\n```");
  const auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("cpp") != std::string::npos);
  EXPECT_TRUE(plain.find("int main() { return 42; }") != std::string::npos);
  EXPECT_TRUE(has_ansi(ansi));
  EXPECT_TRUE(count_ansi_sequences(ansi) >= 4);
}

TEST(Markdown, PythonFencedCodeBlockGetsSyntaxHighlighting) {
  const auto ansi = render_markdown_ansi(
      "```python\ndef greet(name):\n    return f\"Hello, {name}\"\n```");
  const auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("python") != std::string::npos);
  EXPECT_TRUE(plain.find("def greet(name):") != std::string::npos);
  EXPECT_TRUE(plain.find("return f\"Hello, {name}\"") != std::string::npos);
  EXPECT_TRUE(has_ansi(ansi));
  EXPECT_TRUE(count_ansi_sequences(ansi) >= 4);
}

TEST(Markdown, RequestedLanguageCodeBlockSyntaxHighlighting) {
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
      {"rust", "```rust\nfn main() { let answer = 42; }\n```", "fn main()"},
      {"go", "```go\npackage main\nfunc main() { println(\"hi\") }\n```",
       "func main()"},
      {"markdown", "```markdown\n# Title\n\n- item\n```", "# Title"},
      {"ruby", "```ruby\ndef greet(name)\n  puts \"Hi #{name}\"\nend\n```",
       "def greet(name)"},
      {"lua",
       "```lua\nlocal function greet(name)\n  return \"Hi \" .. name\nend\n```",
       "local function greet(name)"},
  };

  for (const auto &c : cases) {

    const auto ansi = render_markdown_ansi(c.markdown);
    const auto plain = strip_ansi(ansi);
    EXPECT_TRUE(plain.find(c.name) != std::string::npos);
    EXPECT_TRUE(plain.find(c.expected) != std::string::npos);
    EXPECT_TRUE(has_ansi(ansi));
    EXPECT_TRUE(count_ansi_sequences(ansi) >= 4);
  }
}

TEST(Markdown, UnorderedListHasBulletAndContent) {
  auto plain = strip_ansi(render_markdown_ansi("- alpha\n- beta\n- gamma"));
  EXPECT_TRUE(plain.find("alpha") != std::string::npos);
  EXPECT_TRUE(plain.find("beta") != std::string::npos);
  EXPECT_TRUE(plain.find("gamma") != std::string::npos);
  // Each item should be on its own line
  EXPECT_TRUE(plain.find('\n') != std::string::npos);
}

TEST(Markdown, OrderedListHasNumbersAndContent) {
  auto plain =
      strip_ansi(render_markdown_ansi("1. first\n2. second\n3. third"));
  EXPECT_TRUE(plain.find("1.") != std::string::npos);
  EXPECT_TRUE(plain.find("first") != std::string::npos);
  EXPECT_TRUE(plain.find("2.") != std::string::npos);
  EXPECT_TRUE(plain.find("second") != std::string::npos);
}

TEST(Markdown, StrikethroughHasANSIAndContent) {
  auto ansi = render_markdown_ansi("~~deleted~~");
  EXPECT_TRUE(has_ansi(ansi));
  auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("deleted") != std::string::npos);
}

TEST(Markdown, LinkShowsTextAndURL) {
  auto plain =
      strip_ansi(render_markdown_ansi("[click here](https://example.com)"));
  EXPECT_TRUE(plain.find("click here") != std::string::npos);
  EXPECT_TRUE(plain.find("https://example.com") != std::string::npos);
}

TEST(Markdown, HorizontalRuleRenders) {
  auto plain = strip_ansi(render_markdown_ansi("---"));
  EXPECT_TRUE(plain.find('\n') != std::string::npos);
  // Should contain some kind of line character
  EXPECT_TRUE(plain.size() > 2);
}

TEST(RenderMarkdownPlain, NoANSICodes) {
  auto plain = render_markdown_plain("**bold** and *italic*");
  EXPECT_TRUE(!has_ansi(plain));
  EXPECT_TRUE(plain.find("bold") != std::string::npos);
  EXPECT_TRUE(plain.find("italic") != std::string::npos);
}

TEST(Markdown, MixedFormattingRendersCorrectly) {
  const char *src = "# Hello\n\n"
                    "Some **bold** and *italic* text.\n\n"
                    "- item one\n"
                    "- item two\n\n"
                    "```cpp\nint x = 42;\n```\n";
  auto ansi = render_markdown_ansi(src);
  EXPECT_TRUE(has_ansi(ansi));
  auto plain = strip_ansi(ansi);
  EXPECT_TRUE(plain.find("# Hello") != std::string::npos);
  EXPECT_TRUE(plain.find("bold") != std::string::npos);
  EXPECT_TRUE(plain.find("italic") != std::string::npos);
  EXPECT_TRUE(plain.find("item one") != std::string::npos);
  EXPECT_TRUE(plain.find("int x = 42;") != std::string::npos);
}

TEST(Markdown, EmptyInputProducesSingleNewline) {
  auto out = render_markdown_ansi("");
  EXPECT_EQ(out, std::string("\n"));
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

TEST(StreamRenderer, PlainTextAppendsWithoutRedraw) {
  int fds[2];
  EXPECT_EQ(::pipe(fds), 0);

  {
    auto renderer = make_diff_renderer(fds[1]);
    renderer->on_text_delta("Hel");
    renderer->on_text_delta("lo");
    renderer->on_message_end({});
  }

  ::close(fds[1]);
  auto out = read_fd_all(fds[0]);
  ::close(fds[0]);

  EXPECT_EQ(out, std::string("Hello\n"));
}

TEST(StreamRenderer, ParagraphGrowthAppendsWithoutRedraw) {
  int fds[2];
  EXPECT_EQ(::pipe(fds), 0);

  {
    auto renderer = make_diff_renderer(fds[1]);
    renderer->on_text_delta("Hello");
    renderer->on_text_delta("\n\nWorld");
    renderer->on_message_end({});
  }

  ::close(fds[1]);
  auto out = read_fd_all(fds[0]);
  ::close(fds[0]);

  EXPECT_EQ(out, std::string("Hello\n\nWorld\n"));
}

TEST(StreamRenderer, TrailingNewlinesArePreservedBetweenUpdates) {
  int fds[2];
  EXPECT_EQ(::pipe(fds), 0);
  EXPECT_TRUE(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

  {
    auto renderer = make_diff_renderer(fds[1]);
    renderer->on_text_delta("Hello");
    EXPECT_EQ(read_fd_available(fds[0]), std::string("Hello"));

    renderer->on_text_delta("\n\n");
    EXPECT_EQ(read_fd_available(fds[0]), std::string("\n\n"));

    renderer->on_text_delta("World");
    EXPECT_EQ(read_fd_available(fds[0]), std::string("World"));

    renderer->on_message_end({});
    EXPECT_EQ(read_fd_available(fds[0]), std::string("\n"));
  }

  ::close(fds[1]);
  auto out = read_fd_all(fds[0]);
  ::close(fds[0]);

  EXPECT_EQ(out, std::string(""));
}

TEST(StreamRenderer, SingleNewlineDeltaStaysVisible) {
  int fds[2];
  EXPECT_EQ(::pipe(fds), 0);
  EXPECT_TRUE(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

  {
    auto renderer = make_diff_renderer(fds[1]);
    renderer->on_text_delta("Hello");
    EXPECT_EQ(read_fd_available(fds[0]), std::string("Hello"));

    renderer->on_text_delta("\n");
    EXPECT_EQ(read_fd_available(fds[0]), std::string("\n"));

    renderer->on_text_delta("World");
    auto next = read_fd_available(fds[0]);
    EXPECT_TRUE(!next.empty());
  }

  ::close(fds[1]);
  (void)read_fd_all(fds[0]);
  ::close(fds[0]);
}

TEST(StreamRenderer, FencedCodeBlockGainsSyntaxHighlightingWhenClosed) {
  int fds[2];
  EXPECT_EQ(::pipe(fds), 0);
  EXPECT_TRUE(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

  TerminalState term;
  std::string raw_out;

  {
    auto renderer = make_diff_renderer(fds[1]);

    renderer->on_text_delta("```python\n");
    auto chunk = read_fd_available(fds[0]);
    raw_out += chunk;
    term.apply(chunk);

    renderer->on_text_delta("def greet(name):\n");
    chunk = read_fd_available(fds[0]);
    raw_out += chunk;
    term.apply(chunk);

    renderer->on_text_delta("    return f\"Hello, {name}\"\n");
    chunk = read_fd_available(fds[0]);
    raw_out += chunk;
    term.apply(chunk);

    EXPECT_EQ(
        term.plain(),
        visible_markdown_plain(
            "```python\ndef greet(name):\n    return f\"Hello, {name}\"\n"));

    renderer->on_text_delta("```");
    chunk = read_fd_available(fds[0]);
    raw_out += chunk;
    term.apply(chunk);

    const auto completed =
        "```python\ndef greet(name):\n    return f\"Hello, {name}\"\n```";
    EXPECT_EQ(term.plain(), visible_markdown_plain(completed));
    EXPECT_TRUE(chunk.find("\033[35mdef\033[0m") != std::string::npos);
    EXPECT_TRUE(chunk.find("\033[1;36mgreet\033[0m") != std::string::npos);
    EXPECT_TRUE(chunk.find("\033[35mreturn\033[0m") != std::string::npos);

    renderer->on_message_end({});
    chunk = read_fd_available(fds[0]);
    raw_out += chunk;
    term.apply(chunk);
    EXPECT_EQ(term.plain(), visible_markdown_plain(completed) + "\n");
  }

  ::close(fds[1]);
  (void)read_fd_all(fds[0]);
  ::close(fds[0]);
}

TEST(StreamRenderer, VisibleStateMatchesMarkdownRenderAfterEachDelta) {
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
    EXPECT_EQ(::pipe(fds), 0);
    EXPECT_TRUE(::fcntl(fds[0], F_SETFL, O_NONBLOCK) >= 0);

    TerminalState term;
    auto renderer = make_diff_renderer(fds[1]);
    std::string input;

    for (const auto &part : parts) {
      input += part;
      renderer->on_text_delta(part);
      term.apply(read_fd_available(fds[0]));
      const auto expected = visible_markdown_plain(input);
      if (term.plain() != expected) {
        std::cerr << "  stream case mismatch at part " << part_index
                  << " input=[" << input << "]\n"
                  << "    actual=[" << term.plain() << "]\n"
                  << "    expect=[" << expected << "]\n";
      }
      EXPECT_EQ(term.plain(), expected);
      ++part_index;
    }

    renderer->on_message_end({});
    term.apply(read_fd_available(fds[0]));
    const auto expected_final = visible_markdown_plain(input) + "\n";
    if (term.plain() != expected_final) {
      std::cerr << "  stream final mismatch input=[" << input << "]\n"
                << "    actual=[" << term.plain() << "]\n"
                << "    expect=[" << expected_final << "]\n";
    }
    EXPECT_EQ(term.plain(), expected_final);

    ::close(fds[1]);
    (void)read_fd_all(fds[0]);
    ::close(fds[0]);
  }
}
