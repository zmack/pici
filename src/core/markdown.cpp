#include "core/markdown.h"

extern "C" {
#include <md4c.h>
}

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {
namespace {

static constexpr std::string_view kReset   = "\033[0m";
static constexpr std::string_view kBold    = "\033[1m";
static constexpr std::string_view kDim     = "\033[2m";
static constexpr std::string_view kItalic  = "\033[3m";
static constexpr std::string_view kStrike  = "\033[9m";
static constexpr std::string_view kCode    = "\033[7m";   // reverse video for inline code
static constexpr std::string_view kH1      = "\033[1;95m";
static constexpr std::string_view kH2      = "\033[1;94m";
static constexpr std::string_view kH3      = "\033[1;96m";
static constexpr std::string_view kLinkDim = "\033[2;4m";

struct ListLevel {
  bool ordered;
  unsigned counter;
};

struct Renderer {
  std::string out;
  int quote_depth{0};
  std::vector<ListLevel> list_stack;
  bool li_pending_prefix{false};
  bool in_code_block{false};
  std::string pending_href;
  bool in_table_head{false};
  int table_col{0};

  void append(std::string_view s) { out += s; }

  void emit_list_prefix() {
    if (!li_pending_prefix) {
      return;
    }
    li_pending_prefix = false;
    for (std::size_t i = 0; i + 1 < list_stack.size(); ++i) {
      out += "  ";
    }
    if (!list_stack.empty()) {
      if (list_stack.back().ordered) {
        out += std::to_string(list_stack.back().counter++) + ". ";
      } else {
        out += "\xe2\x80\xa2 "; // UTF-8 bullet •
      }
    }
  }

  void emit_quote_prefix() {
    for (int i = 0; i < quote_depth; ++i) {
      out += "\xe2\x94\x82 "; // UTF-8 box-drawing │
    }
  }
};

static int cb_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
  auto *r = static_cast<Renderer *>(userdata);
  switch (type) {
  case MD_BLOCK_H: {
    auto *d = static_cast<MD_BLOCK_H_DETAIL *>(detail);
    switch (d->level) {
    case 1: r->append(kH1); break;
    case 2: r->append(kH2); break;
    case 3: r->append(kH3); break;
    default: r->append(kBold); break;
    }
    for (unsigned i = 0; i < d->level; ++i) {
      r->out += '#';
    }
    r->out += ' ';
    break;
  }
  case MD_BLOCK_CODE: {
    r->in_code_block = true;
    auto *d = static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
    r->append(kDim);
    if (d->info.size > 0) {
      r->out.append(d->info.text, d->info.size);
      r->out += '\n';
    }
    break;
  }
  case MD_BLOCK_QUOTE:
    ++r->quote_depth;
    break;
  case MD_BLOCK_UL:
    r->list_stack.push_back({false, 0});
    break;
  case MD_BLOCK_OL: {
    auto *d = static_cast<MD_BLOCK_OL_DETAIL *>(detail);
    r->list_stack.push_back({true, d->start});
    break;
  }
  case MD_BLOCK_LI: {
    auto *d = static_cast<MD_BLOCK_LI_DETAIL *>(detail);
    if (d->is_task) {
      r->li_pending_prefix = true;
      r->emit_list_prefix();
      r->out += (d->task_mark == 'x' || d->task_mark == 'X') ? "[x] " : "[ ] ";
    } else {
      r->li_pending_prefix = true;
    }
    break;
  }
  case MD_BLOCK_HR:
    r->append(kDim);
    for (int i = 0; i < 40; ++i) {
      r->out += "\xe2\x94\x80"; // UTF-8 ─
    }
    r->append(kReset);
    r->out += '\n';
    break;
  case MD_BLOCK_P:
    if (r->quote_depth > 0) {
      r->append(kDim);
      r->emit_quote_prefix();
      r->append(kReset);
    }
    break;
  case MD_BLOCK_THEAD:
    r->in_table_head = true;
    r->table_col = 0;
    break;
  case MD_BLOCK_TBODY:
    r->in_table_head = false;
    break;
  case MD_BLOCK_TR:
    r->table_col = 0;
    break;
  case MD_BLOCK_TH:
  case MD_BLOCK_TD:
    if (r->table_col++ > 0) {
      r->out += " \xe2\x94\x82 "; // │
    }
    if (type == MD_BLOCK_TH) {
      r->append(kBold);
    }
    break;
  default:
    break;
  }
  return 0;
}

static int cb_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
  auto *r = static_cast<Renderer *>(userdata);
  (void)detail;
  switch (type) {
  case MD_BLOCK_H:
    r->append(kReset);
    r->out += "\n\n";
    break;
  case MD_BLOCK_P:
    r->out += '\n';
    if (r->list_stack.empty() && r->quote_depth == 0) {
      r->out += '\n';
    }
    break;
  case MD_BLOCK_CODE:
    r->in_code_block = false;
    r->append(kReset);
    r->out += '\n';
    break;
  case MD_BLOCK_QUOTE:
    --r->quote_depth;
    r->out += '\n';
    break;
  case MD_BLOCK_UL:
  case MD_BLOCK_OL:
    if (!r->list_stack.empty()) {
      r->list_stack.pop_back();
    }
    if (r->list_stack.empty()) {
      r->out += '\n';
    }
    break;
  case MD_BLOCK_TH:
    r->append(kReset);
    break;
  case MD_BLOCK_TR:
    r->out += '\n';
    if (r->in_table_head) {
      r->append(kDim);
      for (int i = 0; i < 40; ++i) {
        r->out += "\xe2\x94\x80";
      }
      r->append(kReset);
      r->out += '\n';
    }
    break;
  default:
    break;
  }
  return 0;
}

static int cb_enter_span(MD_SPANTYPE type, void *detail, void *userdata) {
  auto *r = static_cast<Renderer *>(userdata);
  switch (type) {
  case MD_SPAN_STRONG:
    r->append(kBold);
    break;
  case MD_SPAN_EM:
    r->append(kItalic);
    break;
  case MD_SPAN_CODE:
    r->append(kCode);
    r->out += ' ';
    break;
  case MD_SPAN_DEL:
    r->append(kStrike);
    break;
  case MD_SPAN_A: {
    auto *d = static_cast<MD_SPAN_A_DETAIL *>(detail);
    r->pending_href = std::string(d->href.text, d->href.size);
    break;
  }
  default:
    break;
  }
  return 0;
}

static int cb_leave_span(MD_SPANTYPE type, void *detail, void *userdata) {
  auto *r = static_cast<Renderer *>(userdata);
  (void)detail;
  switch (type) {
  case MD_SPAN_STRONG:
  case MD_SPAN_EM:
  case MD_SPAN_DEL:
    r->append(kReset);
    break;
  case MD_SPAN_CODE:
    r->out += ' ';
    r->append(kReset);
    break;
  case MD_SPAN_A:
    if (!r->pending_href.empty()) {
      r->out += ' ';
      r->append(kLinkDim);
      r->out += '(';
      r->out += r->pending_href;
      r->out += ')';
      r->append(kReset);
      r->pending_href.clear();
    }
    break;
  default:
    break;
  }
  return 0;
}

static int cb_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size,
                   void *userdata) {
  auto *r = static_cast<Renderer *>(userdata);

  if (r->li_pending_prefix && !r->in_code_block &&
      type != MD_TEXT_BR && type != MD_TEXT_SOFTBR) {
    r->emit_list_prefix();
  }

  switch (type) {
  case MD_TEXT_SOFTBR:
    r->out += ' ';
    break;
  case MD_TEXT_BR:
    r->out += '\n';
    break;
  case MD_TEXT_NULLCHAR:
    r->out += "\xef\xbf\xbd"; // U+FFFD
    break;
  default:
    r->out.append(text, size);
    break;
  }
  return 0;
}

static MD_PARSER make_parser(unsigned flags) {
  MD_PARSER p{};
  p.abi_version = 0;
  p.flags = flags;
  p.enter_block = cb_enter_block;
  p.leave_block = cb_leave_block;
  p.enter_span = cb_enter_span;
  p.leave_span = cb_leave_span;
  p.text = cb_text;
  return p;
}

static constexpr unsigned kGfmFlags =
    MD_DIALECT_GITHUB | MD_FLAG_NOINDENTEDCODEBLOCKS;

} // namespace

std::string render_markdown_ansi(std::string_view input) {
  Renderer r;
  r.out.reserve(input.size() * 2);
  auto parser = make_parser(kGfmFlags);
  md_parse(input.data(), static_cast<MD_SIZE>(input.size()), &parser, &r);
  while (!r.out.empty() && r.out.back() == '\n') {
    r.out.pop_back();
  }
  r.out += '\n';
  return r.out;
}

std::string render_markdown_plain(std::string_view input) {
  // Parse with ANSI renderer then strip escape sequences.
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
