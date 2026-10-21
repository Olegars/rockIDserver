#pragma once

#include "rockid/json.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace rockid {

class App;

class Connection {
 public:
  explicit Connection(int fd);
  ~Connection();

  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

  int fd() const { return fd_; }
  int64_t user_id() const { return user_id_; }
  void set_user_id(int64_t id) { user_id_ = id; }

  const std::string &token() const { return token_; }
  void set_token(std::string token) { token_ = std::move(token); }

  bool closing() const { return closing_; }
  void request_close() { closing_ = true; }
  bool want_write() const { return out_pos_ < out_.size(); }

  void set_app(App *app) { app_ = app; }
  void set_on_output(std::function<void(Connection &)> cb) { on_output_ = std::move(cb); }

  void send(const Json &message);
  bool on_read();
  bool on_write();

 private:
  void handle_payload(const std::string &payload);
  bool pull_frames();

  int fd_ = -1;
  int64_t user_id_ = 0;
  std::string token_;
  std::string in_;
  std::string out_;
  size_t out_pos_ = 0;
  bool closing_ = false;
  App *app_ = nullptr;
  std::function<void(Connection &)> on_output_;
};

}
