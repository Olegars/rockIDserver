#include "rockid/database.hpp"

#include "pg_query.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <stdexcept>

namespace rockid {
namespace {

std::string flag(bool value) { return value ? "true" : "false"; }

void hash_secret(const std::string &password, unsigned char salt[16], unsigned char out[32]) {
  if (RAND_bytes(salt, 16) != 1) throw std::runtime_error("RAND_bytes failed");
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, 16, kPbkdfIters,
                        EVP_sha256(), 32, out) != 1)
    throw std::runtime_error("PBKDF2 failed");
}

bool match_secret(const std::string &password, const std::string &salt, const std::string &hash) {
  unsigned char out[32];
  if (salt.empty() || hash.size() != sizeof out) return false;
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        reinterpret_cast<const unsigned char *>(salt.data()),
                        static_cast<int>(salt.size()), kPbkdfIters, EVP_sha256(), sizeof out,
                        out) != 1)
    throw std::runtime_error("PBKDF2 failed");
  return CRYPTO_memcmp(out, hash.data(), sizeof out) == 0;
}

Profile read_profile(Query &q) {
  Profile profile;
  profile.id = q.i64(0);
  profile.username = q.text(1);
  profile.display_name = q.text(2);
  profile.phone = q.text(3);
  profile.bio = q.text(4);
  profile.photo_file_id = q.i64(5);
  profile.premium_until = q.i64(6);
  profile.privacy_phone = q.text(7);
  profile.privacy_last_seen = q.text(8);
  profile.privacy_photo = q.text(9);
  profile.is_bot = pg_flag(q.text(10));
  profile.last_seen_at = q.i64(11);
  profile.preferences = q.text(12);
  profile.cloud_password = pg_flag(q.text(13));
  return profile;
}

constexpr const char *kProfileCols =
    "id, username, display_name, phone, bio, COALESCE(photo_file_id,0), premium_until, "
    "privacy_phone, privacy_last_seen, privacy_photo, is_bot::int, last_seen_at, preferences, "
    "(cloud_hash IS NOT NULL)::int";

Invoice read_invoice(Query &q) {
  Invoice invoice;
  invoice.id = q.i64(0);
  invoice.bot_id = q.i64(1);
  invoice.user_id = q.i64(2);
  invoice.title = q.text(3);
  invoice.amount = q.i64(4);
  invoice.currency = q.text(5);
  invoice.purpose = q.text(6);
  invoice.status = q.text(7);
  invoice.provider_ref = q.text(8);
  return invoice;
}

}  // namespace

void Database::delete_session_id(int64_t user_id, int64_t session_id) {
  Query q(db_, "DELETE FROM sessions WHERE user_id = $1 AND id = $2");
  q.bind(1, user_id);
  q.bind(2, session_id);
  q.exec();
}

std::vector<SessionInfo> Database::list_sessions(int64_t user_id, const std::string &current_token) {
  std::string dig = sha256_bytes(current_token);
  Query q(db_,
          "SELECT id, device, platform, local_lock, created_at, last_seen_at, token_hash "
          "FROM sessions WHERE user_id = $1 ORDER BY id");
  q.bind(1, user_id);
  q.exec();
  std::vector<SessionInfo> rows;
  while (q.next()) {
    SessionInfo info;
    info.id = q.i64(0);
    info.device = q.text(1);
    info.platform = q.text(2);
    info.local_lock = q.text(3);
    info.created_at = q.i64(4);
    info.last_seen_at = q.i64(5);
    info.token = q.blob(6);
    info.current = info.token == dig;
    rows.push_back(std::move(info));
  }
  return rows;
}

void Database::set_local_lock(const std::string &token, const std::string &kind) {
  std::string dig = sha256_bytes(token);
  Query q(db_, "UPDATE sessions SET local_lock = $2 WHERE token_hash = $1");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  q.bind(2, kind);
  q.exec();
}

std::optional<std::string> Database::token_by_session(int64_t user_id, int64_t session_id) {
  Query q(db_, "SELECT token_hash FROM sessions WHERE user_id = $1 AND id = $2");
  q.bind(1, user_id);
  q.bind(2, session_id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return q.blob(0);
}

bool Database::is_contact(int64_t owner, int64_t peer) {
  Query q(db_, "SELECT 1 FROM contacts WHERE owner_id = $1 AND peer_id = $2");
  q.bind(1, owner);
  q.bind(2, peer);
  q.exec();
  return q.next();
}

bool Database::can_see(int64_t viewer, int64_t owner, const std::string &level) {
  if (viewer == owner) return true;
  if (level == "nobody") return false;
  if (level == "contacts") return is_contact(owner, viewer);
  if (level == "close_friends") return is_close_friend(owner, viewer);
  return true;
}

void Database::touch_last_seen(int64_t user_id) {
  Query q(db_, "UPDATE users SET last_seen_at = $2 WHERE id = $1");
  q.bind(1, user_id);
  q.bind(2, now_s());
  q.exec();
}

std::optional<Profile> Database::profile(int64_t id) {
  Query q(db_, std::string("SELECT ") + kProfileCols + " FROM users WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return read_profile(q);
}

void Database::update_profile(int64_t id, const std::string &display_name, const std::string &phone,
                              const std::string &bio, int64_t photo_file_id, bool set_photo) {
  if (set_photo) {
    Query q(db_,
            "UPDATE users SET display_name = $2, phone = $3, bio = $4, photo_file_id = NULLIF($5,0) "
            "WHERE id = $1");
    q.bind(1, id);
    q.bind(2, display_name);
    q.bind(3, phone);
    q.bind(4, bio);
    q.bind(5, photo_file_id);
    q.exec();
    return;
  }
  Query q(db_, "UPDATE users SET display_name = $2, phone = $3, bio = $4 WHERE id = $1");
  q.bind(1, id);
  q.bind(2, display_name);
  q.bind(3, phone);
  q.bind(4, bio);
  q.exec();
}

void Database::set_privacy(int64_t id, const std::string &phone, const std::string &last_seen,
                           const std::string &photo) {
  Query q(db_,
          "UPDATE users SET privacy_phone = $2, privacy_last_seen = $3, privacy_photo = $4 WHERE id = $1");
  q.bind(1, id);
  q.bind(2, phone);
  q.bind(3, last_seen);
  q.bind(4, photo);
  q.exec();
}

void Database::set_preferences(int64_t id, const std::string &preferences) {
  Query q(db_, "UPDATE users SET preferences = $2 WHERE id = $1");
  q.bind(1, id);
  q.bind(2, preferences);
  q.exec();
}

bool Database::premium(int64_t id) {
  Query q(db_, "SELECT premium_until FROM users WHERE id = $1");
  q.bind(1, id);
  q.exec();
  return q.next() && q.i64(0) > now_s();
}

void Database::grant_premium_days(int64_t id, int days) {
  int64_t now = now_s();
  Query q(db_,
          "UPDATE users SET premium_until = GREATEST(premium_until, $2) + $3 WHERE id = $1");
  q.bind(1, id);
  q.bind(2, now);
  q.bind(3, static_cast<int64_t>(days) * 86400);
  q.exec();
}

int64_t Database::channel_subscriptions(int64_t user_id) {
  Query q(db_,
          "SELECT COUNT(*) FROM members m JOIN dialogs d ON d.id = m.dialog_id "
          "WHERE m.user_id = $1 AND d.kind = 'channel' AND m.banned = FALSE");
  q.bind(1, user_id);
  q.exec();
  return q.next() ? q.i64(0) : 0;
}

int64_t Database::folder_count(int64_t user_id) {
  Query q(db_, "SELECT COUNT(*) FROM folders WHERE user_id = $1");
  q.bind(1, user_id);
  q.exec();
  return q.next() ? q.i64(0) : 0;
}

int64_t Database::pin_count(int64_t user_id) {
  Query q(db_, "SELECT COUNT(*) FROM members WHERE user_id = $1 AND pin_pos > 0 AND banned = FALSE");
  q.bind(1, user_id);
  q.exec();
  return q.next() ? q.i64(0) : 0;
}

void Database::set_cloud_password(int64_t id, const std::string &password) {
  unsigned char salt[16];
  unsigned char hash[32];
  hash_secret(password, salt, hash);
  Query q(db_, "UPDATE users SET cloud_salt = $2, cloud_hash = $3 WHERE id = $1");
  q.bind(1, id);
  q.bind_blob(2, salt, 16);
  q.bind_blob(3, hash, 32);
  q.exec();
}

void Database::clear_cloud_password(int64_t id, const std::string &password) {
  if (!check_cloud_password(id, password)) return;
  Query q(db_, "UPDATE users SET cloud_salt = NULL, cloud_hash = NULL WHERE id = $1");
  q.bind(1, id);
  q.exec();
}

bool Database::has_cloud_password(int64_t id) {
  Query q(db_, "SELECT cloud_hash IS NOT NULL FROM users WHERE id = $1");
  q.bind(1, id);
  q.exec();
  return q.next() && pg_flag(q.text(0));
}

bool Database::check_cloud_password(int64_t id, const std::string &password) {
  Query q(db_, "SELECT cloud_salt, cloud_hash FROM users WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return false;
  return match_secret(password, q.blob(0), q.blob(1));
}

std::string Database::create_challenge(int64_t user_id) {
  unsigned char raw[32];
  if (RAND_bytes(raw, sizeof raw) != 1) throw std::runtime_error("RAND_bytes failed");
  std::string token = to_hex(raw, sizeof raw);
  std::string dig = sha256_bytes(token);
  Query q(db_, "INSERT INTO login_challenges(token_hash, user_id, expires_at) VALUES($1,$2,$3)");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  q.bind(2, user_id);
  q.bind(3, now_s() + 300);
  q.exec();
  return token;
}

std::optional<int64_t> Database::take_challenge(const std::string &token) {
  std::string dig = sha256_bytes(token);
  int64_t now = now_s();
  Query q(db_,
          "DELETE FROM login_challenges WHERE token_hash = $1 AND expires_at >= $2 RETURNING user_id");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  q.bind(2, now);
  q.exec();
  if (!q.next()) return std::nullopt;
  return q.i64(0);
}

int64_t Database::create_channel(int64_t owner, const std::string &title) {
  Tx tx(*this);
  int64_t now = now_s();
  Query ins(db_,
            "INSERT INTO dialogs(kind, title, peer_key, created_at) VALUES('channel', $1, NULL, $2) "
            "RETURNING id");
  ins.bind(1, title);
  ins.bind(2, now);
  ins.exec();
  if (!ins.next()) throw std::runtime_error("channel insert returned no id");
  int64_t id = ins.i64(0);
  Query mem(db_,
            "INSERT INTO members(dialog_id, user_id, role, joined_at, can_post, can_delete, can_ban, "
            "can_pin, can_manage) VALUES($1,$2,'owner',$3,TRUE,TRUE,TRUE,TRUE,TRUE)");
  mem.bind(1, id);
  mem.bind(2, owner);
  mem.bind(3, now);
  mem.exec();
  tx.commit();
  return id;
}

int64_t Database::create_secret(int64_t a, int64_t b, int ttl_seconds) {
  int64_t lo = std::min(a, b);
  int64_t hi = std::max(a, b);
  std::string key = "s:" + std::to_string(lo) + ":" + std::to_string(hi);
  Tx tx(*this);
  {
    Query q(db_, "SELECT id FROM dialogs WHERE peer_key = $1");
    q.bind(1, key);
    q.exec();
    if (q.next()) {
      int64_t id = q.i64(0);
      Query upd(db_, "UPDATE dialogs SET ttl_seconds = $2 WHERE id = $1");
      upd.bind(1, id);
      upd.bind(2, static_cast<int64_t>(ttl_seconds));
      upd.exec();
      tx.commit();
      return id;
    }
  }
  int64_t now = now_s();
  Query ins(db_,
            "INSERT INTO dialogs(kind, title, peer_key, created_at, no_forwards, ttl_seconds) "
            "VALUES('secret', '', $1, $2, TRUE, $3) RETURNING id");
  ins.bind(1, key);
  ins.bind(2, now);
  ins.bind(3, static_cast<int64_t>(ttl_seconds));
  ins.exec();
  if (!ins.next()) throw std::runtime_error("secret insert returned no id");
  int64_t id = ins.i64(0);
  for (int64_t user_id : {a, b}) {
    Query mem(db_,
              "INSERT INTO members(dialog_id, user_id, role, joined_at) VALUES($1,$2,'member',$3)");
    mem.bind(1, id);
    mem.bind(2, user_id);
    mem.bind(3, now);
    mem.exec();
  }
  tx.commit();
  return id;
}

int64_t Database::saved_dialog(int64_t user_id) {
  std::string key = "saved:" + std::to_string(user_id);
  Tx tx(*this);
  {
    Query q(db_, "SELECT id FROM dialogs WHERE peer_key = $1");
    q.bind(1, key);
    q.exec();
    if (q.next()) {
      int64_t id = q.i64(0);
      tx.commit();
      return id;
    }
  }
  int64_t now = now_s();
  Query ins(db_,
            "INSERT INTO dialogs(kind, title, peer_key, created_at) "
            "VALUES('saved', 'Избранное', $1, $2) RETURNING id");
  ins.bind(1, key);
  ins.bind(2, now);
  ins.exec();
  if (!ins.next()) throw std::runtime_error("saved insert returned no id");
  int64_t id = ins.i64(0);
  Query mem(db_,
            "INSERT INTO members(dialog_id, user_id, role, joined_at) VALUES($1,$2,'owner',$3)");
  mem.bind(1, id);
  mem.bind(2, user_id);
  mem.bind(3, now);
  mem.exec();
  tx.commit();
  return id;
}

bool Database::add_subscriber(int64_t dialog_id, int64_t user_id) {
  Query existing(db_, "SELECT banned::int FROM members WHERE dialog_id = $1 AND user_id = $2");
  existing.bind(1, dialog_id);
  existing.bind(2, user_id);
  existing.exec();
  if (existing.next()) return false;
  Query q(db_,
          "INSERT INTO members(dialog_id, user_id, role, joined_at, can_post) "
          "VALUES($1,$2,'subscriber',$3,FALSE) ON CONFLICT DO NOTHING");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.bind(3, now_s());
  q.exec();
  return q.affected() > 0;
}

int64_t Database::member_count(int64_t dialog_id) {
  Query q(db_, "SELECT COUNT(*) FROM members WHERE dialog_id = $1 AND banned = FALSE");
  q.bind(1, dialog_id);
  q.exec();
  return q.next() ? q.i64(0) : 0;
}

bool Database::set_member_admin(int64_t dialog_id, int64_t user_id, bool can_delete, bool can_ban,
                                bool can_pin, bool can_manage, bool can_post) {
  Query q(db_,
          "UPDATE members SET role = 'admin', can_delete = $3::boolean, can_ban = $4::boolean, "
          "can_pin = $5::boolean, can_manage = $6::boolean, can_post = $7::boolean "
          "WHERE dialog_id = $1 AND user_id = $2 AND role <> 'owner' AND banned = FALSE");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.bind(3, flag(can_delete));
  q.bind(4, flag(can_ban));
  q.bind(5, flag(can_pin));
  q.bind(6, flag(can_manage));
  q.bind(7, flag(can_post));
  q.exec();
  return q.affected() > 0;
}

bool Database::kick_member(int64_t dialog_id, int64_t user_id) {
  Query q(db_, "DELETE FROM members WHERE dialog_id = $1 AND user_id = $2 AND role <> 'owner'");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.exec();
  return q.affected() > 0;
}

bool Database::set_banned(int64_t dialog_id, int64_t user_id, bool banned) {
  Query q(db_,
          "UPDATE members SET banned = $3::boolean, can_post = FALSE "
          "WHERE dialog_id = $1 AND user_id = $2 AND role <> 'owner'");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.bind(3, flag(banned));
  q.exec();
  return q.affected() > 0;
}

void Database::set_dialog_flags(int64_t dialog_id, int64_t user_id, std::optional<bool> archived,
                                std::optional<int64_t> muted_until, std::optional<int> pin_pos) {
  if (archived) {
    Query q(db_, "UPDATE members SET archived = $3::boolean WHERE dialog_id = $1 AND user_id = $2");
    q.bind(1, dialog_id);
    q.bind(2, user_id);
    q.bind(3, flag(*archived));
    q.exec();
  }
  if (muted_until) {
    Query q(db_, "UPDATE members SET muted_until = $3 WHERE dialog_id = $1 AND user_id = $2");
    q.bind(1, dialog_id);
    q.bind(2, user_id);
    q.bind(3, *muted_until);
    q.exec();
  }
  if (pin_pos) {
    Query q(db_, "UPDATE members SET pin_pos = $3 WHERE dialog_id = $1 AND user_id = $2");
    q.bind(1, dialog_id);
    q.bind(2, user_id);
    q.bind(3, static_cast<int64_t>(*pin_pos));
    q.exec();
  }
}

void Database::set_secret_key(int64_t dialog_id, int64_t user_id, const std::string &public_key) {
  Query q(db_, "UPDATE members SET public_key = $3 WHERE dialog_id = $1 AND user_id = $2");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.bind(3, public_key);
  q.exec();
}

std::vector<std::pair<int64_t, std::string>> Database::secret_keys(int64_t dialog_id) {
  Query q(db_, "SELECT user_id, public_key FROM members WHERE dialog_id = $1 ORDER BY user_id");
  q.bind(1, dialog_id);
  q.exec();
  std::vector<std::pair<int64_t, std::string>> rows;
  while (q.next()) rows.push_back({q.i64(0), q.text(1)});
  return rows;
}

void Database::set_discussion(int64_t channel_id, int64_t group_id) {
  Query q(db_, "UPDATE dialogs SET linked_id = NULLIF($2,0) WHERE id = $1");
  q.bind(1, channel_id);
  q.bind(2, group_id);
  q.exec();
}

std::optional<Membership> Database::membership(int64_t dialog_id, int64_t user_id) {
  Query q(db_,
          "SELECT role, can_post::int, can_delete::int, can_ban::int, can_pin::int, can_manage::int, "
          "banned::int FROM members WHERE dialog_id = $1 AND user_id = $2");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.exec();
  if (!q.next() || pg_flag(q.text(6))) return std::nullopt;
  Membership row;
  row.role = q.text(0);
  bool owner = row.role == "owner";
  row.can_post = owner || pg_flag(q.text(1));
  row.can_delete = owner || pg_flag(q.text(2));
  row.can_ban = owner || pg_flag(q.text(3));
  row.can_pin = owner || pg_flag(q.text(4));
  row.can_manage = owner || pg_flag(q.text(5));
  return row;
}

std::vector<int64_t> Database::presence_recipients(int64_t user_id) {
  auto prof = profile(user_id);
  std::string level = prof ? prof->privacy_last_seen : "everybody";
  std::vector<int64_t> out;
  for (int64_t peer : peer_user_ids(user_id)) {
    if (can_see(peer, user_id, level)) out.push_back(peer);
  }
  return out;
}

int64_t Database::create_topic(int64_t dialog_id, int64_t user_id, const std::string &title) {
  Query q(db_,
          "INSERT INTO topics(dialog_id, title, created_by, created_at) VALUES($1,$2,$3,$4) RETURNING id");
  q.bind(1, dialog_id);
  q.bind(2, title);
  q.bind(3, user_id);
  q.bind(4, now_s());
  q.exec();
  if (!q.next()) throw std::runtime_error("topic insert returned no id");
  return q.i64(0);
}

std::vector<Topic> Database::list_topics(int64_t dialog_id) {
  Query q(db_, "SELECT id, dialog_id, title, created_by FROM topics WHERE dialog_id = $1 ORDER BY id");
  q.bind(1, dialog_id);
  q.exec();
  std::vector<Topic> rows;
  while (q.next()) rows.push_back(Topic{q.i64(0), q.i64(1), q.text(2), q.i64(3)});
  return rows;
}

bool Database::topic_in_dialog(int64_t dialog_id, int64_t topic_id) {
  Query q(db_, "SELECT 1 FROM topics WHERE dialog_id = $1 AND id = $2");
  q.bind(1, dialog_id);
  q.bind(2, topic_id);
  q.exec();
  return q.next();
}

std::optional<Message> Database::message_by_id(int64_t id) {
  Query q(db_, std::string("SELECT ") + kMessageCols + " FROM messages WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return read_message(q);
}

Message Database::edit_message(int64_t id, const std::string &text, const std::string &entities) {
  Query q(db_, std::string("UPDATE messages SET body = $2, entities = $3, edited_at = $4 "
                           "WHERE id = $1 AND deleted = FALSE RETURNING ") +
                     kMessageCols);
  q.bind(1, id);
  q.bind(2, text);
  q.bind(3, entities);
  q.bind(4, now_s());
  q.exec();
  if (!q.next()) throw std::runtime_error("edit missed");
  return read_message(q);
}

void Database::delete_message(int64_t id) {
  Query q(db_, "UPDATE messages SET deleted = TRUE, body = '' WHERE id = $1");
  q.bind(1, id);
  q.exec();
}

void Database::add_views(const std::vector<int64_t> &ids) {
  for (int64_t id : ids) {
    Query q(db_, "UPDATE messages SET views = views + 1 WHERE id = $1 AND deleted = FALSE");
    q.bind(1, id);
    q.exec();
  }
}

bool Database::toggle_reaction(int64_t message_id, int64_t user_id, const std::string &emoji) {
  Query del(db_, "DELETE FROM reactions WHERE message_id = $1 AND user_id = $2 AND emoji = $3");
  del.bind(1, message_id);
  del.bind(2, user_id);
  del.bind(3, emoji);
  del.exec();
  if (del.affected() > 0) return false;
  Query ins(db_,
            "INSERT INTO reactions(message_id, user_id, emoji, created_at) VALUES($1,$2,$3,$4)");
  ins.bind(1, message_id);
  ins.bind(2, user_id);
  ins.bind(3, emoji);
  ins.bind(4, now_s());
  ins.exec();
  return true;
}

void Database::set_translation(int64_t id, const std::string &lang, const std::string &text) {
  Query q(db_, "UPDATE messages SET translation_lang = $2, translation = $3 WHERE id = $1");
  q.bind(1, id);
  q.bind(2, lang);
  q.bind(3, text);
  q.exec();
}

void Database::set_transcript(int64_t id, const std::string &text) {
  Query q(db_, "UPDATE messages SET transcript = $2 WHERE id = $1");
  q.bind(1, id);
  q.bind(2, text);
  q.exec();
}

DueBatch Database::take_due(int64_t now) {
  DueBatch batch;
  {
    Query q(db_, std::string("UPDATE messages SET sent_at = $1 WHERE scheduled_at > 0 AND scheduled_at <= $1 "
                             "AND sent_at = 0 AND deleted = FALSE AND (expires_at = 0 OR expires_at > $1) "
                             "RETURNING ") +
                       kMessageCols);
    q.bind(1, now);
    q.exec();
    while (q.next()) batch.ready.push_back(read_message(q));
  }
  {
    Query q(db_, std::string("UPDATE messages SET deleted = TRUE, body = '' "
                             "WHERE deleted = FALSE AND expires_at > 0 AND expires_at <= $1 RETURNING ") +
                       kMessageCols);
    q.bind(1, now);
    q.exec();
    while (q.next()) batch.expired.push_back(read_message(q));
  }
  return batch;
}

void Database::enqueue_bot_updates(int64_t dialog_id, int64_t sender_id, const std::string &payload) {
  Query bots(db_,
             "SELECT m.user_id FROM members m JOIN users u ON u.id = m.user_id "
             "WHERE m.dialog_id = $1 AND u.is_bot = TRUE AND m.user_id <> $2 AND m.banned = FALSE");
  bots.bind(1, dialog_id);
  bots.bind(2, sender_id);
  bots.exec();
  std::vector<int64_t> ids;
  while (bots.next()) ids.push_back(bots.i64(0));
  int64_t now = now_s();
  for (int64_t bot_id : ids) {
    Query ins(db_, "INSERT INTO bot_updates(bot_id, payload, created_at) VALUES($1,$2,$3)");
    ins.bind(1, bot_id);
    ins.bind(2, payload);
    ins.bind(3, now);
    ins.exec();
  }
}

std::vector<std::string> Database::bot_updates(int64_t bot_id, int limit) {
  Query q(db_,
          "WITH picked AS (SELECT id, payload FROM bot_updates WHERE bot_id = $1 ORDER BY id LIMIT $2) "
          "DELETE FROM bot_updates b USING picked p WHERE b.id = p.id RETURNING p.id, p.payload");
  q.bind(1, bot_id);
  q.bind(2, static_cast<int64_t>(limit));
  q.exec();
  std::vector<std::pair<int64_t, std::string>> rows;
  while (q.next()) rows.push_back({q.i64(0), q.text(1)});
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<std::string> out;
  for (const auto &row : rows) out.push_back(row.second);
  return out;
}

FileInfo Database::create_file(int64_t owner, const std::string &name, const std::string &mime, int64_t size) {
  Query q(db_,
          "INSERT INTO files(owner_id, name, mime, size, created_at) VALUES($1,$2,$3,$4,$5) "
          "RETURNING id, owner_id, name, mime, size, received, ready::int");
  q.bind(1, owner);
  q.bind(2, name);
  q.bind(3, mime);
  q.bind(4, size);
  q.bind(5, now_s());
  q.exec();
  if (!q.next()) throw std::runtime_error("file insert returned no id");
  FileInfo info;
  info.id = q.i64(0);
  info.owner_id = q.i64(1);
  info.name = q.text(2);
  info.mime = q.text(3);
  info.size = q.i64(4);
  info.received = q.i64(5);
  info.ready = pg_flag(q.text(6));
  return info;
}

std::optional<FileInfo> Database::file_info(int64_t id) {
  Query q(db_, "SELECT id, owner_id, name, mime, size, received, ready::int FROM files WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  FileInfo info;
  info.id = q.i64(0);
  info.owner_id = q.i64(1);
  info.name = q.text(2);
  info.mime = q.text(3);
  info.size = q.i64(4);
  info.received = q.i64(5);
  info.ready = pg_flag(q.text(6));
  return info;
}

void Database::file_received(int64_t id, int64_t received, bool ready) {
  Query q(db_, "UPDATE files SET received = $2, ready = $3::boolean WHERE id = $1");
  q.bind(1, id);
  q.bind(2, received);
  q.bind(3, flag(ready));
  q.exec();
}

bool Database::can_read_file(int64_t user_id, int64_t file_id) {
  Query q(db_,
          "SELECT 1 FROM files f WHERE f.id = $1 AND ("
          " f.owner_id = $2"
          " OR EXISTS (SELECT 1 FROM messages m JOIN members mem ON mem.dialog_id = m.dialog_id"
          "   WHERE m.file_id = f.id AND mem.user_id = $2 AND mem.banned = FALSE AND m.deleted = FALSE)"
          " OR EXISTS (SELECT 1 FROM stickers s WHERE s.file_id = f.id)"
          " OR EXISTS (SELECT 1 FROM users u WHERE u.photo_file_id = f.id AND u.id = $2)"
          " OR EXISTS (SELECT 1 FROM stories st WHERE st.file_id = f.id AND st.expires_at > $3 AND ("
          "   st.user_id = $2 OR st.privacy = 'everybody'"
          "   OR (st.privacy = 'contacts' AND EXISTS (SELECT 1 FROM contacts c WHERE c.owner_id = st.user_id AND c.peer_id = $2))"
          "   OR (st.privacy = 'close_friends' AND EXISTS (SELECT 1 FROM close_friends cf WHERE cf.owner_id = st.user_id AND cf.peer_id = $2))"
          " )))");
  q.bind(1, file_id);
  q.bind(2, user_id);
  q.bind(3, now_s());
  q.exec();
  return q.next();
}

Story Database::add_story(int64_t user_id, int64_t file_id, const std::string &caption,
                          const std::string &privacy, bool no_screenshots, int64_t expires_at) {
  Query q(db_,
          "INSERT INTO stories(user_id, file_id, caption, privacy, no_screenshots, expires_at, created_at) "
          "VALUES($1, NULLIF($2,0), $3, $4, $5::boolean, $6, $7) "
          "RETURNING id, user_id, COALESCE(file_id,0), caption, privacy, no_screenshots::int, expires_at, created_at");
  int64_t now = now_s();
  q.bind(1, user_id);
  q.bind(2, file_id);
  q.bind(3, caption);
  q.bind(4, privacy);
  q.bind(5, flag(no_screenshots));
  q.bind(6, expires_at);
  q.bind(7, now);
  q.exec();
  if (!q.next()) throw std::runtime_error("story insert returned no id");
  Story story;
  story.id = q.i64(0);
  story.user_id = q.i64(1);
  story.file_id = q.i64(2);
  story.caption = q.text(3);
  story.privacy = q.text(4);
  story.no_screenshots = pg_flag(q.text(5));
  story.expires_at = q.i64(6);
  story.created_at = q.i64(7);
  return story;
}

std::vector<Story> Database::list_stories(int64_t viewer) {
  Query q(db_,
          "SELECT s.id, s.user_id, COALESCE(s.file_id,0), s.caption, s.privacy, s.no_screenshots::int, "
          "s.expires_at, s.created_at, (SELECT COUNT(*) FROM story_views v WHERE v.story_id = s.id) "
          "FROM stories s WHERE s.expires_at > $2 AND ("
          " s.user_id = $1 OR s.privacy = 'everybody'"
          " OR (s.privacy = 'contacts' AND EXISTS (SELECT 1 FROM contacts c WHERE c.owner_id = s.user_id AND c.peer_id = $1))"
          " OR (s.privacy = 'close_friends' AND EXISTS (SELECT 1 FROM close_friends cf WHERE cf.owner_id = s.user_id AND cf.peer_id = $1))"
          ") ORDER BY s.id DESC LIMIT 100");
  q.bind(1, viewer);
  q.bind(2, now_s());
  q.exec();
  std::vector<Story> rows;
  while (q.next()) {
    Story story;
    story.id = q.i64(0);
    story.user_id = q.i64(1);
    story.file_id = q.i64(2);
    story.caption = q.text(3);
    story.privacy = q.text(4);
    story.no_screenshots = pg_flag(q.text(5));
    story.expires_at = q.i64(6);
    story.created_at = q.i64(7);
    story.views = q.i64(8);
    rows.push_back(std::move(story));
  }
  return rows;
}

std::optional<Story> Database::story_by_id(int64_t id) {
  Query q(db_,
          "SELECT id, user_id, COALESCE(file_id,0), caption, privacy, no_screenshots::int, expires_at, "
          "created_at, (SELECT COUNT(*) FROM story_views v WHERE v.story_id = stories.id) FROM stories WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  Story story;
  story.id = q.i64(0);
  story.user_id = q.i64(1);
  story.file_id = q.i64(2);
  story.caption = q.text(3);
  story.privacy = q.text(4);
  story.no_screenshots = pg_flag(q.text(5));
  story.expires_at = q.i64(6);
  story.created_at = q.i64(7);
  story.views = q.i64(8);
  return story;
}

void Database::view_story(int64_t story_id, int64_t user_id) {
  Query q(db_, "INSERT INTO story_views(story_id, user_id) VALUES($1,$2) ON CONFLICT DO NOTHING");
  q.bind(1, story_id);
  q.bind(2, user_id);
  q.exec();
}

void Database::add_close_friend(int64_t owner, int64_t peer) {
  Query q(db_, "INSERT INTO close_friends(owner_id, peer_id) VALUES($1,$2) ON CONFLICT DO NOTHING");
  q.bind(1, owner);
  q.bind(2, peer);
  q.exec();
}

bool Database::is_close_friend(int64_t owner, int64_t peer) {
  Query q(db_, "SELECT 1 FROM close_friends WHERE owner_id = $1 AND peer_id = $2");
  q.bind(1, owner);
  q.bind(2, peer);
  q.exec();
  return q.next();
}

std::optional<StickerPack> Database::create_pack(int64_t owner, const std::string &title,
                                                      const std::string &short_name, bool premium_pack) {
  Query q(db_,
          "INSERT INTO sticker_packs(owner_id, title, short_name, premium, created_at) "
          "VALUES($1,$2,$3,$4::boolean,$5) RETURNING id");
  q.bind(1, owner);
  q.bind(2, title);
  q.bind(3, short_name);
  q.bind(4, flag(premium_pack));
  q.bind(5, now_s());
  try {
    q.exec();
  } catch (const DbError &error) {
    if (error.sqlstate == "23505") return std::nullopt;
    throw;
  }
  if (!q.next()) throw std::runtime_error("pack insert returned no id");
  return StickerPack{q.i64(0), owner, title, short_name, premium_pack};
}

Sticker Database::add_sticker(int64_t pack_id, int64_t file_id, const std::string &emoji) {
  Query q(db_, "INSERT INTO stickers(pack_id, file_id, emoji) VALUES($1,$2,$3) RETURNING id");
  q.bind(1, pack_id);
  q.bind(2, file_id);
  q.bind(3, emoji);
  q.exec();
  if (!q.next()) throw std::runtime_error("sticker insert returned no id");
  return Sticker{q.i64(0), pack_id, file_id, emoji};
}

std::vector<StickerPack> Database::list_packs() {
  Query q(db_, "SELECT id, owner_id, title, short_name, premium::int FROM sticker_packs ORDER BY id");
  q.exec();
  std::vector<StickerPack> rows;
  while (q.next())
    rows.push_back(StickerPack{q.i64(0), q.i64(1), q.text(2), q.text(3), pg_flag(q.text(4))});
  return rows;
}

std::optional<StickerPack> Database::pack_by_id(int64_t id) {
  Query q(db_, "SELECT id, owner_id, title, short_name, premium::int FROM sticker_packs WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return StickerPack{q.i64(0), q.i64(1), q.text(2), q.text(3), pg_flag(q.text(4))};
}

std::vector<Sticker> Database::stickers_in(int64_t pack_id) {
  Query q(db_, "SELECT id, pack_id, file_id, emoji FROM stickers WHERE pack_id = $1 ORDER BY id");
  q.bind(1, pack_id);
  q.exec();
  std::vector<Sticker> rows;
  while (q.next()) rows.push_back(Sticker{q.i64(0), q.i64(1), q.i64(2), q.text(3)});
  return rows;
}

std::optional<Sticker> Database::sticker_by_file(int64_t file_id) {
  Query q(db_, "SELECT id, pack_id, file_id, emoji FROM stickers WHERE file_id = $1");
  q.bind(1, file_id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return Sticker{q.i64(0), q.i64(1), q.i64(2), q.text(3)};
}

Folder Database::create_folder(int64_t user_id, const std::string &title) {
  int64_t position = folder_count(user_id);
  Query q(db_, "INSERT INTO folders(user_id, title, position) VALUES($1,$2,$3) RETURNING id");
  q.bind(1, user_id);
  q.bind(2, title);
  q.bind(3, position);
  q.exec();
  if (!q.next()) throw std::runtime_error("folder insert returned no id");
  return Folder{q.i64(0), title, static_cast<int>(position), {}};
}

std::vector<Folder> Database::list_folders(int64_t user_id) {
  Query q(db_, "SELECT id, title, position FROM folders WHERE user_id = $1 ORDER BY position, id");
  q.bind(1, user_id);
  q.exec();
  std::vector<Folder> rows;
  while (q.next()) rows.push_back(Folder{q.i64(0), q.text(1), static_cast<int>(q.i64(2)), {}});
  for (Folder &folder : rows) {
    Query items(db_, "SELECT dialog_id FROM folder_items WHERE folder_id = $1 ORDER BY dialog_id");
    items.bind(1, folder.id);
    items.exec();
    while (items.next()) folder.dialog_ids.push_back(items.i64(0));
  }
  return rows;
}

void Database::folder_add(int64_t folder_id, int64_t user_id, int64_t dialog_id) {
  Query own(db_, "SELECT 1 FROM folders WHERE id = $1 AND user_id = $2");
  own.bind(1, folder_id);
  own.bind(2, user_id);
  own.exec();
  if (!own.next()) return;
  Query q(db_, "INSERT INTO folder_items(folder_id, dialog_id) VALUES($1,$2) ON CONFLICT DO NOTHING");
  q.bind(1, folder_id);
  q.bind(2, dialog_id);
  q.exec();
}

void Database::folder_remove(int64_t folder_id, int64_t user_id, int64_t dialog_id) {
  Query q(db_,
          "DELETE FROM folder_items WHERE folder_id = $1 AND dialog_id = $3 "
          "AND EXISTS (SELECT 1 FROM folders f WHERE f.id = $1 AND f.user_id = $2)");
  q.bind(1, folder_id);
  q.bind(2, user_id);
  q.bind(3, dialog_id);
  q.exec();
}

bool Database::delete_folder(int64_t folder_id, int64_t user_id) {
  Query q(db_, "DELETE FROM folders WHERE id = $1 AND user_id = $2");
  q.bind(1, folder_id);
  q.bind(2, user_id);
  q.exec();
  return q.affected() > 0;
}

SavedTab Database::create_tab(int64_t user_id, const std::string &title) {
  Query q(db_, "INSERT INTO saved_tabs(user_id, title) VALUES($1,$2) RETURNING id");
  q.bind(1, user_id);
  q.bind(2, title);
  q.exec();
  if (!q.next()) throw std::runtime_error("tab insert returned no id");
  return SavedTab{q.i64(0), title};
}

std::vector<SavedTab> Database::list_tabs(int64_t user_id) {
  Query q(db_, "SELECT id, title FROM saved_tabs WHERE user_id = $1 ORDER BY id");
  q.bind(1, user_id);
  q.exec();
  std::vector<SavedTab> rows;
  while (q.next()) rows.push_back(SavedTab{q.i64(0), q.text(1)});
  return rows;
}

void Database::tab_add(int64_t tab_id, int64_t user_id, int64_t message_id) {
  Query own(db_, "SELECT 1 FROM saved_tabs WHERE id = $1 AND user_id = $2");
  own.bind(1, tab_id);
  own.bind(2, user_id);
  own.exec();
  if (!own.next()) return;
  Query q(db_, "INSERT INTO saved_tab_items(tab_id, message_id) VALUES($1,$2) ON CONFLICT DO NOTHING");
  q.bind(1, tab_id);
  q.bind(2, message_id);
  q.exec();
}

void Database::add_tag(int64_t user_id, int64_t message_id, const std::string &tag) {
  Query q(db_,
          "INSERT INTO saved_tags(user_id, message_id, tag) VALUES($1,$2,$3) ON CONFLICT DO NOTHING");
  q.bind(1, user_id);
  q.bind(2, message_id);
  q.bind(3, tag);
  q.exec();
}

std::vector<std::string> Database::tags_for(int64_t user_id, int64_t message_id) {
  Query q(db_, "SELECT tag FROM saved_tags WHERE user_id = $1 AND message_id = $2 ORDER BY tag");
  q.bind(1, user_id);
  q.bind(2, message_id);
  q.exec();
  std::vector<std::string> tags;
  while (q.next()) tags.push_back(q.text(0));
  return tags;
}

CallInfo Database::start_call(int64_t dialog_id, int64_t creator) {
  int64_t now = now_s();
  Query q(db_,
          "INSERT INTO calls(dialog_id, creator_id, created_at) VALUES($1,$2,$3) "
          "RETURNING id, dialog_id, creator_id, active::int, recording::int");
  q.bind(1, dialog_id);
  q.bind(2, creator);
  q.bind(3, now);
  q.exec();
  if (!q.next()) throw std::runtime_error("call insert returned no id");
  CallInfo info{q.i64(0), q.i64(1), q.i64(2), pg_flag(q.text(3)), pg_flag(q.text(4))};
  join_call(info.id, creator);
  return info;
}

std::optional<CallInfo> Database::call_by_id(int64_t id) {
  Query q(db_, "SELECT id, dialog_id, creator_id, active::int, recording::int FROM calls WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return CallInfo{q.i64(0), q.i64(1), q.i64(2), pg_flag(q.text(3)), pg_flag(q.text(4))};
}

bool Database::join_call(int64_t call_id, int64_t user_id) {
  Query q(db_,
          "INSERT INTO call_members(call_id, user_id) VALUES($1,$2) ON CONFLICT DO NOTHING");
  q.bind(1, call_id);
  q.bind(2, user_id);
  q.exec();
  return true;
}

void Database::leave_call(int64_t call_id, int64_t user_id) {
  Query q(db_, "DELETE FROM call_members WHERE call_id = $1 AND user_id = $2");
  q.bind(1, call_id);
  q.bind(2, user_id);
  q.exec();
  Query left(db_, "SELECT COUNT(*) FROM call_members WHERE call_id = $1");
  left.bind(1, call_id);
  left.exec();
  if (left.next() && left.i64(0) == 0) {
    Query done(db_, "UPDATE calls SET active = FALSE, recording = FALSE WHERE id = $1");
    done.bind(1, call_id);
    done.exec();
  }
}

void Database::set_call_state(int64_t call_id, int64_t user_id, bool muted, bool video, bool screen) {
  Query q(db_,
          "UPDATE call_members SET muted = $3::boolean, video = $4::boolean, screen = $5::boolean "
          "WHERE call_id = $1 AND user_id = $2");
  q.bind(1, call_id);
  q.bind(2, user_id);
  q.bind(3, flag(muted));
  q.bind(4, flag(video));
  q.bind(5, flag(screen));
  q.exec();
}

void Database::set_call_recording(int64_t call_id, bool recording) {
  Query q(db_, "UPDATE calls SET recording = $2::boolean WHERE id = $1");
  q.bind(1, call_id);
  q.bind(2, flag(recording));
  q.exec();
}

std::vector<CallPeer> Database::call_peers(int64_t call_id) {
  Query q(db_,
          "SELECT user_id, muted::int, video::int, screen::int FROM call_members WHERE call_id = $1 "
          "ORDER BY user_id");
  q.bind(1, call_id);
  q.exec();
  std::vector<CallPeer> rows;
  while (q.next()) rows.push_back(CallPeer{q.i64(0), pg_flag(q.text(1)), pg_flag(q.text(2)), pg_flag(q.text(3))});
  return rows;
}

std::optional<User> Database::create_bot(int64_t owner, const std::string &username,
                                         const std::string &display, const std::string &token) {
  unsigned char salt[16];
  unsigned char hash[32];
  unsigned char password[32];
  if (RAND_bytes(password, sizeof password) != 1) throw std::runtime_error("RAND_bytes failed");
  hash_secret(to_hex(password, sizeof password), salt, hash);
  Tx tx(*this);
  {
    Query exists(db_, "SELECT id FROM users WHERE lower(username) = lower($1)");
    exists.bind(1, username);
    exists.exec();
    if (exists.next()) {
      tx.commit();
      return std::nullopt;
    }
  }
  Query ins(db_,
            "INSERT INTO users(username, display_name, pass_salt, pass_hash, created_at, is_bot, bot_owner_id) "
            "VALUES($1,$2,$3,$4,$5,TRUE,$6) RETURNING id");
  ins.bind(1, username);
  ins.bind(2, display);
  ins.bind_blob(3, salt, 16);
  ins.bind_blob(4, hash, 32);
  ins.bind(5, now_s());
  ins.bind(6, owner);
  try {
    ins.exec();
  } catch (const DbError &error) {
    if (error.sqlstate == "23505") return std::nullopt;
    throw;
  }
  if (!ins.next()) throw std::runtime_error("bot insert returned no id");
  int64_t id = ins.i64(0);
  std::string dig = sha256_bytes(token);
  Query tok(db_, "INSERT INTO bot_tokens(token_hash, bot_id, created_at) VALUES($1,$2,$3)");
  tok.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  tok.bind(2, id);
  tok.bind(3, now_s());
  tok.exec();
  tx.commit();
  return User{id, username, display};
}

std::optional<int64_t> Database::bot_by_token(const std::string &token) {
  std::string dig = sha256_bytes(token);
  Query q(db_, "SELECT bot_id FROM bot_tokens WHERE token_hash = $1");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  q.exec();
  if (!q.next()) return std::nullopt;
  return q.i64(0);
}

std::optional<MiniApp> Database::create_app(int64_t owner, const std::string &title,
                                                 const std::string &short_name, const std::string &url) {
  Query q(db_,
          "INSERT INTO mini_apps(owner_id, title, short_name, url, created_at) "
          "VALUES($1,$2,$3,$4,$5) RETURNING id");
  q.bind(1, owner);
  q.bind(2, title);
  q.bind(3, short_name);
  q.bind(4, url);
  q.bind(5, now_s());
  try {
    q.exec();
  } catch (const DbError &error) {
    if (error.sqlstate == "23505") return std::nullopt;
    throw;
  }
  if (!q.next()) throw std::runtime_error("app insert returned no id");
  return MiniApp{q.i64(0), owner, title, short_name, url};
}

std::optional<MiniApp> Database::app_by_name(const std::string &short_name) {
  Query q(db_, "SELECT id, owner_id, title, short_name, url FROM mini_apps WHERE lower(short_name) = lower($1)");
  q.bind(1, short_name);
  q.exec();
  if (!q.next()) return std::nullopt;
  return MiniApp{q.i64(0), q.i64(1), q.text(2), q.text(3), q.text(4)};
}

std::vector<MiniApp> Database::list_apps() {
  Query q(db_, "SELECT id, owner_id, title, short_name, url FROM mini_apps ORDER BY id");
  q.exec();
  std::vector<MiniApp> rows;
  while (q.next()) rows.push_back(MiniApp{q.i64(0), q.i64(1), q.text(2), q.text(3), q.text(4)});
  return rows;
}

Invoice Database::create_invoice(int64_t bot_id, int64_t user_id, const std::string &title, int64_t amount,
                                 const std::string &currency, const std::string &purpose) {
  Query q(db_,
          "INSERT INTO invoices(bot_id, user_id, title, amount, currency, purpose, status, created_at) "
          "VALUES($1,$2,$3,$4,$5,$6,'pending',$7) "
          "RETURNING id, bot_id, user_id, title, amount, currency, purpose, status, provider_ref");
  q.bind(1, bot_id);
  q.bind(2, user_id);
  q.bind(3, title);
  q.bind(4, amount);
  q.bind(5, currency);
  q.bind(6, purpose);
  q.bind(7, now_s());
  q.exec();
  if (!q.next()) throw std::runtime_error("invoice insert returned no id");
  return read_invoice(q);
}

std::optional<Invoice> Database::invoice_by_id(int64_t id) {
  Query q(db_,
          "SELECT id, bot_id, user_id, title, amount, currency, purpose, status, provider_ref "
          "FROM invoices WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return read_invoice(q);
}

std::vector<Invoice> Database::list_invoices(int64_t user_id) {
  Query q(db_,
          "SELECT id, bot_id, user_id, title, amount, currency, purpose, status, provider_ref "
          "FROM invoices WHERE user_id = $1 OR bot_id = $1 ORDER BY id DESC LIMIT 50");
  q.bind(1, user_id);
  q.exec();
  std::vector<Invoice> rows;
  while (q.next()) rows.push_back(read_invoice(q));
  return rows;
}

bool Database::set_invoice_status(int64_t id, int64_t bot_id, const std::string &status,
                                  const std::string &provider_ref) {
  Query q(db_,
          "UPDATE invoices SET status = $3, provider_ref = $4 "
          "WHERE id = $1 AND bot_id = $2 AND status = 'pending'");
  q.bind(1, id);
  q.bind(2, bot_id);
  q.bind(3, status);
  q.bind(4, provider_ref);
  q.exec();
  return q.affected() > 0;
}

}
