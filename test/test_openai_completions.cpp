#include <string>
#include <vector>

#include "core/agent_state.h"
#include "core/message_types.h"

#include "core/providers/openai_completions.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pi::core;

static Model make_model(std::string id = "gpt-4o",
                        std::string provider = "openai") {
  Model m;
  m.id = std::move(id);
  m.provider = std::move(provider);
  m.api = "openai-completions";
  return m;
}

static AgentContext make_context() {
  AgentContext ctx;
  return ctx;
}

TEST(OpenAICompletions, BuildRequestBasic) {

  OpenAICompatibleClient client;
  auto model = make_model("gpt-4o");
  auto ctx = make_context();
  StreamOptions opts;

  auto json = client.build_request_json(model, ctx, opts);

  EXPECT_EQ(json["model"].get<std::string>(), std::string("gpt-4o"));
  EXPECT_EQ(json["stream"].get<bool>(), true);
  ASSERT_TRUE(json["messages"].is_array());
}
TEST(OpenAICompletions, BuildRequestSystemPrompt) {

  OpenAICompatibleClient client;
  auto model = make_model();
  AgentContext ctx;
  ctx.system_prompt = "You are helpful";
  StreamOptions opts;

  auto json = client.build_request_json(model, ctx, opts);

  ASSERT_TRUE(json["messages"].is_array());
  EXPECT_FALSE(json["messages"].empty());
  EXPECT_EQ(json["messages"][0]["role"].get<std::string>(),
            std::string("system"));
  EXPECT_EQ(json["messages"][0]["content"].get<std::string>(),
            std::string("You are helpful"));
}
TEST(OpenAICompletions, BuildRequestUserMessage) {

  OpenAICompatibleClient client;
  auto model = make_model();
  AgentContext ctx;
  ctx.system_prompt = "sys";
  UserMessage um;
  um.content.push_back(TextContent{.text = "hello"});
  ctx.messages.push_back(std::move(um));
  StreamOptions opts;

  auto json = client.build_request_json(model, ctx, opts);
  auto &msgs = json["messages"];
  ASSERT_FALSE(msgs.empty());

  bool found_user = false;
  for (const auto &m : msgs) {
    if (m["role"] == "user") {
      EXPECT_EQ(m["content"].get<std::string>(), std::string("hello"));
      found_user = true;
    }
  }
  EXPECT_TRUE(found_user);
}
TEST(OpenAICompletions, BuildRequestRuntimeIdentity) {

  OpenAICompatibleClient client;
  auto model = make_model();
  AgentContext ctx;
  ctx.system_prompt = "sys";
  UserMessage identity;
  identity.content.emplace_back(TextContent{
      .text = "[pici runtime context; not user-authored]\n"
              "mailbox agent_id=agt_a; session_id=sess_a; kind=root;\n"
              "Use agents_self when you need the authoritative "
              "structured identity."});
  ctx.messages.emplace_back(std::move(identity));
  ctx.messages.emplace_back(
      UserMessage{.content = {TextContent{.text = "hello"}}});
  StreamOptions opts;

  const auto json = client.build_request_json(model, ctx, opts);
  EXPECT_EQ(json["messages"].size(), std::size_t(3));
  EXPECT_EQ(json["messages"][1]["role"].get<std::string>(), "user");
  EXPECT_THAT(json["messages"][1]["content"].get<std::string>(),
              testing::StartsWith("[pici runtime context; not user-authored]"));
  EXPECT_EQ(json["messages"][2]["content"].get<std::string>(), "hello");
}
TEST(OpenAICompletions, BuildRequestTemperature) {

  OpenAICompatibleClient client;
  auto model = make_model();
  auto ctx = make_context();
  StreamOptions opts;
  opts.temperature = 0.5;

  auto json = client.build_request_json(model, ctx, opts);

  EXPECT_TRUE(json.contains("temperature"));
  EXPECT_EQ(json["temperature"].get<double>(), 0.5);
}
TEST(OpenAICompletions, BuildRequestMaxTokens) {

  OpenAICompatibleClient client;

  {
    auto model = make_model();
    auto ctx = make_context();
    StreamOptions opts;
    opts.max_tokens = 1024;

    auto json = client.build_request_json(model, ctx, opts);
    ASSERT_TRUE(json.contains("max_completion_tokens"));
    EXPECT_FALSE(json.contains("max_tokens"));
    EXPECT_EQ(json["max_completion_tokens"].get<int>(), 1024);
  }

  {
    Model model = make_model("some-model", "other");
    model.base_url = "https://llm.chutes.ai/v1";
    auto ctx = make_context();
    StreamOptions opts;
    opts.max_tokens = 512;

    auto json = client.build_request_json(model, ctx, opts);
    ASSERT_TRUE(json.contains("max_tokens"));
    EXPECT_FALSE(json.contains("max_completion_tokens"));
    EXPECT_EQ(json["max_tokens"].get<int>(), 512);
  }
}
TEST(OpenAICompletions, BuildRequestLlamaCpp) {

  OpenAICompatibleClient client;
  auto model = make_model("Qwen3.6-35B-A3B-UD-IQ4_NL.gguf", "llamacpp");
  model.base_url = "http://127.0.0.1:8080/v1";
  auto ctx = make_context();
  StreamOptions opts;
  opts.max_tokens = 128;

  auto json = client.build_request_json(model, ctx, opts);

  EXPECT_EQ(json["model"].get<std::string>(),
            std::string("Qwen3.6-35B-A3B-UD-IQ4_NL.gguf"));
  ASSERT_TRUE(json.contains("max_tokens"));
  EXPECT_FALSE(json.contains("max_completion_tokens"));
  EXPECT_FALSE(json.contains("store"));
  EXPECT_FALSE(json.contains("stream_options"));
  EXPECT_EQ(json["stream"].get<bool>(), true);
  EXPECT_TRUE(json.contains("chat_template_kwargs"));
  EXPECT_EQ(json["chat_template_kwargs"]["enable_thinking"].get<bool>(), false);
}
TEST(OpenAICompletions, BuildRequestFireworks) {

  OpenAICompatibleClient client;
  auto model = make_model("accounts/fireworks/models/glm-5p2", "fireworks");
  model.base_url = "https://api.fireworks.ai/inference/v1";
  auto ctx = make_context();
  StreamOptions opts;
  opts.max_tokens = 256;

  auto json = client.build_request_json(model, ctx, opts);

  EXPECT_EQ(json["stream"].get<bool>(), true);
  ASSERT_TRUE(json.contains("max_tokens"));
  EXPECT_FALSE(json.contains("max_completion_tokens"));
  EXPECT_TRUE(json.contains("stream_options"));
}
TEST(OpenAICompletions, BuildRequestPromptCacheMeta) {

  OpenAICompatibleClient client;
  auto model = make_model("muse-spark-1.1", "meta-chat");
  model.base_url = "https://api.meta.ai/v1";
  auto ctx = make_context();
  StreamOptions opts;

  auto json = client.build_request_json(model, ctx, opts);

  EXPECT_TRUE(json.contains("prompt_cache_key"));
  EXPECT_EQ(json["prompt_cache_key"].get<std::string>(), std::string("pici"));
}
TEST(OpenAICompletions, BuildRequestPromptCacheOther) {

  OpenAICompatibleClient client;
  auto model = make_model("gpt-4o", "openai");
  auto ctx = make_context();
  StreamOptions opts;

  auto json = client.build_request_json(model, ctx, opts);

  EXPECT_FALSE(json.contains("prompt_cache_key"));
}
TEST(OpenAICompletions, MapFinishReasonStop) {

  EXPECT_EQ(OpenAICompatibleClient::map_finish_reason("stop"),
            StopReason::stop);
  EXPECT_EQ(OpenAICompatibleClient::map_finish_reason("end"), StopReason::stop);
}
TEST(OpenAICompletions, MapFinishReasonToolCalls) {

  EXPECT_EQ(OpenAICompatibleClient::map_finish_reason("tool_calls"),
            StopReason::tool_use);
  EXPECT_EQ(OpenAICompatibleClient::map_finish_reason("function_call"),
            StopReason::tool_use);
}
TEST(OpenAICompletions, MapFinishReasonUnknown) {

  EXPECT_EQ(OpenAICompatibleClient::map_finish_reason("xyz"),
            StopReason::error);
}
