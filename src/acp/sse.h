#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string_view>

namespace pi::acp {

// RAII SSE response writer.
// Sets the required headers and provides emit() for writing named events.
class SseWriter {
public:
  explicit SseWriter(httplib::DataSink &sink) : sink_(sink) {}

  // Emit one SSE event.  data must be a single-line JSON object.
  void emit(std::string_view event_name, const nlohmann::json &data) {
    std::string frame;
    frame += "event: ";
    frame += event_name;
    frame += "\ndata: ";
    frame += data.dump();
    frame += "\n\n";
    sink_.write(frame.data(), frame.size());
  }

  // Send a keep-alive comment (useful for long-running runs)
  void keep_alive() {
    static const char kPing[] = ": ping\n\n";
    sink_.write(kPing, sizeof(kPing) - 1);
  }

private:
  httplib::DataSink &sink_;
};

} // namespace pi::acp
