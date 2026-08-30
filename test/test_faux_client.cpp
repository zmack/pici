#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <source_location>
#include <stop_token>
#include <string>
#include <vector>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/faux.h"

using namespace pi::core;

// ─── EventRecorder helper ─────────────────────────────────────────────────

struct EventRecorder {
  std::vector<AssistantMessageEvent> events;
  AssistantEventCallback callback() {
    return [this](const AssistantMessageEvent &ev) { events.push_back(ev); };
  }
};

// ─── Helpers to build test messages/models ────────────────────────────────

static Model make_model() {
  Model m;
  m.id = "faux-model";
  m.api = "faux";
  m.provider = "faux";
  return m;
}

static AgentContext make_ctx() { return AgentContext{}; }

static StreamOptions make_opts() { return StreamOptions{}; }

// ─── Tests ────────────────────────────────────────────────────────────────

TEST(FauxClient, EmitsStartAndDoneEvents) {
  AssistantMessage partial;
  partial.model = "faux-model";

  AssistantMessage final_msg;
  final_msg.model = "faux-model";
  final_msg.stop_reason = StopReason::stop;

  FauxClient::Script script;
  script.events.push_back(AssistantMessageStartEvent{partial});
  script.events.push_back(
      AssistantMessageDoneEvent{StopReason::stop, final_msg});

  FauxClient client({script});
  EventRecorder rec;

  auto result = client.stream(make_model(), make_ctx(), make_opts(),
                              rec.callback(), std::stop_token{});

  EXPECT_TRUE(rec.events.size() == 2);
  EXPECT_TRUE(std::get_if<AssistantMessageStartEvent>(&rec.events[0]) !=
              nullptr);
  EXPECT_TRUE(std::get_if<AssistantMessageDoneEvent>(&rec.events[1]) !=
              nullptr);
  EXPECT_TRUE(result != nullptr);
  EXPECT_TRUE(result->stop_reason == StopReason::stop);
}

TEST(FauxClient, EmitsTextDeltasInOrder) {
  AssistantMessage partial;
  partial.model = "faux-model";

  AssistantMessage final_msg;
  final_msg.model = "faux-model";
  final_msg.stop_reason = StopReason::stop;

  FauxClient::Script script;
  script.events.push_back(AssistantMessageStartEvent{partial});
  script.events.push_back(AssistantMessageTextStartEvent{0, partial});
  script.events.push_back(AssistantMessageTextDeltaEvent{0, "hello", partial});
  script.events.push_back(AssistantMessageTextDeltaEvent{0, " world", partial});
  script.events.push_back(
      AssistantMessageTextEndEvent{0, "hello world", partial});
  script.events.push_back(
      AssistantMessageDoneEvent{StopReason::stop, final_msg});

  FauxClient client({script});
  EventRecorder rec;

  client.stream(make_model(), make_ctx(), make_opts(), rec.callback(),
                std::stop_token{});

  std::vector<std::string> deltas;
  for (const auto &ev : rec.events) {
    if (const auto *d = std::get_if<AssistantMessageTextDeltaEvent>(&ev)) {
      deltas.push_back(d->delta);
    }
  }

  EXPECT_TRUE(deltas.size() == 2);
  EXPECT_TRUE(deltas[0] == "hello");
  EXPECT_TRUE(deltas[1] == " world");
}

TEST(FauxClient, ReturnsErrorWhenScriptsExhausted) {
  FauxClient client({});
  EventRecorder rec;

  auto result = client.stream(make_model(), make_ctx(), make_opts(),
                              rec.callback(), std::stop_token{});

  EXPECT_TRUE(result != nullptr);
  EXPECT_TRUE(result->stop_reason == StopReason::error);
  EXPECT_TRUE(rec.events.size() == 1);
  EXPECT_TRUE(std::get_if<AssistantMessageErrorEvent>(&rec.events[0]) !=
              nullptr);
}

TEST(FauxClient, AdvancesCallCountAcrossCalls) {
  AssistantMessage msg1;
  msg1.model = "faux-model";
  msg1.stop_reason = StopReason::stop;
  msg1.content.push_back(TextContent{"first"});

  AssistantMessage msg2;
  msg2.model = "faux-model";
  msg2.stop_reason = StopReason::stop;
  msg2.content.push_back(TextContent{"second"});

  FauxClient::Script s1, s2;
  s1.events.push_back(AssistantMessageDoneEvent{StopReason::stop, msg1});
  s2.events.push_back(AssistantMessageDoneEvent{StopReason::stop, msg2});

  FauxClient client({s1, s2});

  auto r1 = client.stream(make_model(), make_ctx(), make_opts(), {},
                          std::stop_token{});
  auto r2 = client.stream(make_model(), make_ctx(), make_opts(), {},
                          std::stop_token{});

  EXPECT_TRUE(r1 != nullptr && r1->stop_reason == StopReason::stop);
  EXPECT_TRUE(r2 != nullptr && r2->stop_reason == StopReason::stop);

  const auto *t1 =
      r1->content.empty() ? nullptr : std::get_if<TextContent>(&r1->content[0]);
  const auto *t2 =
      r2->content.empty() ? nullptr : std::get_if<TextContent>(&r2->content[0]);
  EXPECT_TRUE(t1 != nullptr && t1->text == "first");
  EXPECT_TRUE(t2 != nullptr && t2->text == "second");
}

TEST(FauxClient, RespectsStopToken) {
  AssistantMessage partial;
  partial.model = "faux-model";

  AssistantMessage final_msg;
  final_msg.model = "faux-model";
  final_msg.stop_reason = StopReason::stop;

  FauxClient::Script script;
  script.delay_between = std::chrono::milliseconds(1);
  for (int i = 0; i < 5; ++i) {
    script.events.push_back(AssistantMessageTextDeltaEvent{0, "x", partial});
  }
  script.events.push_back(
      AssistantMessageDoneEvent{StopReason::stop, final_msg});

  FauxClient client({script});

  std::stop_source src;
  std::atomic<int> count{0};

  auto cb = [&](const AssistantMessageEvent &ev) {
    count++;
    if (count >= 2) {
      src.request_stop();
    }
  };

  client.stream(make_model(), make_ctx(), make_opts(), cb, src.get_token());

  EXPECT_TRUE(count < 5);
}
