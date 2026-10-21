#include "rockid/app.hpp"
#include "rockid/database.hpp"
#include "rockid/log.hpp"
#include "rockid/server.hpp"

#include <csignal>
#include <iostream>
#include <string>

namespace {

rockid::Server *g_server = nullptr;

void on_signal(int) {
  if (g_server) g_server->request_stop();
}

void usage() {
  std::cerr << "usage: rockidserver [--port N] [--db CONNINFO]\n"
            << "  --port   listen port (default 7410)\n"
            << "  --db     PostgreSQL conninfo (default host=/var/run/postgresql dbname=rockid)\n";
}

}  // namespace

int main(int argc, char **argv) {
  int port = 7410;
  std::string conninfo = "host=/var/run/postgresql dbname=rockid";
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
        conninfo = argv[++i];
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
    rockid::Database db(conninfo);
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
    rockid::log_info("postgres " + conninfo);
    server.run();
    g_server = nullptr;
  } catch (const std::exception &ex) {
    rockid::log_error(ex.what());
    return 1;
  }
  return 0;
}
