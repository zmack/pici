#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace pi::core {

// Privacy-safe, opt-in timing trace for diagnosing streaming regressions.
// The trace records event names, byte counts, and elapsed times, never prompt
// or response content.
class StreamDiagnostics {
public:
  explicit StreamDiagnostics(std::string path);
  ~StreamDiagnostics();

  StreamDiagnostics(const StreamDiagnostics &) = delete;
  StreamDiagnostics &operator=(const StreamDiagnostics &) = delete;

  void record_transport_line(std::string_view line);
  void record_transport_chunk(std::size_t bytes);
  void record_parser_event(std::string_view event, std::size_t bytes = 0);
  void record_renderer_event(std::string_view event, std::size_t bytes = 0);

  // Records a unary compaction-request lifecycle event (request sent,
  // response received, parsed, failed, ...) under a "compact" stage so it is
  // distinguishable in the trace from ordinary streaming "transport" events.
  // Like every other record_* method, only an event label and a byte count
  // are ever written — never request/response body content, prompts, or
  // secrets.
  void record_compact_event(std::string_view event, std::size_t bytes = 0);

private:
  void record(std::string_view stage, std::string_view event,
              std::size_t bytes);

  std::mutex mutex_;
  std::string path_;
  std::string current_sse_event_;
  std::ofstream file_;
  std::int64_t started_ms_{0};
};

} // namespace pi::core
