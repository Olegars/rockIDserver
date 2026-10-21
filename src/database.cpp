#include "rockid/database.hpp"

#include "pg_query.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rockid {
namespace {

void burn_password(const std::string &password) {
  static const unsigned char salt[16] = {0};
  unsigned char out[32];
  PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, sizeof salt,
                    kPbkdfIters, EVP_sha256(), sizeof out, out);
}

}  // namespace

Database::Tx::Tx(Database &database) : db(database) {
  db.exec("BEGIN");
  open = true;
}

void Database::Tx::commit() {
  db.exec("COMMIT");
  open = false;
}

Database::Tx::~Tx() {
  if (!open) return;
  try {
    db.exec("ROLLBACK");
  } catch (...) {
  }
}

Database::Database(const std::string &conninfo) {
  db_ = PQconnectdb(conninfo.c_str());
  if (!db_ || PQstatus(db_) != CONNECTION_OK) {
    std::string message = db_ ? PQerrorMessage(db_) : "postgres connect failed";
    if (db_) PQfinish(db_);
    db_ = nullptr;
    throw std::runtime_error(message);
  }
  migrate();
}

Database::~Database() {
  if (db_) PQfinish(db_);
}

void Database::exec(const char *sql) {
  PGresult *result = PQexec(db_, sql);
  if (!result) throw DbError(PQerrorMessage(db_));
  ExecStatusType status = PQresultStatus(result);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::string message = PQresultErrorMessage(result);
    PQclear(result);
    throw DbError(message);
  }
  PQclear(result);
}

void Database::migrate() {
  const char *statements[] = {
      "CREATE TABLE IF NOT EXISTS users ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  username TEXT NOT NULL,"
      "  display_name TEXT NOT NULL,"
      "  pass_salt BYTEA NOT NULL,"
      "  pass_hash BYTEA NOT NULL,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE UNIQUE INDEX IF NOT EXISTS users_username_lower_idx ON users (lower(username))",
      "CREATE TABLE IF NOT EXISTS sessions ("
      "  token_hash BYTEA PRIMARY KEY,"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS dialogs ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  kind TEXT NOT NULL,"
      "  title TEXT NOT NULL DEFAULT '',"
      "  peer_key TEXT UNIQUE,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS members ("
      "  dialog_id BIGINT NOT NULL REFERENCES dialogs(id),"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  role TEXT NOT NULL,"
      "  joined_at BIGINT NOT NULL,"
      "  PRIMARY KEY (dialog_id, user_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS messages ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  dialog_id BIGINT NOT NULL REFERENCES dialogs(id),"
      "  sender_id BIGINT NOT NULL REFERENCES users(id),"
      "  body TEXT NOT NULL,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE INDEX IF NOT EXISTS idx_messages_dialog ON messages (dialog_id, id)",
      "CREATE TABLE IF NOT EXISTS read_state ("
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  dialog_id BIGINT NOT NULL REFERENCES dialogs(id),"
      "  last_read_id BIGINT NOT NULL DEFAULT 0,"
      "  PRIMARY KEY (user_id, dialog_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS contacts ("
      "  owner_id BIGINT NOT NULL REFERENCES users(id),"
      "  peer_id BIGINT NOT NULL REFERENCES users(id),"
      "  created_at BIGINT NOT NULL,"
      "  PRIMARY KEY (owner_id, peer_id)"
      ")",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS phone TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS bio TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS photo_file_id BIGINT",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS premium_until BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS privacy_phone TEXT NOT NULL DEFAULT 'everybody'",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS privacy_last_seen TEXT NOT NULL DEFAULT 'everybody'",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS privacy_photo TEXT NOT NULL DEFAULT 'everybody'",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS cloud_salt BYTEA",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS cloud_hash BYTEA",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS is_bot BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS bot_owner_id BIGINT",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS last_seen_at BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS preferences TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE sessions ADD COLUMN IF NOT EXISTS id BIGSERIAL",
      "CREATE UNIQUE INDEX IF NOT EXISTS sessions_id_idx ON sessions (id)",
      "ALTER TABLE sessions ADD COLUMN IF NOT EXISTS device TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE sessions ADD COLUMN IF NOT EXISTS platform TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE sessions ADD COLUMN IF NOT EXISTS last_seen_at BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE sessions ADD COLUMN IF NOT EXISTS local_lock TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE dialogs ADD COLUMN IF NOT EXISTS linked_id BIGINT",
      "ALTER TABLE dialogs ADD COLUMN IF NOT EXISTS forum BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE dialogs ADD COLUMN IF NOT EXISTS no_forwards BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE dialogs ADD COLUMN IF NOT EXISTS ttl_seconds INT NOT NULL DEFAULT 0",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS muted_until BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS archived BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS pin_pos INT NOT NULL DEFAULT 0",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS banned BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS can_post BOOLEAN NOT NULL DEFAULT TRUE",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS can_delete BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS can_ban BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS can_pin BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS can_manage BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE members ADD COLUMN IF NOT EXISTS public_key TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS kind TEXT NOT NULL DEFAULT 'text'",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS reply_to BIGINT",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS quote TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS file_id BIGINT",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS silent BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS scheduled_at BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS sent_at BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS edited_at BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS deleted BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS entities TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS views BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS expires_at BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS topic_id BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS transcript TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS translation TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS translation_lang TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE messages ADD COLUMN IF NOT EXISTS no_forward BOOLEAN NOT NULL DEFAULT FALSE",
      "CREATE INDEX IF NOT EXISTS idx_messages_due ON messages (scheduled_at) WHERE sent_at = 0 AND deleted = FALSE",
      "CREATE TABLE IF NOT EXISTS files ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  owner_id BIGINT NOT NULL REFERENCES users(id),"
      "  name TEXT NOT NULL,"
      "  mime TEXT NOT NULL,"
      "  size BIGINT NOT NULL,"
      "  received BIGINT NOT NULL DEFAULT 0,"
      "  ready BOOLEAN NOT NULL DEFAULT FALSE,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS topics ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  dialog_id BIGINT NOT NULL REFERENCES dialogs(id),"
      "  title TEXT NOT NULL,"
      "  created_by BIGINT NOT NULL REFERENCES users(id),"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS reactions ("
      "  message_id BIGINT NOT NULL REFERENCES messages(id),"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  emoji TEXT NOT NULL,"
      "  created_at BIGINT NOT NULL,"
      "  PRIMARY KEY (message_id, user_id, emoji)"
      ")",
      "CREATE TABLE IF NOT EXISTS stories ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  file_id BIGINT,"
      "  caption TEXT NOT NULL DEFAULT '',"
      "  privacy TEXT NOT NULL,"
      "  no_screenshots BOOLEAN NOT NULL DEFAULT FALSE,"
      "  expires_at BIGINT NOT NULL,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS story_views ("
      "  story_id BIGINT NOT NULL REFERENCES stories(id),"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  PRIMARY KEY (story_id, user_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS close_friends ("
      "  owner_id BIGINT NOT NULL REFERENCES users(id),"
      "  peer_id BIGINT NOT NULL REFERENCES users(id),"
      "  PRIMARY KEY (owner_id, peer_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS sticker_packs ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  owner_id BIGINT NOT NULL REFERENCES users(id),"
      "  title TEXT NOT NULL,"
      "  short_name TEXT NOT NULL UNIQUE,"
      "  premium BOOLEAN NOT NULL DEFAULT FALSE,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS stickers ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  pack_id BIGINT NOT NULL REFERENCES sticker_packs(id),"
      "  file_id BIGINT NOT NULL,"
      "  emoji TEXT NOT NULL DEFAULT ''"
      ")",
      "CREATE TABLE IF NOT EXISTS folders ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  title TEXT NOT NULL,"
      "  position INT NOT NULL DEFAULT 0"
      ")",
      "CREATE TABLE IF NOT EXISTS folder_items ("
      "  folder_id BIGINT NOT NULL REFERENCES folders(id) ON DELETE CASCADE,"
      "  dialog_id BIGINT NOT NULL,"
      "  PRIMARY KEY (folder_id, dialog_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS saved_tabs ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  title TEXT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS saved_tab_items ("
      "  tab_id BIGINT NOT NULL REFERENCES saved_tabs(id) ON DELETE CASCADE,"
      "  message_id BIGINT NOT NULL,"
      "  PRIMARY KEY (tab_id, message_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS saved_tags ("
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  message_id BIGINT NOT NULL,"
      "  tag TEXT NOT NULL,"
      "  PRIMARY KEY (user_id, message_id, tag)"
      ")",
      "CREATE TABLE IF NOT EXISTS calls ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  dialog_id BIGINT NOT NULL REFERENCES dialogs(id),"
      "  creator_id BIGINT NOT NULL REFERENCES users(id),"
      "  active BOOLEAN NOT NULL DEFAULT TRUE,"
      "  recording BOOLEAN NOT NULL DEFAULT FALSE,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS call_members ("
      "  call_id BIGINT NOT NULL REFERENCES calls(id),"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  muted BOOLEAN NOT NULL DEFAULT FALSE,"
      "  video BOOLEAN NOT NULL DEFAULT FALSE,"
      "  screen BOOLEAN NOT NULL DEFAULT FALSE,"
      "  PRIMARY KEY (call_id, user_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS bot_tokens ("
      "  token_hash BYTEA PRIMARY KEY,"
      "  bot_id BIGINT NOT NULL UNIQUE REFERENCES users(id),"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS bot_updates ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  bot_id BIGINT NOT NULL REFERENCES users(id),"
      "  payload TEXT NOT NULL,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS mini_apps ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  owner_id BIGINT NOT NULL REFERENCES users(id),"
      "  title TEXT NOT NULL,"
      "  short_name TEXT NOT NULL UNIQUE,"
      "  url TEXT NOT NULL,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS invoices ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  bot_id BIGINT NOT NULL REFERENCES users(id),"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  title TEXT NOT NULL,"
      "  amount BIGINT NOT NULL,"
      "  currency TEXT NOT NULL,"
      "  purpose TEXT NOT NULL DEFAULT '',"
      "  status TEXT NOT NULL,"
      "  provider_ref TEXT NOT NULL DEFAULT '',"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE TABLE IF NOT EXISTS login_challenges ("
      "  token_hash BYTEA PRIMARY KEY,"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  expires_at BIGINT NOT NULL"
      ")",
      "ALTER TABLE users ADD COLUMN IF NOT EXISTS online BOOLEAN NOT NULL DEFAULT FALSE",
      "UPDATE users SET online = FALSE",
      "ALTER TABLE mini_apps ADD COLUMN IF NOT EXISTS content TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE invoices ADD COLUMN IF NOT EXISTS pay_url TEXT NOT NULL DEFAULT ''",
      "CREATE TABLE IF NOT EXISTS offline_pushes ("
      "  id BIGSERIAL PRIMARY KEY,"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  payload TEXT NOT NULL,"
      "  created_at BIGINT NOT NULL"
      ")",
      "CREATE INDEX IF NOT EXISTS idx_offline_user ON offline_pushes (user_id, id)",
      "CREATE TABLE IF NOT EXISTS device_accounts ("
      "  device_id TEXT NOT NULL,"
      "  user_id BIGINT NOT NULL REFERENCES users(id),"
      "  created_at BIGINT NOT NULL,"
      "  PRIMARY KEY (device_id, user_id)"
      ")",
      "CREATE TABLE IF NOT EXISTS user_keys ("
      "  user_id BIGINT PRIMARY KEY REFERENCES users(id),"
      "  public_key BYTEA NOT NULL"
      ")",
  };
  for (const char *sql : statements) exec(sql);
}

std::optional<User> Database::create_user(const std::string &username, const std::string &password,
                                          const std::string &display_name) {
  unsigned char salt[16];
  if (RAND_bytes(salt, sizeof salt) != 1) throw std::runtime_error("RAND_bytes failed");
  unsigned char hash[32];
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, sizeof salt,
                        kPbkdfIters, EVP_sha256(), sizeof hash, hash) != 1)
    throw std::runtime_error("PBKDF2 failed");

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
            "INSERT INTO users(username, display_name, pass_salt, pass_hash, created_at) "
            "VALUES($1,$2,$3,$4,$5) RETURNING id");
  ins.bind(1, username);
  ins.bind(2, display_name);
  ins.bind_blob(3, salt, static_cast<int>(sizeof salt));
  ins.bind_blob(4, hash, static_cast<int>(sizeof hash));
  ins.bind(5, now_s());
  try {
    ins.exec();
  } catch (const DbError &error) {
    if (error.sqlstate == "23505") return std::nullopt;
    throw;
  }
  if (!ins.next()) throw std::runtime_error("user insert returned no id");
  int64_t id = ins.i64(0);
  tx.commit();
  return User{id, username, display_name};
}

std::optional<User> Database::authenticate(const std::string &username, const std::string &password) {
  Query q(db_,
          "SELECT id, username, display_name, pass_salt, pass_hash FROM users "
          "WHERE lower(username) = lower($1)");
  q.bind(1, username);
  q.exec();
  if (!q.next()) {
    burn_password(password);
    return std::nullopt;
  }
  User user = read_user(q, 0);
  std::string salt = q.blob(3);
  std::string hash = q.blob(4);
  unsigned char out[32];
  if (salt.empty() || hash.size() != sizeof out) return std::nullopt;
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        reinterpret_cast<const unsigned char *>(salt.data()),
                        static_cast<int>(salt.size()), kPbkdfIters, EVP_sha256(), sizeof out,
                        out) != 1)
    throw std::runtime_error("PBKDF2 failed");
  if (CRYPTO_memcmp(out, hash.data(), sizeof out) != 0) return std::nullopt;
  return user;
}

std::optional<User> Database::user_by_id(int64_t id) {
  Query q(db_, "SELECT id, username, display_name FROM users WHERE id = $1");
  q.bind(1, id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return read_user(q, 0);
}

std::optional<User> Database::user_by_username(const std::string &username) {
  Query q(db_,
          "SELECT id, username, display_name FROM users WHERE lower(username) = lower($1)");
  q.bind(1, username);
  q.exec();
  if (!q.next()) return std::nullopt;
  return read_user(q, 0);
}

std::vector<User> Database::search_users(const std::string &query, int64_t exclude, int limit) {
  std::string like;
  like.push_back('%');
  for (char c : query) {
    if (c == '%' || c == '_' || c == '\\') like.push_back('\\');
    like.push_back(c);
  }
  like.push_back('%');
  Query q(db_,
          "SELECT id, username, display_name FROM users "
          "WHERE id <> $1 AND username ILIKE $2 ESCAPE '\\' "
          "ORDER BY username LIMIT $3");
  q.bind(1, exclude);
  q.bind(2, like);
  q.bind(3, static_cast<int64_t>(limit));
  q.exec();
  std::vector<User> users;
  while (q.next()) users.push_back(read_user(q, 0));
  return users;
}

std::string Database::create_session(int64_t user_id, const std::string &device,
                                      const std::string &platform) {
  unsigned char raw[32];
  if (RAND_bytes(raw, sizeof raw) != 1) throw std::runtime_error("RAND_bytes failed");
  std::string token = to_hex(raw, sizeof raw);
  std::string dig = sha256_bytes(token);
  int64_t now = now_s();
  Query ins(db_,
            "INSERT INTO sessions(token_hash, user_id, created_at, device, platform, last_seen_at) "
            "VALUES($1,$2,$3,$4,$5,$6)");
  ins.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  ins.bind(2, user_id);
  ins.bind(3, now);
  ins.bind(4, device);
  ins.bind(5, platform);
  ins.bind(6, now);
  ins.exec();
  return token;
}

std::optional<int64_t> Database::user_id_by_token(const std::string &token) {
  std::string dig = sha256_bytes(token);
  Query q(db_, "SELECT user_id FROM sessions WHERE token_hash = $1");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  q.exec();
  if (!q.next()) return std::nullopt;
  return q.i64(0);
}

void Database::delete_session(const std::string &token) {
  std::string dig = sha256_bytes(token);
  Query q(db_, "DELETE FROM sessions WHERE token_hash = $1");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  q.exec();
}

void Database::add_contact(int64_t owner, int64_t peer) {
  Query q(db_,
          "INSERT INTO contacts(owner_id, peer_id, created_at) VALUES($1,$2,$3) "
          "ON CONFLICT DO NOTHING");
  q.bind(1, owner);
  q.bind(2, peer);
  q.bind(3, now_s());
  q.exec();
}

std::vector<User> Database::list_contacts(int64_t owner) {
  Query q(db_,
          "SELECT u.id, u.username, u.display_name FROM contacts c "
          "JOIN users u ON u.id = c.peer_id WHERE c.owner_id = $1 ORDER BY u.username");
  q.bind(1, owner);
  q.exec();
  std::vector<User> users;
  while (q.next()) users.push_back(read_user(q, 0));
  return users;
}

int64_t Database::create_private(int64_t a, int64_t b) {
  int64_t lo = std::min(a, b);
  int64_t hi = std::max(a, b);
  std::string key = std::to_string(lo) + ":" + std::to_string(hi);
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
  int64_t id = 0;
  {
    Query ins(db_,
              "INSERT INTO dialogs(kind, title, peer_key, created_at) "
              "VALUES('private', '', $1, $2) RETURNING id");
    ins.bind(1, key);
    ins.bind(2, now);
    ins.exec();
    if (!ins.next()) throw std::runtime_error("dialog insert returned no id");
    id = ins.i64(0);
  }
  for (int64_t user_id : {a, b}) {
    Query mem(db_,
              "INSERT INTO members(dialog_id, user_id, role, joined_at) "
              "VALUES($1,$2,'member',$3)");
    mem.bind(1, id);
    mem.bind(2, user_id);
    mem.bind(3, now);
    mem.exec();
  }
  tx.commit();
  return id;
}

int64_t Database::create_group(int64_t owner, const std::string &title,
                               const std::vector<int64_t> &members, bool forum) {
  Tx tx(*this);
  int64_t now = now_s();
  Query ins(db_,
            "INSERT INTO dialogs(kind, title, peer_key, created_at, forum) "
            "VALUES('group', $1, NULL, $2, $3::boolean) RETURNING id");
  ins.bind(1, title);
  ins.bind(2, now);
  ins.bind(3, forum ? std::string("true") : std::string("false"));
  ins.exec();
  if (!ins.next()) throw std::runtime_error("dialog insert returned no id");
  int64_t id = ins.i64(0);
  auto insert_member = [&](int64_t user_id, const char *role) {
    Query mem(db_,
              "INSERT INTO members(dialog_id, user_id, role, joined_at) VALUES($1,$2,$3,$4)");
    mem.bind(1, id);
    mem.bind(2, user_id);
    mem.bind(3, std::string(role));
    mem.bind(4, now);
    mem.exec();
  };
  insert_member(owner, "owner");
  for (int64_t user_id : members) insert_member(user_id, "member");
  tx.commit();
  return id;
}

bool Database::add_member(int64_t dialog_id, int64_t user_id) {
  Query q(db_,
          "INSERT INTO members(dialog_id, user_id, role, joined_at) "
          "VALUES($1, $2, 'member', $3) "
          "ON CONFLICT (dialog_id, user_id) DO NOTHING");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.bind(3, now_s());
  q.exec();
  return q.affected() > 0;
}

std::optional<DialogHead> Database::find_dialog(int64_t dialog_id) {
  Query q(db_,
          "SELECT id, kind, title, no_forwards::int, forum::int, ttl_seconds, COALESCE(linked_id,0) "
          "FROM dialogs WHERE id = $1");
  q.bind(1, dialog_id);
  q.exec();
  if (!q.next()) return std::nullopt;
  DialogHead head;
  head.id = q.i64(0);
  head.kind = q.text(1);
  head.title = q.text(2);
  head.no_forwards = pg_flag(q.text(3));
  head.forum = pg_flag(q.text(4));
  head.ttl_seconds = static_cast<int>(q.i64(5));
  head.linked_id = q.i64(6);
  return head;
}

std::optional<std::string> Database::member_role(int64_t dialog_id, int64_t user_id) {
  Query q(db_,
          "SELECT role FROM members WHERE dialog_id = $1 AND user_id = $2 AND banned = FALSE");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.exec();
  if (!q.next()) return std::nullopt;
  return q.text(0);
}

std::vector<int64_t> Database::member_ids(int64_t dialog_id) {
  Query q(db_, "SELECT user_id FROM members WHERE dialog_id = $1 ORDER BY user_id");
  q.bind(1, dialog_id);
  q.exec();
  std::vector<int64_t> ids;
  while (q.next()) ids.push_back(q.i64(0));
  return ids;
}

std::vector<Member> Database::members(int64_t dialog_id, int limit, int offset) {
  Query q(db_,
          "SELECT user_id, role FROM members WHERE dialog_id = $1 AND banned = FALSE "
          "ORDER BY user_id LIMIT $2 OFFSET $3");
  q.bind(1, dialog_id);
  q.bind(2, static_cast<int64_t>(limit));
  q.bind(3, static_cast<int64_t>(offset));
  q.exec();
  std::vector<Member> rows;
  while (q.next()) rows.push_back(Member{q.i64(0), q.text(1)});
  return rows;
}

std::vector<int64_t> Database::peer_user_ids(int64_t user_id) {
  Query q(db_,
          "SELECT DISTINCT m2.user_id FROM members m1 "
          "JOIN members m2 ON m2.dialog_id = m1.dialog_id AND m2.user_id != m1.user_id "
          "WHERE m1.user_id = $1");
  q.bind(1, user_id);
  q.exec();
  std::vector<int64_t> ids;
  while (q.next()) ids.push_back(q.i64(0));
  return ids;
}

DialogInfo Database::load_dialog(int64_t viewer, int64_t dialog_id, const std::string &kind,
                                 const std::string &title) {
  DialogInfo info;
  info.id = dialog_id;
  info.kind = kind;
  info.title = title;

  {
    Query q(db_, "SELECT COUNT(*) FROM members WHERE dialog_id = $1 AND banned = FALSE");
    q.bind(1, dialog_id);
    q.exec();
    if (q.next()) info.member_count = q.i64(0);
  }
  if (kind == "private") {
    Query q(db_,
            "SELECT u.id, u.username, u.display_name FROM members m "
            "JOIN users u ON u.id = m.user_id WHERE m.dialog_id = $1 AND m.user_id <> $2");
    q.bind(1, dialog_id);
    q.bind(2, viewer);
    q.exec();
    if (q.next()) info.peer = read_user(q, 0);
  }
  {
    Query flags(db_,
                "SELECT forum::int, no_forwards::int, ttl_seconds, COALESCE(linked_id,0) "
                "FROM dialogs WHERE id = $1");
    flags.bind(1, dialog_id);
    flags.exec();
    if (flags.next()) {
      info.forum = pg_flag(flags.text(0));
      info.no_forwards = pg_flag(flags.text(1));
      info.ttl_seconds = static_cast<int>(flags.i64(2));
      info.linked_id = flags.i64(3);
    }
  }
  {
    Query mine(db_,
               "SELECT archived::int, pin_pos, muted_until FROM members "
               "WHERE dialog_id = $1 AND user_id = $2");
    mine.bind(1, dialog_id);
    mine.bind(2, viewer);
    mine.exec();
    if (mine.next()) {
      info.archived = pg_flag(mine.text(0));
      info.pin_pos = static_cast<int>(mine.i64(1));
      info.muted_until = mine.i64(2);
    }
  }
  {
    int64_t now = now_s();
    std::string sql =
        "SELECT " + std::string(kMessageCols) +
        " FROM messages WHERE dialog_id = $1 AND deleted = FALSE "
        "AND (scheduled_at = 0 OR sent_at > 0) AND (expires_at = 0 OR expires_at > $2) "
        "ORDER BY id DESC LIMIT 1";
    Query q(db_, sql);
    q.bind(1, dialog_id);
    q.bind(2, now);
    q.exec();
    if (q.next()) info.last = read_message(q);
  }
  {
    Query q(db_,
            "SELECT COUNT(*) FROM messages WHERE dialog_id = $1 AND sender_id <> $2 AND id > "
            "COALESCE((SELECT last_read_id FROM read_state WHERE user_id = $3 AND dialog_id = $4), 0)");
    q.bind(1, dialog_id);
    q.bind(2, viewer);
    q.bind(3, viewer);
    q.bind(4, dialog_id);
    q.exec();
    if (q.next()) info.unread = q.i64(0);
  }
  return info;
}

std::optional<DialogInfo> Database::dialog_for(int64_t viewer, int64_t dialog_id) {
  Query q(db_,
          "SELECT d.kind, d.title FROM dialogs d "
          "JOIN members m ON m.dialog_id = d.id AND m.user_id = $1 AND m.banned = FALSE "
          "WHERE d.id = $2");
  q.bind(1, viewer);
  q.bind(2, dialog_id);
  q.exec();
  if (!q.next()) return std::nullopt;
  std::string kind = q.text(0);
  std::string title = q.text(1);
  return load_dialog(viewer, dialog_id, kind, title);
}

std::vector<DialogInfo> Database::list_dialogs(int64_t viewer, bool archived) {
  struct Row {
    int64_t id;
    std::string kind;
    std::string title;
  };
  std::vector<Row> rows;
  {
    Query q(db_,
            "SELECT d.id, d.kind, d.title FROM dialogs d "
            "JOIN members m ON m.dialog_id = d.id AND m.user_id = $1 AND m.banned = FALSE "
            "AND m.archived = $2::boolean "
            "ORDER BY m.pin_pos DESC, "
            "COALESCE((SELECT MAX(id) FROM messages WHERE dialog_id = d.id AND deleted = FALSE), 0) DESC, "
            "d.id DESC");
    q.bind(1, viewer);
    q.bind(2, archived ? std::string("true") : std::string("false"));
    q.exec();
    while (q.next()) rows.push_back(Row{q.i64(0), q.text(1), q.text(2)});
  }
  std::vector<DialogInfo> out;
  out.reserve(rows.size());
  for (const Row &row : rows) out.push_back(load_dialog(viewer, row.id, row.kind, row.title));
  return out;
}

Message Database::add_message(int64_t dialog_id, int64_t sender_id, const Outgoing &draft) {
  int64_t now = now_s();
  int64_t sent_at = draft.scheduled_at > now ? 0 : now;
  Query ins(db_,
            "INSERT INTO messages(dialog_id, sender_id, body, created_at, kind, reply_to, quote, "
            "file_id, silent, scheduled_at, sent_at, entities, expires_at, topic_id, no_forward) "
            "VALUES($1,$2,$3,$4,$5,NULLIF($6,0),$7,NULLIF($8,0),$9::boolean,$10,$11,$12,$13,$14,$15::boolean) "
            "RETURNING " +
                std::string(kMessageCols));
  ins.bind(1, dialog_id);
  ins.bind(2, sender_id);
  ins.bind(3, draft.text);
  ins.bind(4, now);
  ins.bind(5, draft.kind);
  ins.bind(6, draft.reply_to);
  ins.bind(7, draft.quote);
  ins.bind(8, draft.file_id);
  ins.bind(9, draft.silent ? std::string("true") : std::string("false"));
  ins.bind(10, draft.scheduled_at);
  ins.bind(11, sent_at);
  ins.bind(12, draft.entities);
  ins.bind(13, draft.expires_at);
  ins.bind(14, draft.topic_id);
  ins.bind(15, draft.no_forward ? std::string("true") : std::string("false"));
  ins.exec();
  if (!ins.next()) throw std::runtime_error("message insert returned no id");
  return read_message(ins);
}

std::vector<Message> Database::history(int64_t dialog_id, int64_t before_id, int limit, int64_t viewer,
                                       int64_t topic_id) {
  int64_t now = now_s();
  bool hide_sponsor = premium(viewer);
  std::string sql = "SELECT " + std::string(kMessageCols) +
                    " FROM messages WHERE dialog_id = $1 AND ($2 = 0 OR id < $2) "
                    "AND deleted = FALSE AND (scheduled_at = 0 OR sent_at > 0) "
                    "AND (expires_at = 0 OR expires_at > $3) "
                    "AND ($4 = 0 OR topic_id = $4) "
                    "AND ($5 = 0 OR kind <> 'sponsor') "
                    "ORDER BY id DESC LIMIT $6";
  Query q(db_, sql);
  q.bind(1, dialog_id);
  q.bind(2, before_id);
  q.bind(3, now);
  q.bind(4, topic_id);
  q.bind(5, hide_sponsor ? static_cast<int64_t>(1) : static_cast<int64_t>(0));
  q.bind(6, static_cast<int64_t>(limit));
  q.exec();
  std::vector<Message> rows;
  while (q.next()) rows.push_back(read_message(q));
  std::reverse(rows.begin(), rows.end());
  if (!rows.empty()) {
    std::string in = "SELECT message_id, emoji, COUNT(*), SUM(CASE WHEN user_id = $1 THEN 1 ELSE 0 END) "
                     "FROM reactions WHERE message_id IN (";
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i) in += ",";
      in += "$" + std::to_string(i + 2);
    }
    in += ") GROUP BY message_id, emoji";
    Query reactions(db_, in);
    reactions.bind(1, viewer);
    for (size_t i = 0; i < rows.size(); ++i) reactions.bind(static_cast<int>(i + 2), rows[i].id);
    reactions.exec();
    while (reactions.next()) {
      int64_t message_id = reactions.i64(0);
      for (Message &message : rows) {
        if (message.id != message_id) continue;
        ReactionCount count;
        count.emoji = reactions.text(1);
        count.count = reactions.i64(2);
        count.mine = reactions.i64(3) > 0;
        message.reactions.push_back(std::move(count));
      }
    }
  }
  return rows;
}

void Database::mark_read(int64_t user_id, int64_t dialog_id, int64_t max_id) {
  Query q(db_,
          "INSERT INTO read_state(user_id, dialog_id, last_read_id) VALUES($1,$2,$3) "
          "ON CONFLICT (user_id, dialog_id) DO UPDATE SET "
          "last_read_id = GREATEST(read_state.last_read_id, EXCLUDED.last_read_id)");
  q.bind(1, user_id);
  q.bind(2, dialog_id);
  q.bind(3, max_id);
  q.exec();
}

}
