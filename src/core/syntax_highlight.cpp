#include "core/syntax_highlight.h"
#include <cstdint>
#include <utility>

extern "C" {
#include <tree_sitter/api.h>
const TSLanguage *tree_sitter_bash();
const TSLanguage *tree_sitter_c();
const TSLanguage *tree_sitter_cpp();
const TSLanguage *tree_sitter_go();
const TSLanguage *tree_sitter_json();
const TSLanguage *tree_sitter_lua();
const TSLanguage *tree_sitter_markdown();
const TSLanguage *tree_sitter_python();
const TSLanguage *tree_sitter_ruby();
const TSLanguage *tree_sitter_rust();
const TSLanguage *tree_sitter_javascript();
const TSLanguage *tree_sitter_tsx();
const TSLanguage *tree_sitter_typescript();
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pi::core {
namespace {

constexpr std::string_view kReset = "\033[0m";
constexpr std::string_view kComment = "\033[2;32m";
constexpr std::string_view kString = "\033[33m";
constexpr std::string_view kKeyword = "\033[35m";
constexpr std::string_view kNumber = "\033[36m";
constexpr std::string_view kType = "\033[34m";
constexpr std::string_view kFunction = "\033[1;36m";
constexpr std::string_view kConstant = "\033[1;33m";
constexpr std::string_view kProperty = "\033[94m";

struct HighlightSpan {
  uint32_t start;
  uint32_t end;
  std::string_view style;
};

enum class LanguageId {
  Unknown,
  C,
  Cpp,
  Python,
  Bash,
  Json,
  JavaScript,
  TypeScript,
  Tsx,
  Rust,
  Go,
  Markdown,
  Ruby,
  Lua,
};

std::string to_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

LanguageId parse_language(std::string_view language) {
  const auto lang = to_lower(language);
  if (lang == "c") {
    return LanguageId::C;
  }
  if (lang == "cc" || lang == "cpp" || lang == "cxx" || lang == "c++") {
    return LanguageId::Cpp;
  }
  if (lang == "py" || lang == "python") {
    return LanguageId::Python;
  }
  if (lang == "bash" || lang == "sh" || lang == "shell" || lang == "zsh") {
    return LanguageId::Bash;
  }
  if (lang == "json") {
    return LanguageId::Json;
  }
  if (lang == "js" || lang == "javascript") {
    return LanguageId::JavaScript;
  }
  if (lang == "ts" || lang == "typescript") {
    return LanguageId::TypeScript;
  }
  if (lang == "tsx") {
    return LanguageId::Tsx;
  }
  if (lang == "rs" || lang == "rust") {
    return LanguageId::Rust;
  }
  if (lang == "go" || lang == "golang") {
    return LanguageId::Go;
  }
  if (lang == "md" || lang == "markdown") {
    return LanguageId::Markdown;
  }
  if (lang == "rb" || lang == "ruby") {
    return LanguageId::Ruby;
  }
  if (lang == "lua") {
    return LanguageId::Lua;
  }
  return LanguageId::Unknown;
}

const TSLanguage *language_for(LanguageId id) {
  switch (id) {
  case LanguageId::C:
    return tree_sitter_c();
  case LanguageId::Cpp:
    return tree_sitter_cpp();
  case LanguageId::Python:
    return tree_sitter_python();
  case LanguageId::Bash:
    return tree_sitter_bash();
  case LanguageId::Json:
    return tree_sitter_json();
  case LanguageId::JavaScript:
    return tree_sitter_javascript();
  case LanguageId::TypeScript:
    return tree_sitter_typescript();
  case LanguageId::Tsx:
    return tree_sitter_tsx();
  case LanguageId::Rust:
    return tree_sitter_rust();
  case LanguageId::Go:
    return tree_sitter_go();
  case LanguageId::Markdown:
    return tree_sitter_markdown();
  case LanguageId::Ruby:
    return tree_sitter_ruby();
  case LanguageId::Lua:
    return tree_sitter_lua();
  case LanguageId::Unknown:
    break;
  }
  return nullptr;
}

bool is_keyword_token(LanguageId id, std::string_view type) {
  static const std::unordered_set<std::string_view> common = {
      "if",          "else",     "for",        "while",     "return",
      "break",       "continue", "switch",     "case",      "default",
      "do",          "goto",     "sizeof",     "typedef",   "struct",
      "class",       "enum",     "union",      "namespace", "template",
      "typename",    "using",    "public",     "private",   "protected",
      "virtual",     "override", "const",      "static",    "inline",
      "extern",      "volatile", "constexpr",  "auto",      "new",
      "delete",      "try",      "catch",      "throw",     "import",
      "from",        "as",       "def",        "lambda",    "with",
      "yield",       "await",    "async",      "elif",      "except",
      "finally",     "pass",     "in",         "is",        "not",
      "and",         "or",       "then",       "fi",        "done",
      "function",    "select",   "until",      "local",     "declare",
      "export",      "unset",    "readonly",   "time",      "coproc",
      "esac",        "let",      "const",      "var",       "interface",
      "type",        "extends",  "implements", "package",   "func",
      "defer",       "go",       "chan",       "map",       "range",
      "fallthrough", "trait",    "impl",       "fn",        "mut",
      "pub",         "crate",    "super",      "self",      "Self",
      "match",       "where",    "loop",       "move",      "ref",
      "mod",         "use",      "begin",      "rescue",    "ensure",
      "end",         "module",   "unless",     "elsif",     "require",
      "include",     "extend",   "nil",        "and",       "or",
      "then",        "when",     "repeat",     "elseif"};

  if (common.contains(type)) {
    return true;
  }

  if (id == LanguageId::Json) {
    return type == "true" || type == "false" || type == "null";
  }
  if (id == LanguageId::Ruby) {
    return type == "true" || type == "false" || type == "nil";
  }
  if (id == LanguageId::Lua) {
    return type == "true" || type == "false" || type == "nil";
  }

  return false;
}

std::string_view style_for_node(LanguageId id, std::string_view type,
                                bool named) {
  if (type == "comment") {
    return kComment;
  }
  if (type == "atx_heading" || type == "setext_heading") {
    return kKeyword;
  }
  if (type == "fenced_code_block" || type == "code_fence_content" ||
      type == "indented_code_block") {
    return kString;
  }
  if (type == "link_destination" || type == "uri_autolink") {
    return kProperty;
  }
  if (type.contains("list_marker") || type.contains("delimiter")) {
    return kKeyword;
  }
  if (type.contains("string") || type == "char_literal") {
    return kString;
  }
  if (type == "regex" || type == "regex_literal" ||
      type == "interpreted_string_literal" || type == "raw_string_literal" ||
      type == "rune_literal") {
    return kString;
  }
  if (type.contains("escape")) {
    return kConstant;
  }
  if (type.contains("number") || type == "integer" || type == "float") {
    return kNumber;
  }
  if (type == "primitive_type" || type == "type_identifier" ||
      type == "sized_type_specifier" || type == "type_qualifier" ||
      type == "namespace_identifier" || type == "type" ||
      type == "predefined_type" || type == "built_in_type" ||
      type == "qualified_type" || type == "generic_type") {
    return kType;
  }
  if (type == "function_definition" || type == "call_expression" ||
      type == "function_declarator" || type == "function_declaration" ||
      type == "function_item" || type == "method_declaration" ||
      type == "method" || type == "call" || type == "identifier" ||
      type == "command_name") {
    return {};
  }
  if (type == "field_identifier" || type == "property_identifier" ||
      type == "shorthand_property_identifier_pattern" ||
      type == "member_identifier" || type == "label_name") {
    return kProperty;
  }
  if (type == "true" || type == "false" || type == "null" || type == "none" ||
      type == "None" || type == "nil") {
    return kConstant;
  }
  if (!named && is_keyword_token(id, type)) {
    return kKeyword;
  }
  return {};
}

bool node_covers_text(TSNode node) {
  if (ts_node_is_null(node)) {
    return false;
  }
  return ts_node_start_byte(node) < ts_node_end_byte(node);
}

void collect_highlights(LanguageId id, TSNode node,
                        std::vector<HighlightSpan> &spans) {
  const auto type = std::string_view(ts_node_type(node));
  const bool named = ts_node_is_named(node);
  if (const auto style = style_for_node(id, type, named); !style.empty()) {
    spans.push_back({.start = ts_node_start_byte(node),
                     .end = ts_node_end_byte(node),
                     .style = style});
    return;
  }

  const auto child_count = ts_node_child_count(node);
  for (uint32_t i = 0; i < child_count; ++i) {
    const TSNode child = ts_node_child(node, i);
    if (node_covers_text(child)) {
      collect_highlights(id, child, spans);
    }
  }
}

void add_identifier_highlights(LanguageId id, TSNode node,
                               std::vector<HighlightSpan> &spans) {
  const auto type = std::string_view(ts_node_type(node));
  if (id == LanguageId::C || id == LanguageId::Cpp) {
    if (type == "function_declarator" || type == "call_expression") {
      const TSNode ident = ts_node_child_by_field_name(node, "function", 8);
      if (node_covers_text(ident)) {
        spans.push_back({.start = ts_node_start_byte(ident),
                         .end = ts_node_end_byte(ident),
                         .style = kFunction});
      }
    }
  } else if (id == LanguageId::Python) {
    if (type == "function_definition") {
      const TSNode ident = ts_node_child_by_field_name(node, "name", 4);
      if (node_covers_text(ident)) {
        spans.push_back({.start = ts_node_start_byte(ident),
                         .end = ts_node_end_byte(ident),
                         .style = kFunction});
      }
    } else if (type == "call") {
      const TSNode ident = ts_node_child_by_field_name(node, "function", 8);
      if (node_covers_text(ident)) {
        spans.push_back({.start = ts_node_start_byte(ident),
                         .end = ts_node_end_byte(ident),
                         .style = kFunction});
      }
    }
  } else if (id == LanguageId::TypeScript || id == LanguageId::Tsx ||
             id == LanguageId::Rust || id == LanguageId::Go ||
             id == LanguageId::Ruby || id == LanguageId::Lua) {
    if (type == "function_declaration" || type == "function_definition" ||
        type == "function_item" || type == "method_declaration" ||
        type == "method" || type == "method_definition") {
      const TSNode ident = ts_node_child_by_field_name(node, "name", 4);
      if (node_covers_text(ident)) {
        spans.push_back({.start = ts_node_start_byte(ident),
                         .end = ts_node_end_byte(ident),
                         .style = kFunction});
      }
    } else if (type == "call_expression" || type == "call") {
      const TSNode ident = ts_node_child_by_field_name(node, "function", 8);
      if (node_covers_text(ident)) {
        spans.push_back({.start = ts_node_start_byte(ident),
                         .end = ts_node_end_byte(ident),
                         .style = kFunction});
      }
    }
  } else if (id == LanguageId::Json && type == "pair") {
    const TSNode key = ts_node_child_by_field_name(node, "key", 3);
    if (node_covers_text(key)) {
      spans.push_back({.start = ts_node_start_byte(key),
                       .end = ts_node_end_byte(key),
                       .style = kProperty});
    }
  } else if (id == LanguageId::Bash && type == "command_name") {
    spans.push_back({.start = ts_node_start_byte(node),
                     .end = ts_node_end_byte(node),
                     .style = kFunction});
  }

  const auto child_count = ts_node_child_count(node);
  for (uint32_t i = 0; i < child_count; ++i) {
    const TSNode child = ts_node_child(node, i);
    if (node_covers_text(child)) {
      add_identifier_highlights(id, child, spans);
    }
  }
}

std::string render_highlighted(std::string_view code,
                               std::vector<HighlightSpan> spans) {
  std::ranges::sort(spans, [](const HighlightSpan &a, const HighlightSpan &b) {
    if (a.start != b.start) {
      return a.start < b.start;
    }
    return a.end > b.end;
  });

  std::vector<HighlightSpan> filtered;
  filtered.reserve(spans.size());
  uint32_t covered_until = 0;
  for (const auto &span : spans) {
    if (span.end <= span.start || span.start < covered_until) {
      continue;
    }
    filtered.push_back(span);
    covered_until = span.end;
  }

  std::string out;
  out.reserve(code.size() * 2);
  uint32_t pos = 0;
  for (const auto &span : filtered) {
    if (pos < span.start) {
      out.append(code.substr(pos, span.start - pos));
    }
    out += span.style;
    out.append(code.substr(span.start, span.end - span.start));
    out += kReset;
    pos = span.end;
  }
  if (pos < code.size()) {
    out.append(code.substr(pos));
  }
  return out;
}

} // namespace

bool supports_code_language(std::string_view language) {
  return parse_language(language) != LanguageId::Unknown;
}

std::string highlight_code_ansi(std::string_view code,
                                std::string_view language) {
  const LanguageId id = parse_language(language);
  const TSLanguage *lang = language_for(id);
  if (lang == nullptr) {
    return std::string(code);
  }

  TSParser *parser = ts_parser_new();
  if (parser == nullptr) {
    return std::string(code);
  }

  if (!ts_parser_set_language(parser, lang)) {
    ts_parser_delete(parser);
    return std::string(code);
  }

  TSTree *tree =
      ts_parser_parse_string(parser, nullptr, code.data(), code.size());
  ts_parser_delete(parser);
  if (tree == nullptr) {
    return std::string(code);
  }

  std::vector<HighlightSpan> spans;
  TSNode root = ts_tree_root_node(tree);
  collect_highlights(id, root, spans);
  add_identifier_highlights(id, root, spans);
  ts_tree_delete(tree);

  if (spans.empty()) {
    return std::string(code);
  }
  return render_highlighted(code, std::move(spans));
}

} // namespace pi::core
