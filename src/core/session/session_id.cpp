#include "core/session/session_id.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace pi::core {

std::string generate_session_id() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_utc{};
  gmtime_r(&t, &tm_utc);

  std::ostringstream oss;
  oss << std::put_time(&tm_utc, "%Y%m%dT%H%M%S");

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<unsigned> dis(0, 0xFFFFFF);
  oss << '-' << std::hex << std::setfill('0') << std::setw(6) << dis(gen);

  return oss.str();
}

} // namespace pi::core
