#include "core/markdown.h"
#include "core/syntax_highlight.h"

extern "C" {
#include <cmark.h>
}

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {
namespace {

static constexpr std::string_view kReset = "\033[0m";
static constexpr std::string_view kBold = "\033[1m";
static constexpr std::string_view kDim = "\033[2m";
static constexpr std::string_view kItalic = "\033[3m";
static constexpr std::string_view kStrike = "\033[9m";
static constexpr std::string_view kCode = "\033[7m";
static constexpr std::string_view kH1 = "\033[1;95m";
static constexpr std::string_view kH2 = "\033[1;94m";
static constexpr std::string_view kH3 = "\033[1;96m";
static constexpr std::string_view kLinkDim = "\033[2;4m";

struct ListLevel {
  bool ordered;
  int counter;
};

struct Renderer {
  std::string out;
  int quote_depth{0};
  std::vector<ListLevel> list_stack;

  void append(std::string_view s) { out += s; }

  void emit_list_prefix() {
    for (std::size_t i = 0; i + 1 < list_stack.size(); ++i) {
      out += "  ";
    }
    if (list_stack.empty()) {
      return;
    }
    if (list_stack.back().ordered) {
      out += std::to_string(list_stack.back().counter++) + ". ";
    } else {
      out += "\xe2\x80\xa2 ";
    }
  }

  void emit_quote_prefix() {
    for (int i = 0; i < quote_depth; ++i) {
      out += "\xe2\x94\x82 ";
    }
  }
};

static const char *nonnull(const char *s) { return s == nullptr ? "" : s; }

static std::string first_info_word(std::string_view info) {
  std::string out(info);
  const auto space = out.find_first_of(" \t\r\n");
  if (space != std::string::npos) {
    out.resize(space);
  }
  return out;
}

static void render_children(Renderer &r, cmark_node *node);

static bool is_inline_node(cmark_node *node) {
  const auto type = cmark_node_get_type(node);
  return type >= CMARK_NODE_TEXT && type <= CMARK_NODE_IMAGE;
}

static void render_text_with_strikethrough(Renderer &r, std::string_view text) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto start = text.find("~~", pos);
    if (start == std::string_view::npos) {
      r.out.append(text.substr(pos));
      return;
    }

    const auto end = text.find("~~", start + 2);
    if (end == std::string_view::npos) {
      r.out.append(text.substr(pos));
      return;
    }

    r.out.append(text.substr(pos, start - pos));
    r.append(kStrike);
    r.out.append(text.substr(start + 2, end - start - 2));
    r.append(kReset);
    pos = end + 2;
  }
}

static void render_code_block(Renderer &r, cmark_node *node) {
  const std::string language =
      first_info_word(nonnull(cmark_node_get_fence_info(node)));
  const std::string_view code = nonnull(cmark_node_get_literal(node));

  r.append(kDim);
  if (!language.empty()) {
    r.out += language;
    r.out += '\n';
  }

  if (!code.empty()) {
    if (supports_code_language(language)) {
      r.out += highlight_code_ansi(code, language);
    } else {
      r.append(kDim);
      r.out += code;
    }
  }

  r.append(kReset);
  r.out += '\n';
}

static void render_block(Renderer &r, cmark_node *node) {
  switch (cmark_node_get_type(node)) {
  case CMARK_NODE_DOCUMENT:
    render_children(r, node);
    break;
  case CMARK_NODE_PARAGRAPH:
    if (r.quote_depth > 0) {
      r.append(kDim);
      r.emit_quote_prefix();
      r.append(kReset);
    }
    render_children(r, node);
    r.out += '\n';
    if (r.list_stack.empty() && r.quote_depth == 0) {
      r.out += '\n';
    }
    break;
  case CMARK_NODE_HEADING: {
    const int level = cmark_node_get_heading_level(node);
    switch (level) {
    case 1:
      r.append(kH1);
      break;
    case 2:
      r.append(kH2);
      break;
    case 3:
      r.append(kH3);
      break;
    default:
      r.append(kBold);
      break;
    }
    for (int i = 0; i < level; ++i) {
      r.out += '#';
    }
    r.out += ' ';
    render_children(r, node);
    r.append(kReset);
    r.out += "\n\n";
    break;
  }
  case CMARK_NODE_CODE_BLOCK:
    render_code_block(r, node);
    break;
  case CMARK_NODE_BLOCK_QUOTE:
    ++r.quote_depth;
    render_children(r, node);
    --r.quote_depth;
    r.out += '\n';
    break;
  case CMARK_NODE_LIST: {
    const bool ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
    const int start = ordered ? cmark_node_get_list_start(node) : 0;
    r.list_stack.push_back({ordered, start});
    render_children(r, node);
    r.list_stack.pop_back();
    if (r.list_stack.empty()) {
      r.out += '\n';
    }
    break;
  }
  case CMARK_NODE_ITEM:
    r.emit_list_prefix();
    render_children(r, node);
    break;
  case CMARK_NODE_THEMATIC_BREAK:
    r.append(kDim);
    for (int i = 0; i < 40; ++i) {
      r.out += "\xe2\x94\x80";
    }
    r.append(kReset);
    r.out += '\n';
    break;
  case CMARK_NODE_HTML_BLOCK:
    r.out += nonnull(cmark_node_get_literal(node));
    r.out += '\n';
    break;
  default:
    render_children(r, node);
    break;
  }
}

static void render_inline(Renderer &r, cmark_node *node) {
  switch (cmark_node_get_type(node)) {
  case CMARK_NODE_TEXT:
    render_text_with_strikethrough(r, nonnull(cmark_node_get_literal(node)));
    break;
  case CMARK_NODE_SOFTBREAK:
    r.out += ' ';
    break;
  case CMARK_NODE_LINEBREAK:
    r.out += '\n';
    break;
  case CMARK_NODE_CODE:
    r.append(kCode);
    r.out += ' ';
    r.out += nonnull(cmark_node_get_literal(node));
    r.out += ' ';
    r.append(kReset);
    break;
  case CMARK_NODE_EMPH:
    r.append(kItalic);
    render_children(r, node);
    r.append(kReset);
    break;
  case CMARK_NODE_STRONG:
    r.append(kBold);
    render_children(r, node);
    r.append(kReset);
    break;
  case CMARK_NODE_LINK:
    render_children(r, node);
    if (const char *url = cmark_node_get_url(node);
        url != nullptr && url[0] != '\0') {
      r.out += ' ';
      r.append(kLinkDim);
      r.out += '(';
      r.out += url;
      r.out += ')';
      r.append(kReset);
    }
    break;
  case CMARK_NODE_HTML_INLINE:
    r.out += nonnull(cmark_node_get_literal(node));
    break;
  default:
    render_children(r, node);
    break;
  }
}

static void render_node(Renderer &r, cmark_node *node) {
  if (is_inline_node(node)) {
    render_inline(r, node);
  } else {
    render_block(r, node);
  }
}

static void render_children(Renderer &r, cmark_node *node) {
  for (cmark_node *child = cmark_node_first_child(node); child != nullptr;
       child = cmark_node_next(child)) {
    render_node(r, child);
  }
}

} // namespace

std::string render_markdown_ansi(std::string_view input) {
  Renderer r;
  r.out.reserve(input.size() * 2);

  cmark_node *doc =
      cmark_parse_document(input.data(), input.size(), CMARK_OPT_VALIDATE_UTF8);
  if (doc != nullptr) {
    render_node(r, doc);
    cmark_node_free(doc);
  }

  while (!r.out.empty() && r.out.back() == '\n') {
    r.out.pop_back();
  }
  r.out += '\n';
  return r.out;
}

std::string render_markdown_plain(std::string_view input) {
  auto ansi = render_markdown_ansi(input);
  std::string plain;
  plain.reserve(ansi.size());
  for (std::size_t i = 0; i < ansi.size(); ++i) {
    if (ansi[i] == '\033' && i + 1 < ansi.size() && ansi[i + 1] == '[') {
      i += 2;
      while (i < ansi.size() && ansi[i] != 'm') {
        ++i;
      }
    } else {
      plain += ansi[i];
    }
  }
  return plain;
}

} // namespace pi::core
