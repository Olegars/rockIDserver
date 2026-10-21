#pragma once

#include "rockid/database.hpp"
#include "rockid/hub.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rockid {

class Connection;
class CallBoard;

class App {
 public:
  App(Database &db, Hub &hub);
  ~App();

  void handle(Connection &connection, const Json &request);
  void broadcast_presence(int64_t user_id, bool online);
  void flush_due();
  void remember_offline(int64_t user_id, const Json &message);
  std::string handle_http(const std::string &method, const std::string &path, const std::string &body);
  void set_media_port(int port) { media_port_ = port; }
  // 0 — сокет принят, 401 — токен, 404 — человека нет в этой комнате.
  int accept_media(const std::string &token, int64_t call_id, int64_t &user_id);
  // 0 — переслать, 1 — отбросить, 2 — закрыть сокет.
  int accept_audio(int64_t user_id, int64_t call_id, const std::string &bytes, uint32_t &seq, int64_t &ts,
                   std::vector<int64_t> &peers);

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
  void tick_calls();
  void leave_live(int64_t user_id);
  Json room_json(const struct LiveRoom &room) const;
  void push_room(const struct LiveRoom &room, const char *name, Connection *except);
  Json drop_peer(int64_t call_id, int64_t user_id, Connection *except);
  void finish_call(int64_t call_id);
  bool save_call_recording(int64_t user_id, int64_t call_id);

  Database &db_;
  Hub &hub_;
  int media_port_ = 0;
  std::unique_ptr<CallBoard> calls_;
};

}
