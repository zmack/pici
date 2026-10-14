#pragma once

// Loopback-only streamable-HTTP MCP server for transport/client tests. Binds
// 127.0.0.1 on an ephemeral port and runs in-process on its own thread; no
// external network is ever touched.

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pi::test {

class FakeHttpMcpServer {
public:
  // Called before the default behavior; return true when it handled the
  // request completely.
  using Override =
      std::function<bool(const httplib::Request &, httplib::Response &)>;

  FakeHttpMcpServer() {
    server_.Post("/mcp", [this](const httplib::Request &req,
                                httplib::Response &res) { handle_post(req, res); });
    server_.Delete("/mcp", [this](const httplib::Request &req,
                                  httplib::Response &) {
      std::lock_guard<std::mutex> lock(mutex_);
      deleted_sessions_.push_back(req.get_header_value("Mcp-Session-Id"));
    });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  ~FakeHttpMcpServer() { stop(); }

  FakeHttpMcpServer(const FakeHttpMcpServer &) = delete;
  FakeHttpMcpServer &operator=(const FakeHttpMcpServer &) = delete;

  void stop() {
    server_.stop();
    if (thread_.joinable())
      thread_.join();
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/mcp";
  }

  void set_override(Override override_fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    override_ = std::move(override_fn);
  }
  void set_tools(std::vector<std::string> names) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_ = std::move(names);
  }
  // Changing the session id makes requests carrying the old one get 404.
  void set_session(std::string session) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_ = std::move(session);
  }
  void set_sse(bool on) { sse_ = on; }
  // Raw SSE text emitted before the response event (notifications etc.).
  void set_sse_prefix(std::string prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    sse_prefix_ = std::move(prefix);
  }

  int initialize_count() const { return initialize_count_.load(); }
  std::string last_authorization() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_authorization_;
  }
  std::string last_protocol_header() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_protocol_;
  }
  std::vector<nlohmann::json> bodies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bodies_;
  }
  std::vector<std::string> deleted_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deleted_sessions_;
  }

private:
  void reply(httplib::Response &res, const nlohmann::json &message) {
    std::string prefix;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      prefix = sse_prefix_;
    }
    if (sse_) {
      res.set_content(prefix + "event: message\ndata: " + message.dump() +
                          "\n\n",
                      "text/event-stream");
    } else {
      res.set_content(message.dump(), "application/json");
    }
  }

  void handle_post(const httplib::Request &req, httplib::Response &res) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    Override override_fn;
    std::string session;
    std::vector<std::string> tools;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_authorization_ = req.get_header_value("Authorization");
      last_protocol_ = req.get_header_value("MCP-Protocol-Version");
      if (!body.is_discarded())
        bodies_.push_back(body);
      override_fn = override_;
      session = session_;
      tools = tools_;
    }
    if (override_fn && override_fn(req, res))
      return;
    if (body.is_discarded() || !body.is_object()) {
      res.status = 400;
      return;
    }
    const std::string method = body.value("method", std::string{});
    const bool is_init = method == "initialize";
    if (!is_init && req.get_header_value("Mcp-Session-Id") != session) {
      res.status = 404;
      return;
    }
    if (!body.contains("id")) {
      res.status = 202;
      return;
    }
    const auto id = body.at("id");
    if (is_init) {
      ++initialize_count_;
      res.set_header("Mcp-Session-Id", session);
      reply(res, {{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result",
                   {{"protocolVersion", "2025-06-18"},
                    {"capabilities", {{"tools", nlohmann::json::object()}}},
                    {"serverInfo", {{"name", "fake"}, {"version", "1"}}}}}});
    } else if (method == "tools/list") {
      nlohmann::json list = nlohmann::json::array();
      for (const auto &name : tools)
        list.push_back({{"name", name},
                        {"description", "fake tool"},
                        {"inputSchema", {{"type", "object"}}}});
      reply(res, {{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result", {{"tools", list}}}});
    } else if (method == "tools/call") {
      const auto text =
          body.at("params").at("arguments").value("text", std::string{});
      reply(res, {{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result",
                   {{"content", {{{"type", "text"}, {"text", text}}}}}}});
    } else {
      reply(res, {{"jsonrpc", "2.0"},
                  {"id", id},
                  {"error", {{"code", -32601}, {"message", "no such method"}}}});
    }
  }

  httplib::Server server_;
  int port_{0};
  std::thread thread_;
  mutable std::mutex mutex_;
  Override override_;
  std::string session_{"sess-1"};
  std::vector<std::string> tools_{"echo"};
  std::string sse_prefix_;
  std::string last_authorization_;
  std::string last_protocol_;
  std::vector<nlohmann::json> bodies_;
  std::vector<std::string> deleted_sessions_;
  std::atomic<bool> sse_{false};
  std::atomic<int> initialize_count_{0};
};

} // namespace pi::test
