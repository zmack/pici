#include "core/stream_diagnostics.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)

namespace pi::core {
namespace {

std::int64_t monotonic_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

std::string trim_cr(std::string_view line) {
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  return std::string(line);
}

std::string after_field(std::string_view line, std::size_t prefix_size) {
  line.remove_prefix(prefix_size);
  if (!line.empty() && line.front() == ' ')
    line.remove_prefix(1);
  return std::string(line);
}

} // namespace

StreamDiagnostics::StreamDiagnostics(std::string path)
    : path_(std::move(path)), file_(path_, std::ios::out | std::ios::trunc),
      started_ms_(monotonic_ms()) {
  if (!file_) {
    throw std::runtime_error("Unable to open stream trace: " + path_);
  }
}

StreamDiagnostics::~StreamDiagnostics() {
  std::scoped_lock lock(mutex_);
  file_.flush();
}

void StreamDiagnostics::record_transport_line(std::string_view raw_line) {
  const auto line = trim_cr(raw_line);
  if (line.starts_with("event:")) {
    current_sse_event_ = after_field(line, 6);
    record("transport", "sse_event:" + current_sse_event_, line.size());
    return;
  }

  if (line.starts_with("data:")) {
    const auto data = after_field(line, 5);
    std::string event = current_sse_event_;
    if (data == "[DONE]") {
      event = "done";
    } else {
      auto parsed = nlohmann::json::parse( // NOLINT(misc-include-cleaner)
          data, nullptr, false);
      if (!parsed.is_discarded() && parsed.is_object()) {
        if (const auto type_it = parsed.find("type");
            type_it != parsed.end() && type_it->is_string()) {
          event = type_it->get<std::string>();
        }
      }
    }
    if (event.empty())
      event = "data";
    record("transport", "sse_data:" + event, line.size());
    return;
  }

  record("transport", "line", line.size());
}

void StreamDiagnostics::record_transport_chunk(std::size_t bytes) {
  record("transport", "curl_write", bytes);
}

void StreamDiagnostics::record_parser_event(std::string_view event,
                                            std::size_t bytes) {
  record("parser", event, bytes);
}

void StreamDiagnostics::record_renderer_event(std::string_view event,
                                              std::size_t bytes) {
  record("renderer", event, bytes);
}

void StreamDiagnostics::record(std::string_view stage, std::string_view event,
                               std::size_t bytes) {
  std::scoped_lock lock(mutex_);
  if (!file_)
    return;

  nlohmann::json entry = {
      {"elapsed_ms", monotonic_ms() - started_ms_},
      {"stage", stage},
      {"event", event},
      {"bytes", bytes},
  };
  file_ << entry.dump() << '\n';
  file_.flush();
}

} // namespace pi::core
