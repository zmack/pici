#include "core/memory_stats.h"

#include <bits/types/struct_rusage.h>
#include <dlfcn.h>
#include <optional>
#include <sys/resource.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace pi::core {

namespace {

#if PI_MEMSTATS_HAVE_MALLCTL

using MallctlFn = int (*)(const char *, void *, size_t *, void *, size_t);

// jemalloc's sentinel meaning "the default arena pool" for thread.arena.
constexpr unsigned kArenaAll = 0xFFFFFFFFu;

struct MemoryStatsRuntime {
  MallctlFn mallctl{nullptr};
  std::atomic<bool> initialized{false};
  std::atomic<bool> available{false};

  std::mutex mutex;
  std::vector<unsigned> recycle_pool;
  std::set<unsigned> live_arenas;
};

MemoryStatsRuntime &runtime() {
  static MemoryStatsRuntime instance;
  return instance;
}

// Per-thread mirror of the bound session arena. The real binding lives in
// jemalloc's own TLS (set via the thread.arena mallctl); this slot exists so
// a spawning thread can capture its arena and hand it to a child thread via
// inherit_arena().
std::optional<SessionArena> &thread_arena_slot() {
  thread_local std::optional<SessionArena> slot;
  return slot;
}

// jemalloc caches all stats.<...> values per "epoch": they only refresh
// when the epoch mallctl is written (jemalloc(3): "stats.allocated ... The
// contents are only synchronized when the epoch is advanced"). Without
// this, every read returns whatever was true at process start.
void refresh_stats_epoch() {
  std::uint64_t epoch = 1;
  size_t sz = sizeof(epoch);
  static_cast<void>(runtime().mallctl("epoch", &epoch, &sz, &epoch, sz));
}

bool mallctl_u64(const char *name, std::uint64_t *value) {
  refresh_stats_epoch();
  size_t size = sizeof(*value);
  return runtime().mallctl(name, value, &size, nullptr, 0) == 0;
}

bool mallctl_unsigned(const char *name, unsigned *value) {
  size_t size = sizeof(*value);
  return runtime().mallctl(name, value, &size, nullptr, 0) == 0;
}

// Startup canary (§Design 1): a resolved mallctl symbol does NOT prove the
// process's global allocator is jemalloc (weak symbols, link order,
// --as-needed can leave glibc malloc in charge). Allocate through the normal
// malloc path and confirm stats.allocated moved by roughly that much.
bool run_allocation_canary() {
  const char *const kName = "stats.allocated";
  constexpr size_t kCanaryBytes = 8u << 20; // 8 MiB
  constexpr int kAttempts = 3;

  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    std::uint64_t before = 0;
    if (!mallctl_u64(kName, &before))
      return false; // stats disabled or not jemalloc semantics
    auto *canary = static_cast<std::byte *>(std::malloc(kCanaryBytes));
    if (canary == nullptr)
      return false;
    std::memset(canary, 0xAB, kCanaryBytes); // make it live
    std::uint64_t after = 0;
    const bool read_ok = mallctl_u64(kName, &after);
    std::free(canary);
    if (!read_ok)
      return false;
    const auto delta =
        static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before);
    // Tolerate concurrent churn from background threads; demand most of the
    // canary show up as live allocator bytes.
    if (delta >= static_cast<std::int64_t>(kCanaryBytes) / 2)
      return true;
  }
  return false;
}

void ensure_initialized() {
  auto &rt = runtime();
  if (rt.initialized.load(std::memory_order_acquire))
    return;
  std::scoped_lock lock(rt.mutex);
  if (rt.initialized.load(std::memory_order_relaxed))
    return;
  rt.mallctl = reinterpret_cast<MallctlFn>(dlsym(
      RTLD_DEFAULT, "mallctl")); // NOLINT(cppcoreguidelines-init-variables)
  bool ok = rt.mallctl != nullptr;
  if (ok)
    ok = run_allocation_canary();
  rt.available.store(ok, std::memory_order_relaxed);
  rt.initialized.store(true, std::memory_order_release);
}

bool mallctl_purge_arena(unsigned index) {
  const auto name = "arena." + std::to_string(index) + ".purge";
  return runtime().mallctl(name.c_str(), nullptr, nullptr, nullptr, 0) == 0;
}

#endif

} // namespace

bool memory_stats_available() {
#if !PI_MEMSTATS_HAVE_MALLCTL
  return false;
#else
  ensure_initialized();
  return runtime().available.load(std::memory_order_relaxed);
#endif
}

std::optional<SessionArena> acquire_session_arena() {
#if !PI_MEMSTATS_HAVE_MALLCTL
  return std::nullopt;
#else
  ensure_initialized();
  auto &rt = runtime();
  if (!rt.available.load(std::memory_order_relaxed))
    return std::nullopt;

  std::scoped_lock lock(rt.mutex);
  if (!rt.recycle_pool.empty()) {
    const unsigned index = rt.recycle_pool.back();
    rt.recycle_pool.pop_back();
    rt.live_arenas.insert(index);
    return SessionArena{index};
  }
  unsigned index = 0;
  size_t size = sizeof(index);
  // arenas.create: creates a non-default arena and returns its index.
  if (rt.mallctl("arenas.create", &index, &size, nullptr, 0) != 0)
    return std::nullopt;
  rt.live_arenas.insert(index);
  return SessionArena{index};
#endif
}

void release_session_arena(std::optional<SessionArena> arena) {
#if PI_MEMSTATS_HAVE_MALLCTL
  if (!arena.has_value())
    return;
  ensure_initialized();
  auto &rt = runtime();
  if (!rt.available.load(std::memory_order_relaxed))
    return;

  std::scoped_lock lock(rt.mutex);
  // Only retire arenas this module created and that are currently live.
  // Releasing an unknown/recycled index is a caller bug; ignore it rather
  // than corrupting the pool.
  if (rt.live_arenas.erase(arena->index) == 0)
    return;
  // Purge always-safe (returns currently-free pages to the OS). Deliberately
  // no arena.<i>.destroy here: see plan §Design 2 — destroy's flush-all-
  // thread-caches precondition cannot be met in this codebase because the
  // parent reads child results after close.
  static_cast<void>(mallctl_purge_arena(arena->index));
  rt.recycle_pool.push_back(arena->index);
#else
  (void)arena;
#endif
}

std::optional<SessionArena> current_arena() {
#if !PI_MEMSTATS_HAVE_MALLCTL
  return std::nullopt;
#else
  ensure_initialized();
  if (!runtime().available.load(std::memory_order_relaxed))
    return std::nullopt;
  return thread_arena_slot();
#endif
}

void bind_current_thread(SessionArena arena) {
#if PI_MEMSTATS_HAVE_MALLCTL
  ensure_initialized();
  auto &rt = runtime();
  if (!rt.available.load(std::memory_order_relaxed))
    return;
  {
    std::scoped_lock lock(rt.mutex);
    if (!rt.live_arenas.contains(arena.index))
      return; // never bind an unknown/recycled arena
  }
  thread_arena_slot() = arena;
  unsigned index = arena.index;
  static_cast<void>(
      rt.mallctl("thread.arena", nullptr, nullptr, &index, sizeof(index)));
#else
  (void)arena;
#endif
}

void unbind_current_thread() {
#if PI_MEMSTATS_HAVE_MALLCTL
  ensure_initialized();
  auto &rt = runtime();
  if (!rt.available.load(std::memory_order_relaxed))
    return;
  thread_arena_slot().reset();
  unsigned pool = kArenaAll;
  static_cast<void>(
      rt.mallctl("thread.arena", nullptr, nullptr, &pool, sizeof(pool)));
#endif
}

std::optional<ArenaStats> read_arena_stats(SessionArena arena) {
#if !PI_MEMSTATS_HAVE_MALLCTL
  (void)arena;
  return std::nullopt;
#else
  ensure_initialized();
  auto &rt = runtime();
  if (!rt.available.load(std::memory_order_relaxed))
    return std::nullopt;
  {
    std::scoped_lock lock(rt.mutex);
    if (!rt.live_arenas.contains(arena.index))
      return std::nullopt;
  }
  // There is no stats.arenas.<i>.allocated; sum small + large extents.
  const auto small_name =
      "stats.arenas." + std::to_string(arena.index) + ".small.allocated";
  const auto large_name =
      "stats.arenas." + std::to_string(arena.index) + ".large.allocated";
  std::uint64_t small = 0;
  std::uint64_t large = 0;
  if (!mallctl_u64(small_name.c_str(), &small))
    return std::nullopt;
  if (!mallctl_u64(large_name.c_str(), &large))
    return std::nullopt;
  return ArenaStats{small + large};
#endif
}

ProcessMemorySnapshot read_process_snapshot() {
  ProcessMemorySnapshot snapshot;
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0)
    snapshot.rss_bytes =
        static_cast<std::uint64_t>(
            usage
                .ru_maxrss) * // NOLINT(cppcoreguidelines-pro-type-union-access)
        1024U; // NOLINT(cppcoreguidelines-pro-type-union-access) Linux: KiB
#if PI_MEMSTATS_HAVE_MALLCTL
  ensure_initialized();
  auto &rt = runtime();
  if (rt.available.load(std::memory_order_relaxed)) {
    std::uint64_t allocated = 0;
    if (mallctl_u64("stats.allocated", &allocated))
      snapshot.allocator_allocated_bytes = allocated;
    std::uint64_t resident = 0;
    if (mallctl_u64("stats.resident", &resident))
      snapshot.allocator_resident_bytes = resident;
  }
#endif
  return snapshot;
}

} // namespace pi::core
