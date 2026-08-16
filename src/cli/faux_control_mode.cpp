#include "cli/faux_control_mode.h"

#include "core/event_json.h"
#include "core/event_types.h"
#include "core/providers/faux_control.h"
#include "core/session/agent_session.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace pi::cli {

namespace {

bool write_all(int fd, std::string_view data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const auto count =
        ::send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

class SocketGuard {
public:
  SocketGuard(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~SocketGuard() {
    if (fd_ >= 0)
      ::close(fd_);
    if (!path_.empty())
      ::unlink(path_.c_str());
  }

  SocketGuard(const SocketGuard &) = delete;
  SocketGuard &operator=(const SocketGuard &) = delete;

private:
  int fd_;
  std::string path_;
};

bool valid_socket_path(const std::string &path) {
  return !path.empty() && path.size() < sizeof(sockaddr_un::sun_path);
}

} // namespace

FauxControlMode::FauxControlMode(
    core::AgentSession &session, core::RemoteFauxClient &client,
    std::shared_ptr<core::ScriptedToolRegistry> tool_registry, Output output,
    ToolRegistrar tool_registrar, EventObserver event_observer)
    : session_(session), client_(client),
      tool_registry_(std::move(tool_registry)), output_(std::move(output)),
      tool_registrar_(std::move(tool_registrar)),
      event_observer_(std::move(event_observer)) {}

FauxControlMode::~FauxControlMode() { stop(false); }

void FauxControlMode::emit(const nlohmann::json &value) const {
  std::scoped_lock lock(output_mutex_);
  if (output_)
    output_(value);
}

void FauxControlMode::response(const nlohmann::json &command, bool success,
                               nlohmann::json data, std::string error) const {
  nlohmann::json result = {{"type", "response"},
                           {"command", command.value("type", "")},
                           {"success", success}};
  if (command.contains("id"))
    result["id"] = command["id"];
  if (success && !data.is_null())
    result["data"] = std::move(data);
  if (!success)
    result["error"] = std::move(error);
  emit(result);
}

void FauxControlMode::handle_round(const nlohmann::json &command) {
  if (!tool_registry_) {
    response(command, false, nullptr, "scripted tool registry is unavailable");
    return;
  }

  std::string error;
  auto script = core::compile_round(command, *tool_registry_, error);
  if (!script) {
    response(command, false, nullptr, std::move(error));
    return;
  }

  if (command.contains("content") && command["content"].is_array()) {
    for (const auto &block : command["content"]) {
      if (!block.is_object() || block.value("type", "") != "tool_call")
        continue;
      const auto name = block.value("name", std::string{});
      if (name.empty() || known_tool_names_.contains(name))
        continue;
      if (tool_registrar_)
        tool_registrar_(name);
      known_tool_names_.insert(name);
    }
  }
  client_.push_round(std::move(*script));
  response(command, true);
}

void FauxControlMode::handle_turn(const nlohmann::json &command) {
  bool expected = false;
  if (!run_active_.compare_exchange_strong(expected, true)) {
    response(command, false, nullptr,
             "agent is already processing; wait for turn.completed");
    return;
  }
  if (session_.agent().is_streaming()) {
    run_active_ = false;
    response(command, false, nullptr,
             "agent is already processing; wait for turn.completed");
    return;
  }
  if (run_thread_.joinable())
    run_thread_.join();

  response(command, true);
  run_thread_ = std::jthread([this] {
    try {
      const auto result =
          session_.run_prompt("", [this](const core::AgentEvent &event) {
            if (event_observer_)
              event_observer_(event);
            emit(core::event_to_json(event));
          });
      if (result.error)
        emit({{"type", "turn.failed"}, {"error", *result.error}});
      else
        emit({{"type", "turn.completed"}});
    } catch (const std::exception &exception) {
      emit({{"type", "turn.failed"}, {"error", exception.what()}});
    } catch (...) {
      emit({{"type", "turn.failed"}, {"error", "unknown turn failure"}});
    }
    run_active_ = false;
  });
}

void FauxControlMode::handle(const nlohmann::json &command) {
  if (!command.is_object() || !command.contains("type") ||
      !command["type"].is_string()) {
    emit({{"type", "response"},
          {"command", ""},
          {"success", false},
          {"error", "command must be an object with a string type"}});
    return;
  }

  try {
    const auto type = command["type"].get<std::string>();
    if (type == "round")
      handle_round(command);
    else if (type == "turn")
      handle_turn(command);
    else if (type == "quit") {
      response(command, true);
      should_quit_ = true;
      stop();
    } else {
      response(command, false, nullptr, "unknown command: " + type);
    }
  } catch (const std::exception &exception) {
    response(command, false, nullptr, exception.what());
  } catch (...) {
    response(command, false, nullptr, "unknown command failure");
  }
}

void FauxControlMode::wait_for_idle() {
  if (run_thread_.joinable())
    run_thread_.join();
}

void FauxControlMode::stop(bool close_client) {
  if (close_client)
    client_.close();
  session_.agent().interrupt(core::TurnAbortReason::shutdown);
  wait_for_idle();
  run_active_ = false;
}

int run_faux_control_socket(
    core::AgentSession &session, core::RemoteFauxClient &client,
    const std::shared_ptr<core::ScriptedToolRegistry> &tool_registry,
    const std::string &socket_path,
    const FauxControlMode::ToolRegistrar &tool_registrar,
    const FauxControlMode::EventObserver &event_observer) {
  if (!valid_socket_path(socket_path))
    return 1;

  struct stat existing {};
  if (::lstat(socket_path.c_str(), &existing) == 0) {
    if (!S_ISSOCK(existing.st_mode) || ::unlink(socket_path.c_str()) < 0)
      return 1;
  } else if (errno != ENOENT) {
    return 1;
  }
  const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0)
    return 1;
  SocketGuard listener_guard(listener, socket_path);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path.c_str(),
               sizeof(address.sun_path) - 1);
  const auto *socket_address = reinterpret_cast<const sockaddr *>(
      &address); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  if (::bind(listener, socket_address, sizeof(address)) < 0 ||
      ::listen(listener, 1) < 0)
    return 1;

  while (true) {
    int connection = ::accept(listener, nullptr, nullptr);
    if (connection < 0) {
      if (errno == EINTR)
        continue;
      return 1;
    }

    std::atomic<bool> write_failed{false};
    std::mutex connection_output_mutex;
    const auto send = [&write_failed, &connection_output_mutex,
                       connection](const nlohmann::json &value) {
      const auto line = value.dump() + '\n';
      std::scoped_lock lock(connection_output_mutex);
      if (!write_all(connection, line))
        write_failed = true;
    };
    FauxControlMode mode(session, client, tool_registry, send, tool_registrar,
                         event_observer);

    std::string tail;
    std::array<char, 4096> buffer{};
    while (!mode.quit_requested() && !write_failed) {
      const auto count = ::read(connection, buffer.data(), buffer.size());
      if (count == 0)
        break;
      if (count < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      tail.append(buffer.data(), static_cast<std::size_t>(count));
      while (true) {
        const auto newline = tail.find('\n');
        if (newline == std::string::npos)
          break;
        auto line = tail.substr(0, newline);
        tail.erase(0, newline + 1);
        if (line.empty())
          continue;
        const auto command = nlohmann::json::parse(line, nullptr, false);
        if (command.is_discarded()) {
          send({{"type", "response"},
                {"command", ""},
                {"success", false},
                {"error", "invalid JSON"}});
          continue;
        }
        mode.handle(command);
        if (mode.quit_requested() || write_failed)
          break;
      }
    }
    // An active dropped turn closes the client and stops the run.
    // Idle disconnects preserve the client and queued rounds for reconnect.
    const bool close_client = mode.quit_requested() || mode.turn_active();
    mode.stop(close_client);
    ::close(connection);
    if (mode.quit_requested())
      return 0;
  }
}

} // namespace pi::cli
