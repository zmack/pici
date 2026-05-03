#pragma once

#include <atomic>
#include <condition_variable>
#include <concepts>
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
#include "core/message_types.h"

namespace pi::core {

template<typename EventT, typename FinalResultT = std::monostate>
class EventStream {
public:
    using Event = EventT;
    using FinalResult = FinalResultT;

    // Predicate to determine if an event signals completion
    using DonePredicate = std::function<bool(const Event&)>;
    // Extractor to convert final event to result
    using ResultExtractor = std::function<FinalResultT(const Event&)>;

    EventStream(DonePredicate done, ResultExtractor extract)
        : state_(std::make_shared<State>(std::move(done), std::move(extract))) {}

    EventStream(const EventStream&) = default;
    EventStream& operator=(const EventStream&) = default;
    EventStream(EventStream&&) noexcept = default;
    EventStream& operator=(EventStream&&) noexcept = default;

    // ── Push events ────────────────────────────────────────────────────

    // Push an event. Returns true if the stream is not yet complete.
    bool push(Event event) {
        bool complete = false;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->is_complete) return false;
            complete = state_->done && state_->done(event);
            if (complete) {
                state_->result = state_->extract ? state_->extract(event)
                                                 : FinalResultT{};
                state_->has_result = true;
            }
            state_->queue.push(std::move(event));
            state_->has_error = false;
            if (complete) {
                state_->is_complete = true;
            }
        }
        state_->condition.notify_all();
        return !complete;
    }

    // Push and check if this event marks the stream as done.
    // If done, the result is computed and the stream is closed.
    bool push_and_check(Event event) {
        bool done = false;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->is_complete) return false;

            done = state_->done && state_->done(event);
            if (done) {
                state_->result = state_->extract ? state_->extract(event)
                                                 : FinalResultT{};
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

    // ── Iterator interface ─────────────────────────────────────────────
    // Blocks until an event is available or the stream is done.

    // Returns the next event, blocking. Returns std::nullopt when done.
    std::optional<Event> next() {
        std::unique_lock lock(state_->mutex);
        state_->condition.wait(lock, [this] {
            return !state_->queue.empty() || state_->is_complete;
        });
        if (state_->queue.empty()) {
            return std::nullopt;
        }
        Event ev = std::move(state_->queue.front());
        state_->queue.pop();
        return ev;
    }

    // ── Range-based iteration ──────────────────────────────────────────

    class iterator {
        EventStream* stream_{nullptr};

    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Event;
        using difference_type = std::ptrdiff_t;

        iterator() = default;
        explicit iterator(EventStream* s) : stream_(s) { advance(); }

        Event& operator*() { return current_; }
        Event* operator->() { return &current_; }

        iterator& operator++() {
            advance();
            return *this;
        }

        bool operator==(const iterator&) const { return !has_value; }
        bool operator!=(const iterator&) const { return has_value; }

    private:
        Event current_;
        bool has_value{false};

        void advance() {
            if (!stream_) {
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

    // ── Consume all events via a callback ──────────────────────────────

    void for_each(std::function<void(const Event&)> callback) {
        for (auto& ev : *this) {
            callback(ev);
        }
    }

    // ── Block until done and return final result ───────────────────────

    std::pair<std::optional<FinalResultT>, std::optional<std::string>>
    wait() {
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

    // ── Drain remaining events ─────────────────────────────────────────

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
    };

    std::shared_ptr<State> state_;
};

// ─── AsyncEventStream: Non-blocking stream with callbacks ───────────────────

class AsyncEventStream {
public:
    using Event = AgentEvent;
    using Callback = std::function<void(const Event&)>;

    explicit AsyncEventStream(std::function<void()> on_complete = nullptr)
        : on_complete_(std::move(on_complete)) {}

    bool push(Event event) {
        bool is_last = false;
        {
            std::lock_guard lock(mutex_);
            if (is_complete_) return false;

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
            if (on_complete_) on_complete_();
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
            std::lock_guard lock(mutex_);
            events = std::move(event_queue_);
        }
        return events;
    }

    bool is_complete() {
        std::lock_guard lock(mutex_);
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
