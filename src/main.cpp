#include "rockid/app.hpp"
#include "rockid/database.hpp"
#include "rockid/log.hpp"
#include "rockid/server.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

rockid::Server *g_server = nullptr;

void on_signal(int) {
  if (g_server) g_server->request_stop();
}

void usage() {
  std::cerr << "usage: rockidserver [--port N] [--db PATH]\n"
            << "  --port   listen port (default 7410)\n"
            << "  --db     sqlite database path (default data/rockid.db)\n";
}

}  // namespace

int main(int argc, char **argv) {
  int port = 7410;
  std::string db_path = "data/rockid.db";
  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        usage();
        return 0;
      }
      if (arg == "--port" && i + 1 < argc) {
        port = std::stoi(argv[++i]);
      } else if (arg == "--db" && i + 1 < argc) {
        db_path = argv[++i];
      } else {
        usage();
        return 2;
      }
    }
  } catch (const std::exception &) {
    usage();
    return 2;
  }
  if (port < 1 || port > 65535) {
    usage();
    return 2;
  }

  try {
    std::filesystem::path path(db_path);
    if (path.has_parent_path() && !path.parent_path().empty())
      std::filesystem::create_directories(path.parent_path());

    rockid::Database db(db_path);
    rockid::Server server(port);
    rockid::App app(db, server);
    server.set_app(&app);
    server.set_presence_handler([&app](int64_t user_id, bool online) {
      app.broadcast_presence(user_id, online);
    });

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    g_server = &server;
    rockid::log_info("database " + db_path);
    server.run();
    g_server = nullptr;
  } catch (const std::exception &ex) {
    rockid::log_error(ex.what());
    return 1;
  }
  return 0;
}
