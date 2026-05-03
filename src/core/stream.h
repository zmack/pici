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
        : done_(std::move(done)), extract_(std::move(extract)) {}

    // Non-copyable
    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    // Movable (internal mutex is locked during move)
    EventStream(EventStream&& other) noexcept {
        std::scoped_lock lk(mutex_, other.mutex_);
        std::swap(queue_, other.queue_);
        std::swap(done_, other.done_);
        std::swap(extract_, other.extract_);
        std::swap(is_complete_, other.is_complete_);
        std::swap(result_, other.result_);
        std::swap(has_result_, other.has_result_);
        std::swap(has_error_, other.has_error_);
        std::swap(error_, other.error_);
    }

    EventStream& operator=(EventStream&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lk(mutex_, other.mutex_);
            std::swap(queue_, other.queue_);
            std::swap(done_, other.done_);
            std::swap(extract_, other.extract_);
            std::swap(is_complete_, other.is_complete_);
            std::swap(result_, other.result_);
            std::swap(has_result_, other.has_result_);
            std::swap(has_error_, other.has_error_);
            std::swap(error_, other.error_);
        }
        return *this;
    }

    // ── Push events ────────────────────────────────────────────────────

    // Push an event. Returns true if the stream is not yet complete.
    bool push(Event event) {
        {
            std::lock_guard lock(mutex_);
            if (is_complete_) return false;
            queue_.push(std::move(event));
            has_error_ = false;
            condition_.notify_one();
        }
        return !is_complete_;
    }

    // Push and check if this event marks the stream as done.
    // If done, the result is computed and the stream is closed.
    bool push_and_check(Event event) {
        bool done = false;
        {
            std::lock_guard lock(mutex_);
            if (is_complete_) return false;

            queue_.push(std::move(event));

            if (done_ && done_(queue_.front())) {
                done = true;
                result_ = extract_ ? extract_(queue_.front()) : FinalResultT{};
                has_result_ = true;
                queue_.pop();
            } else {
                has_error_ = false;
            }

            if (done) {
                // Drain remaining events if any, then close
                is_complete_ = true;
                condition_.notify_all();
            }
        }
        if (done) {
            condition_.notify_all();
        }
        return !is_complete_;
    }

    // Push a final event that ends the stream
    void finish(FinalResultT result) {
        std::lock_guard lock(mutex_);
        is_complete_ = true;
        result_ = std::move(result);
        has_result_ = true;
        condition_.notify_all();
    }

    void finish_error(std::string error) {
        std::lock_guard lock(mutex_);
        is_complete_ = true;
        error_ = std::move(error);
        has_error_ = true;
        condition_.notify_all();
    }

    bool is_done() {
        std::lock_guard lock(mutex_);
        return is_complete_;
    }

    // ── Iterator interface ─────────────────────────────────────────────
    // Blocks until an event is available or the stream is done.

    // Returns the next event, blocking. Returns std::nullopt when done.
    std::optional<Event> next() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] {
            return !queue_.empty() || is_complete_;
        });
        if (queue_.empty() && is_complete_) {
            return std::nullopt;
        }
        if (queue_.empty()) {
            return std::nullopt;
        }
        Event ev = std::move(queue_.front());
        queue_.pop();
        return ev;
    }

    // ── Range-based iteration ──────────────────────────────────────────

    class iterator {
        EventStream* stream_;

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
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return is_complete_; });

        if (has_result_) {
            return {std::move(result_), std::nullopt};
        }
        if (has_error_) {
            return {std::optional<FinalResultT>{}, std::move(error_)};
        }
        return {std::optional<FinalResultT>{}, std::nullopt};
    }

    // ── Drain remaining events ─────────────────────────────────────────

    std::vector<Event> drain() {
        std::vector<Event> events;
        {
            std::lock_guard lock(mutex_);
            while (!queue_.empty()) {
                events.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        }
        return events;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Event> queue_;

    DonePredicate done_;
    ResultExtractor extract_;

    bool is_complete_{false};
    FinalResultT result_;
    bool has_result_{false};

    std::string error_;
    bool has_error_{false};
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
            if (event_queue_.back().index() == 0) {
                // Check if it's an agent_end event
                if (std::holds_alternative<AgentEndEvent>(event_queue_.back())) {
                    is_last = true;
                }
            }

            if (is_last ||
                (event_queue_.back().index() == 0 &&
                 std::holds_alternative<AgentEndEvent>(event_queue_.back()))) {
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
