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

 private:
  Json dispatch(Connection &connection, const std::string &method, const Json &params);
  Json dialog_json(const DialogInfo &dialog);
  void push_dialog_new(int64_t dialog_id, int64_t except_user);

  Database &db_;
  Hub &hub_;
};

}
