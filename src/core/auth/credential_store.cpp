#include "core/auth/credential_store.h"
#include "nlohmann/json_fwd.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/file.h>
#include <sys/stat.h>

namespace pi::auth {

namespace {

using json = nlohmann::json;

constexpr int kSchemaVersion = 1;

void reject_symlink(const std::filesystem::path &path);

// strerror_r() has two incompatible signatures depending on feature-test
// macros in scope when <cstring> was included: the POSIX form returns int
// and always fills buffer, while the GNU form returns a char* that may
// point elsewhere (e.g. a static string) instead of buffer. Overloading on
// the actual return type selects the right behavior at compile time without
// depending on ambient _GNU_SOURCE state, and unlike strerror(), both forms
// are thread-safe.
std::string strerror_r_result(char *message, const std::array<char, 256> &) {
  return {message};
}
std::string strerror_r_result(int rc, const std::array<char, 256> &buffer) {
  return rc == 0 ? std::string(buffer.data()) : "errno " + std::to_string(rc);
}

std::string errno_message(int err) {
  std::array<char, 256> buffer{};
  // strerror_r is a POSIX extension declared via <cstring> on this
  // platform; the tool has no better header to suggest for it. `auto` (not
  // `auto*`) is deliberate: strerror_r_result()'s overload set is exactly
  // how this code stays portable between strerror_r's two incompatible
  // return types (int on POSIX, char* on GNU) — narrowing this to a
  // pointer type would fail to compile wherever the POSIX form is active.
  // NOLINTNEXTLINE(misc-include-cleaner, readability-qualified-auto)
  const auto result = ::strerror_r(err, buffer.data(), buffer.size());
  return strerror_r_result(result, buffer);
}

class FileLock {
public:
  FileLock(const std::filesystem::path &path, const std::stop_token &stop_tok) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                   : path.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec)
      throw std::runtime_error("failed to create auth lock directory: " +
                               ec.message());

    reject_symlink(path);
#ifdef O_NOFOLLOW
    constexpr int nofollow = O_NOFOLLOW;
#else
    constexpr int nofollow = 0;
#endif
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | nofollow, 0600);
    if (fd_ < 0)
      throw std::runtime_error("failed to open auth lock: " +
                               errno_message(errno));
    if (::fchmod(fd_, S_IRUSR | S_IWUSR) != 0) {
      close();
      throw std::runtime_error("failed to secure auth lock: " +
                               errno_message(errno));
    }

    while (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        close();
        throw std::runtime_error("failed to lock auth file: " +
                                 errno_message(errno));
      }
      if (stop_tok.stop_requested()) {
        close();
        throw std::runtime_error("authentication operation was cancelled");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  ~FileLock() { close(); }

  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;
  FileLock(FileLock &&) = delete;
  FileLock &operator=(FileLock &&) = delete;

private:
  void close() {
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd_{-1};
};

void ensure_secure_parent(const std::filesystem::path &path) {
  const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                 : path.parent_path();
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec)
    throw std::runtime_error("failed to create auth directory: " +
                             ec.message());

  const auto status = std::filesystem::status(parent, ec);
  if (ec || !std::filesystem::is_directory(status))
    throw std::runtime_error("auth path parent is not a directory");
  const auto perms = status.permissions();
  struct stat parent_stat {};
  const bool sticky = ::stat(parent.c_str(), &parent_stat) == 0 &&
                      (parent_stat.st_mode & S_ISVTX) != 0;
  if (!sticky && ((perms & (std::filesystem::perms::group_write |
                            std::filesystem::perms::group_read |
                            std::filesystem::perms::group_exec |
                            std::filesystem::perms::others_write |
                            std::filesystem::perms::others_read |
                            std::filesystem::perms::others_exec)) !=
                  std::filesystem::perms::none)) {
    // A newly-created temporary/config parent can inherit a permissive umask.
    // Tighten it before any credential data is written; leaving these bits in
    // place would make the security check fail nondeterministically by host.
    std::filesystem::permissions(parent,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    if (ec)
      throw std::runtime_error("failed to secure auth directory: " +
                               ec.message());
  }
}

void reject_symlink(const std::filesystem::path &path) {
  struct stat st {};
  const int result = ::lstat(path.c_str(), &st);
  if (result == 0 && S_ISLNK(st.st_mode))
    throw std::runtime_error("auth path must not be a symbolic link");
  if (result != 0 && errno != ENOENT)
    throw std::runtime_error("failed to inspect auth file: " +
                             errno_message(errno));
}

void ensure_secure_file(const std::filesystem::path &path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) {
    if (errno == ENOENT)
      return;
    throw std::runtime_error("failed to inspect auth file: " +
                             errno_message(errno));
  }
  if (S_ISLNK(st.st_mode))
    throw std::runtime_error("auth path must not be a symbolic link");
  if (!S_ISREG(st.st_mode))
    throw std::runtime_error("auth path is not a regular file");
  constexpr mode_t unsafe = S_IRWXG | S_IRWXO;
  if ((st.st_mode & unsafe) != 0)
    throw std::runtime_error("auth file is group/world accessible");
}

json empty_document() {
  return json{{"version", kSchemaVersion}, {"providers", json::object()}};
}

json load_document(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input)
    return empty_document();

  json document;
  try {
    input >> document;
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("invalid auth file JSON: ") +
                             e.what());
  }
  if (!document.is_object() || document.value("version", 0) != kSchemaVersion ||
      !document.contains("providers") || !document["providers"].is_object()) {
    throw std::runtime_error("unsupported auth file schema");
  }
  return document;
}

OAuthCredential parse_oauth(const json &value, std::string_view provider) {
  if (!value.is_object() || value.value("type", "") != "oauth" ||
      !value["access_token"].is_string() ||
      !value["refresh_token"].is_string() ||
      !value["expires_at_ms"].is_number_integer() ||
      !value["account_id"].is_string()) {
    throw std::runtime_error("invalid OAuth credential for provider '" +
                             std::string(provider) + "'");
  }

  OAuthCredential credential{
      .access_token = value["access_token"].get<std::string>(),
      .refresh_token = value["refresh_token"].get<std::string>(),
      .expires_at_ms = value["expires_at_ms"].get<std::int64_t>(),
      .account_id = value["account_id"].get<std::string>(),
  };
  if (credential.access_token.empty() || credential.refresh_token.empty() ||
      credential.expires_at_ms <= 0 || credential.account_id.empty()) {
    throw std::runtime_error("incomplete OAuth credential for provider '" +
                             std::string(provider) + "'");
  }
  return credential;
}

json serialize_oauth(const OAuthCredential &credential) {
  return json{{"type", "oauth"},
              {"access_token", credential.access_token},
              {"refresh_token", credential.refresh_token},
              {"expires_at_ms", credential.expires_at_ms},
              {"account_id", credential.account_id}};
}

void write_all(int fd, std::string_view contents) {
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto written =
        ::write(fd, contents.data() + offset, contents.size() - offset);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      throw std::runtime_error("failed to write auth file: " +
                               errno_message(errno));
    }
    offset += static_cast<std::size_t>(written);
  }
}

void save_document(const std::filesystem::path &path, const json &document) {
  ensure_secure_parent(path);
  reject_symlink(path);

  auto temp = path;
  temp += ".tmp.XXXXXX";
  std::string temp_string = temp.string();
  std::vector<char> temp_buffer(temp_string.begin(), temp_string.end());
  temp_buffer.push_back('\0');
  // mkstemp is a POSIX extension declared via <cstdlib> on this platform;
  // the tool has no better header to suggest for it.
  // NOLINTNEXTLINE(misc-include-cleaner)
  const int fd = ::mkstemp(temp_buffer.data());
  if (fd < 0)
    throw std::runtime_error("failed to create temporary auth file: " +
                             errno_message(errno));
  const std::filesystem::path temp_path(temp_buffer.data());
  bool closed = false;

  try {
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0)
      throw std::runtime_error("failed to secure temporary auth file: " +
                               errno_message(errno));
    const std::string contents = document.dump(2) + "\n";
    write_all(fd, contents);
    if (::fsync(fd) != 0)
      throw std::runtime_error("failed to flush temporary auth file: " +
                               errno_message(errno));
    if (::close(fd) != 0)
      throw std::runtime_error("failed to close temporary auth file: " +
                               errno_message(errno));
    closed = true;
    if (::rename(temp_path.c_str(), path.c_str()) != 0)
      throw std::runtime_error("failed to replace auth file: " +
                               errno_message(errno));
  } catch (...) {
    if (!closed)
      ::close(fd);
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    throw;
  }

  const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                 : path.parent_path();
  const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
  if (dir_fd >= 0) {
    ::fsync(dir_fd);
    ::close(dir_fd);
  }
}

} // namespace

std::filesystem::path default_auth_file_path() {
  // getenv() is only called here during startup path resolution, before any
  // worker thread could concurrently call setenv()/putenv(), so the
  // reentrancy hazard the check warns about doesn't apply in practice.
  if (const char *override_path =
          std::getenv("PICI_AUTH_FILE"); // NOLINT(concurrency-mt-unsafe)
      override_path != nullptr && *override_path != '\0') {
    return override_path;
  }
  if (const char *xdg =
          std::getenv("XDG_CONFIG_HOME"); // NOLINT(concurrency-mt-unsafe)
      xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "pici" / "auth.json";
  }
  if (const char *home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
      home != nullptr && *home != '\0')
    return std::filesystem::path(home) / ".config" / "pici" / "auth.json";
  throw std::runtime_error(
      "cannot determine auth file path; set PICI_AUTH_FILE");
}

CredentialStore::CredentialStore() : path_(default_auth_file_path()) {}

CredentialStore::CredentialStore(std::filesystem::path path)
    : path_(std::move(path)) {
  if (path_.empty())
    throw std::invalid_argument("auth file path must not be empty");
}

std::optional<OAuthCredential>
CredentialStore::read_oauth(std::string_view provider) const {
  ensure_secure_parent(path_);
  reject_symlink(path_);
  ensure_secure_file(path_);
  FileLock lock(path_.string() + ".lock", {});
  const auto document = load_document(path_);
  const auto &providers = document["providers"];
  auto it = providers.find(std::string(provider));
  if (it == providers.end())
    return std::nullopt;
  return parse_oauth(*it, provider);
}

std::vector<CredentialInfo> CredentialStore::list() const {
  ensure_secure_parent(path_);
  reject_symlink(path_);
  ensure_secure_file(path_);
  FileLock lock(path_.string() + ".lock", {});
  const auto document = load_document(path_);
  std::vector<CredentialInfo> result;
  for (auto it = document["providers"].begin();
       it != document["providers"].end(); ++it) {
    const auto &value = it.value();
    if (!value.is_object() || !value["type"].is_string())
      throw std::runtime_error("invalid credential entry for provider '" +
                               it.key() + "'");
    CredentialInfo info{.provider = it.key(),
                        .type = value["type"].get<std::string>()};
    if (value.contains("expires_at_ms") &&
        value["expires_at_ms"].is_number_integer())
      info.expires_at_ms = value["expires_at_ms"].get<std::int64_t>();
    result.push_back(std::move(info));
  }
  return result;
}

std::optional<OAuthCredential> CredentialStore::modify_oauth(
    std::string_view provider,
    const std::function<std::optional<OAuthCredential>(
        const std::optional<OAuthCredential> &)> &fn,
    const std::stop_token &stop_tok) {
  if (provider.empty() || !fn)
    throw std::invalid_argument(
        "provider and credential callback are required");
  ensure_secure_parent(path_);
  reject_symlink(path_);
  ensure_secure_file(path_);
  FileLock lock(path_.string() + ".lock", stop_tok);
  auto document = load_document(path_);
  auto &providers = document["providers"];
  std::optional<OAuthCredential> current;
  auto it = providers.find(std::string(provider));
  if (it != providers.end())
    current = parse_oauth(*it, provider);

  auto next = fn(current);
  if (next)
    providers[std::string(provider)] = serialize_oauth(*next);
  else
    providers.erase(std::string(provider));
  save_document(path_, document);
  return next;
}

void CredentialStore::erase(std::string_view provider,
                            const std::stop_token &stop_tok) {
  if (provider.empty())
    throw std::invalid_argument("provider must not be empty");
  ensure_secure_parent(path_);
  reject_symlink(path_);
  ensure_secure_file(path_);
  FileLock lock(path_.string() + ".lock", stop_tok);
  auto document = load_document(path_);
  document["providers"].erase(std::string(provider));
  save_document(path_, document);
}

} // namespace pi::auth
