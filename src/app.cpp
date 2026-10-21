#include "rockid/app.hpp"

#include "call_board.hpp"
#include "rockid/connection.hpp"
#include "rockid/log.hpp"
#include "seal.hpp"

#include <ctime>
#include <set>
#include <string>

namespace rockid {
namespace {

bool public_method(const std::string &method) {
  return method == "ping" || method == "auth.signUp" || method == "auth.signIn" ||
         method == "auth.bind" || method == "auth.checkCloudPassword" || method == "auth.botSignIn";
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

}  // namespace

Json App::user_json(const User &user) const {
  Json j = Json::object();
  j.o["id"] = Json::integer(user.id);
  j.o["username"] = Json::str(user.username);
  j.o["display_name"] = Json::str(user.display_name);
  return j;
}

Json App::message_json(const Message &message) const {
  Json j = Json::object();
  j.o["id"] = Json::integer(message.id);
  j.o["dialog_id"] = Json::integer(message.dialog_id);
  j.o["sender_id"] = Json::integer(message.sender_id);
  j.o["text"] = Json::str(message.text);
  j.o["date"] = Json::integer(message.created_at);
  j.o["kind"] = Json::str(message.kind);
  j.o["reply_to"] = message.reply_to ? Json::integer(message.reply_to) : Json::nul();
  j.o["quote"] = Json::str(message.quote);
  j.o["file_id"] = message.file_id ? Json::integer(message.file_id) : Json::nul();
  j.o["silent"] = Json::boolean(message.silent);
  j.o["scheduled_at"] = Json::integer(message.scheduled_at);
  j.o["edited_at"] = Json::integer(message.edited_at);
  j.o["entities"] = Json::str(message.entities);
  j.o["views"] = Json::integer(message.views);
  j.o["expires_at"] = Json::integer(message.expires_at);
  j.o["topic_id"] = Json::integer(message.topic_id);
  j.o["no_forward"] = Json::boolean(message.no_forward);
  j.o["transcript"] = Json::str(message.transcript);
  j.o["translation"] = Json::str(message.translation);
  j.o["translation_lang"] = Json::str(message.translation_lang);
  Json reactions = Json::array();
  for (const ReactionCount &reaction : message.reactions) {
    Json row = Json::object();
    row.o["emoji"] = Json::str(reaction.emoji);
    row.o["count"] = Json::integer(reaction.count);
    row.o["mine"] = Json::boolean(reaction.mine);
    reactions.a.push_back(std::move(row));
  }
  j.o["reactions"] = std::move(reactions);
  return j;
}

namespace {

int64_t clamp_limit(const Json &params, int64_t fallback, int64_t max_value) {
  auto limit = optional_int(params, "limit");
  if (!limit) return fallback;
  if (*limit < 1 || *limit > max_value) throw ApiError(400, "BAD_LIMIT");
  return *limit;
}

}  // namespace

App::App(Database &db, Hub &hub) : db_(db), hub_(hub), calls_(std::make_unique<CallBoard>()) {}

App::~App() = default;

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
    if (!public_method(method->s)) ensure_session(connection);
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

void App::ensure_session(Connection &connection) {
  if (connection.token().empty()) {
    hub_.unbind(&connection);
    throw ApiError(401, "UNAUTHORIZED");
  }
  auto user_id = db_.user_id_by_token(connection.token());
  if (!user_id || *user_id != connection.user_id()) {
    hub_.unbind(&connection);
    throw ApiError(401, "SESSION_REVOKED");
  }
}

void App::claim_device(int64_t user_id, const Json &params) {
  auto device_id = optional_string(params, "device_id");
  if (!device_id || device_id->empty()) return;
  if (device_id->size() > 64) throw ApiError(400, "BAD_DEVICE");
  bool premium = db_.premium(user_id);
  int64_t count = db_.claim_device(*device_id, user_id, premium);
  if (count > (premium ? 4 : 3)) throw ApiError(400, "TOO_MANY_ACCOUNTS");
}

void App::note_delivered(int64_t dialog_id, int64_t message_id, int64_t user_id, int64_t sender_id) {
  if (user_id == sender_id || sender_id == 0) return;
  Json data = Json::object();
  data.o["dialog_id"] = Json::integer(dialog_id);
  data.o["message_id"] = Json::integer(message_id);
  data.o["user_id"] = Json::integer(user_id);
  Json update = Json::object();
  update.o["update"] = Json::str("message.delivered");
  update.o["data"] = std::move(data);
  hub_.push_user(sender_id, update, nullptr);
}

void App::flush_offline(int64_t user_id) {
  for (const std::string &payload : db_.take_pushes(user_id)) {
    Json message;
    try {
      message = parse_json(payload);
    } catch (const JsonError &) {
      continue;
    }
    hub_.push_user(user_id, message, nullptr);
    const Json *update = message.find("update");
    const Json *data = message.find("data");
    if (!update || !update->is_string() || update->s != "message.new" || !data || !data->is_object()) continue;
    auto dialog_id = optional_int(*data, "dialog_id");
    auto message_id = optional_int(*data, "id");
    auto sender_id = optional_int(*data, "sender_id");
    if (dialog_id && message_id && sender_id) note_delivered(*dialog_id, *message_id, user_id, *sender_id);
  }
}

void App::remember_offline(int64_t user_id, const Json &message) {
  const Json *update = message.find("update");
  if (!update || !update->is_string()) return;
  if (update->s == "presence" || update->s == "call.media" || update->s == "call.update") return;
  try {
    db_.queue_push(user_id, dump_json(message));
  } catch (const std::exception &ex) {
    log_error(std::string("offline: ") + ex.what());
  }
}

void App::broadcast_presence(int64_t user_id, bool online) {
  try {
    if (!online) leave_live(user_id);
    db_.set_user_online(user_id, online);
    if (!online) db_.touch_last_seen(user_id);
    Json data = Json::object();
    data.o["user_id"] = Json::integer(user_id);
    data.o["online"] = Json::boolean(online);
    Json update = Json::object();
    update.o["update"] = Json::str("presence");
    update.o["data"] = std::move(data);
    hub_.push_many(db_.presence_recipients(user_id), update, nullptr);
    if (online) flush_offline(user_id);
  } catch (const std::exception &ex) {
    log_error(std::string("presence: ") + ex.what());
  }
}

void App::push_message(const Message &message, Connection *except) {
  Json update = Json::object();
  update.o["update"] = Json::str("message.new");
  update.o["data"] = message_json(message);
  std::vector<int64_t> ids = db_.member_ids(message.dialog_id);
  if (message.kind == "sponsor") {
    std::vector<int64_t> visible;
    for (int64_t user_id : ids) {
      if (!db_.premium(user_id)) visible.push_back(user_id);
    }
    ids.swap(visible);
  }
  hub_.push_many(ids, update, except);
  for (int64_t user_id : ids) {
    if (hub_.online(user_id)) note_delivered(message.dialog_id, message.id, user_id, message.sender_id);
  }
  try {
    db_.enqueue_bot_updates(message.dialog_id, message.sender_id, dump_json(update));
  } catch (const std::exception &ex) {
    log_error(std::string("bot update: ") + ex.what());
  }
}

Json App::dialog_json(const DialogInfo &dialog) {
  Json j = Json::object();
  j.o["id"] = Json::integer(dialog.id);
  j.o["type"] = Json::str(dialog.kind);
  j.o["title"] = Json::str(dialog.title);
  j.o["member_count"] = Json::integer(dialog.member_count);
  j.o["unread"] = Json::integer(dialog.unread);
  j.o["archived"] = Json::boolean(dialog.archived);
  j.o["pinned"] = Json::boolean(dialog.pin_pos > 0);
  j.o["pin_pos"] = Json::integer(dialog.pin_pos);
  j.o["muted_until"] = Json::integer(dialog.muted_until);
  j.o["no_forwards"] = Json::boolean(dialog.no_forwards);
  j.o["forum"] = Json::boolean(dialog.forum);
  j.o["ttl_seconds"] = Json::integer(dialog.ttl_seconds);
  j.o["linked_id"] = dialog.linked_id ? Json::integer(dialog.linked_id) : Json::nul();
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
    std::string device = optional_string(params, "device").value_or("");
    std::string platform = optional_string(params, "platform").value_or("");
    if (device.size() > 64 || platform.size() > 32) throw ApiError(400, "BAD_DEVICE");
    claim_device(user->id, params);
    std::string token = db_.create_session(user->id, device, platform);
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
    if (db_.has_cloud_password(user->id)) {
      Json result = Json::object();
      result.o["need_cloud_password"] = Json::boolean(true);
      result.o["challenge"] = Json::str(db_.create_challenge(user->id));
      result.o["user"] = user_json(*user);
      return result;
    }
    std::string device = optional_string(params, "device").value_or("");
    std::string platform = optional_string(params, "platform").value_or("");
    if (device.size() > 64 || platform.size() > 32) throw ApiError(400, "BAD_DEVICE");
    claim_device(user->id, params);
    std::string token = db_.create_session(user->id, device, platform);
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
    auto prof = db_.profile(connection.user_id());
    if (!prof) throw ApiError(401, "UNAUTHORIZED");
    Json result = user_json(*prof);
    result.o["phone"] = Json::str(prof->phone);
    result.o["bio"] = Json::str(prof->bio);
    result.o["photo_file_id"] = prof->photo_file_id ? Json::integer(prof->photo_file_id) : Json::nul();
    result.o["premium"] = Json::boolean(prof->premium_until > std::time(nullptr));
    result.o["premium_until"] = Json::integer(prof->premium_until);
    result.o["is_bot"] = Json::boolean(prof->is_bot);
    result.o["cloud_password"] = Json::boolean(prof->cloud_password);
    result.o["preferences"] = Json::str(prof->preferences);
    result.o["privacy_phone"] = Json::str(prof->privacy_phone);
    result.o["privacy_last_seen"] = Json::str(prof->privacy_last_seen);
    result.o["privacy_photo"] = Json::str(prof->privacy_photo);
    bool premium = prof->premium_until > std::time(nullptr);
    Json limits = Json::object();
    limits.o["file_bytes"] = Json::integer(premium ? 4LL * 1024 * 1024 * 1024 : 2LL * 1024 * 1024 * 1024);
    limits.o["folders"] = Json::integer(premium ? 20 : 10);
    limits.o["pins"] = Json::integer(premium ? 10 : 5);
    limits.o["channel_subscriptions"] = Json::integer(premium ? 1000 : 500);
    limits.o["accounts"] = Json::integer(premium ? 4 : 3);
    limits.o["download_chunk"] = Json::integer(premium ? 700 * 1024 : 64 * 1024);
    limits.o["upload_chunk"] = Json::integer(premium ? 700 * 1024 : 384 * 1024);
    result.o["limits"] = std::move(limits);
    return result;
  }

  if (method == "users.get") {
    int64_t user_id = require_int(params, "user_id");
    auto prof = db_.profile(user_id);
    if (!prof) throw ApiError(404, "USER_NOT_FOUND");
    Json result = user_json(*prof);
    result.o["bio"] = Json::str(prof->bio);
    result.o["is_bot"] = Json::boolean(prof->is_bot);
    result.o["premium"] = Json::boolean(prof->premium_until > std::time(nullptr));
    bool see_phone = db_.can_see(connection.user_id(), user_id, prof->privacy_phone);
    bool see_photo = db_.can_see(connection.user_id(), user_id, prof->privacy_photo);
    bool see_seen = db_.can_see(connection.user_id(), user_id, prof->privacy_last_seen);
    result.o["phone"] = see_phone ? Json::str(prof->phone) : Json::nul();
    result.o["photo_file_id"] = see_photo && prof->photo_file_id ? Json::integer(prof->photo_file_id) : Json::nul();
    result.o["online"] = Json::boolean(see_seen && hub_.online(user_id));
    result.o["last_seen"] = see_seen ? Json::integer(prof->last_seen_at) : Json::nul();
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
    bool archived = false;
    if (auto flag = optional_bool(params, "archived")) archived = *flag;
    Json arr = Json::array();
    for (const DialogInfo &dialog : db_.list_dialogs(connection.user_id(), archived))
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
    if (static_cast<int64_t>(members.size()) + 1 > 200000) throw ApiError(400, "TOO_MANY_MEMBERS");
    bool forum = false;
    if (auto flag = optional_bool(params, "forum")) forum = *flag;
    int64_t dialog_id = db_.create_group(connection.user_id(), title, members, forum);
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
    auto access = db_.membership(dialog_id, connection.user_id());
    if (!access || !access->can_manage) throw ApiError(403, "NOT_OWNER");
    if (db_.member_count(dialog_id) >= 200000) throw ApiError(400, "TOO_MANY_MEMBERS");
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
    Outgoing draft;
    if (auto text = optional_string(params, "text")) draft.text = *text;
    if (auto kind = optional_string(params, "kind")) draft.kind = *kind;
    if (draft.kind.empty()) draft.kind = "text";
    if (auto quote = optional_string(params, "quote")) draft.quote = *quote;
    if (auto entities = optional_string(params, "entities")) draft.entities = *entities;
    if (auto reply = optional_int(params, "reply_to")) draft.reply_to = *reply;
    if (auto file_id = optional_int(params, "file_id")) draft.file_id = *file_id;
    if (auto silent = optional_bool(params, "silent")) draft.silent = *silent;
    if (auto topic = optional_int(params, "topic_id")) draft.topic_id = *topic;
    if (auto when = optional_int(params, "schedule_at")) draft.scheduled_at = *when;
    bool media = draft.kind == "voice" || draft.kind == "video_note" || draft.kind == "file" ||
                 draft.kind == "photo" || draft.kind == "sticker";
    if (draft.kind != "text" && draft.kind != "voice" && draft.kind != "video_note" &&
        draft.kind != "file" && draft.kind != "photo" && draft.kind != "sticker" &&
        draft.kind != "sponsor")
      throw ApiError(400, "BAD_KIND");
    if (!media && (draft.text.empty() || draft.text.size() > 4096)) throw ApiError(400, "BAD_TEXT");
    if (draft.text.size() > 65536) throw ApiError(400, "BAD_TEXT");
    for (unsigned char c : draft.text) {
      if (c < 0x20 && c != '\n' && c != '\t') throw ApiError(400, "BAD_TEXT");
    }
    if (draft.quote.size() > 256) throw ApiError(400, "BAD_QUOTE");
    if (draft.entities.size() > 4096) throw ApiError(400, "BAD_ENTITIES");
    auto access = db_.membership(dialog_id, connection.user_id());
    if (!access) throw ApiError(403, "NOT_MEMBER");
    if (!access->can_post) throw ApiError(403, "READ_ONLY");
    auto head = db_.find_dialog(dialog_id);
    if (!head) throw ApiError(404, "DIALOG_NOT_FOUND");
    if (draft.kind == "sponsor" && head->kind != "channel") throw ApiError(400, "NOT_A_CHANNEL");
    if (media && draft.file_id <= 0) throw ApiError(400, "FILE_REQUIRED");
    if (draft.file_id && !db_.file_info(draft.file_id)) throw ApiError(404, "FILE_NOT_FOUND");
    if (draft.kind == "sticker") {
      auto sticker = db_.sticker_by_file(draft.file_id);
      if (!sticker) throw ApiError(400, "NOT_A_STICKER");
      auto pack = db_.pack_by_id(sticker->pack_id);
      if (pack && pack->premium && !db_.premium(connection.user_id())) throw ApiError(403, "PREMIUM_REQUIRED");
    }
    if (draft.reply_to) {
      auto replied = db_.message_by_id(draft.reply_to);
      if (!replied || replied->dialog_id != dialog_id || replied->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    }
    if (draft.topic_id && !db_.topic_in_dialog(dialog_id, draft.topic_id)) throw ApiError(404, "TOPIC_NOT_FOUND");
    if (draft.scheduled_at < 0 || (draft.scheduled_at > 0 && draft.scheduled_at > std::time(nullptr) + 366L * 86400))
      throw ApiError(400, "BAD_SCHEDULE");
    if (head->kind == "secret") {
      draft.no_forward = true;
      std::string sealed = seal_dialog(draft.text, db_.keys_for_dialog(dialog_id));
      if (!sealed.empty()) draft.text = std::move(sealed);
      if (head->ttl_seconds > 0 && draft.scheduled_at == 0)
        draft.expires_at = static_cast<int64_t>(std::time(nullptr)) + head->ttl_seconds;
    }
    if (head->no_forwards) draft.no_forward = true;
    Message message = db_.add_message(dialog_id, connection.user_id(), draft);
    if (message.scheduled_at <= message.created_at) push_message(message, &connection);
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
    int64_t topic_id = 0;
    if (auto topic = optional_int(params, "topic_id")) {
      if (*topic < 0) throw ApiError(400, "BAD_TOPIC");
      topic_id = *topic;
    }
    for (const Message &message : db_.history(dialog_id, before_id,
                                             static_cast<int>(clamp_limit(params, 50, 100)),
                                             connection.user_id(), topic_id))
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

  return dispatch_extra(connection, method, params);
}

}
