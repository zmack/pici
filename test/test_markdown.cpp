#include <fcntl.h>
#include <ostream>
#include <string>
#include <string_view>
#include <unistd.h>

#include "core/markdown.h"
#include "core/stream_renderer.h"

#include <gmock/gmock.h>
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
  EXPECT_THAT(out, testing::HasSubstr("Hello, world."));
}

TEST(Markdown, BoldHasANSIAndPlainContent) {
  auto ansi = render_markdown_ansi("**bold text**");
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("bold text"));
}

TEST(Markdown, ItalicHasANSIAndPlainContent) {
  auto ansi = render_markdown_ansi("*italic text*");
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("italic text"));
}

TEST(Markdown, InlineCodeHasANSIAndPlainContent) {
  auto ansi = render_markdown_ansi("Use `foo()` here.");
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("foo()"));
}

TEST(Markdown, HeadingsHavePrefixAndANSI) {
  auto h1 = render_markdown_ansi("# Title");
  EXPECT_THAT(h1, testing::HasSubstr("\033["));
  EXPECT_THAT(strip_ansi(h1), testing::HasSubstr("# Title"));

  auto h2 = render_markdown_ansi("## Section");
  EXPECT_THAT(strip_ansi(h2), testing::HasSubstr("## Section"));

  auto h3 = render_markdown_ansi("### Sub");
  EXPECT_THAT(strip_ansi(h3), testing::HasSubstr("### Sub"));
}

TEST(Markdown, FencedCodeBlockHasANSIAndContent) {
  auto ansi = render_markdown_ansi("```\nfn main() {}\n```");
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("fn main()"));
}

TEST(Markdown, CodeBlockLanguageShown) {
  auto plain = strip_ansi(render_markdown_ansi("```rust\nlet x = 1;\n```"));
  EXPECT_THAT(plain, testing::HasSubstr("rust"));
  EXPECT_THAT(plain, testing::HasSubstr("let x = 1;"));
}

TEST(Markdown, SupportedFencedCodeBlockGetsSyntaxHighlighting) {
  const auto ansi =
      render_markdown_ansi("```cpp\nint main() { return 42; }\n```");
  const auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("cpp"));
  EXPECT_THAT(plain, testing::HasSubstr("int main() { return 42; }"));
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  EXPECT_GE(count_ansi_sequences(ansi), 4);
}

TEST(Markdown, FencedLanguageInfoIgnoresTrailingMetadata) {
  const auto ansi = render_markdown_ansi(
      "```cpp linenums title=demo\nint main() { return 42; }\n```");
  const auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("cpp"));
  EXPECT_THAT(plain, testing::HasSubstr("int main() { return 42; }"));
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  EXPECT_GE(count_ansi_sequences(ansi), 4);
}

TEST(Markdown, PythonFencedCodeBlockGetsSyntaxHighlighting) {
  const auto ansi = render_markdown_ansi(
      "```python\ndef greet(name):\n    return f\"Hello, {name}\"\n```");
  const auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("python"));
  EXPECT_THAT(plain, testing::HasSubstr("def greet(name):"));
  EXPECT_THAT(plain, testing::HasSubstr("return f\"Hello, {name}\""));
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  EXPECT_GE(count_ansi_sequences(ansi), 4);
}

struct RequestedLanguageCase {
  std::string_view name;
  std::string_view markdown;
  std::string_view expected;
};

void PrintTo(const RequestedLanguageCase &value, std::ostream *stream) {
  *stream << "{language=" << value.name << ", expected=" << value.expected
          << ", markdown=" << value.markdown << "}";
}

class RequestedLanguageCodeBlockTest
    : public testing::TestWithParam<RequestedLanguageCase> {};

TEST_P(RequestedLanguageCodeBlockTest, SyntaxHighlighting) {
  const auto &c = GetParam();
  const auto ansi = render_markdown_ansi(c.markdown);
  const auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr(c.name));
  EXPECT_THAT(plain, testing::HasSubstr(c.expected));
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  EXPECT_GE(count_ansi_sequences(ansi), 4);
}

INSTANTIATE_TEST_SUITE_P(
    SupportedLanguages, RequestedLanguageCodeBlockTest,
    testing::Values(
        RequestedLanguageCase{
            "javascript",
            "```javascript\nfunction greet(name) { return 'Hi ' + name; }\n```",
            "function greet(name)"},
        RequestedLanguageCase{
            "typescript",
            "```typescript\ntype User = { name: string };\n"
            "function greet(user: User) { return user.name; }\n```",
            "function greet(user: User)"},
        RequestedLanguageCase{"tsx",
                              "```tsx\nexport function Card() { return "
                              "<div>{\"Hi\"}</div>; }\n```",
                              "export function Card()"},
        RequestedLanguageCase{"rust",
                              "```rust\nfn main() { let answer = 42; }\n```",
                              "fn main()"},
        RequestedLanguageCase{
            "go", "```go\npackage main\nfunc main() { println(\"hi\") }\n```",
            "func main()"},
        RequestedLanguageCase{"markdown", "```markdown\n# Title\n\n- item\n```",
                              "# Title"},
        RequestedLanguageCase{
            "ruby", "```ruby\ndef greet(name)\n  puts \"Hi #{name}\"\nend\n```",
            "def greet(name)"},
        RequestedLanguageCase{"lua",
                              "```lua\nlocal function greet(name)\n  return "
                              "\"Hi \" .. name\nend\n```",
                              "local function greet(name)"}),
    [](const testing::TestParamInfo<RequestedLanguageCase> &info) {
      return std::string(info.param.name);
    });

TEST(Markdown, UnorderedListHasBulletAndContent) {
  auto plain = strip_ansi(render_markdown_ansi("- alpha\n- beta\n- gamma"));
  EXPECT_THAT(plain, testing::HasSubstr("alpha"));
  EXPECT_THAT(plain, testing::HasSubstr("beta"));
  EXPECT_THAT(plain, testing::HasSubstr("gamma"));
  // Each item should be on its own line
  EXPECT_THAT(plain, testing::HasSubstr("\n"));
}

TEST(Markdown, OrderedListHasNumbersAndContent) {
  auto plain =
      strip_ansi(render_markdown_ansi("1. first\n2. second\n3. third"));
  EXPECT_THAT(plain, testing::HasSubstr("1."));
  EXPECT_THAT(plain, testing::HasSubstr("first"));
  EXPECT_THAT(plain, testing::HasSubstr("2."));
  EXPECT_THAT(plain, testing::HasSubstr("second"));
}

TEST(Markdown, StrikethroughHasANSIAndContent) {
  auto ansi = render_markdown_ansi("~~deleted~~");
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("deleted"));
}

TEST(Markdown, LinkShowsTextAndURL) {
  auto plain =
      strip_ansi(render_markdown_ansi("[click here](https://example.com)"));
  EXPECT_THAT(plain, testing::HasSubstr("click here"));
  EXPECT_THAT(plain, testing::HasSubstr("https://example.com"));
}

TEST(Markdown, HorizontalRuleRenders) {
  auto plain = strip_ansi(render_markdown_ansi("---"));
  EXPECT_THAT(plain, testing::HasSubstr("\n"));
  // Should contain some kind of line character
  EXPECT_GT(plain.size(), 2U);
}

TEST(RenderMarkdownPlain, NoANSICodes) {
  auto plain = render_markdown_plain("**bold** and *italic*");
  EXPECT_THAT(plain, testing::Not(testing::HasSubstr("\033[")));
  EXPECT_THAT(plain, testing::HasSubstr("bold"));
  EXPECT_THAT(plain, testing::HasSubstr("italic"));
}

TEST(Markdown, MixedFormattingRendersCorrectly) {
  const char *src = "# Hello\n\n"
                    "Some **bold** and *italic* text.\n\n"
                    "- item one\n"
                    "- item two\n\n"
                    "```cpp\nint x = 42;\n```\n";
  auto ansi = render_markdown_ansi(src);
  EXPECT_THAT(ansi, testing::HasSubstr("\033["));
  auto plain = strip_ansi(ansi);
  EXPECT_THAT(plain, testing::HasSubstr("# Hello"));
  EXPECT_THAT(plain, testing::HasSubstr("bold"));
  EXPECT_THAT(plain, testing::HasSubstr("italic"));
  EXPECT_THAT(plain, testing::HasSubstr("item one"));
  EXPECT_THAT(plain, testing::HasSubstr("int x = 42;"));
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
  ASSERT_GE(::fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);

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
  ASSERT_GE(::fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);

  {
    auto renderer = make_diff_renderer(fds[1]);
    renderer->on_text_delta("Hello");
    EXPECT_EQ(read_fd_available(fds[0]), std::string("Hello"));

    renderer->on_text_delta("\n");
    EXPECT_EQ(read_fd_available(fds[0]), std::string("\n"));

    renderer->on_text_delta("World");
    auto next = read_fd_available(fds[0]);
    EXPECT_FALSE(next.empty());
  }

  ::close(fds[1]);
  (void)read_fd_all(fds[0]);
  ::close(fds[0]);
}

TEST(StreamRenderer, FencedCodeBlockGainsSyntaxHighlightingWhenClosed) {
  int fds[2];
  EXPECT_EQ(::pipe(fds), 0);
  ASSERT_GE(::fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);

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
    EXPECT_THAT(chunk, testing::HasSubstr("\033[35mdef\033[0m"));
    EXPECT_THAT(chunk, testing::HasSubstr("\033[1;36mgreet\033[0m"));
    EXPECT_THAT(chunk, testing::HasSubstr("\033[35mreturn\033[0m"));

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

  for (std::size_t case_index = 0; case_index < cases.size(); ++case_index) {
    SCOPED_TRACE("stream case " + std::to_string(case_index));
    const auto &parts = cases[case_index];
    std::size_t part_index = 0;
    int fds[2];
    EXPECT_EQ(::pipe(fds), 0);
    ASSERT_GE(::fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);

    TerminalState term;
    auto renderer = make_diff_renderer(fds[1]);
    std::string input;

    for (const auto &part : parts) {
      SCOPED_TRACE("stream part " + std::to_string(part_index));
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
