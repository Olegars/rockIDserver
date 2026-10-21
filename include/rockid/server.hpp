#pragma once

#include "rockid/hub.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rockid {

class App;
class Connection;

class Server : public Hub {
 public:
  explicit Server(int port);
  ~Server() override;

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  void set_app(App *app) { app_ = app; }
  void set_presence_handler(std::function<void(int64_t, bool)> handler) {
    presence_ = std::move(handler);
  }

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
  void accept_all();
  void update_events(Connection &connection);
  void close_conn(int fd);

  int port_ = 0;
  int listen_fd_ = -1;
  int epoll_fd_ = -1;
  std::atomic<bool> stop_{false};
  App *app_ = nullptr;
  std::function<void(int64_t, bool)> presence_;
  std::unordered_map<int, std::unique_ptr<Connection>> conns_;
  std::unordered_map<int64_t, std::vector<Connection *>> online_;
};

}
