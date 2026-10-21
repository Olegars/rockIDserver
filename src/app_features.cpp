#include "rockid/app.hpp"

#include "rockid/connection.hpp"
#include "rockid/log.hpp"

#include <openssl/rand.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

namespace rockid {
namespace {

constexpr int64_t kMaxGroup = 200000;
constexpr int64_t kFreeFile = 2LL * 1024 * 1024 * 1024;
constexpr int64_t kPremiumFile = 4LL * 1024 * 1024 * 1024;
constexpr int64_t kWriteChunk = 384 * 1024;
constexpr int64_t kFreeRead = 64 * 1024;
constexpr int64_t kPremiumRead = 384 * 1024;

std::string file_path(int64_t id) {
  return "/var/www/rockIDserver/data/files/" + std::to_string(id);
}

int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string b64_decode(const std::string &in) {
  std::string out;
  int value = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    int digit = b64_value(c);
    if (digit < 0) throw ApiError(400, "BAD_BASE64");
    value = (value << 6) | digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((value >> bits) & 0xFF));
    }
  }
  return out;
}

std::string b64_encode(const std::string &in) {
  static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int value = 0;
  int bits = 0;
  for (unsigned char c : in) {
    value = (value << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(alphabet[(value >> bits) & 0x3F]);
    }
  }
  if (bits > 0) out.push_back(alphabet[(value << (6 - bits)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

void check_plain(const std::string &value, size_t max_len, const char *error, bool allow_empty) {
  if ((value.empty() && !allow_empty) || value.size() > max_len) throw ApiError(400, error);
  for (unsigned char c : value) {
    if (c < 0x20 && c != '\n' && c != '\t') throw ApiError(400, error);
  }
}

void check_privacy(const std::string &value) {
  if (value != "everybody" && value != "contacts" && value != "nobody") throw ApiError(400, "BAD_PRIVACY");
}

void reject_card_fields(const Json &params) {
  for (const char *key : {"card", "pan", "cvv", "cvc", "card_number", "wallet_key", "seed"}) {
    if (params.find(key)) throw ApiError(400, "PAYMENT_DETAILS_REJECTED");
  }
}

std::string random_token() {
  unsigned char raw[32];
  if (RAND_bytes(raw, sizeof raw) != 1) throw ApiError(500, "INTERNAL");
  static const char *hex = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < sizeof raw; ++i) {
    out[i * 2] = hex[raw[i] >> 4];
    out[i * 2 + 1] = hex[raw[i] & 0x0F];
  }
  return out;
}

Json file_json(const FileInfo &info) {
  Json j = Json::object();
  j.o["id"] = Json::integer(info.id);
  j.o["name"] = Json::str(info.name);
  j.o["mime"] = Json::str(info.mime);
  j.o["size"] = Json::integer(info.size);
  j.o["received"] = Json::integer(info.received);
  j.o["ready"] = Json::boolean(info.ready);
  return j;
}

Json story_json(const Story &story) {
  Json j = Json::object();
  j.o["id"] = Json::integer(story.id);
  j.o["user_id"] = Json::integer(story.user_id);
  j.o["file_id"] = story.file_id ? Json::integer(story.file_id) : Json::nul();
  j.o["caption"] = Json::str(story.caption);
  j.o["privacy"] = Json::str(story.privacy);
  j.o["no_screenshots"] = Json::boolean(story.no_screenshots);
  j.o["expires_at"] = Json::integer(story.expires_at);
  j.o["views"] = Json::integer(story.views);
  return j;
}

Json invoice_json(const Invoice &invoice) {
  Json j = Json::object();
  j.o["id"] = Json::integer(invoice.id);
  j.o["bot_id"] = Json::integer(invoice.bot_id);
  j.o["user_id"] = Json::integer(invoice.user_id);
  j.o["title"] = Json::str(invoice.title);
  j.o["amount"] = Json::integer(invoice.amount);
  j.o["currency"] = Json::str(invoice.currency);
  j.o["purpose"] = Json::str(invoice.purpose);
  j.o["status"] = Json::str(invoice.status);
  j.o["provider_ref"] = Json::str(invoice.provider_ref);
  return j;
}

}  // namespace

void App::flush_due() {
  try {
    DueBatch batch = db_.take_due(static_cast<int64_t>(std::time(nullptr)));
    for (const Message &message : batch.ready) push_message(message, nullptr);
    for (const Message &message : batch.expired) {
      Json data = Json::object();
      data.o["dialog_id"] = Json::integer(message.dialog_id);
      data.o["message_id"] = Json::integer(message.id);
      Json update = Json::object();
      update.o["update"] = Json::str("message.deleted");
      update.o["data"] = std::move(data);
      hub_.push_many(db_.member_ids(message.dialog_id), update, nullptr);
    }
  } catch (const std::exception &ex) {
    log_error(std::string("flush: ") + ex.what());
  }
}

Json App::dispatch_extra(Connection &connection, const std::string &method, const Json &params) {
  int64_t me = connection.user_id();

  if (method == "auth.checkCloudPassword") {
    std::string challenge = require_string(params, "challenge");
    std::string password = require_string(params, "password");
    if (password.size() < 8 || password.size() > 128) throw ApiError(400, "BAD_PASSWORD");
    auto user_id = db_.take_challenge(challenge);
    if (!user_id) throw ApiError(401, "BAD_CHALLENGE");
    if (!db_.check_cloud_password(*user_id, password)) throw ApiError(401, "INVALID_CREDENTIALS");
    auto user = db_.user_by_id(*user_id);
    if (!user) throw ApiError(401, "INVALID_CREDENTIALS");
    std::string device = optional_string(params, "device").value_or("");
    std::string platform = optional_string(params, "platform").value_or("");
    if (device.size() > 64 || platform.size() > 32) throw ApiError(400, "BAD_DEVICE");
    std::string token = db_.create_session(user->id, device, platform);
    connection.set_token(token);
    hub_.bind_user(&connection, user->id);
    Json result = Json::object();
    result.o["token"] = Json::str(token);
    result.o["user"] = user_json(*user);
    return result;
  }

  if (method == "auth.botSignIn") {
    std::string token = require_string(params, "token");
    auto bot_id = db_.bot_by_token(token);
    if (!bot_id) throw ApiError(401, "INVALID_TOKEN");
    auto user = db_.user_by_id(*bot_id);
    if (!user) throw ApiError(401, "INVALID_TOKEN");
    std::string session = db_.create_session(user->id, "bot", "bot");
    connection.set_token(session);
    hub_.bind_user(&connection, user->id);
    Json result = Json::object();
    result.o["token"] = Json::str(session);
    result.o["user"] = user_json(*user);
    return result;
  }

  if (method == "account.setCloudPassword") {
    std::string password = require_string(params, "password");
    if (password.size() < 8 || password.size() > 128) throw ApiError(400, "BAD_PASSWORD");
    if (db_.has_cloud_password(me)) {
      std::string current = require_string(params, "current");
      if (!db_.check_cloud_password(me, current)) throw ApiError(401, "INVALID_CREDENTIALS");
    }
    db_.set_cloud_password(me, password);
    return Json::object();
  }

  if (method == "account.clearCloudPassword") {
    std::string password = require_string(params, "password");
    if (!db_.check_cloud_password(me, password)) throw ApiError(401, "INVALID_CREDENTIALS");
    db_.clear_cloud_password(me, password);
    return Json::object();
  }

  if (method == "account.sessions") {
    Json arr = Json::array();
    for (const SessionInfo &session : db_.list_sessions(me, connection.token())) {
      Json row = Json::object();
      row.o["id"] = Json::integer(session.id);
      row.o["device"] = Json::str(session.device);
      row.o["platform"] = Json::str(session.platform);
      row.o["local_lock"] = Json::str(session.local_lock);
      row.o["created_at"] = Json::integer(session.created_at);
      row.o["last_seen"] = Json::integer(session.last_seen_at);
      row.o["current"] = Json::boolean(session.current);
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["sessions"] = std::move(arr);
    return result;
  }

  if (method == "account.terminateSession") {
    int64_t session_id = require_int(params, "session_id");
    auto hash = db_.token_by_session(me, session_id);
    if (!hash) throw ApiError(404, "SESSION_NOT_FOUND");
    db_.delete_session_id(me, session_id);
    hub_.close_hash(me, *hash);
    return Json::object();
  }

  if (method == "account.terminateOthers") {
    for (const SessionInfo &session : db_.list_sessions(me, connection.token())) {
      if (session.current) continue;
      db_.delete_session_id(me, session.id);
      hub_.close_hash(me, session.token);
    }
    return Json::object();
  }

  if (method == "account.setLocalLock") {
    std::string kind = require_string(params, "kind");
    if (kind != "none" && kind != "pin" && kind != "bio") throw ApiError(400, "BAD_LOCK");
    db_.set_local_lock(connection.token(), kind == "none" ? "" : kind);
    Json result = Json::object();
    result.o["kind"] = Json::str(kind);
    return result;
  }

  if (method == "account.setPreferences") {
    std::string preferences = require_string(params, "preferences");
    if (preferences.size() > 4096) throw ApiError(400, "BAD_PREFERENCES");
    Json parsed = parse_json(preferences);
    if (!parsed.is_object()) throw ApiError(400, "BAD_PREFERENCES");
    db_.set_preferences(me, preferences);
    return Json::object();
  }

  if (method == "users.updateProfile") {
    auto prof = db_.profile(me);
    if (!prof) throw ApiError(401, "UNAUTHORIZED");
    std::string display = optional_string(params, "display_name").value_or(prof->display_name);
    std::string phone = optional_string(params, "phone").value_or(prof->phone);
    std::string bio = optional_string(params, "bio").value_or(prof->bio);
    check_plain(display, 64, "BAD_DISPLAY_NAME", false);
    check_plain(bio, 256, "BAD_BIO", true);
    if (!phone.empty()) {
      if (phone.size() < 8 || phone.size() > 16 || phone[0] != '+') throw ApiError(400, "BAD_PHONE");
      for (size_t i = 1; i < phone.size(); ++i) {
        if (phone[i] < '0' || phone[i] > '9') throw ApiError(400, "BAD_PHONE");
      }
    }
    bool set_photo = params.find("photo_file_id") != nullptr;
    int64_t photo = prof->photo_file_id;
    if (set_photo) {
      if (const Json *raw = params.find("photo_file_id"); raw && raw->is_null()) photo = 0;
      else photo = require_int(params, "photo_file_id");
      if (photo && !db_.can_read_file(me, photo)) throw ApiError(404, "FILE_NOT_FOUND");
    }
    db_.update_profile(me, display, phone, bio, photo, set_photo);
    return Json::object();
  }

  if (method == "users.setPrivacy") {
    auto prof = db_.profile(me);
    if (!prof) throw ApiError(401, "UNAUTHORIZED");
    std::string phone = optional_string(params, "phone").value_or(prof->privacy_phone);
    std::string last_seen = optional_string(params, "last_seen").value_or(prof->privacy_last_seen);
    std::string photo = optional_string(params, "photo").value_or(prof->privacy_photo);
    check_privacy(phone);
    check_privacy(last_seen);
    check_privacy(photo);
    db_.set_privacy(me, phone, last_seen, photo);
    return Json::object();
  }

  if (method == "users.addCloseFriend") {
    int64_t peer = require_int(params, "user_id");
    if (!db_.user_by_id(peer) || peer == me) throw ApiError(400, "BAD_PEER");
    db_.add_close_friend(me, peer);
    return Json::object();
  }

  auto require_dialog = [&](int64_t dialog_id) {
    auto head = db_.find_dialog(dialog_id);
    if (!head) throw ApiError(404, "DIALOG_NOT_FOUND");
    if (!db_.member_role(dialog_id, me)) throw ApiError(403, "NOT_MEMBER");
    return *head;
  };

  if (method == "dialogs.createChannel") {
    std::string title = require_string(params, "title");
    check_plain(title, 128, "BAD_TITLE", false);
    int64_t dialog_id = db_.create_channel(me, title);
    auto view = db_.dialog_for(me, dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    return dialog_json(*view);
  }

  if (method == "dialogs.createSecret") {
    int64_t peer = require_int(params, "user_id");
    int ttl = 0;
    if (auto raw = optional_int(params, "ttl_seconds")) ttl = static_cast<int>(*raw);
    if (peer == me || !db_.user_by_id(peer)) throw ApiError(400, "BAD_PEER");
    if (ttl < 0 || ttl > 604800) throw ApiError(400, "BAD_TTL");
    int64_t dialog_id = db_.create_secret(me, peer, ttl);
    push_dialog_new(dialog_id, me);
    auto view = db_.dialog_for(me, dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    return dialog_json(*view);
  }

  if (method == "dialogs.getSaved") {
    int64_t dialog_id = db_.saved_dialog(me);
    auto view = db_.dialog_for(me, dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    return dialog_json(*view);
  }

  if (method == "dialogs.join") {
    int64_t dialog_id = require_int(params, "dialog_id");
    auto head = db_.find_dialog(dialog_id);
    if (!head) throw ApiError(404, "DIALOG_NOT_FOUND");
    if (head->kind != "channel") throw ApiError(400, "NOT_A_CHANNEL");
    if (db_.member_role(dialog_id, me)) {
      auto view = db_.dialog_for(me, dialog_id);
      return dialog_json(*view);
    }
    bool premium = db_.premium(me);
    int64_t cap = premium ? 1000 : 500;
    if (db_.channel_subscriptions(me) >= cap) throw ApiError(400, "TOO_MANY_SUBSCRIPTIONS");
    if (!db_.add_subscriber(dialog_id, me)) throw ApiError(403, "BANNED");
    auto view = db_.dialog_for(me, dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    return dialog_json(*view);
  }

  if (method == "dialogs.leave" || method == "dialogs.kick" || method == "dialogs.ban" ||
      method == "dialogs.unban" || method == "dialogs.setAdmin") {
    int64_t dialog_id = require_int(params, "dialog_id");
    auto head = db_.find_dialog(dialog_id);
    if (!head) throw ApiError(404, "DIALOG_NOT_FOUND");
    if (method == "dialogs.leave") {
      if (head->kind == "private" || head->kind == "secret" || head->kind == "saved")
        throw ApiError(400, "BAD_DIALOG");
      if (!db_.kick_member(dialog_id, me)) throw ApiError(403, "NOT_MEMBER");
      return Json::object();
    }
    int64_t user_id = require_int(params, "user_id");
    auto access = db_.membership(dialog_id, me);
    if (!access) throw ApiError(403, "NOT_MEMBER");
    if (method == "dialogs.setAdmin") {
      if (!access->can_manage) throw ApiError(403, "FORBIDDEN");
      bool ok = db_.set_member_admin(dialog_id, user_id, optional_bool(params, "can_delete").value_or(false),
                                     optional_bool(params, "can_ban").value_or(false),
                                     optional_bool(params, "can_pin").value_or(false),
                                     optional_bool(params, "can_manage").value_or(false),
                                     optional_bool(params, "can_post").value_or(true));
      if (!ok) throw ApiError(404, "USER_NOT_FOUND");
      return Json::object();
    }
    if (!access->can_ban) throw ApiError(403, "FORBIDDEN");
    bool ok = method == "dialogs.kick" ? db_.kick_member(dialog_id, user_id)
                                       : db_.set_banned(dialog_id, user_id, method == "dialogs.ban");
    if (!ok) throw ApiError(404, "USER_NOT_FOUND");
    return Json::object();
  }

  if (method == "dialogs.setSecretKey") {
    int64_t dialog_id = require_int(params, "dialog_id");
    auto head = require_dialog(dialog_id);
    if (head.kind != "secret") throw ApiError(400, "NOT_SECRET");
    std::string key = require_string(params, "public_key");
    if (key.empty() || key.size() > 4096) throw ApiError(400, "BAD_KEY");
    db_.set_secret_key(dialog_id, me, key);
    return Json::object();
  }

  if (method == "dialogs.secretKeys") {
    int64_t dialog_id = require_int(params, "dialog_id");
    auto head = require_dialog(dialog_id);
    if (head.kind != "secret") throw ApiError(400, "NOT_SECRET");
    Json arr = Json::array();
    for (const auto &row : db_.secret_keys(dialog_id)) {
      Json item = Json::object();
      item.o["user_id"] = Json::integer(row.first);
      item.o["public_key"] = Json::str(row.second);
      arr.a.push_back(std::move(item));
    }
    Json result = Json::object();
    result.o["keys"] = std::move(arr);
    return result;
  }

  if (method == "dialogs.createTopic") {
    int64_t dialog_id = require_int(params, "dialog_id");
    auto head = require_dialog(dialog_id);
    if (!head.forum) throw ApiError(400, "NOT_A_FORUM");
    std::string title = require_string(params, "title");
    check_plain(title, 128, "BAD_TITLE", false);
    int64_t id = db_.create_topic(dialog_id, me, title);
    Json result = Json::object();
    result.o["id"] = Json::integer(id);
    result.o["title"] = Json::str(title);
    return result;
  }

  if (method == "dialogs.topics") {
    int64_t dialog_id = require_int(params, "dialog_id");
    require_dialog(dialog_id);
    Json arr = Json::array();
    for (const Topic &topic : db_.list_topics(dialog_id)) {
      Json row = Json::object();
      row.o["id"] = Json::integer(topic.id);
      row.o["title"] = Json::str(topic.title);
      row.o["created_by"] = Json::integer(topic.created_by);
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["topics"] = std::move(arr);
    return result;
  }

  if (method == "dialogs.setDiscussion") {
    int64_t channel_id = require_int(params, "channel_id");
    int64_t group_id = require_int(params, "group_id");
    auto channel = require_dialog(channel_id);
    auto group = require_dialog(group_id);
    if (channel.kind != "channel" || group.kind != "group") throw ApiError(400, "BAD_DIALOG");
    auto access = db_.membership(channel_id, me);
    if (!access || !access->can_manage) throw ApiError(403, "FORBIDDEN");
    db_.set_discussion(channel_id, group_id);
    return Json::object();
  }

  if (method == "dialogs.archive" || method == "dialogs.mute" || method == "dialogs.pin") {
    int64_t dialog_id = require_int(params, "dialog_id");
    require_dialog(dialog_id);
    if (method == "dialogs.pin") {
      bool pinned = optional_bool(params, "pinned").value_or(true);
      if (pinned && db_.pin_count(me) >= (db_.premium(me) ? 10 : 5)) throw ApiError(400, "TOO_MANY_PINS");
      db_.set_dialog_flags(dialog_id, me, std::nullopt, std::nullopt, pinned ? 1 : 0);
    } else if (method == "dialogs.archive") {
      db_.set_dialog_flags(dialog_id, me, optional_bool(params, "archived").value_or(true), std::nullopt,
                           std::nullopt);
    } else {
      int64_t until = require_int(params, "until");
      if (until < -1) throw ApiError(400, "BAD_MUTE");
      db_.set_dialog_flags(dialog_id, me, std::nullopt, until, std::nullopt);
    }
    auto view = db_.dialog_for(me, dialog_id);
    if (!view) throw ApiError(500, "INTERNAL");
    return dialog_json(*view);
  }

  if (method == "messages.edit") {
    int64_t message_id = require_int(params, "message_id");
    std::string text = require_string(params, "text");
    check_plain(text, 4096, "BAD_TEXT", false);
    std::string entities = optional_string(params, "entities").value_or("");
    if (entities.size() > 4096) throw ApiError(400, "BAD_ENTITIES");
    auto message = db_.message_by_id(message_id);
    if (!message || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    require_dialog(message->dialog_id);
    if (message->sender_id != me) throw ApiError(403, "FORBIDDEN");
    Message edited = db_.edit_message(message_id, text, entities);
    Json update = Json::object();
    update.o["update"] = Json::str("message.edit");
    update.o["data"] = message_json(edited);
    hub_.push_many(db_.member_ids(edited.dialog_id), update, &connection);
    return message_json(edited);
  }

  if (method == "messages.delete") {
    int64_t message_id = require_int(params, "message_id");
    auto message = db_.message_by_id(message_id);
    if (!message || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    auto head = require_dialog(message->dialog_id);
    auto access = db_.membership(message->dialog_id, me);
    bool personal = head.kind == "private" || head.kind == "secret" || head.kind == "saved";
    if (message->sender_id != me && !(personal || (access && access->can_delete)))
      throw ApiError(403, "FORBIDDEN");
    db_.delete_message(message_id);
    Json data = Json::object();
    data.o["dialog_id"] = Json::integer(message->dialog_id);
    data.o["message_id"] = Json::integer(message_id);
    Json update = Json::object();
    update.o["update"] = Json::str("message.deleted");
    update.o["data"] = data;
    hub_.push_many(db_.member_ids(message->dialog_id), update, &connection);
    return data;
  }

  if (method == "messages.forward") {
    int64_t message_id = require_int(params, "message_id");
    int64_t dialog_id = require_int(params, "dialog_id");
    auto message = db_.message_by_id(message_id);
    if (!message || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    auto source = require_dialog(message->dialog_id);
    auto target = require_dialog(dialog_id);
    if (message->no_forward || source.no_forwards || source.kind == "secret" || target.kind == "secret")
      throw ApiError(403, "NO_FORWARD");
    auto access = db_.membership(dialog_id, me);
    if (!access || !access->can_post) throw ApiError(403, "READ_ONLY");
    Outgoing draft;
    draft.kind = message->kind == "sponsor" ? "text" : message->kind;
    draft.text = message->text;
    draft.file_id = message->file_id;
    draft.quote = message->quote;
    Message copy = db_.add_message(dialog_id, me, draft);
    push_message(copy, &connection);
    return message_json(copy);
  }

  if (method == "messages.react") {
    int64_t message_id = require_int(params, "message_id");
    std::string emoji = require_string(params, "emoji");
    check_plain(emoji, 32, "BAD_EMOJI", false);
    auto message = db_.message_by_id(message_id);
    if (!message || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    require_dialog(message->dialog_id);
    bool added = db_.toggle_reaction(message_id, me, emoji);
    Json data = Json::object();
    data.o["dialog_id"] = Json::integer(message->dialog_id);
    data.o["message_id"] = Json::integer(message_id);
    data.o["user_id"] = Json::integer(me);
    data.o["emoji"] = Json::str(emoji);
    data.o["added"] = Json::boolean(added);
    Json update = Json::object();
    update.o["update"] = Json::str("message.reaction");
    update.o["data"] = data;
    hub_.push_many(db_.member_ids(message->dialog_id), update, &connection);
    return data;
  }

  if (method == "messages.view") {
    int64_t dialog_id = require_int(params, "dialog_id");
    require_dialog(dialog_id);
    const Json *ids = params.find("message_ids");
    if (!ids || !ids->is_array() || ids->a.empty() || ids->a.size() > 100) throw ApiError(400, "BAD_IDS");
    std::vector<int64_t> list;
    for (const Json &item : ids->a) {
      if (!item.is_int()) throw ApiError(400, "BAD_IDS");
      auto message = db_.message_by_id(item.i);
      if (!message || message->dialog_id != dialog_id) throw ApiError(404, "MESSAGE_NOT_FOUND");
      list.push_back(item.i);
    }
    db_.add_views(list);
    Json result = Json::object();
    result.o["viewed"] = Json::integer(static_cast<int64_t>(list.size()));
    return result;
  }

  if (method == "messages.reportScreenshot") {
    int64_t dialog_id = require_int(params, "dialog_id");
    auto head = require_dialog(dialog_id);
    if (head.kind != "secret") throw ApiError(400, "NOT_SECRET");
    Json data = Json::object();
    data.o["dialog_id"] = Json::integer(dialog_id);
    data.o["user_id"] = Json::integer(me);
    Json update = Json::object();
    update.o["update"] = Json::str("screenshot");
    update.o["data"] = data;
    hub_.push_many(db_.member_ids(dialog_id), update, &connection);
    return data;
  }

  if (method == "messages.setTranslation") {
    int64_t message_id = require_int(params, "message_id");
    std::string lang = require_string(params, "lang");
    std::string text = require_string(params, "text");
    check_plain(lang, 16, "BAD_LANG", false);
    check_plain(text, 4096, "BAD_TEXT", false);
    auto message = db_.message_by_id(message_id);
    if (!message || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    require_dialog(message->dialog_id);
    db_.set_translation(message_id, lang, text);
    Json result = Json::object();
    result.o["lang"] = Json::str(lang);
    result.o["text"] = Json::str(text);
    result.o["engine"] = Json::str("client");
    return result;
  }

  if (method == "messages.translate") {
    int64_t message_id = require_int(params, "message_id");
    auto message = db_.message_by_id(message_id);
    if (!message || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    require_dialog(message->dialog_id);
    if (message->translation.empty()) throw ApiError(404, "NO_TRANSLATION");
    Json result = Json::object();
    result.o["lang"] = Json::str(message->translation_lang);
    result.o["text"] = Json::str(message->translation);
    result.o["engine"] = Json::str("client");
    return result;
  }

  if (method == "messages.setTranscript") {
    if (!db_.premium(me)) throw ApiError(403, "PREMIUM_REQUIRED");
    int64_t message_id = require_int(params, "message_id");
    std::string text = require_string(params, "text");
    check_plain(text, 8192, "BAD_TEXT", false);
    auto message = db_.message_by_id(message_id);
    if (!message || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    require_dialog(message->dialog_id);
    if (message->kind != "voice" && message->kind != "video_note") throw ApiError(400, "NOT_MEDIA");
    db_.set_transcript(message_id, text);
    Json result = Json::object();
    result.o["text"] = Json::str(text);
    result.o["engine"] = Json::str("client");
    return result;
  }

  if (method == "files.create") {
    std::string name = require_string(params, "name");
    std::string mime = require_string(params, "mime");
    int64_t size = require_int(params, "size");
    check_plain(name, 255, "BAD_NAME", false);
    check_plain(mime, 128, "BAD_MIME", false);
    int64_t cap = db_.premium(me) ? kPremiumFile : kFreeFile;
    if (size < 1 || size > cap) throw ApiError(400, "BAD_SIZE");
    FileInfo info = db_.create_file(me, name, mime, size);
    std::filesystem::create_directories("/var/www/rockIDserver/data/files");
    std::ofstream created(file_path(info.id), std::ios::binary | std::ios::trunc);
    if (!created) throw ApiError(500, "FILE_IO");
    Json result = file_json(info);
    result.o["chunk_bytes"] = Json::integer(kWriteChunk);
    return result;
  }

  if (method == "files.write") {
    int64_t file_id = require_int(params, "file_id");
    int64_t offset = require_int(params, "offset");
    std::string data = b64_decode(require_string(params, "data"));
    auto info = db_.file_info(file_id);
    if (!info || info->owner_id != me) throw ApiError(404, "FILE_NOT_FOUND");
    if (info->ready) throw ApiError(400, "FILE_CLOSED");
    if (offset != info->received || data.empty() || static_cast<int64_t>(data.size()) > kWriteChunk)
      throw ApiError(400, "BAD_CHUNK");
    if (info->received + static_cast<int64_t>(data.size()) > info->size) throw ApiError(400, "BAD_CHUNK");
    std::fstream out(file_path(file_id), std::ios::binary | std::ios::in | std::ios::out);
    if (!out) throw ApiError(500, "FILE_IO");
    out.seekp(offset);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) throw ApiError(500, "FILE_IO");
    int64_t received = info->received + static_cast<int64_t>(data.size());
    db_.file_received(file_id, received, false);
    Json result = Json::object();
    result.o["received"] = Json::integer(received);
    return result;
  }

  if (method == "files.finish") {
    int64_t file_id = require_int(params, "file_id");
    auto info = db_.file_info(file_id);
    if (!info || info->owner_id != me) throw ApiError(404, "FILE_NOT_FOUND");
    if (info->received != info->size) throw ApiError(400, "INCOMPLETE");
    db_.file_received(file_id, info->received, true);
    info->ready = true;
    return file_json(*info);
  }

  if (method == "files.read") {
    int64_t file_id = require_int(params, "file_id");
    int64_t offset = optional_int(params, "offset").value_or(0);
    if (offset < 0) throw ApiError(400, "BAD_OFFSET");
    if (!db_.can_read_file(me, file_id)) throw ApiError(404, "FILE_NOT_FOUND");
    auto info = db_.file_info(file_id);
    if (!info || !info->ready) throw ApiError(404, "FILE_NOT_FOUND");
    int64_t cap = db_.premium(me) ? kPremiumRead : kFreeRead;
    int64_t length = optional_int(params, "length").value_or(cap);
    if (length < 1 || length > cap) throw ApiError(400, "BAD_LENGTH");
    if (offset > info->size) throw ApiError(400, "BAD_OFFSET");
    if (offset + length > info->size) length = info->size - offset;
    std::ifstream in(file_path(file_id), std::ios::binary);
    if (!in) throw ApiError(500, "FILE_IO");
    in.seekg(offset);
    std::string bytes(static_cast<size_t>(length), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(length));
    bytes.resize(static_cast<size_t>(in.gcount()));
    Json result = file_json(*info);
    result.o["offset"] = Json::integer(offset);
    result.o["data"] = Json::str(b64_encode(bytes));
    return result;
  }

  if (method == "stories.post") {
    int hours = static_cast<int>(require_int(params, "hours"));
    if (hours != 6 && hours != 12 && hours != 24 && hours != 48) throw ApiError(400, "BAD_TTL");
    std::string privacy = optional_string(params, "privacy").value_or("everybody");
    if (privacy != "everybody" && privacy != "contacts" && privacy != "close_friends" && privacy != "nobody")
      throw ApiError(400, "BAD_PRIVACY");
    std::string caption = optional_string(params, "caption").value_or("");
    check_plain(caption, 256, "BAD_CAPTION", true);
    int64_t file_id = optional_int(params, "file_id").value_or(0);
    if (file_id && !db_.can_read_file(me, file_id)) throw ApiError(404, "FILE_NOT_FOUND");
    bool no_screenshots = optional_bool(params, "no_screenshots").value_or(false);
    Story story = db_.add_story(me, file_id, caption, privacy, no_screenshots,
                                static_cast<int64_t>(std::time(nullptr)) + static_cast<int64_t>(hours) * 3600);
    return story_json(story);
  }

  if (method == "stories.get") {
    Json arr = Json::array();
    for (const Story &story : db_.list_stories(me)) arr.a.push_back(story_json(story));
    Json result = Json::object();
    result.o["stories"] = std::move(arr);
    return result;
  }

  if (method == "stories.view" || method == "stories.reportScreenshot") {
    int64_t story_id = require_int(params, "story_id");
    auto story = db_.story_by_id(story_id);
    if (!story || story->expires_at <= std::time(nullptr)) throw ApiError(404, "STORY_NOT_FOUND");
    if (!db_.can_see(me, story->user_id, story->privacy == "close_friends" ? "close_friends" : story->privacy))
      throw ApiError(403, "FORBIDDEN");
    if (method == "stories.view") {
      db_.view_story(story_id, me);
      return story_json(*db_.story_by_id(story_id));
    }
    if (!story->no_screenshots) return Json::object();
    Json data = Json::object();
    data.o["story_id"] = Json::integer(story_id);
    data.o["user_id"] = Json::integer(me);
    Json update = Json::object();
    update.o["update"] = Json::str("story.screenshot");
    update.o["data"] = data;
    hub_.push_user(story->user_id, update, &connection);
    return data;
  }

  if (method == "stickers.createPack") {
    std::string title = require_string(params, "title");
    std::string short_name = require_string(params, "short_name");
    check_plain(title, 64, "BAD_TITLE", false);
    check_plain(short_name, 32, "BAD_NAME", false);
    bool premium_pack = optional_bool(params, "premium").value_or(false);
    if (premium_pack && !db_.premium(me)) throw ApiError(403, "PREMIUM_REQUIRED");
    auto pack = db_.create_pack(me, title, short_name, premium_pack);
    if (!pack) throw ApiError(409, "NAME_TAKEN");
    Json result = Json::object();
    result.o["id"] = Json::integer(pack->id);
    result.o["short_name"] = Json::str(pack->short_name);
    result.o["premium"] = Json::boolean(pack->premium);
    return result;
  }

  if (method == "stickers.add") {
    int64_t pack_id = require_int(params, "pack_id");
    int64_t file_id = require_int(params, "file_id");
    std::string emoji = optional_string(params, "emoji").value_or("");
    auto pack = db_.pack_by_id(pack_id);
    if (!pack || pack->owner_id != me) throw ApiError(404, "PACK_NOT_FOUND");
    if (!db_.can_read_file(me, file_id)) throw ApiError(404, "FILE_NOT_FOUND");
    Sticker sticker = db_.add_sticker(pack_id, file_id, emoji);
    Json result = Json::object();
    result.o["id"] = Json::integer(sticker.id);
    result.o["file_id"] = Json::integer(sticker.file_id);
    return result;
  }

  if (method == "stickers.list") {
    Json arr = Json::array();
    for (const StickerPack &pack : db_.list_packs()) {
      if (pack.premium && !db_.premium(me)) continue;
      Json row = Json::object();
      row.o["id"] = Json::integer(pack.id);
      row.o["title"] = Json::str(pack.title);
      row.o["short_name"] = Json::str(pack.short_name);
      row.o["premium"] = Json::boolean(pack.premium);
      Json stickers = Json::array();
      for (const Sticker &sticker : db_.stickers_in(pack.id)) {
        Json item = Json::object();
        item.o["id"] = Json::integer(sticker.id);
        item.o["file_id"] = Json::integer(sticker.file_id);
        item.o["emoji"] = Json::str(sticker.emoji);
        stickers.a.push_back(std::move(item));
      }
      row.o["stickers"] = std::move(stickers);
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["packs"] = std::move(arr);
    return result;
  }

  if (method == "folders.create") {
    std::string title = require_string(params, "title");
    check_plain(title, 64, "BAD_TITLE", false);
    if (db_.folder_count(me) >= (db_.premium(me) ? 20 : 10)) throw ApiError(400, "TOO_MANY_FOLDERS");
    Folder folder = db_.create_folder(me, title);
    Json result = Json::object();
    result.o["id"] = Json::integer(folder.id);
    result.o["title"] = Json::str(folder.title);
    return result;
  }

  if (method == "folders.list") {
    Json arr = Json::array();
    for (const Folder &folder : db_.list_folders(me)) {
      Json row = Json::object();
      row.o["id"] = Json::integer(folder.id);
      row.o["title"] = Json::str(folder.title);
      Json ids = Json::array();
      for (int64_t id : folder.dialog_ids) ids.a.push_back(Json::integer(id));
      row.o["dialog_ids"] = std::move(ids);
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["folders"] = std::move(arr);
    return result;
  }

  if (method == "folders.add" || method == "folders.remove" || method == "folders.delete") {
    int64_t folder_id = require_int(params, "folder_id");
    bool mine = false;
    for (const Folder &folder : db_.list_folders(me)) {
      if (folder.id == folder_id) mine = true;
    }
    if (!mine) throw ApiError(404, "FOLDER_NOT_FOUND");
    if (method == "folders.delete") {
      db_.delete_folder(folder_id, me);
      return Json::object();
    }
    int64_t dialog_id = require_int(params, "dialog_id");
    require_dialog(dialog_id);
    if (method == "folders.add") db_.folder_add(folder_id, me, dialog_id);
    else db_.folder_remove(folder_id, me, dialog_id);
    return Json::object();
  }

  if (method == "saved.tabs" || method == "saved.createTab" || method == "saved.addToTab" ||
      method == "saved.tag" || method == "saved.tags") {
    int64_t saved = db_.saved_dialog(me);
    if (method == "saved.createTab") {
      std::string title = require_string(params, "title");
      check_plain(title, 64, "BAD_TITLE", false);
      SavedTab tab = db_.create_tab(me, title);
      Json result = Json::object();
      result.o["id"] = Json::integer(tab.id);
      result.o["title"] = Json::str(tab.title);
      result.o["dialog_id"] = Json::integer(saved);
      return result;
    }
    if (method == "saved.tabs") {
      Json arr = Json::array();
      for (const SavedTab &tab : db_.list_tabs(me)) {
        Json row = Json::object();
        row.o["id"] = Json::integer(tab.id);
        row.o["title"] = Json::str(tab.title);
        arr.a.push_back(std::move(row));
      }
      Json result = Json::object();
      result.o["tabs"] = std::move(arr);
      result.o["dialog_id"] = Json::integer(saved);
      return result;
    }
    int64_t message_id = require_int(params, "message_id");
    auto message = db_.message_by_id(message_id);
    if (!message || message->dialog_id != saved || message->deleted) throw ApiError(404, "MESSAGE_NOT_FOUND");
    if (method == "saved.tag" || method == "saved.tags") {
      if (!db_.premium(me)) throw ApiError(403, "PREMIUM_REQUIRED");
      if (method == "saved.tags") {
        Json arr = Json::array();
        for (const std::string &tag : db_.tags_for(me, message_id)) arr.a.push_back(Json::str(tag));
        Json result = Json::object();
        result.o["tags"] = std::move(arr);
        return result;
      }
      std::string tag = require_string(params, "tag");
      check_plain(tag, 32, "BAD_TAG", false);
      db_.add_tag(me, message_id, tag);
      return Json::object();
    }
    int64_t tab_id = require_int(params, "tab_id");
    db_.tab_add(tab_id, me, message_id);
    return Json::object();
  }

  if (method == "calls.start" || method == "calls.join" || method == "calls.leave" ||
      method == "calls.setState" || method == "calls.record") {
    int64_t call_id = 0;
    CallInfo call;
    if (method == "calls.start") {
      int64_t dialog_id = require_int(params, "dialog_id");
      require_dialog(dialog_id);
      call = db_.start_call(dialog_id, me);
      call_id = call.id;
    } else {
      call_id = require_int(params, "call_id");
      auto loaded = db_.call_by_id(call_id);
      if (!loaded || !loaded->active) throw ApiError(404, "CALL_NOT_FOUND");
      call = *loaded;
      require_dialog(call.dialog_id);
    }
    if (method == "calls.join" || method == "calls.start") db_.join_call(call_id, me);
    if (method == "calls.leave") db_.leave_call(call_id, me);
    if (method == "calls.setState") {
      db_.set_call_state(call_id, me, optional_bool(params, "muted").value_or(false),
                         optional_bool(params, "video").value_or(false),
                         optional_bool(params, "screen").value_or(false));
    }
    if (method == "calls.record") {
      bool recording = optional_bool(params, "recording").value_or(true);
      db_.set_call_recording(call_id, recording);
      if (!recording) {
        int64_t saved = db_.saved_dialog(me);
        Outgoing draft;
        draft.kind = "file";
        draft.text = "запись эфира";
        if (auto file_id = optional_int(params, "file_id")) {
          if (!db_.can_read_file(me, *file_id)) throw ApiError(404, "FILE_NOT_FOUND");
          draft.file_id = *file_id;
        } else {
          draft.kind = "text";
        }
        Message note = db_.add_message(saved, me, draft);
        push_message(note, nullptr);
      }
    }
    Json peers = Json::array();
    for (const CallPeer &peer : db_.call_peers(call_id)) {
      Json row = Json::object();
      row.o["user_id"] = Json::integer(peer.user_id);
      row.o["muted"] = Json::boolean(peer.muted);
      row.o["video"] = Json::boolean(peer.video);
      row.o["screen"] = Json::boolean(peer.screen);
      peers.a.push_back(std::move(row));
    }
    auto fresh = db_.call_by_id(call_id);
    Json data = Json::object();
    data.o["id"] = Json::integer(call_id);
    data.o["dialog_id"] = Json::integer(call.dialog_id);
    data.o["active"] = Json::boolean(fresh && fresh->active);
    data.o["recording"] = Json::boolean(fresh && fresh->recording);
    data.o["peers"] = peers;
    Json update = Json::object();
    update.o["update"] = Json::str("call.update");
    update.o["data"] = data;
    hub_.push_many(db_.member_ids(call.dialog_id), update, &connection);
    return data;
  }

  if (method == "bots.create") {
    std::string username = require_string(params, "username");
    std::string display = require_string(params, "display_name");
    check_plain(display, 64, "BAD_DISPLAY_NAME", false);
    if (username.size() < 3 || username.size() > 32) throw ApiError(400, "BAD_USERNAME");
    std::string token = random_token();
    auto bot = db_.create_bot(me, username, display, token);
    if (!bot) throw ApiError(409, "USERNAME_TAKEN");
    Json result = user_json(*bot);
    result.o["token"] = Json::str(token);
    return result;
  }

  if (method == "bots.getUpdates") {
    auto prof = db_.profile(me);
    if (!prof || !prof->is_bot) throw ApiError(403, "NOT_A_BOT");
    int limit = 50;
    if (auto raw = optional_int(params, "limit")) {
      if (*raw < 1 || *raw > 100) throw ApiError(400, "BAD_LIMIT");
      limit = static_cast<int>(*raw);
    }
    Json arr = Json::array();
    for (const std::string &payload : db_.bot_updates(me, limit)) {
      try {
        arr.a.push_back(parse_json(payload));
      } catch (const JsonError &) {
      }
    }
    Json result = Json::object();
    result.o["updates"] = std::move(arr);
    return result;
  }

  if (method == "apps.create") {
    std::string title = require_string(params, "title");
    std::string short_name = require_string(params, "short_name");
    std::string url = require_string(params, "url");
    check_plain(title, 64, "BAD_TITLE", false);
    check_plain(short_name, 32, "BAD_NAME", false);
    if (url.size() > 512 || (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0))
      throw ApiError(400, "BAD_URL");
    auto app = db_.create_app(me, title, short_name, url);
    if (!app) throw ApiError(409, "NAME_TAKEN");
    Json result = Json::object();
    result.o["id"] = Json::integer(app->id);
    result.o["short_name"] = Json::str(app->short_name);
    result.o["url"] = Json::str(app->url);
    return result;
  }

  if (method == "apps.get" || method == "apps.list") {
    if (method == "apps.get") {
      auto app = db_.app_by_name(require_string(params, "short_name"));
      if (!app) throw ApiError(404, "APP_NOT_FOUND");
      Json result = Json::object();
      result.o["id"] = Json::integer(app->id);
      result.o["title"] = Json::str(app->title);
      result.o["short_name"] = Json::str(app->short_name);
      result.o["url"] = Json::str(app->url);
      return result;
    }
    Json arr = Json::array();
    for (const MiniApp &app : db_.list_apps()) {
      Json row = Json::object();
      row.o["id"] = Json::integer(app.id);
      row.o["title"] = Json::str(app.title);
      row.o["short_name"] = Json::str(app.short_name);
      row.o["url"] = Json::str(app.url);
      arr.a.push_back(std::move(row));
    }
    Json result = Json::object();
    result.o["apps"] = std::move(arr);
    return result;
  }

  if (method == "payments.createInvoice" || method == "payments.confirm" || method == "payments.cancel" ||
      method == "payments.list" || method == "payments.get") {
    reject_card_fields(params);
    if (method == "payments.list") {
      Json arr = Json::array();
      for (const Invoice &invoice : db_.list_invoices(me)) arr.a.push_back(invoice_json(invoice));
      Json result = Json::object();
      result.o["invoices"] = std::move(arr);
      return result;
    }
    if (method == "payments.get") {
      auto invoice = db_.invoice_by_id(require_int(params, "invoice_id"));
      if (!invoice || (invoice->user_id != me && invoice->bot_id != me)) throw ApiError(404, "INVOICE_NOT_FOUND");
      return invoice_json(*invoice);
    }
    auto prof = db_.profile(me);
    if (!prof || !prof->is_bot) throw ApiError(403, "NOT_A_BOT");
    if (method == "payments.createInvoice") {
      std::string title = require_string(params, "title");
      std::string currency = require_string(params, "currency");
      std::string purpose = optional_string(params, "purpose").value_or("");
      int64_t amount = require_int(params, "amount");
      int64_t user_id = require_int(params, "user_id");
      check_plain(title, 128, "BAD_TITLE", false);
      if (currency.size() != 3) throw ApiError(400, "BAD_CURRENCY");
      for (char c : currency) {
        if (c < 'A' || c > 'Z') throw ApiError(400, "BAD_CURRENCY");
      }
      if (amount < 1 || purpose.size() > 32) throw ApiError(400, "BAD_AMOUNT");
      if (!db_.user_by_id(user_id)) throw ApiError(404, "USER_NOT_FOUND");
      return invoice_json(db_.create_invoice(me, user_id, title, amount, currency, purpose));
    }
    int64_t invoice_id = require_int(params, "invoice_id");
    std::string status = method == "payments.confirm" ? "paid" : "cancelled";
    std::string ref = optional_string(params, "provider_ref").value_or("");
    if (ref.size() > 128) throw ApiError(400, "BAD_REF");
    auto before = db_.invoice_by_id(invoice_id);
    if (!before || before->bot_id != me) throw ApiError(404, "INVOICE_NOT_FOUND");
    if (!db_.set_invoice_status(invoice_id, me, status, ref)) throw ApiError(409, "INVOICE_CLOSED");
    if (status == "paid" && before->purpose == "premium") db_.grant_premium_days(before->user_id, 30);
    return invoice_json(*db_.invoice_by_id(invoice_id));
  }

  (void)kMaxGroup;
  throw ApiError(404, "UNKNOWN_METHOD");
}

}
