#include <iostream>
#include <string>

#include "core/markdown.h"

// Each section renders one construct in isolation with a label, then a divider.

static void show(const char *label, const char *markdown) {
  std::cout << "\033[2m── " << label << " ──\033[0m\n";
  std::cout << pi::core::render_markdown_ansi(markdown);
  std::cout << "\n";
}

int main() {
  std::cout << "\033[1;97m=== Markdown rendering demo ===\033[0m\n\n";

  show("plain paragraph",
       "The quick brown fox jumps over the lazy dog. "
       "This is a second sentence to pad the paragraph out a bit.");

  show("bold / italic / combined",
       "This has **bold text**, *italic text*, and ***bold italic*** together.");

  show("strikethrough",
       "The price is ~~$100~~ **$50** today only.");

  show("inline code",
       "Call `render_markdown_ansi(input)` and pass the result to `std::cout`.");

  show("h1 / h2 / h3",
       "# Heading one\n\n## Heading two\n\n### Heading three\n\n#### Heading four");

  show("unordered list",
       "- Alpha\n- Beta\n- Gamma");

  show("nested unordered list",
       "- Fruits\n  - Apple\n  - Banana\n- Vegetables\n  - Carrot");

  show("ordered list",
       "1. First item\n2. Second item\n3. Third item");

  show("task list",
       "- [x] Done\n- [ ] Not done\n- [x] Also done");

  show("fenced code block (no lang)",
       "```\nfor i in range(10):\n    print(i)\n```");

  show("fenced code block (with lang)",
       "```cpp\nstd::string greet(std::string_view name) {\n"
       "    return \"Hello, \" + std::string(name);\n}\n```");

  show("fenced python block",
       "```python\ndef greet(name):\n"
       "    message = f\"Hello, {name}\"\n"
       "    return message\n```");

  show("blockquote",
       "> The art of programming is the art of organizing complexity.\n"
       "> — Dijkstra");

  show("horizontal rule",
       "Above the rule\n\n---\n\nBelow the rule");

  show("link",
       "See the [project readme](https://github.com/example/pici) for details.");

  show("table",
       "| Name    | Role      | Score |\n"
       "| ------- | --------- | ----- |\n"
       "| Alice   | Engineer  | 98    |\n"
       "| Bob     | Designer  | 91    |");

  show("mixed — typical LLM response",
       "Here's how to reverse a string in C++:\n\n"
       "```cpp\n"
       "#include <algorithm>\n"
       "#include <string>\n\n"
       "std::string reverse(std::string s) {\n"
       "    std::reverse(s.begin(), s.end());\n"
       "    return s;\n"
       "}\n"
       "```\n\n"
       "Key points:\n\n"
       "- `std::reverse` operates **in-place**\n"
       "- The function takes the string *by value* so the original is untouched\n"
       "- Works with any sequence of `char`\n\n"
       "> **Note:** For Unicode text, reversing bytes is incorrect. Use a proper\n"
       "> Unicode library instead.\n\n"
       "See the [cppreference page](https://en.cppreference.com/w/cpp/algorithm/reverse)"
       " for the full signature.");

  show("long line (wrap test — make your terminal narrow)",
       "This is a very long line of plain text that should wrap at the terminal width boundary and cause the visual line counter to count more than one row for this single markdown paragraph.");

  return 0;
}
