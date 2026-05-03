#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

#include "core/message_types.h"

namespace pi::core {

// ─── AgentState: Thread-safe mutable agent state ───────────────────────────

class AgentState {
public:
  AgentState() = default;

  explicit AgentState(std::string system_prompt, Model model,
                      ThinkingLevel thinking_level = ThinkingLevel::off)
      : system_prompt_(std::move(system_prompt)), model_(std::move(model)),
        thinking_level_(thinking_level) {}

  // ── Properties ─────────────────────────────────────────────────────

  std::string system_prompt() const {
    std::scoped_lock lock(mutex_);
    return system_prompt_;
  }

  void set_system_prompt(std::string prompt) {
    std::scoped_lock lock(mutex_);
    system_prompt_ = std::move(prompt);
  }

  Model model() const {
    std::scoped_lock lock(mutex_);
    return model_;
  }

  void set_model(Model m) {
    std::scoped_lock lock(mutex_);
    model_ = std::move(m);
  }

  ThinkingLevel thinking_level() const {
    std::scoped_lock lock(mutex_);
    return thinking_level_;
  }

  void set_thinking_level(ThinkingLevel level) {
    std::scoped_lock lock(mutex_);
    thinking_level_ = level;
  }

  // ── Messages (transcript) ──────────────────────────────────────────

  std::vector<Message> messages() const {
    std::scoped_lock lock(mutex_);
    return messages_;
  }

  void set_messages(std::vector<Message> msgs) {
    std::scoped_lock lock(mutex_);
    messages_ = std::move(msgs);
  }

  void append_message(Message msg) {
    std::scoped_lock lock(mutex_);
    messages_.push_back(std::move(msg));
  }

  void append_messages(std::vector<Message> msgs) {
    std::scoped_lock lock(mutex_);
    messages_.insert(messages_.end(), std::make_move_iterator(msgs.begin()),
                     std::make_move_iterator(msgs.end()));
  }

  Message last_message() const {
    std::scoped_lock lock(mutex_);
    return messages_.back();
  }

  // ── Tools ──────────────────────────────────────────────────────────

  std::vector<std::shared_ptr<const ToolDefinition>> tools() const {
    std::scoped_lock lock(mutex_);
    return tools_;
  }

  void set_tools(std::vector<std::shared_ptr<const ToolDefinition>> t) {
    std::scoped_lock lock(mutex_);
    tools_ = std::move(t);
  }

  void add_tool(std::shared_ptr<const ToolDefinition> t) {
    std::scoped_lock lock(mutex_);
    tools_.push_back(std::move(t));
  }

  // ── Runtime state ──────────────────────────────────────────────────

  bool is_streaming() const {
    std::scoped_lock lock(mutex_);
    return is_streaming_;
  }

  bool is_complete() const {
    std::scoped_lock lock(mutex_);
    return is_complete_;
  }

  void set_streaming(bool streaming) {
    std::scoped_lock lock(mutex_);
    is_streaming_ = streaming;
  }

  void set_complete(bool complete) {
    std::scoped_lock lock(mutex_);
    is_complete_ = complete;
  }

  std::set<std::string> pending_tool_calls() const {
    std::scoped_lock lock(mutex_);
    return pending_tool_calls_;
  }

  void add_pending_tool_call(std::string call_id) {
    std::scoped_lock lock(mutex_);
    pending_tool_calls_.insert(std::move(call_id));
  }

  void remove_pending_tool_call(const std::string &call_id) {
    std::scoped_lock lock(mutex_);
    pending_tool_calls_.erase(call_id);
  }

  std::optional<std::string> error_message() const {
    std::scoped_lock lock(mutex_);
    return error_message_;
  }

  void set_error_message(std::string err) {
    std::scoped_lock lock(mutex_);
    error_message_ = std::move(err);
  }

  std::stop_token stop_token() const { return stop_tok_; }
  std::stop_source &stop_source() { return stop_src_; }

  // ── Reset ──────────────────────────────────────────────────────────

  void reset() {
    std::unique_lock lock(mutex_);
    system_prompt_.clear();
    model_ = Model{};
    thinking_level_ = ThinkingLevel::off;
    messages_.clear();
    tools_.clear();
    pending_tool_calls_.clear();
    error_message_.reset();
    is_streaming_ = false;
    is_complete_ = false;
  }

private:
  mutable std::mutex mutex_;
  std::string system_prompt_;
  Model model_;
  ThinkingLevel thinking_level_{ThinkingLevel::off};
  std::vector<Message> messages_;
  std::vector<std::shared_ptr<const ToolDefinition>> tools_;

  bool is_streaming_{false};
  bool is_complete_{false};
  std::set<std::string> pending_tool_calls_;
  std::optional<std::string> error_message_;

  std::stop_source stop_src_;
  std::stop_token stop_tok_{stop_src_.get_token()};
};

// ─── AgentContext: Snapshot passed to the loop ─────────────────────────────

struct AgentContext {
  std::string system_prompt;
  std::vector<Message> messages;
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
};

} // namespace pi::core
