#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/event_types.h"
#include "core/message_types.h"

#include "core/stream.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pi::core;

// ─── EventStream tests ────────────────────────────────────────────────────

TEST(EventStream, StreamBlockingNext) {
  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  std::atomic<int> consumed{0};

  std::thread consumer([&stream, &consumed]() {
    while (auto ev = stream.next()) {
      consumed++;
      if (consumed >= 3)
        break;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stream.push(AgentStartEvent());
  stream.push(TurnStartEvent());
  stream.push(AgentEndEvent(std::vector<Message>{}));

  consumer.join();
  stream.wait();

  EXPECT_GE(consumed, 3);
}

TEST(EventStream, StreamIterator) {
  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  stream.push(AgentStartEvent());
  stream.push(TurnStartEvent());
  stream.push(TurnStartEvent());
  stream.push(AgentEndEvent(std::vector<Message>{}));

  int count = 0;
  for (auto &ev : stream) {
    count++;
  }

  EXPECT_GE(count, 4);
}

TEST(EventStream, StreamConcurrentPushes) {
  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  std::atomic<int> pushed{0};
  std::vector<std::thread> writers;

  for (int i = 0; i < 10; i++) {
    writers.emplace_back([&stream, &pushed, i]() {
      switch (i % 3) {
      case 0:
        stream.push(AgentStartEvent());
        break;
      case 1:
        stream.push(TurnStartEvent());
        break;
      case 2:
        stream.push(AgentEndEvent(std::vector<Message>{}));
        break;
      }
      pushed++;
    });
  }

  for (auto &t : writers) {
    t.join();
  }

  EXPECT_EQ(pushed, 10);
}

TEST(EventStream, StreamDrain) {
  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  stream.push(AgentStartEvent());
  stream.push(TurnStartEvent());
  stream.push(
      MessageStartEvent(UserMessage{}, std::source_location::current()));

  auto events = stream.drain();
  EXPECT_EQ(events.size(), std::size_t(3));
}

TEST(EventStream, StreamFinishEmpty) {
  EventStream<AgentEvent, std::monostate> stream(
      [](const AgentEvent &) { return false; },
      [](const AgentEvent &) { return std::monostate{}; });

  stream.finish();

  auto [result, error] = stream.wait();
  EXPECT_FALSE(error.has_value());
}

TEST(EventStream, StreamSecondPushAfterFinish) {
  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  stream.push(AgentStartEvent());
  stream.finish(std::vector<Message>{});

  bool pushed = stream.push(AgentEndEvent(std::vector<Message>{}));
  EXPECT_FALSE(pushed);
}

TEST(EventStream, AsyncStreamCallback) {
  std::atomic<bool> callback_called{false};
  AsyncEventStream stream([&callback_called]() { callback_called = true; });

  stream.push(AgentStartEvent());
  stream.push(AgentEndEvent(std::vector<Message>{}));
  stream.wait();

  EXPECT_TRUE(callback_called.load());
  EXPECT_TRUE(stream.is_complete());
}

TEST(EventStream, AsyncStreamDrain) {
  AsyncEventStream stream;
  stream.push(AgentStartEvent());
  stream.push(TurnStartEvent());
  stream.push(AgentEndEvent(std::vector<Message>{}));
  stream.wait();

  auto events = stream.drain();
  EXPECT_EQ(events.size(), std::size_t(3));
}

TEST(EventStream, StreamManyEvents) {
  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  for (int i = 0; i < 100; i++) {
    switch (i % 3) {
    case 0:
      stream.push(AgentStartEvent());
      break;
    case 1:
      stream.push(TurnStartEvent());
      break;
    case 2:
      stream.push(AgentEndEvent(std::vector<Message>{}));
      break;
    }
  }

  int count = 0;
  for (auto &ev : stream) {
    count++;
  }

  EXPECT_EQ(count, 3);
}

// ─── Main ──────────────────────────────────────────────────────────────────
