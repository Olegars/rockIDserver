#include "rockid/database.hpp"

#include <sqlite3.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace rockid {
namespace {

constexpr int kPbkdfIters = 100000;

int64_t now_s() { return static_cast<int64_t>(std::time(nullptr)); }

std::string to_hex(const unsigned char *data, size_t n) {
  static const char *hex = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

std::string sha256_bytes(const std::string &value) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), out);
  return std::string(reinterpret_cast<const char *>(out), SHA256_DIGEST_LENGTH);
}

void burn_password(const std::string &password) {
  static const unsigned char salt[16] = {0};
  unsigned char out[32];
  PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, sizeof salt,
                    kPbkdfIters, EVP_sha256(), sizeof out, out);
}

class Stmt {
 public:
  Stmt(sqlite3 *db, const char *sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql, -1, &st_, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db_));
  }
  ~Stmt() { sqlite3_finalize(st_); }

  Stmt(const Stmt &) = delete;
  Stmt &operator=(const Stmt &) = delete;

  void bind(int index, int64_t value) {
    if (sqlite3_bind_int64(st_, index, value) != SQLITE_OK) fail();
  }
  void bind(int index, const std::string &value) {
    if (sqlite3_bind_text(st_, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
      fail();
  }
  void bind_blob(int index, const void *data, int size) {
    if (sqlite3_bind_blob(st_, index, data, size, SQLITE_TRANSIENT) != SQLITE_OK) fail();
  }

  int step() {
    int rc = sqlite3_step(st_);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return rc;
  }

  void reset() {
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
  }

  int64_t i64(int col) const { return sqlite3_column_int64(st_, col); }

  std::string text(int col) const {
    const unsigned char *p = sqlite3_column_text(st_, col);
    if (!p) return {};
    return reinterpret_cast<const char *>(p);
  }

  std::string blob(int col) const {
    const void *p = sqlite3_column_blob(st_, col);
    int n = sqlite3_column_bytes(st_, col);
    if (!p || n <= 0) return {};
    return std::string(static_cast<const char *>(p), static_cast<size_t>(n));
  }

 private:
  void fail() { throw std::runtime_error(sqlite3_errmsg(db_)); }

  sqlite3 *db_;
  sqlite3_stmt *st_ = nullptr;
};

User read_user(Stmt &st, int col) {
  User user;
  user.id = st.i64(col);
  user.username = st.text(col + 1);
  user.display_name = st.text(col + 2);
  return user;
}

}  // namespace

struct Database::Tx {
  Database &db;
  bool open = false;
  explicit Tx(Database &database) : db(database) {
    db.exec("BEGIN IMMEDIATE");
    open = true;
  }
  void commit() {
    db.exec("COMMIT");
    open = false;
  }
  ~Tx() {
    if (!open) return;
    try {
      db.exec("ROLLBACK");
    } catch (...) {
    }
  }
};

Database::Database(const std::string &path) {
  int rc = sqlite3_open_v2(path.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                           nullptr);
  if (rc != SQLITE_OK) {
    std::string message = db_ ? sqlite3_errmsg(db_) : "sqlite open failed";
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error(message);
  }
  sqlite3_busy_timeout(db_, 3000);
  migrate();
}

Database::~Database() {
  if (db_) sqlite3_close(db_);
}

void Database::exec(const char *sql) {
  char *err = nullptr;
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string message = err ? err : sqlite3_errmsg(db_);
    sqlite3_free(err);
    throw std::runtime_error(message);
  }
}

void Database::migrate() {
  exec(
      "PRAGMA journal_mode=WAL;"
      "PRAGMA foreign_keys=ON;"
      "CREATE TABLE IF NOT EXISTS users ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  username TEXT NOT NULL UNIQUE COLLATE NOCASE,"
      "  display_name TEXT NOT NULL,"
      "  pass_salt BLOB NOT NULL,"
      "  pass_hash BLOB NOT NULL,"
      "  created_at INTEGER NOT NULL"
      ");"
      "CREATE TABLE IF NOT EXISTS sessions ("
      "  token_hash BLOB PRIMARY KEY,"
      "  user_id INTEGER NOT NULL REFERENCES users(id),"
      "  created_at INTEGER NOT NULL"
      ");"
      "CREATE TABLE IF NOT EXISTS dialogs ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  kind TEXT NOT NULL,"
      "  title TEXT NOT NULL DEFAULT '',"
      "  peer_key TEXT UNIQUE,"
      "  created_at INTEGER NOT NULL"
      ");"
      "CREATE TABLE IF NOT EXISTS members ("
      "  dialog_id INTEGER NOT NULL REFERENCES dialogs(id),"
      "  user_id INTEGER NOT NULL REFERENCES users(id),"
      "  role TEXT NOT NULL,"
      "  joined_at INTEGER NOT NULL,"
      "  PRIMARY KEY(dialog_id, user_id)"
      ");"
      "CREATE TABLE IF NOT EXISTS messages ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  dialog_id INTEGER NOT NULL REFERENCES dialogs(id),"
      "  sender_id INTEGER NOT NULL REFERENCES users(id),"
      "  body TEXT NOT NULL,"
      "  created_at INTEGER NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_messages_dialog ON messages(dialog_id, id);"
      "CREATE TABLE IF NOT EXISTS read_state ("
      "  user_id INTEGER NOT NULL REFERENCES users(id),"
      "  dialog_id INTEGER NOT NULL REFERENCES dialogs(id),"
      "  last_read_id INTEGER NOT NULL DEFAULT 0,"
      "  PRIMARY KEY(user_id, dialog_id)"
      ");"
      "CREATE TABLE IF NOT EXISTS contacts ("
      "  owner_id INTEGER NOT NULL REFERENCES users(id),"
      "  peer_id INTEGER NOT NULL REFERENCES users(id),"
      "  created_at INTEGER NOT NULL,"
      "  PRIMARY KEY(owner_id, peer_id)"
      ");");
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
    Stmt exists(db_, "SELECT id FROM users WHERE username = ?");
    exists.bind(1, username);
    if (exists.step() == SQLITE_ROW) {
      tx.commit();
      return std::nullopt;
    }
  }
  Stmt ins(db_,
           "INSERT INTO users(username, display_name, pass_salt, pass_hash, created_at) "
           "VALUES(?,?,?,?,?)");
  ins.bind(1, username);
  ins.bind(2, display_name);
  ins.bind_blob(1 + 2, salt, static_cast<int>(sizeof salt));
  ins.bind_blob(4, hash, static_cast<int>(sizeof hash));
  ins.bind(5, now_s());
  ins.step();
  int64_t id = sqlite3_last_insert_rowid(db_);
  tx.commit();
  return User{id, username, display_name};
}

std::optional<User> Database::authenticate(const std::string &username, const std::string &password) {
  Stmt q(db_, "SELECT id, username, display_name, pass_salt, pass_hash FROM users WHERE username = ?");
  q.bind(1, username);
  if (q.step() != SQLITE_ROW) {
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
  Stmt q(db_, "SELECT id, username, display_name FROM users WHERE id = ?");
  q.bind(1, id);
  if (q.step() != SQLITE_ROW) return std::nullopt;
  return read_user(q, 0);
}

std::optional<User> Database::user_by_username(const std::string &username) {
  Stmt q(db_, "SELECT id, username, display_name FROM users WHERE username = ?");
  q.bind(1, username);
  if (q.step() != SQLITE_ROW) return std::nullopt;
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
  Stmt q(db_,
         "SELECT id, username, display_name FROM users "
         "WHERE id != ? AND username LIKE ? ESCAPE '\\' "
         "ORDER BY username LIMIT ?");
  q.bind(1, exclude);
  q.bind(2, like);
  q.bind(3, static_cast<int64_t>(limit));
  std::vector<User> users;
  while (q.step() == SQLITE_ROW) users.push_back(read_user(q, 0));
  return users;
}

std::string Database::create_session(int64_t user_id) {
  unsigned char raw[32];
  if (RAND_bytes(raw, sizeof raw) != 1) throw std::runtime_error("RAND_bytes failed");
  std::string token = to_hex(raw, sizeof raw);
  std::string dig = sha256_bytes(token);
  Stmt ins(db_, "INSERT INTO sessions(token_hash, user_id, created_at) VALUES(?,?,?)");
  ins.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  ins.bind(2, user_id);
  ins.bind(3, now_s());
  ins.step();
  return token;
}

std::optional<int64_t> Database::user_id_by_token(const std::string &token) {
  std::string dig = sha256_bytes(token);
  Stmt q(db_, "SELECT user_id FROM sessions WHERE token_hash = ?");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  if (q.step() != SQLITE_ROW) return std::nullopt;
  return q.i64(0);
}

void Database::delete_session(const std::string &token) {
  std::string dig = sha256_bytes(token);
  Stmt q(db_, "DELETE FROM sessions WHERE token_hash = ?");
  q.bind_blob(1, dig.data(), static_cast<int>(dig.size()));
  q.step();
}

void Database::add_contact(int64_t owner, int64_t peer) {
  Stmt q(db_, "INSERT OR IGNORE INTO contacts(owner_id, peer_id, created_at) VALUES(?,?,?)");
  q.bind(1, owner);
  q.bind(2, peer);
  q.bind(3, now_s());
  q.step();
}

std::vector<User> Database::list_contacts(int64_t owner) {
  Stmt q(db_,
         "SELECT u.id, u.username, u.display_name FROM contacts c "
         "JOIN users u ON u.id = c.peer_id WHERE c.owner_id = ? ORDER BY u.username");
  q.bind(1, owner);
  std::vector<User> users;
  while (q.step() == SQLITE_ROW) users.push_back(read_user(q, 0));
  return users;
}

int64_t Database::create_private(int64_t a, int64_t b) {
  int64_t lo = std::min(a, b);
  int64_t hi = std::max(a, b);
  std::string key = std::to_string(lo) + ":" + std::to_string(hi);
  Tx tx(*this);
  {
    Stmt q(db_, "SELECT id FROM dialogs WHERE peer_key = ?");
    q.bind(1, key);
    if (q.step() == SQLITE_ROW) {
      int64_t id = q.i64(0);
      tx.commit();
      return id;
    }
  }
  int64_t now = now_s();
  {
    Stmt ins(db_,
             "INSERT INTO dialogs(kind, title, peer_key, created_at) VALUES('private', '', ?, ?)");
    ins.bind(1, key);
    ins.bind(2, now);
    ins.step();
  }
  int64_t id = sqlite3_last_insert_rowid(db_);
  Stmt mem(db_, "INSERT INTO members(dialog_id, user_id, role, joined_at) VALUES(?,?, 'member', ?)");
  for (int64_t user_id : {a, b}) {
    mem.reset();
    mem.bind(1, id);
    mem.bind(2, user_id);
    mem.bind(3, now);
    mem.step();
  }
  tx.commit();
  return id;
}

int64_t Database::create_group(int64_t owner, const std::string &title,
                               const std::vector<int64_t> &members) {
  Tx tx(*this);
  int64_t now = now_s();
  {
    Stmt ins(db_, "INSERT INTO dialogs(kind, title, peer_key, created_at) VALUES('group', ?, NULL, ?)");
    ins.bind(1, title);
    ins.bind(2, now);
    ins.step();
  }
  int64_t id = sqlite3_last_insert_rowid(db_);
  auto insert_member = [&](int64_t user_id, const char *role) {
    Stmt mem(db_, "INSERT INTO members(dialog_id, user_id, role, joined_at) VALUES(?,?,?,?)");
    mem.bind(1, id);
    mem.bind(2, user_id);
    mem.bind(3, std::string(role));
    mem.bind(4, now);
    mem.step();
  };
  insert_member(owner, "owner");
  for (int64_t user_id : members) insert_member(user_id, "member");
  tx.commit();
  return id;
}

bool Database::add_member(int64_t dialog_id, int64_t user_id) {
  Stmt q(db_,
         "INSERT OR IGNORE INTO members(dialog_id, user_id, role, joined_at) "
         "VALUES(?, ?, 'member', ?)");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  q.bind(3, now_s());
  q.step();
  return sqlite3_changes(db_) > 0;
}

std::optional<DialogHead> Database::find_dialog(int64_t dialog_id) {
  Stmt q(db_, "SELECT id, kind, title FROM dialogs WHERE id = ?");
  q.bind(1, dialog_id);
  if (q.step() != SQLITE_ROW) return std::nullopt;
  DialogHead head;
  head.id = q.i64(0);
  head.kind = q.text(1);
  head.title = q.text(2);
  return head;
}

std::optional<std::string> Database::member_role(int64_t dialog_id, int64_t user_id) {
  Stmt q(db_, "SELECT role FROM members WHERE dialog_id = ? AND user_id = ?");
  q.bind(1, dialog_id);
  q.bind(2, user_id);
  if (q.step() != SQLITE_ROW) return std::nullopt;
  return q.text(0);
}

std::vector<int64_t> Database::member_ids(int64_t dialog_id) {
  Stmt q(db_, "SELECT user_id FROM members WHERE dialog_id = ? ORDER BY user_id");
  q.bind(1, dialog_id);
  std::vector<int64_t> ids;
  while (q.step() == SQLITE_ROW) ids.push_back(q.i64(0));
  return ids;
}

std::vector<Member> Database::members(int64_t dialog_id) {
  Stmt q(db_, "SELECT user_id, role FROM members WHERE dialog_id = ? ORDER BY user_id");
  q.bind(1, dialog_id);
  std::vector<Member> rows;
  while (q.step() == SQLITE_ROW) rows.push_back(Member{q.i64(0), q.text(1)});
  return rows;
}

std::vector<int64_t> Database::peer_user_ids(int64_t user_id) {
  Stmt q(db_,
         "SELECT DISTINCT m2.user_id FROM members m1 "
         "JOIN members m2 ON m2.dialog_id = m1.dialog_id AND m2.user_id != m1.user_id "
         "WHERE m1.user_id = ?");
  q.bind(1, user_id);
  std::vector<int64_t> ids;
  while (q.step() == SQLITE_ROW) ids.push_back(q.i64(0));
  return ids;
}

DialogInfo Database::load_dialog(int64_t viewer, int64_t dialog_id, const std::string &kind,
                                 const std::string &title) {
  DialogInfo info;
  info.id = dialog_id;
  info.kind = kind;
  info.title = title;

  {
    Stmt q(db_, "SELECT COUNT(*) FROM members WHERE dialog_id = ?");
    q.bind(1, dialog_id);
    if (q.step() == SQLITE_ROW) info.member_count = q.i64(0);
  }
  if (kind == "private") {
    Stmt q(db_,
           "SELECT u.id, u.username, u.display_name FROM members m "
           "JOIN users u ON u.id = m.user_id WHERE m.dialog_id = ? AND m.user_id != ?");
    q.bind(1, dialog_id);
    q.bind(2, viewer);
    if (q.step() == SQLITE_ROW) info.peer = read_user(q, 0);
  }
  {
    Stmt q(db_,
           "SELECT id, dialog_id, sender_id, body, created_at FROM messages "
           "WHERE dialog_id = ? ORDER BY id DESC LIMIT 1");
    q.bind(1, dialog_id);
    if (q.step() == SQLITE_ROW) {
      Message message;
      message.id = q.i64(0);
      message.dialog_id = q.i64(1);
      message.sender_id = q.i64(2);
      message.text = q.text(3);
      message.created_at = q.i64(4);
      info.last = std::move(message);
    }
  }
  {
    Stmt q(db_,
           "SELECT COUNT(*) FROM messages WHERE dialog_id = ? AND sender_id != ? AND id > "
           "COALESCE((SELECT last_read_id FROM read_state WHERE user_id = ? AND dialog_id = ?), 0)");
    q.bind(1, dialog_id);
    q.bind(2, viewer);
    q.bind(3, viewer);
    q.bind(4, dialog_id);
    if (q.step() == SQLITE_ROW) info.unread = q.i64(0);
  }
  return info;
}

std::optional<DialogInfo> Database::dialog_for(int64_t viewer, int64_t dialog_id) {
  Stmt q(db_,
         "SELECT d.kind, d.title FROM dialogs d "
         "JOIN members m ON m.dialog_id = d.id AND m.user_id = ? WHERE d.id = ?");
  q.bind(1, viewer);
  q.bind(2, dialog_id);
  if (q.step() != SQLITE_ROW) return std::nullopt;
  std::string kind = q.text(0);
  std::string title = q.text(1);
  return load_dialog(viewer, dialog_id, kind, title);
}

std::vector<DialogInfo> Database::list_dialogs(int64_t viewer) {
  struct Row {
    int64_t id;
    std::string kind;
    std::string title;
  };
  std::vector<Row> rows;
  {
    Stmt q(db_,
           "SELECT d.id, d.kind, d.title FROM dialogs d "
           "JOIN members m ON m.dialog_id = d.id AND m.user_id = ? "
           "ORDER BY IFNULL((SELECT MAX(id) FROM messages WHERE dialog_id = d.id), 0) DESC, d.id DESC");
    q.bind(1, viewer);
    while (q.step() == SQLITE_ROW) rows.push_back(Row{q.i64(0), q.text(1), q.text(2)});
  }
  std::vector<DialogInfo> out;
  out.reserve(rows.size());
  for (const Row &row : rows) out.push_back(load_dialog(viewer, row.id, row.kind, row.title));
  return out;
}

Message Database::add_message(int64_t dialog_id, int64_t sender_id, const std::string &text) {
  int64_t now = now_s();
  Stmt ins(db_, "INSERT INTO messages(dialog_id, sender_id, body, created_at) VALUES(?,?,?,?)");
  ins.bind(1, dialog_id);
  ins.bind(2, sender_id);
  ins.bind(3, text);
  ins.bind(4, now);
  ins.step();
  Message message;
  message.id = sqlite3_last_insert_rowid(db_);
  message.dialog_id = dialog_id;
  message.sender_id = sender_id;
  message.text = text;
  message.created_at = now;
  return message;
}

std::vector<Message> Database::history(int64_t dialog_id, int64_t before_id, int limit) {
  Stmt q(db_,
         "SELECT id, dialog_id, sender_id, body, created_at FROM messages "
         "WHERE dialog_id = ? AND (? = 0 OR id < ?) ORDER BY id DESC LIMIT ?");
  q.bind(1, dialog_id);
  q.bind(2, before_id);
  q.bind(3, before_id);
  q.bind(4, static_cast<int64_t>(limit));
  std::vector<Message> rows;
  while (q.step() == SQLITE_ROW) {
    Message message;
    message.id = q.i64(0);
    message.dialog_id = q.i64(1);
    message.sender_id = q.i64(2);
    message.text = q.text(3);
    message.created_at = q.i64(4);
    rows.push_back(std::move(message));
  }
  std::reverse(rows.begin(), rows.end());
  return rows;
}

void Database::mark_read(int64_t user_id, int64_t dialog_id, int64_t max_id) {
  Stmt q(db_,
         "INSERT INTO read_state(user_id, dialog_id, last_read_id) VALUES(?,?,?) "
         "ON CONFLICT(user_id, dialog_id) DO UPDATE SET "
         "last_read_id = MAX(read_state.last_read_id, excluded.last_read_id)");
  q.bind(1, user_id);
  q.bind(2, dialog_id);
  q.bind(3, max_id);
  q.step();
}

}
