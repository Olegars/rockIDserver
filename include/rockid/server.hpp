#pragma once

#include "rockid/hub.hpp"

#include <openssl/ssl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rockid {

class App;
class Connection;

class Server : public Hub {
 public:
  explicit Server(int port, int http_port = 0, int media_port = 0);
  ~Server() override;

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  void set_app(App *app) { app_ = app; }
  void set_presence_handler(std::function<void(int64_t, bool)> handler) {
    presence_ = std::move(handler);
  }
  void set_offline_handler(std::function<void(int64_t, const Json &)> handler) {
    offline_ = std::move(handler);
  }
  bool enable_tls(const std::string &cert, const std::string &key);
  int media_port() const { return media_fd_ >= 0 ? media_port_ : 0; }

  void run();
  void request_stop();

  void bind_user(Connection *connection, int64_t user_id) override;
  void unbind(Connection *connection) override;
  void push_user(int64_t user_id, const Json &message, Connection *except) override;
  void push_many(const std::vector<int64_t> &user_ids, const Json &message,
                 Connection *except) override;
  bool online(int64_t user_id) const override;
  void close_token(int64_t user_id, const std::string &token) override;
  void close_hash(int64_t user_id, const std::string &token_hash) override;

 private:
  struct MediaLink {
    std::string in;
    std::string out;
    size_t out_pos = 0;
    int64_t user_id = 0;
    int64_t call_id = 0;
    bool ready = false;
  };

  int open_listen(int port);
  void accept_all(int listen_fd, int kind);
  void update_events(Connection &connection);
  void close_conn(int fd);
  void on_http(int fd);
  void on_media(int fd, uint32_t ev);
  bool read_media(int fd);
  bool write_media(int fd);
  void close_media(int fd);
  void relay_audio(const std::vector<int64_t> &users, int64_t from, uint32_t seq, int64_t ts,
                   const std::string &bytes);

  int port_ = 0;
  int http_port_ = 0;
  int media_port_ = 0;
  int listen_fd_ = -1;
  int http_fd_ = -1;
  int media_fd_ = -1;
  int epoll_fd_ = -1;
  bool ipv6_ = false;
  std::atomic<bool> stop_{false};
  App *app_ = nullptr;
  SSL_CTX *tls_ = nullptr;
  std::function<void(int64_t, bool)> presence_;
  std::function<void(int64_t, const Json &)> offline_;
  std::unordered_map<int, std::unique_ptr<Connection>> conns_;
  std::unordered_map<int64_t, std::vector<Connection *>> online_;
  std::unordered_map<int, std::string> http_buf_;
  std::unordered_map<int, MediaLink> media_;
  std::unordered_map<int64_t, int> media_user_;
};

}
