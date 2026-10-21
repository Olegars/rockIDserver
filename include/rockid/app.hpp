#pragma once

#include "rockid/database.hpp"
#include "rockid/hub.hpp"

namespace rockid {

class Connection;

class App {
 public:
  App(Database &db, Hub &hub);

  void handle(Connection &connection, const Json &request);
  void broadcast_presence(int64_t user_id, bool online);
  void flush_due();
  void remember_offline(int64_t user_id, const Json &message);
  std::string handle_http(const std::string &method, const std::string &path, const std::string &body);

  Json user_json(const User &user) const;
  Json message_json(const Message &message) const;
  Json dialog_json(const DialogInfo &dialog);
  void push_message(const Message &message, Connection *except);
  void push_dialog_new(int64_t dialog_id, int64_t except_user);

 private:
  Json dispatch(Connection &connection, const std::string &method, const Json &params);
  Json dispatch_extra(Connection &connection, const std::string &method, const Json &params);
  void ensure_session(Connection &connection);
  void claim_device(int64_t user_id, const Json &params);
  void note_delivered(int64_t dialog_id, int64_t message_id, int64_t user_id, int64_t sender_id);
  void flush_offline(int64_t user_id);

  Database &db_;
  Hub &hub_;
};

}
