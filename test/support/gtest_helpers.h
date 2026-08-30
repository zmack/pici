#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace pi::test {

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view prefix = "pici-test") {
    const auto base = std::filesystem::temp_directory_path();
    std::string safe_prefix;
    safe_prefix.reserve(prefix.size());
    for (const char character : prefix) {
      if (std::isalnum(static_cast<unsigned char>(character)) ||
          character == '-' || character == '_')
        safe_prefix.push_back(character);
      else
        safe_prefix.push_back('-');
    }
    if (safe_prefix.empty())
      safe_prefix = "pici-test";

    static std::atomic<unsigned long long> sequence{0};
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch();
      const auto unique =
          static_cast<unsigned long long>(stamp.count()) + sequence++;
      const auto candidate =
          base / (safe_prefix + "-" + std::to_string(unique) + "-" +
                  std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
      if (error && error != std::errc::file_exists)
        throw std::filesystem::filesystem_error(
            "create temporary test directory", candidate, error);
    }
    throw std::runtime_error(
        "could not create a unique temporary test directory");
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  TemporaryDirectory(TemporaryDirectory &&other) noexcept
      : path_(std::exchange(other.path_, {})) {}

  TemporaryDirectory &operator=(TemporaryDirectory &&other) noexcept {
    if (this != &other) {
      cleanup();
      path_ = std::exchange(other.path_, {});
    }
    return *this;
  }

  ~TemporaryDirectory() noexcept { cleanup(); }

  const std::filesystem::path &path() const noexcept { return path_; }

private:
  void cleanup() noexcept {
    if (path_.empty())
      return;
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    path_.clear();
  }

  std::filesystem::path path_;
};

class ScopedEnvironmentVariable {
public:
  ScopedEnvironmentVariable(std::string_view name, std::string_view value)
      : name_(name), previous_(read(name)) {
    ::setenv(name_.c_str(), std::string(value).c_str(), 1);
  }

  ScopedEnvironmentVariable(std::string_view name, std::nullopt_t)
      : name_(name), previous_(read(name)) {
    ::unsetenv(name_.c_str());
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable &) = delete;
  ScopedEnvironmentVariable &
  operator=(const ScopedEnvironmentVariable &) = delete;

  ~ScopedEnvironmentVariable() noexcept {
    if (previous_)
      ::setenv(name_.c_str(), previous_->c_str(), 1);
    else
      ::unsetenv(name_.c_str());
  }

private:
  static std::optional<std::string> read(std::string_view name) {
    const char *value = ::getenv(std::string(name).c_str());
    if (value == nullptr)
      return std::nullopt;
    return std::string(value);
  }

  std::string name_;
  std::optional<std::string> previous_;
};

template <typename Predicate>
::testing::AssertionResult wait_until(
    Predicate &&predicate, std::chrono::milliseconds timeout,
    std::string_view description = "condition",
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(1)) {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const auto deadline = start + timeout;
  const auto interval = std::max(poll_interval, std::chrono::milliseconds(1));
  unsigned int attempts = 0;

  while (true) {
    ++attempts;
    if (static_cast<bool>(std::invoke(predicate)))
      return ::testing::AssertionSuccess();

    const auto now = Clock::now();
    if (now >= deadline)
      break;

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds::zero())
      continue;
    std::this_thread::sleep_for(std::min(interval, remaining));
  }

  return ::testing::AssertionFailure()
         << "timed out waiting for " << description << " after "
         << timeout.count() << " ms (" << attempts << " attempts)";
}

} // namespace pi::test
