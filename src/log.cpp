#include "rockid/log.hpp"

#include <ctime>
#include <iostream>

namespace rockid {
namespace {

void write_log(const char *level, const std::string &message) {
  std::time_t now = std::time(nullptr);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%F %T", std::localtime(&now));
  std::cerr << stamp << ' ' << level << ' ' << message << '\n';
}

}  // namespace

void log_info(const std::string &message) { write_log("info", message); }

void log_error(const std::string &message) { write_log("error", message); }

}
