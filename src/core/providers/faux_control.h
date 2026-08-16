#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/faux.h"

namespace pi::core {

struct ScriptedToolUpdate {
  int after_ms{0};
  std::string partial;
};

struct ScriptedToolBehavior {
  std::vector<ScriptedToolUpdate> updates;
  std::string result_content;
  bool is_error{false};
  int finish_after_ms{0};
  std::optional<MailboxReplyQueuedNotice> presentation_notice;
};

class ScriptedToolRegistry {
public:
  void register_behavior(std::string call_id, ScriptedToolBehavior behavior);
  std::optional<ScriptedToolBehavior> take_behavior(const std::string &call_id);

private:
  std::mutex mutex_;
  std::unordered_map<std::string, ScriptedToolBehavior> behaviors_;
};

class RemoteFauxClient : public LLMClient {
public:
  void push_round(FauxClient::Script script);
  void close();

  std::shared_ptr<AssistantMessage> stream(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options,
                                           AssistantEventCallback on_event,
                                           std::stop_token stop_tok) override;

  std::string_view provider_name() const override { return "faux-control"; }
  std::string_view api_id() const override { return "faux-control"; }

private:
  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::deque<FauxClient::Script> queue_;
  bool closed_{false};
};

class ScriptedTool : public ToolDefinition {
public:
  ScriptedTool(std::string name,
               std::shared_ptr<ScriptedToolRegistry> registry);

  std::string_view name() const override { return name_; }
  std::string_view description() const override {
    return "Remote-controlled scripted tool (faux-control mode)";
  }
  ToolSchema &schema() const override;
  std::shared_ptr<ToolResult>
  execute(std::string_view call_id, std::string_view args_json,
          std::stop_token stop_tok,
          ToolUpdateCallback on_update) const override;
  std::shared_ptr<ToolResult>
  execute(std::string_view args_json,
          ToolExecutionContext context) const override;

private:
  std::string name_;
  std::shared_ptr<ScriptedToolRegistry> registry_;
};

std::optional<FauxClient::Script> compile_round(const nlohmann::json &round,
                                                ScriptedToolRegistry &registry,
                                                std::string &error);

} // namespace pi::core
