#pragma once

#include <cstdint>
#include <optional>
#include <utility>

// Feature gate: PI_MEMSTATS_HAVE_MALLCTL is 1 only when compiled with
// -DPI_CPP_MEMSTATS_ENABLED on a glibc x86_64/arm64 target — i.e. exactly
// when dlsym(RTLD_DEFAULT, "mallctl") can possibly resolve.
#if defined(PI_CPP_MEMSTATS_ENABLED) && defined(__GLIBC__) &&                  \
    (defined(__x86_64__) || defined(__aarch64__))
#define PI_MEMSTATS_HAVE_MALLCTL 1 // NOLINT(cppcoreguidelines-macro-usage)
#else
#define PI_MEMSTATS_HAVE_MALLCTL 0 // NOLINT(cppcoreguidelines-macro-usage)
#endif

// Per-session allocator-level memory accounting (plan:
// plans/session-memory-stats.md §Design 2). Resolves jemalloc's mallctl via
// dlsym at startup and attributes allocations to per-session arenas by
// binding the threads that actually allocate.
//
// Availability: every function here is a safe no-op / nullopt when
// memory_stats_available() is false, so call sites never need #ifdefs.
// True availability requires BOTH that dlsym resolved mallctl AND that the
// startup canary proved jemalloc is this process's active global allocator
// (weak-symbol resolution or --as-needed can otherwise leave glibc malloc
// in charge even with jemalloc present in the process).

namespace pi::core {

bool memory_stats_available();

struct SessionArena {
  unsigned index{0};

  friend bool operator==(const SessionArena &, const SessionArena &) = default;
};

// Creates a new jemalloc arena, or reuses one from the recycle pool. Arenas
// are never destroyed for the life of the process; they are recycled between
// sessions (see release_session_arena).
std::optional<SessionArena> acquire_session_arena();

// Unbinds any thread still bound to the arena, purges its free pages back to
// the OS, and returns the index to the recycle pool. Deliberately does NOT
// call arena.<i>.destroy — see plan §Design 2 ("no destroy, only
// purge-and-recycle"). Safe to call with a nullopt (no-op) or on an already-
// released arena.
void release_session_arena(std::optional<SessionArena> arena);

// Reads/sets the calling thread's arena context (TLS mirror plus the real
// jemalloc thread.arena mallctl).
std::optional<SessionArena> current_arena();
void bind_current_thread(SessionArena arena);
void unbind_current_thread(); // back to jemalloc's default pool

#if PI_MEMSTATS_HAVE_MALLCTL

namespace detail {

// Callable wrapper used by inherit_arena(). Defined here, inside the same
// namespace block and below the declarations it names, so declaration order
// can never break the way a separately-included internal header would.
template <typename F> class ArenaInheritor {
public:
  ArenaInheritor(std::optional<SessionArena> arena, F &&f)
      : arena_(std::move(arena)), f_(std::forward<F>(f)) {}

  template <typename... Args>
  auto operator()(Args &&...args)
      -> decltype(std::declval<F &>()(std::forward<Args>(args)...)) {
    // Re-bind the arena captured at wrap time as this thread's very first
    // action, then restore whatever the thread had before. If nothing was
    // captured (stats unavailable on the capturing thread) run unchanged.
    struct Restore {
      std::optional<SessionArena> previous;
      bool restore{false};
      ~Restore() {
        if (!restore)
          return;
        if (previous.has_value())
          bind_current_thread(*previous);
        else
          unbind_current_thread();
      }
    };
    if (!arena_.has_value())
      return f_(std::forward<Args>(args)...);
    Restore guard{current_arena(), true};
    bind_current_thread(*arena_);
    return f_(std::forward<Args>(args)...);
  }

private:
  std::optional<SessionArena> arena_;
  F f_;
};

} // namespace detail

#endif // PI_MEMSTATS_HAVE_MALLCTL

// Wraps a callable so that when it runs — on whatever thread that turns out
// to be — it first binds that thread to the arena captured from the calling
// thread at wrap time, runs the callable, then restores the previous
// binding. This is what carries a session's arena across the jthread spawn
// chain in Agent/EventStream/AgentTaskManager. Zero-overhead passthrough
// when memory stats are unavailable at compile time.
template <typename F> auto inherit_arena(F &&f) {
#if !PI_MEMSTATS_HAVE_MALLCTL
  return std::forward<F>(f);
#else
  return detail::ArenaInheritor<std::decay_t<F>>(current_arena(),
                                                 std::forward<F>(f));
#endif
}

struct ArenaStats {
  std::uint64_t allocated_bytes{0}; // small.allocated + large.allocated
};

std::optional<ArenaStats> read_arena_stats(SessionArena arena);

struct ProcessMemorySnapshot {
  std::uint64_t rss_bytes{0}; // getrusage ru_maxrss; always available
  std::optional<std::uint64_t> allocator_allocated_bytes; // stats.allocated
  std::optional<std::uint64_t> allocator_resident_bytes;  // stats.resident
};

ProcessMemorySnapshot read_process_snapshot();

} // namespace pi::core
