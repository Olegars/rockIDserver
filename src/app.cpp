#include "rockid/app.hpp"

#include "rockid/connection.hpp"
#include "rockid/log.hpp"

#include <set>
#include <string>

namespace rockid {
namespace {

bool public_method(const std::string &method) {
  return method == "ping" || method == "auth.signUp" || method == "auth.signIn" ||
         method == "auth.bind";
}

void check_username(const std::string &username) {
  if (username.size() < 3 || username.size() > 32) throw ApiError(400, "BAD_USERNAME");
  for (unsigned char c : username) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) throw ApiError(400, "BAD_USERNAME");
  }
}

void check_text(const std::string &value, size_t max_len, const char *error) {
  if (value.empty() || value.size() > max_len) throw ApiError(400, error);
  for (unsigned char c : value) {
    if (c < 0x20) throw ApiError(400, error);
  }
}

Json user_json(const User &user) {
  Json j = Json::object();
  j.o["id"] = Json::integer(user.id);
  j.o["username"] = Json::str(user.username);
  j.o["display_name"] = Json::str(user.display_name);
  return j;
}

Json message_json(const Message &message) {
  Json j = Json::object();
  j.o["id"] = Json::integer(message.id);
  j.o["dialog_id"] = Json::integer(message.dialog_id);
  j.o["sender_id"] = Json::integer(message.sender_id);
  j.o["text"] = Json::str(message.text);
  j.o["date"] = Json::integer(message.created_at);
  return j;
}

int64_t clamp_limit(const Json &params, int64_t fallback, int64_t max_value) {
  auto limit = optional_int(params, "limit");
  if (!limit) return fallback;
  if (*limit < 1 || *limit > max_value) throw ApiError(400, "BAD_LIMIT");
  return *limit;
}

}  // namespace

App::App(Database &db, Hub &hub) : db_(db), hub_(hub) {}

void App::handle(Connection &connection, const Json &request) {
  Json id = Json::nul();
  try {
    if (!request.is_object()) throw ApiError(400, "BAD_REQUEST");
    if (const Json *raw_id = request.find("id")) {
      if (!raw_id->is_int() && !raw_id->is_string() && !raw_id->is_null())
        throw ApiError(400, "BAD_ID");
      id = *raw_id;
    }
    const Json *method = request.find("method");
    if (!method || !method->is_string() || method->s.empty()) throw ApiError(400, "BAD_METHOD");
    Json empty = Json::object();
    const Json *params = request.find("params");
    if (params && !params->is_null() && !params->is_object()) throw ApiError(400, "BAD_PARAMS");
    const Json &body = (params && params->is_object()) ? *params : empty;
    if (!public_method(method->s) && connection.user_id() == 0) throw ApiError(401, "UNAUTHORIZED");
    Json result = dispatch(connection, method->s, body);
    Json resp = Json::object();
    resp.o["id"] = std::move(id);
    resp.o["ok"] = Json::boolean(true);
    resp.o["result"] = std::move(result);
    connection.send(resp);
  } catch (const ApiError &ex) {
    connection.send(error_response(id, ex.code, ex.what()));
  } catch (const std::exception &ex) {
    log_error(ex.what());
    connection.send(error_response(id, 500, "INTERNAL"));
  }
}

void App::broadcast_presence(int64_t user_id, bool online) {
  try {
    Json data = Json::object();
    data.o["user_id"] = Json::integer(user_id);
    data.o["online"] = Json::boolean(online);
    Json update = Json::object();
    update.o["update"] = Json::str("presence");
    update.o["data"] = std::move(data);
    hub_.push_many(db_.peer_user_ids(user_id), update, nullptr);
  } catch (const std::exception &ex) {
    log_error(std::string("presence: ") + ex.what());
  }
}

Json App::dialog_json(const DialogInfo &dialog) {
  Json j = Json::object();
  j.o["id"] = Json::integer(dialog.id);
  j.o["type"] = Json::str(dialog.kind);
  j.o["title"] = Json::str(dialog.title);
  j.o["member_count"] = Json::integer(dialog.member_count);
  j.o["unread"] = Json::integer(dialog.unread);
  if (dialog.peer) {
    Json peer = user_json(*dialog.peer);
    peer.o["online"] = Json::boolean(hub_.online(dialog.peer->id));
    j.o["peer"] = std::move(peer);
  } else {
    j.o["peer"] = Json::nul();
  }
  if (dialog.last) j.o["last_message"] = message_json(*dialog.last);
  else j.o["last_message"] = Json::nul();
  return j;
}

void App::push_dialog_new(int64_t dialog_id, int64_t except_user) {
  for (int64_t user_id : db_.member_ids(dialog_id)) {
    if (user_id == except_user) continue;
    auto view = db_.dialog_for(user_id, dialog_id);
    if (!view) continue;
    Json update = Json::object();
    update.o["update"] = Json::str("dialog.new");
    update.o["data"] = dialog_json(*view);
    hub_.push_user(user_id, update, nullptr);
  }
}

Json App::dispatch(Connection &connection, const std::string &method, const Json &params) {
  if (method == "ping") {
    Json result = Json::object();
    result.o["pong"] = Json::boolean(true);
    return result;
  }

  if (method == "auth.signUp") {
    std::string username = require_string(params, "username");
    std::string password = require_string(params, "password");
    std::string display = require_string(params, "display_name");
    check_username(username);
    if (password.size() < 8 || password.size() > 128) throw ApiError(400, "BAD_PASSWORD");
    check_text(display, 64, "BAD_DISPLAY_NAME");
    auto user = db_.create_user(username, password, display);
    if (!user) throw ApiError(409, "USERNAME_TAKEN");
    std::string token = db_.create_session(user->id);
    connection.set_token(token);
    hub_.bind_user(&connection, user->id);
    Json result = Json::object();
    result.o["token"] = Json::str(token);
    result.o["user"] = user_json(*user);
    return result;
  }

  if (method == "auth.signIn") {
    std::string username = require_string(params, "username");
    std::string password = require_string(params, "password");
    auto user = db_.authenticate(username, password);
    if (!user) throw ApiError(401, "INVALID_CREDENTIALS");
    std::string token = db_.create_session(user->id);
    connection.set_token(token);
    hub_.bind_user(&connection, user->id);
    Json result = Json::object();
    result.o["token"] = Json::str(token);
    result.o["user"] = user_json(*user);
    return result;
  }

  if (method == "auth.bind") {
    std::string token = require_string(params, "token");
    auto user_id = db_.user_id_by_token(token);
    if (!user_id) throw ApiError(401, "INVALID_TOKEN");
    auto user = db_.user_by_id(*user_id);
    if (!user) throw ApiError(401, "INVALID_TOKEN");
    connection.set_token(token);
    hub_.bind_user(&connection, user->id);
    Json result = Json::object();
    result.o["user"] = user_json(*user);
    return result;
  }

  if (method == "auth.signOut") {
    if (!connection.token().empty()) db_.delete_session(connection.token());
    hub_.unbind(&connection);
    return Json::object();
  }

  if (method == "users.getMe") {
    auto user = db_.user_by_id(connection.user_id());
    if (!user) throw ApiError(401, "UNAUTHORIZED");
    return user_json(*user);
  }

  if (method == "users.get") {
    int64_t user_id = require_int(params, "user_id");
    auto user = db_.user_by_id(user_id);
    if (!user) throw ApiError(404, "USER_NOT_FOUND");
    Json result = user_json(*user);
    result.o["online"] = Json::boolean(hub_.online(user_id));
    return result;
  }

  if (method == "users.search") {
    std::string query = require_string(params, "query");
    if (query.empty() || query.size() > 32) throw ApiError(400, "BAD_QUERY");
    auto users = db_.search_users(query, connection.user_id(), static_cast<int>(clamp_limit(params, 20, 50)));
    Json arr = Json::array();
    for (const User &user : users) {
      Json row = user_json(user);
      row.o["online"] = Json::boolean(hub_.online(user.id));
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["users"] = std::move(arr);
    return result;
  }

  if (method == "contacts.add") {
    std::optional<User> peer;
    if (auto user_id = optional_int(params, "user_id")) peer = db_.user_by_id(*user_id);
    else if (auto username = optional_string(params, "username")) peer = db_.user_by_username(*username);
    else throw ApiError(400, "BAD_PARAMS");
    if (!peer) throw ApiError(404, "USER_NOT_FOUND");
    if (peer->id == connection.user_id()) throw ApiError(400, "BAD_CONTACT");
    db_.add_contact(connection.user_id(), peer->id);
    Json result = user_json(*peer);
    result.o["online"] = Json::boolean(hub_.online(peer->id));
    return result;
  }

  if (method == "contacts.get") {
    Json arr = Json::array();
    for (const User &user : db_.list_contacts(connection.user_id())) {
      Json row = user_json(user);
      row.o["online"] = Json::boolean(hub_.online(user.id));
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["contacts"] = std::move(arr);
    return result;
  }

  if (method == "dialogs.get") {
    Json arr = Json::array();
    for (const DialogInfo &dialog : db_.list_dialogs(connection.user_id()))
      arr.a.push_back(dialog_json(dialog));
    Json result = Json::object();
    result.o["dialogs"] = std::move(arr);
    return result;
  }

  if (method == "dialogs.createPrivate") {
    int64_t peer_id = require_int(params, "user_id");
    if (peer_id == connection.user_id()) throw ApiError(400, "BAD_PEER");
    if (!db_.user_by_id(peer_id)) throw ApiError(404, "USER_NOT_FOUND");
    int64_t dialog_id = db_.create_private(connection.user_id(), peer_id);
    push_dialog_new(dialog_id, connection.user_id());
    auto view = db_.dialog_for(connection.user_id(), dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    return dialog_json(*view);
  }

  if (method == "dialogs.createGroup") {
    std::string title = require_string(params, "title");
    check_text(title, 128, "BAD_TITLE");
    const Json *raw_members = params.find("member_ids");
    std::vector<int64_t> members;
    if (raw_members) {
      if (!raw_members->is_array()) throw ApiError(400, "BAD_MEMBERS");
      std::set<int64_t> seen;
      seen.insert(connection.user_id());
      for (const Json &item : raw_members->a) {
        if (!item.is_int()) throw ApiError(400, "BAD_MEMBERS");
        if (!seen.insert(item.i).second) continue;
        if (!db_.user_by_id(item.i)) throw ApiError(404, "USER_NOT_FOUND");
        members.push_back(item.i);
      }
    }
    if (members.size() + 1 > 200) throw ApiError(400, "TOO_MANY_MEMBERS");
    int64_t dialog_id = db_.create_group(connection.user_id(), title, members);
    push_dialog_new(dialog_id, connection.user_id());
    auto view = db_.dialog_for(connection.user_id(), dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    return dialog_json(*view);
  }

  if (method == "dialogs.addMember") {
    int64_t dialog_id = require_int(params, "dialog_id");
    int64_t user_id = require_int(params, "user_id");
    auto head = db_.find_dialog(dialog_id);
    if (!head) throw ApiError(404, "DIALOG_NOT_FOUND");
    if (head->kind != "group") throw ApiError(400, "NOT_A_GROUP");
    auto role = db_.member_role(dialog_id, connection.user_id());
    if (!role) throw ApiError(403, "NOT_MEMBER");
    if (*role != "owner") throw ApiError(403, "NOT_OWNER");
    auto user = db_.user_by_id(user_id);
    if (!user) throw ApiError(404, "USER_NOT_FOUND");
    bool added = db_.add_member(dialog_id, user_id);
    if (added) {
      push_dialog_new(dialog_id, connection.user_id());
      Json data = Json::object();
      data.o["dialog_id"] = Json::integer(dialog_id);
      data.o["user"] = user_json(*user);
      Json update = Json::object();
      update.o["update"] = Json::str("dialog.member");
      update.o["data"] = std::move(data);
      hub_.push_many(db_.member_ids(dialog_id), update, &connection);
    }
    Json result = Json::object();
    result.o["added"] = Json::boolean(added);
    auto view = db_.dialog_for(connection.user_id(), dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    result.o["dialog"] = dialog_json(*view);
    return result;
  }

  if (method == "dialogs.members") {
    int64_t dialog_id = require_int(params, "dialog_id");
    if (!db_.member_role(dialog_id, connection.user_id())) throw ApiError(403, "NOT_MEMBER");
    Json arr = Json::array();
    for (const Member &member : db_.members(dialog_id)) {
      auto user = db_.user_by_id(member.user_id);
      if (!user) continue;
      Json row = user_json(*user);
      row.o["role"] = Json::str(member.role);
      row.o["online"] = Json::boolean(hub_.online(member.user_id));
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["members"] = std::move(arr);
    return result;
  }

  if (method == "messages.send") {
    int64_t dialog_id = require_int(params, "dialog_id");
    std::string text = require_string(params, "text");
    if (text.empty() || text.size() > 4096) throw ApiError(400, "BAD_TEXT");
    for (unsigned char c : text) {
      if (c < 0x20 && c != '\n' && c != '\t') throw ApiError(400, "BAD_TEXT");
    }
    if (!db_.member_role(dialog_id, connection.user_id())) throw ApiError(403, "NOT_MEMBER");
    Message message = db_.add_message(dialog_id, connection.user_id(), text);
    Json update = Json::object();
    update.o["update"] = Json::str("message.new");
    update.o["data"] = message_json(message);
    hub_.push_many(db_.member_ids(dialog_id), update, &connection);
    return message_json(message);
  }

  if (method == "messages.getHistory") {
    int64_t dialog_id = require_int(params, "dialog_id");
    if (!db_.member_role(dialog_id, connection.user_id())) throw ApiError(403, "NOT_MEMBER");
    int64_t before_id = 0;
    if (auto before = optional_int(params, "before_id")) {
      if (*before < 0) throw ApiError(400, "BAD_BEFORE");
      before_id = *before;
    }
    Json arr = Json::array();
    for (const Message &message :
         db_.history(dialog_id, before_id, static_cast<int>(clamp_limit(params, 50, 100))))
      arr.a.push_back(message_json(message));
    Json result = Json::object();
    result.o["messages"] = std::move(arr);
    return result;
  }

  if (method == "messages.read") {
    int64_t dialog_id = require_int(params, "dialog_id");
    int64_t max_id = require_int(params, "max_id");
    if (max_id < 0) throw ApiError(400, "BAD_MAX_ID");
    if (!db_.member_role(dialog_id, connection.user_id())) throw ApiError(403, "NOT_MEMBER");
    db_.mark_read(connection.user_id(), dialog_id, max_id);
    Json result = Json::object();
    result.o["last_read_id"] = Json::integer(max_id);
    return result;
  }

  throw ApiError(404, "UNKNOWN_METHOD");
}

}
