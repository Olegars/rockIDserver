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

  Json user_json(const User &user) const;
  Json message_json(const Message &message) const;
  Json dialog_json(const DialogInfo &dialog);
  void push_message(const Message &message, Connection *except);
  void push_dialog_new(int64_t dialog_id, int64_t except_user);

 private:
  Json dispatch(Connection &connection, const std::string &method, const Json &params);
  Json dispatch_extra(Connection &connection, const std::string &method, const Json &params);
  void ensure_session(Connection &connection);

  Database &db_;
  Hub &hub_;
};

}
