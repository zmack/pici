#pragma once

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "core/event_types.h"
#include "core/memory_stats.h"
#include "core/message_types.h"

namespace pi::core {

template <typename EventT, typename FinalResultT = std::monostate>
class EventStream {
private:
  struct State;
  struct WorkerState;

public:
  using Event = EventT;
  using FinalResult = FinalResultT;
  using PushFn = std::function<bool(Event)>;

  // Predicate to determine if an event signals completion
  using DonePredicate = std::function<bool(const Event &)>;
  // Extractor to convert final event to result
  using ResultExtractor = std::function<FinalResultT(const Event &)>;

  EventStream(DonePredicate done, ResultExtractor extract)
      : state_(std::make_shared<State>(std::move(done), std::move(extract))) {}

  EventStream(const EventStream &) = default;
  EventStream &operator=(const EventStream &) = default;
  EventStream(EventStream &&) noexcept = default;
  EventStream &operator=(EventStream &&) noexcept = default;
  ~EventStream() = default;

  // Start a joinable worker owned by the stream state. The worker receives a
  // push function instead of an EventStream instance to avoid a self-cycle.
  void start_worker(std::function<void(PushFn)> worker) {
    auto state = state_;
    auto worker_state = std::make_shared<WorkerState>();
    {
      std::scoped_lock lock(state->mutex);
      if (state->worker)
        return;
      state->worker = worker_state;
    }

    std::weak_ptr<State> weak_state = state;
    worker_state->thread =
        std::jthread([weak_state, worker = inherit_arena(std::move(worker))](
                         const std::stop_token &) mutable {
          // §Design 2: inherit_arena() captured the spawning thread's arena
          // context at wrap time; the ArenaInheritor re-binds it as this
          // thread's first action and restores on exit. Passthrough when
          // memory stats are unavailable.
          PushFn push = [weak_state](Event event) {
            auto state = weak_state.lock();
            if (!state)
              return false;
            return push_state(state, std::move(event));
          };
          worker(std::move(push));
        });
  }

  // Push an event. Returns true if the stream is not yet complete.
  bool push(Event event) { return push_state(state_, std::move(event)); }

  // Push and check if this event marks the stream as done.
  // If done, the result is computed and the stream is closed.
  bool push_and_check(Event event) {
    bool done = false;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->is_complete)
        return false;

      done = state_->done && state_->done(event);
      if (done) {
        state_->result =
            state_->extract ? state_->extract(event) : FinalResultT{};
        state_->has_result = true;
      } else {
        state_->has_error = false;
      }
      state_->queue.push(std::move(event));
      if (done) {
        state_->is_complete = true;
      }
    }
    state_->condition.notify_all();
    return !done;
  }

  // Push a final event that ends the stream
  void finish(FinalResultT result = FinalResultT{}) {
    {
      std::lock_guard lock(state_->mutex);
      state_->is_complete = true;
      state_->result = std::move(result);
      state_->has_result = true;
    }
    state_->condition.notify_all();
  }

  void finish_error(std::string error) {
    {
      std::lock_guard lock(state_->mutex);
      state_->is_complete = true;
      state_->error = std::move(error);
      state_->has_error = true;
    }
    state_->condition.notify_all();
  }

  bool is_done() {
    std::lock_guard lock(state_->mutex);
    return state_->is_complete;
  }

  // Blocks until an event is available or the stream is done.

  // Returns the next event, blocking. Returns std::nullopt when done.
  std::optional<Event> next() {
    std::unique_lock lock(state_->mutex);
    state_->condition.wait(
        lock, [this] { return !state_->queue.empty() || state_->is_complete; });
    if (state_->queue.empty()) {
      return std::nullopt;
    }
    Event ev = std::move(state_->queue.front());
    state_->queue.pop();
    return ev;
  }

  class iterator {
    EventStream *stream_{nullptr};

  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = Event;
    using difference_type = std::ptrdiff_t;

    iterator() = default;
    explicit iterator(EventStream *s) : stream_(s) { advance(); }

    Event &operator*() { return current_; }
    Event *operator->() { return &current_; }

    iterator &operator++() {
      advance();
      return *this;
    }

    bool operator==(const iterator &other) const {
      return has_value == other.has_value;
    }
    bool operator!=(const iterator &other) const { return !(*this == other); }

  private:
    Event current_;
    bool has_value{false};

    void advance() {
      if (stream_ == nullptr) {
        has_value = false;
        return;
      }
      auto ev = stream_->next();
      if (ev) {
        current_ = std::move(ev.value());
        has_value = true;
      } else {
        has_value = false;
      }
    }
  };

  iterator begin() { return iterator(this); }
  iterator end() { return iterator{}; }

  void for_each(std::function<void(const Event &)> callback) {
    for (auto &ev : *this) {
      callback(ev);
    }
  }

  std::pair<std::optional<FinalResultT>, std::optional<std::string>> wait() {
    std::unique_lock lock(state_->mutex);
    state_->condition.wait(lock, [this] { return state_->is_complete; });

    if (state_->has_result) {
      return {state_->result, std::nullopt};
    }
    if (state_->has_error) {
      return {std::optional<FinalResultT>{}, state_->error};
    }
    return {std::optional<FinalResultT>{}, std::nullopt};
  }

  std::vector<Event> drain() {
    std::vector<Event> events;
    {
      std::lock_guard lock(state_->mutex);
      while (!state_->queue.empty()) {
        events.push_back(std::move(state_->queue.front()));
        state_->queue.pop();
      }
    }
    return events;
  }

private:
  struct State {
    State(DonePredicate done_in, ResultExtractor extract_in)
        : done(std::move(done_in)), extract(std::move(extract_in)) {}

    std::mutex mutex;
    std::condition_variable condition;
    std::queue<Event> queue;

    DonePredicate done;
    ResultExtractor extract;

    bool is_complete{false};
    FinalResultT result{};
    bool has_result{false};

    std::string error;
    bool has_error{false};

    std::shared_ptr<WorkerState> worker;
  };

  struct WorkerState {
    std::jthread thread;

    WorkerState() = default;
    WorkerState(const WorkerState &) = delete;
    WorkerState &operator=(const WorkerState &) = delete;
    WorkerState(WorkerState &&) = delete;
    WorkerState &operator=(WorkerState &&) = delete;

    // The last owning reference to State (and therefore to this WorkerState)
    // can be dropped from inside the worker thread itself: start_worker's
    // push callback only holds a weak_ptr<State> (to avoid a State<->thread
    // reference cycle), and momentarily locks it into a strong ref while
    // publishing an event. If the external owner releases its reference in
    // that same window, this thread's own temporary lock ends up being the
    // last owner, so this destructor runs on the worker thread itself.
    // std::jthread's default destructor would then call join() on itself,
    // which throws (EDEADLK). Detach instead in that case; the thread is
    // already unwinding back to its entry point and will exit on its own.
    ~WorkerState() {
      if (thread.joinable() && thread.get_id() == std::this_thread::get_id())
        thread.detach();
    }
  };

  static bool push_state(const std::shared_ptr<State> &state, Event event) {
    bool complete = false;
    {
      std::lock_guard lock(state->mutex);
      if (state->is_complete)
        return false;
      complete = state->done && state->done(event);
      if (complete) {
        state->result = state->extract ? state->extract(event) : FinalResultT{};
        state->has_result = true;
      }
      state->queue.push(std::move(event));
      state->has_error = false;
      if (complete)
        state->is_complete = true;
    }
    state->condition.notify_all();
    return !complete;
  };

  std::shared_ptr<State> state_;
};

class AsyncEventStream {
public:
  using Event = AgentEvent;
  using Callback = std::function<void(const Event &)>;

  explicit AsyncEventStream(std::function<void()> on_complete = nullptr)
      : on_complete_(std::move(on_complete)) {}

  bool push(Event event) {
    bool is_last = false;
    {
      std::scoped_lock lock(mutex_);
      if (is_complete_)
        return false;

      // Run callback on a worker thread if one exists
      event_queue_.push_back(std::move(event));
      if (std::holds_alternative<AgentEndEvent>(event_queue_.back())) {
        is_last = true;
      }

      if (is_last) {
        is_complete_ = true;
      }
    }

    condition_.notify_all();

    if (is_last) {
      if (on_complete_)
        on_complete_();
    }

    return true;
  }

  // Block until complete
  void wait() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return is_complete_; });
  }

  // Get all queued events (non-blocking)
  std::vector<Event> drain() {
    std::vector<Event> events;
    {
      std::scoped_lock lock(mutex_);
      events = std::move(event_queue_);
    }
    return events;
  }

  bool is_complete() {
    std::scoped_lock lock(mutex_);
    return is_complete_;
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<Event> event_queue_;
  bool is_complete_{false};
  std::function<void()> on_complete_;
};

} // namespace pi::core
