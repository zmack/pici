#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace pi::core {

class StreamRenderer {
public:
  virtual ~StreamRenderer() = default;

  // Called for each incoming text delta.
  virtual void update(std::string_view delta) = 0;

  // Called when the message is complete.
  virtual void finish() = 0;

  // Reset state between messages.
  virtual void reset() = 0;
};

// Writes raw text deltas as they arrive. Suitable for non-TTY output.
std::unique_ptr<StreamRenderer> make_raw_renderer(int fd = 1);

// Re-renders accumulated markdown on each delta, diffing against the previous
// frame and only redrawing lines that changed.
std::unique_ptr<StreamRenderer> make_diff_renderer(int fd = 1);

// Picks make_diff_renderer when fd is a TTY, otherwise make_raw_renderer.
std::unique_ptr<StreamRenderer> make_auto_renderer(int fd = 1);

// ─── Registry ────────────────────────────────────────────────────────────────

// Global registry mapping renderer names to factories.
// Built-in names ("auto", "markdown", "raw") are pre-registered.
// Call register_renderer() to add custom renderers (e.g. from Lua add-ons).
class StreamRendererRegistry {
public:
  using Factory = std::function<std::unique_ptr<StreamRenderer>(int fd)>;

  void register_renderer(std::string name, Factory factory);
  std::unique_ptr<StreamRenderer> make(const std::string &name, int fd) const;
  bool has(const std::string &name) const;

  static StreamRendererRegistry &instance();

private:
  StreamRendererRegistry();
  std::map<std::string, Factory> factories_;
  mutable std::mutex mutex_;
};

} // namespace pi::core
