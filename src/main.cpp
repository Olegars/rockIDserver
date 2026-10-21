#include "rockid/app.hpp"
#include "rockid/database.hpp"
#include "rockid/log.hpp"
#include "rockid/server.hpp"
#include "paths.hpp"

#include <csignal>
#include <iostream>
#include <string>

namespace {

rockid::Server *g_server = nullptr;

void on_signal(int) {
  if (g_server) g_server->request_stop();
}

void usage() {
  std::cerr << "usage: rockidserver [--port N] [--http N] [--media N] [--db CONNINFO] [--files DIR]\n"
            << "                  [--tls-cert PEM --tls-key PEM]\n"
            << "  --port      messenger port (default 7410)\n"
            << "  --http      mini app and bot API port (default 7411, 0 disables)\n"
            << "  --media     raw audio port (default port+2, 0 disables)\n"
            << "  --db        PostgreSQL conninfo (default host=/var/run/postgresql dbname=rockid)\n"
            << "  --files     directory for uploaded files\n"
            << "  --tls-cert  PEM certificate for the messenger port\n"
            << "  --tls-key   PEM private key\n";
}

}  // namespace

int main(int argc, char **argv) {
  int port = 7410;
  int http_port = 7411;
  int media_port = -1;
  std::string conninfo = "host=/var/run/postgresql dbname=rockid";
  std::string files;
  std::string cert;
  std::string key;
  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        usage();
        return 0;
      }
      if (arg == "--port" && i + 1 < argc) {
        port = std::stoi(argv[++i]);
      } else if (arg == "--http" && i + 1 < argc) {
        http_port = std::stoi(argv[++i]);
      } else if (arg == "--media" && i + 1 < argc) {
        media_port = std::stoi(argv[++i]);
      } else if (arg == "--db" && i + 1 < argc) {
        conninfo = argv[++i];
      } else if (arg == "--files" && i + 1 < argc) {
        files = argv[++i];
      } else if (arg == "--tls-cert" && i + 1 < argc) {
        cert = argv[++i];
      } else if (arg == "--tls-key" && i + 1 < argc) {
        key = argv[++i];
      } else {
        usage();
        return 2;
      }
    }
  } catch (const std::exception &) {
    usage();
    return 2;
  }
  if (media_port < 0) media_port = port + 2;
  if (port < 1 || port > 65535 || http_port < 0 || http_port > 65535 || media_port < 0 ||
      media_port > 65535 || http_port == port || (media_port != 0 && (media_port == port || media_port == http_port))) {
    usage();
    return 2;
  }

  try {
    rockid::Database db(conninfo);
    if (!db.lock_instance()) throw std::runtime_error("another rockidserver holds this database");
    if (!files.empty()) rockid::files_dir() = files;
    rockid::Server server(port, http_port, media_port);
    if ((!cert.empty() || !key.empty()) && !server.enable_tls(cert, key))
      throw std::runtime_error("tls setup failed");
    rockid::App app(db, server);
    app.set_media_port(server.media_port());
    server.set_app(&app);
    server.set_offline_handler([&app](int64_t user_id, const rockid::Json &message) {
      app.remember_offline(user_id, message);
    });
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
