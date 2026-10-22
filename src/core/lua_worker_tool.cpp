#include "core/lua_worker_tool.h"
#include "core/sandbox.h"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/openat2.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pi::core {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kOutputLimit = 512UL * 1024;
constexpr std::size_t kResultLimit = 1024UL * 1024;
constexpr std::string_view kSchema =
    R"({"type":"object","properties":{"requires":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":16},"script":{"type":"string","maxLength":65536},"cwd":{"type":"string","maxLength":4096},"mode":{"type":"string","enum":["run","check"]},"timeout_ms":{"type":"integer","minimum":1,"maximum":120000}},"required":["requires","script"],"additionalProperties":false})";
constexpr std::string_view kDescription =
    "Run Lua 5.4 in a fresh, disposable OS sandbox. Declare optional libraries "
    "in requires: io, os, lfs; import with require('io'), etc. Base, string "
    "(including patterns), table, math, utf8, coroutine and json.encode/decode "
    "are built in. json.null represents JSON null; use json.array() for an "
    "empty JSON array (unmarked empty tables are objects). requires selects "
    "APIs; session sandbox mounts independently "
    "control read/write access. Full io includes popen; os includes execute. "
    "Use io.open for files, lfs.dir/chdir for directories, os.execute or "
    "io.popen for commands. Paths/cwd are relative to /workspace. Network "
    "is disabled; only sanitized environment and configured mounts exist. "
    "Each call starts fresh; globals/cwd do not persist, workspace files do. "
    "Print and child stdout/stderr are captured; return JSON-compatible "
    "values. "
    "mode=check validates/compiles without executing or loading libraries. "
    "timeout_ms defaults to 30000 (max 120000). Limits: 64 KiB source, "
    "512 KiB combined output, 1 MiB result, session-configured per-process "
    "address-space limit, 256 MiB per-file writes, 64 MiB temporary "
    "filesystem. Errors/cancellation "
    "do not roll back writes. No host pici APIs or persistent scratch. "
    "Only use libraries permitted by the session's Lua worker policy.";

class Result final : public ToolResult {
public:
  Result(bool error, Json value) : error_(error), value_(std::move(value)) {}
  bool is_error() const override { return error_; }
  std::string content() const override {
    return value_.dump(-1, ' ', false, Json::error_handler_t::replace);
  }
  std::optional<std::string> details() const override { return std::nullopt; }

private:
  bool error_;
  Json value_;
};

std::shared_ptr<ToolResult> failure(const std::string &error) {
  return std::make_shared<Result>(true, Json{{"ok", false}, {"error", error}});
}

class Fd {
public:
  explicit Fd(int value) {
    if (value < 0)
      throw std::runtime_error(
          std::string("worker descriptor: ") +
          std::error_code(errno, std::generic_category()).message());
    fd_ = fcntl(value, F_DUPFD_CLOEXEC, 128);
    const int saved = errno;
    close(value);
    if (fd_ < 0)
      throw std::runtime_error(
          std::string("worker descriptor: ") +
          std::error_code(saved, std::generic_category()).message());
  }
  ~Fd() {
    if (fd_ >= 0)
      close(fd_);
  }
  Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd &operator=(Fd &&) = delete;
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  int get() const { return fd_; }
  void reset() {
    if (fd_ >= 0)
      close(std::exchange(fd_, -1));
  }

private:
  int fd_{-1};
};

struct ResultFile {
  std::array<char, 32> path{};
  Fd descriptor;
  ResultFile() : descriptor(create()) {}
  ~ResultFile() { unlink(path.data()); }
  ResultFile(const ResultFile &) = delete;
  ResultFile &operator=(const ResultFile &) = delete;

private:
  Fd create() {
    constexpr std::string_view pattern = "/tmp/pici-lua-result-XXXXXX";
    std::copy(pattern.begin(), pattern.end(), path.begin());
    const int source = mkostemp(path.data(), O_CLOEXEC);
    try {
      return Fd(source);
    } catch (...) {
      if (source >= 0)
        unlink(path.data());
      throw;
    }
  }
};

std::array<Fd, 2> channel(bool socket) {
  std::array<int, 2> pair{};
  const int status =
      socket ? socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data())
             : pipe2(pair.data(), O_CLOEXEC);
  if (status != 0)
    throw std::runtime_error("cannot create worker channel");
  auto first = [&] {
    try {
      return Fd(pair[0]);
    } catch (...) {
      close(pair[1]);
      throw;
    }
  }();
  return {std::move(first), Fd(pair[1])};
}

void nonblocking(int fd) {
  if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
    throw std::runtime_error("cannot configure worker channel");
}

std::filesystem::path relative_root(const std::filesystem::path &root,
                                    const std::filesystem::path &workspace) {
  const auto absolute =
      std::filesystem::canonical(root.is_absolute() ? root : workspace / root);
  const auto relative = absolute.lexically_relative(workspace);
  if (relative.empty() || *relative.begin() == ".." ||
      !std::filesystem::is_directory(absolute))
    throw std::invalid_argument(
        "Lua roots must be existing directories inside the workspace");
  return relative;
}

Fd open_root(int workspace, const std::filesystem::path &relative) {
  open_how how{};
  how.flags = O_PATH | O_CLOEXEC | O_DIRECTORY;
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS;
  return Fd(static_cast<int>(
      syscall(SYS_openat2, workspace, relative.c_str(), &how, sizeof(how))));
}

struct SpawnActions {
  posix_spawn_file_actions_t actions{};
  posix_spawnattr_t attributes{};
  SpawnActions() {
    if (posix_spawn_file_actions_init(&actions) != 0)
      throw std::runtime_error("cannot initialize worker spawn actions");
    if (posix_spawnattr_init(&attributes) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      throw std::runtime_error("cannot initialize worker spawn attributes");
    }
  }
  ~SpawnActions() {
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
  }
  SpawnActions(const SpawnActions &) = delete;
  SpawnActions &operator=(const SpawnActions &) = delete;
};

void checked(int status) {
  if (status != 0)
    throw std::runtime_error(
        std::string("worker spawn: ") +
        std::error_code(status, std::generic_category()).message());
}

struct Child {
  explicit Child(pid_t value) : pid(value) {}
  Child(const Child &) = delete;
  Child &operator=(const Child &) = delete;
  pid_t pid;
  bool reaped{false};
  ~Child() {
    if (!reaped) {
      kill(-pid, SIGKILL);
      kill(pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
};

struct CapturedOutput {
  std::string stdout_text;
  std::string stderr_text;
  std::string error;
  int status{};
};

void drain_output(int fd, std::string &text, const std::string &other,
                  bool &closed, std::string &error) {
  std::array<char, 8192> buffer{};
  for (int pass = 0; pass < 16; ++pass) {
    const auto count = read(fd, buffer.data(), buffer.size());
    if (count == 0) {
      closed = true;
      break;
    }
    if (count < 0)
      break;
    if (text.size() + other.size() + static_cast<std::size_t>(count) >
        kOutputLimit) {
      error = "combined stdout/stderr exceeded 512 KiB; earlier effects remain";
      break;
    }
    text.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

void send_request(int fd, std::string_view payload, std::size_t &sent,
                  bool &closed) {
  if (closed)
    return;
  const auto remaining = payload.substr(sent);
  const auto count = send(fd, remaining.data(), remaining.size(), MSG_NOSIGNAL);
  if (count > 0)
    sent += static_cast<std::size_t>(count);
  if (sent == payload.size() ||
      (count < 0 && errno != EAGAIN && errno != EINTR)) {
    shutdown(fd, SHUT_WR);
    closed = true;
  }
}

CapturedOutput capture_output(Child &child, int input, int output, int errors,
                              std::string_view payload, int timeout_ms,
                              const std::stop_token &stop) {
  nonblocking(input);
  nonblocking(output);
  nonblocking(errors);
  CapturedOutput captured;
  std::size_t sent = 0;
  bool input_closed = false;
  bool stdout_closed = false;
  bool stderr_closed = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  auto drain = [&] {
    drain_output(output, captured.stdout_text, captured.stderr_text,
                 stdout_closed, captured.error);
    drain_output(errors, captured.stderr_text, captured.stdout_text,
                 stderr_closed, captured.error);
  };
  while (captured.error.empty()) {
    if (stop.stop_requested()) {
      captured.error = "Lua worker cancelled; earlier effects remain";
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      captured.error = "Lua worker timed out; earlier effects remain";
      break;
    }
    send_request(input, payload, sent, input_closed);
    drain();
    const auto waited = waitpid(child.pid, &captured.status, WNOHANG);
    if (waited == child.pid) {
      child.reaped = true;
      // Drain enlarged pipe buffers after namespace exit, up to the output cap.
      for (int pass = 0; pass < 5 && captured.error.empty(); ++pass)
        drain();
      break;
    }
    if (waited < 0 && errno != EINTR)
      throw std::runtime_error("cannot wait for Lua worker");
    std::array<pollfd, 3> pollers{{{stdout_closed ? -1 : output, POLLIN, 0},
                                   {stderr_closed ? -1 : errors, POLLIN, 0},
                                   {input_closed ? -1 : input, POLLOUT, 0}}};
    poll(pollers.data(), pollers.size(), 10);
  }
  return captured;
}

Json read_result_frame(int fd) {
  struct stat metadata {};
  if (fstat(fd, &metadata) != 0 || metadata.st_size <= 0 ||
      metadata.st_size > static_cast<off_t>(kResultLimit))
    return {{"ok", false},
            {"error", "worker exited without a valid bounded result"}};
  std::string content(static_cast<std::size_t>(metadata.st_size), '\0');
  const auto count = pread(fd, content.data(), content.size(), 0);
  if (count != static_cast<ssize_t>(content.size()))
    throw std::runtime_error("cannot read Lua result");
  auto frame = Json::parse(
      content,
      [](int depth, Json::parse_event_t, Json &) {
        if (depth > 40)
          throw std::runtime_error("worker result nesting exceeded");
        return true;
      },
      false);
  if (!frame.is_object() || !frame.contains("ok") || !frame["ok"].is_boolean())
    return {{"ok", false}, {"error", "invalid worker result frame"}};
  if (frame["ok"].get<bool>()) {
    if (!frame.contains("values") || !frame["values"].is_array())
      return {{"ok", false}, {"error", "invalid worker result values"}};
  } else if (!frame.contains("error") || !frame["error"].is_string())
    return {{"ok", false}, {"error", "invalid worker error result"}};
  return frame;
}

std::shared_ptr<ToolResult> finish_result(CapturedOutput captured,
                                          int result_fd,
                                          const LuaWorkerPolicy &policy,
                                          const Json &request) {
  Json frame;
  if (!captured.error.empty())
    frame = {{"ok", false}, {"error", captured.error}};
  else if (!WIFEXITED(captured.status) || WEXITSTATUS(captured.status) != 0)
    frame = {
        {"ok", false},
        {"error",
         "worker/sandbox exited unsuccessfully; earlier effects may remain"}};
  else
    frame = read_result_frame(result_fd);
  const bool error = !frame["ok"].get<bool>();
  frame["stdout"] = std::move(captured.stdout_text);
  frame["stderr"] = std::move(captured.stderr_text);
  frame["requires"] = request["requires"];
  frame["address_space_mb"] = policy.address_space_mb;
  frame["sandbox"] = "required; network off; session mounts enforced; writable "
                     "temporary storage";
  frame["read_roots"] = Json::array();
  frame["write_roots"] = Json::array();
  if (request.value("mode", "run") != "check") {
    for (const auto &root : policy.read_roots)
      frame["read_roots"].push_back(root.string());
    for (const auto &root : policy.write_roots)
      frame["write_roots"].push_back(root.string());
  }
  return std::make_shared<Result>(error, std::move(frame));
}

std::optional<std::string> validate_libraries(const Json &libraries,
                                              const LuaWorkerPolicy &policy) {
  if (!libraries.is_array() || libraries.size() > 16)
    return "requires must be an array of at most 16 libraries";
  std::set<std::string> seen;
  for (const auto &library : libraries) {
    if (!library.is_string())
      return "requires entries must be strings";
    const auto name = library.get<std::string>();
    if (!seen.insert(name).second)
      return "duplicate library: " + name;
    if (std::ranges::find(policy.libraries, name) == policy.libraries.end())
      return "library not allowed by session policy: " + name;
  }
  return std::nullopt;
}

std::optional<std::string> validate_cwd(const Json &request) {
  if (request.contains("cwd") &&
      (!request["cwd"].is_string() ||
       request["cwd"].get_ref<const std::string &>().size() > 4096 ||
       request["cwd"].get_ref<const std::string &>().find('\0') !=
           std::string::npos))
    return "cwd must be a path string without NUL bytes";
  const auto cwd = std::filesystem::path(request.value("cwd", std::string(".")))
                       .lexically_normal();
  if (cwd.is_absolute() || (!cwd.empty() && *cwd.begin() == ".."))
    return "cwd must be relative to /workspace and cannot escape it";
  return std::nullopt;
}

std::shared_ptr<ToolResult> launch(const LuaWorkerPolicy &policy,
                                   int worker_image, const Json &request,
                                   const std::stop_token &stop) {
  auto input = channel(true);
  auto output = channel(false);
  auto errors = channel(false);
  ResultFile result_file;
  const auto &result = result_file.descriptor;
  Fd workspace(open(policy.workspace.c_str(),
                    O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  std::vector<Fd> mounts;
  SandboxProgram program;
  program.address_space_mb = policy.address_space_mb;
  auto mount = [&](Fd source, std::string destination, bool writable = false,
                   bool copy_data = false) {
    const auto index = mounts.size();
    mounts.push_back(std::move(source));
    program.mounts.push_back({static_cast<int>(index + 3),
                              std::move(destination), writable, copy_data});
  };
  // Runtime infrastructure is visible read-only, never host /etc or HOME.
  for (const char *path : {"/usr", "/bin", "/sbin", "/lib", "/lib64"}) {
    if (std::filesystem::exists(path))
      mount(Fd(open(path, O_PATH | O_CLOEXEC)), path);
  }
  // A fresh open-file description gives concurrent copies independent offsets.
  const auto image_path = "/proc/self/fd/" + std::to_string(worker_image);
  mount(Fd(open(image_path.c_str(), O_RDONLY | O_CLOEXEC)), "/pici/worker",
        false, true);
  // Duplicate so every source descriptor follows the same RAII/mapping path.
  mount(Fd(dup(result.get())), "/pici/result", true);
  const bool check = request.value("mode", "run") == "check";
  if (!check) {
    for (const auto &root : policy.read_roots)
      mount(open_root(workspace.get(), root),
            (std::filesystem::path("/workspace") / root)
                .lexically_normal()
                .string());
    for (const auto &root : policy.write_roots)
      mount(open_root(workspace.get(), root),
            (std::filesystem::path("/workspace") / root)
                .lexically_normal()
                .string(),
            true);
    const auto cwd =
        std::filesystem::path(request.value("cwd", std::string(".")))
            .lexically_normal();
    if (cwd.is_absolute() || (!cwd.empty() && *cwd.begin() == ".."))
      throw std::invalid_argument(
          "cwd must be relative to /workspace and cannot escape it");
    program.cwd =
        (std::filesystem::path("/workspace") / cwd).lexically_normal().string();
  }
  program.argv = {"/pici/worker"};
  auto argv = SandboxLauncher::program_argv(program);
  std::vector<char *> pointers;
  pointers.reserve(argv.size() + 1);
  for (auto &arg : argv)
    pointers.push_back(arg.data());
  pointers.push_back(nullptr);

  SpawnActions spawn;
  checked(posix_spawn_file_actions_adddup2(&spawn.actions, input[1].get(),
                                           STDIN_FILENO));
  checked(posix_spawn_file_actions_adddup2(&spawn.actions, output[1].get(),
                                           STDOUT_FILENO));
  checked(posix_spawn_file_actions_adddup2(&spawn.actions, errors[1].get(),
                                           STDERR_FILENO));
  for (std::size_t i = 0; i < mounts.size(); ++i)
    checked(posix_spawn_file_actions_adddup2(&spawn.actions, mounts[i].get(),
                                             static_cast<int>(i + 3)));
  checked(posix_spawn_file_actions_addclosefrom_np(
      &spawn.actions, static_cast<int>(mounts.size() + 3)));
  checked(posix_spawnattr_setflags(
      &spawn.attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF));
  checked(posix_spawnattr_setpgroup(&spawn.attributes, 0));
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  checked(posix_spawnattr_setsigdefault(&spawn.attributes, &defaults));
  std::array<char *, 1> environment{nullptr};
  pid_t pid = -1;
  checked(posix_spawn(&pid, argv.front().c_str(), &spawn.actions,
                      &spawn.attributes, pointers.data(), environment.data()));
  Child child{pid};
  // The parent must not retain child write ends or a second input reader.
  input[1].reset();
  output[1].reset();
  errors[1].reset();
  auto worker_request = request;
  worker_request["address_space_mb"] = policy.address_space_mb;
  const auto payload = worker_request.dump();
  auto captured =
      capture_output(child, input[0].get(), output[0].get(), errors[0].get(),
                     payload, request.value("timeout_ms", 30000), stop);
  return finish_result(std::move(captured), result.get(), policy, request);
}

class Schema final : public JsonSchemaToolSchema {
public:
  std::string serialize() const override { return std::string(kSchema); }
  std::map<std::string, std::string> to_definition() const override {
    std::map<std::string, std::string> result;
    const auto parsed = Json::parse(kSchema);
    for (const auto &[key, value] : parsed.items())
      result[key] = value.is_string() ? value.get<std::string>() : value.dump();
    return result;
  }
};

class LuaWorkerTool final : public ToolDefinition {
public:
  LuaWorkerTool(LuaWorkerPolicy policy,
                std::shared_ptr<const LuaWorkerAdapter> adapter)
      : policy_(std::move(policy)), adapter_(std::move(adapter)) {}
  std::string_view name() const override { return "lua"; }
  std::string_view description() const override { return kDescription; }
  ToolSchema &schema() const override { return schema_; }
  ToolExecutionMode execution_mode() const override {
    return ToolExecutionMode::sequential;
  }
  std::shared_ptr<ToolResult> execute(std::string_view,
                                      std::string_view arguments,
                                      std::stop_token stop,
                                      ToolUpdateCallback) const override {
    return adapter_->execute(policy_, arguments, stop);
  }

private:
  LuaWorkerPolicy policy_;
  std::shared_ptr<const LuaWorkerAdapter> adapter_;
  mutable Schema schema_;
};

void write_image_bytes(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = write(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("cannot copy Lua worker image");
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
}

void copy_worker_image(int source, int destination) {
  std::array<char, 8192> buffer{};
  std::size_t total = 0;
  for (;;) {
    const auto count = read(source, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      throw std::runtime_error("cannot read Lua worker image");
    if (count == 0)
      break;
    total += static_cast<std::size_t>(count);
    if (total > 32UL * 1024 * 1024)
      throw std::runtime_error("Lua worker image exceeds 32 MiB");
    write_image_bytes(
        destination,
        std::string_view(buffer.data(), static_cast<std::size_t>(count)));
  }
  if (total == 0)
    throw std::runtime_error("Lua worker image is empty");
}
} // namespace

// Process-lifetime sealed executable bytes, owned only by the reusable Adapter.
struct LuaWorkerAdapter::Image {
  Fd descriptor;
  explicit Image(Fd value) : descriptor(std::move(value)) {}
};

std::shared_ptr<const LuaWorkerAdapter::Image> LuaWorkerAdapter::image() const {
  std::scoped_lock lock(image_mutex_);
  if (image_)
    return image_;
  if (access(executable_.c_str(), X_OK) != 0)
    throw std::invalid_argument(
        "pi-lua-worker must be installed beside pi-cli");
  Fd source(open(executable_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  struct stat metadata {};
  if (fstat(source.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode))
    throw std::invalid_argument("Lua worker must be a regular executable file");
  Fd snapshot(static_cast<int>(syscall(SYS_memfd_create, "pici-lua-image",
                                       MFD_CLOEXEC | MFD_ALLOW_SEALING)));
  copy_worker_image(source.get(), snapshot.get());
  if (fcntl(snapshot.get(), F_ADD_SEALS,
            F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK) != 0)
    throw std::runtime_error("cannot seal Lua worker image");
  image_ = std::make_shared<Image>(std::move(snapshot));
  return image_;
}

void LuaWorkerAdapter::validate() const { (void)image(); }

LuaWorkerAdapter::LuaWorkerAdapter(std::filesystem::path executable)
    : executable_(std::move(executable)) {
  if (executable_.empty()) {
    std::array<char, 4096> buffer{};
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (size <= 0 || static_cast<std::size_t>(size) == buffer.size())
      throw std::runtime_error(
          "cannot locate pi-lua-worker beside this executable");
    executable_ =
        std::filesystem::path(
            std::string(buffer.data(), static_cast<std::size_t>(size)))
            .parent_path() /
        "pi-lua-worker";
  }
}

std::shared_ptr<ToolResult>
LuaWorkerAdapter::execute(const LuaWorkerPolicy &policy,
                          std::string_view arguments,
                          const std::stop_token &stop) const {
  try {
    if (stop.stop_requested())
      return failure("Lua worker cancelled before launch");
    if (arguments.size() > 128UL * 1024)
      return failure("Lua request exceeds 128 KiB");
    const auto request =
        Json::parse(arguments, [](int depth, Json::parse_event_t, Json &) {
          if (depth > 8)
            throw std::invalid_argument("Lua request nesting exceeded");
          return true;
        });
    if (!request.is_object())
      return failure("Lua arguments must be an object");
    for (const auto &[key, value] : request.items()) {
      (void)value;
      if (key != "requires" && key != "script" && key != "mode" &&
          key != "cwd" && key != "timeout_ms")
        return failure("unknown Lua argument: " + key);
    }
    if (!request.contains("script") || !request["script"].is_string() ||
        request["script"].get_ref<const std::string &>().size() > 65536 ||
        !request.contains("requires") || !request["requires"].is_array() ||
        request["requires"].size() > 16)
      return failure("script (max 64 KiB) and requires array are required");
    if (const auto error = validate_libraries(request["requires"], policy))
      return failure(*error);
    if (request.contains("mode") &&
        (!request["mode"].is_string() ||
         (request["mode"] != "run" && request["mode"] != "check")))
      return failure("mode must be run or check");
    if (request.contains("timeout_ms") &&
        (!request["timeout_ms"].is_number_integer() ||
         request["timeout_ms"] < 1 || request["timeout_ms"] > 120000))
      return failure("timeout_ms must be an integer from 1 to 120000");
    if (const auto error = validate_cwd(request))
      return failure(*error);
    const auto executable = image();
    return launch(policy, executable->descriptor.get(), request, stop);
  } catch (const std::exception &error) {
    return failure(error.what());
  }
}

std::shared_ptr<const ToolDefinition>
make_lua_worker_tool(LuaWorkerPolicy policy,
                     std::shared_ptr<const LuaWorkerAdapter> adapter) {
  if (!adapter)
    throw std::invalid_argument("Lua worker adapter is missing");
  adapter->validate();
  (void)SandboxLauncher::program_argv({.argv = {"/pici/worker"}});
  policy.workspace = std::filesystem::canonical(policy.workspace);
  if (!std::filesystem::is_directory(policy.workspace))
    throw std::invalid_argument("Lua workspace must be a directory");
  if (policy.read_roots.size() + policy.write_roots.size() > 32)
    throw std::invalid_argument("at most 32 Lua workspace roots are supported");
  if (policy.address_space_mb > 65536 ||
      (policy.address_space_mb != 0 && policy.address_space_mb < 64))
    throw std::invalid_argument(
        "Lua address_space_mb must be 0 or between 64 and 65536");
  std::set<std::string> seen;
  for (const auto &library : policy.libraries) {
    if ((library != "io" && library != "os" && library != "lfs") ||
        !seen.insert(library).second)
      throw std::invalid_argument("unsupported or duplicate Lua library: " +
                                  library);
  }
  for (auto *roots : {&policy.read_roots, &policy.write_roots}) {
    for (auto &root : *roots)
      root = relative_root(root, policy.workspace);
    std::ranges::sort(*roots);
    roots->erase(std::unique(roots->begin(), roots->end()), roots->end());
  }
  return std::make_shared<LuaWorkerTool>(std::move(policy), std::move(adapter));
}
} // namespace pi::core
