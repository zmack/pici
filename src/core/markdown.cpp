#include "core/markdown.h"
#include "core/syntax_highlight.h"
#include <utility>

extern "C" {
#include <cmark.h>
}

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {
namespace {

constexpr std::string_view kReset = "\033[0m";
constexpr std::string_view kBold = "\033[1m";
constexpr std::string_view kDim = "\033[2m";
constexpr std::string_view kItalic = "\033[3m";
constexpr std::string_view kStrike = "\033[9m";
constexpr std::string_view kCode = "\033[7m";
constexpr std::string_view kH1 = "\033[1;95m";
constexpr std::string_view kH2 = "\033[1;94m";
constexpr std::string_view kH3 = "\033[1;96m";
constexpr std::string_view kLinkDim = "\033[2;4m";

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

const char *nonnull(const char *s) { return s == nullptr ? "" : s; }

std::string first_info_word(std::string_view info) {
  std::string out(info);
  const auto space = out.find_first_of(" \t\r\n");
  if (space != std::string::npos) {
    out.resize(space);
  }
  return out;
}

static void render_children(Renderer &r, cmark_node *node);

bool is_inline_node(cmark_node *node) {
  const auto type = cmark_node_get_type(node);
  return type >= CMARK_NODE_TEXT && type <= CMARK_NODE_IMAGE;
}

void render_text_with_strikethrough(Renderer &r, std::string_view text) {
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

void render_code_block(Renderer &r, cmark_node *node) {
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

// NOLINTNEXTLINE(misc-no-recursion)
void render_block(Renderer &r, cmark_node *node) {
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
    r.list_stack.push_back({.ordered = ordered, .counter = start});
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

// NOLINTNEXTLINE(misc-no-recursion)
void render_inline(Renderer &r, cmark_node *node) {
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
        url != nullptr && *url != '\0') {
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

// NOLINTNEXTLINE(misc-no-recursion)
void render_node(Renderer &r, cmark_node *node) {
  if (is_inline_node(node)) {
    render_inline(r, node);
  } else {
    render_block(r, node);
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void render_children(Renderer &r, cmark_node *node) {
  for (cmark_node *child = cmark_node_first_child(node); child != nullptr;
       child = cmark_node_next(child)) {
    render_node(r, child);
  }
}

// ── GFM table support
// ─────────────────────────────────────────────────────────
//
// cmark 0.31.1 does not parse GFM tables. We pre-process the input, detect
// table blocks (header + separator + optional body), render them with
// box-drawing characters, and pass everything else to cmark unchanged.

std::string_view trim_sv(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

// Split one table row on '|', stripping outer pipes and per-cell whitespace.
std::vector<std::string_view> split_row(std::string_view line) {
  if (!line.empty() && line.front() == '|')
    line.remove_prefix(1);
  if (!line.empty() && line.back() == '|')
    line.remove_suffix(1);
  std::vector<std::string_view> cells;
  std::size_t pos = 0;
  while (true) {
    const auto sep = line.find('|', pos);
    cells.push_back(trim_sv(
        line.substr(pos, sep == std::string_view::npos ? sep : sep - pos)));
    if (sep == std::string_view::npos)
      break;
    pos = sep + 1;
  }
  return cells;
}

// GFM separator row: only -, :, |, space/tab — and must contain both - and |.
bool is_table_sep(std::string_view line) {
  bool has_dash = false;
  bool has_pipe = false;
  for (char c : line) {
    switch (c) {
    case '-':
      has_dash = true;
      break;
    case '|':
      has_pipe = true;
      break;
    case ':':
    case ' ':
    case '\t':
      break;
    default:
      return false;
    }
  }
  return has_dash && has_pipe;
}

enum class Align { left, center, right };

Align parse_align(std::string_view cell) {
  cell = trim_sv(cell);
  const bool lc = !cell.empty() && cell.front() == ':';
  const bool rc = !cell.empty() && cell.back() == ':';
  if (lc && rc)
    return Align::center;
  if (rc)
    return Align::right;
  return Align::left;
}

std::string render_table(const std::vector<std::string_view> &block) {
  if (block.size() < 2)
    return {};

  auto header = split_row(block[0]);
  auto sep = split_row(block[1]);
  const std::size_t nc = header.size();

  std::vector<Align> aligns(nc, Align::left);
  for (std::size_t c = 0; c < std::min(nc, sep.size()); ++c)
    aligns[c] = parse_align(sep[c]);

  std::vector<std::vector<std::string_view>> rows;
  for (std::size_t i = 2; i < block.size(); ++i) {
    auto row = split_row(block[i]);
    row.resize(nc);
    rows.push_back(std::move(row));
  }

  std::vector<std::size_t> widths(nc, 1);
  for (std::size_t c = 0; c < nc; ++c) {
    widths[c] = std::max(widths[c], header[c].size());
    for (const auto &row : rows)
      widths[c] = std::max(widths[c], row[c].size());
  }

  static constexpr const char *kH = "\xe2\x94\x80";  // ─
  static constexpr const char *kV = "\xe2\x94\x82";  // │
  static constexpr const char *kTL = "\xe2\x94\x8c"; // ┌
  static constexpr const char *kTM = "\xe2\x94\xac"; // ┬
  static constexpr const char *kTR = "\xe2\x94\x90"; // ┐
  static constexpr const char *kML = "\xe2\x94\x9c"; // ├
  static constexpr const char *kMM = "\xe2\x94\xbc"; // ┼
  static constexpr const char *kMR = "\xe2\x94\xa4"; // ┤
  static constexpr const char *kBL = "\xe2\x94\x94"; // └
  static constexpr const char *kBM = "\xe2\x94\xb4"; // ┴
  static constexpr const char *kBR = "\xe2\x94\x98"; // ┘

  auto hline = [&](const char *l, const char *m, const char *r) {
    std::string s = l;
    for (std::size_t c = 0; c < nc; ++c) {
      if (c)
        s += m;
      for (std::size_t i = 0; i < widths[c] + 2; ++i)
        s += kH;
    }
    return s + r + '\n';
  };

  auto padded = [&](std::string_view text, std::size_t w, Align a) {
    const std::size_t len = text.size();
    const std::size_t pad = w > len ? w - len : 0;
    std::string s;
    if (a == Align::right) {
      s.append(pad, ' ');
      s += text;
    } else if (a == Align::center) {
      const std::size_t lp = pad / 2;
      s.append(lp, ' ');
      s += text;
      s.append(pad - lp, ' ');
    } else {
      s += text;
      s.append(pad, ' ');
    }
    return s;
  };

  auto dline = [&](const std::vector<std::string_view> &cells, bool bold) {
    std::string s = kV;
    for (std::size_t c = 0; c < nc; ++c) {
      s += ' ';
      if (bold) {
        s += kBold;
      }
      s += padded(cells[c], widths[c], aligns[c]);
      if (bold) {
        s += kReset;
      }
      s += ' ';
      s += kV;
    }
    return s + '\n';
  };

  std::string out;
  out += hline(kTL, kTM, kTR);
  out += dline(header, /*bold=*/true);
  out += hline(kML, kMM, kMR);
  for (const auto &row : rows)
    out += dline(row, /*bold=*/false);
  out += hline(kBL, kBM, kBR);
  out += '\n';
  return out;
}

} // namespace

std::string render_markdown_ansi(std::string_view input) {
  // Split into lines for table detection.
  std::vector<std::string_view> lines;
  {
    std::size_t pos = 0;
    while (pos < input.size()) {
      const auto nl = input.find('\n', pos);
      const auto end = (nl == std::string_view::npos) ? input.size() : nl;
      lines.push_back(input.substr(pos, end - pos));
      pos = end + 1;
    }
  }

  Renderer r;
  r.out.reserve(input.size() * 2);

  // Non-table lines are buffered here and flushed to cmark in one call so that
  // multi-paragraph structure is preserved.
  std::string buf;

  auto flush = [&]() {
    if (buf.empty())
      return;
    cmark_node *doc =
        cmark_parse_document(buf.data(), buf.size(), CMARK_OPT_VALIDATE_UTF8);
    if (doc) {
      render_node(r, doc);
      cmark_node_free(doc);
    }
    buf.clear();
  };

  std::size_t i = 0;
  while (i < lines.size()) {
    // Table detection: current line has '|' and next line is a GFM separator.
    if (i + 1 < lines.size() && lines[i].contains('|') &&
        is_table_sep(lines[i + 1])) {
      flush();
      // Collect contiguous non-blank lines as the table block.
      const std::size_t start = i;
      while (i < lines.size() && !lines[i].empty() && lines[i].contains('|'))
        ++i;
      r.out += render_table(
          {std::next(lines.begin(), static_cast<std::ptrdiff_t>(start)),
           std::next(lines.begin(), static_cast<std::ptrdiff_t>(i))});
    } else {
      buf += lines[i];
      buf += '\n';
      ++i;
    }
  }

  flush();

  while (!r.out.empty() && r.out.back() == '\n')
    r.out.pop_back();
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
