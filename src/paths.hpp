#pragma once

#include <string>

namespace rockid {

inline std::string &files_dir() {
  static std::string dir = "/var/www/rockIDserver/data/files";
  return dir;
}

inline std::string file_path(int64_t id) { return files_dir() + "/" + std::to_string(id); }

}
