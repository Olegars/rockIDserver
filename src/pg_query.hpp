#pragma once

#include "rockid/database.hpp"

#include <libpq-fe.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

namespace rockid {

inline constexpr int kPbkdfIters = 100000;
inline constexpr const char *kMessageCols =
    "id, dialog_id, sender_id, body, created_at, kind, COALESCE(reply_to,0), quote, "
    "COALESCE(file_id,0), silent::int, scheduled_at, edited_at, deleted::int, entities, views, "
    "expires_at, topic_id, transcript, translation, translation_lang, no_forward::int";

class DbError : public std::runtime_error {
 public:
  std::string sqlstate;
  explicit DbError(std::string message, std::string state = {})
      : std::runtime_error(std::move(message)), sqlstate(std::move(state)) {}
};

inline int64_t now_s() { return static_cast<int64_t>(std::time(nullptr)); }

inline std::string to_hex(const unsigned char *data, size_t n) {
  static const char *hex = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

inline int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  throw DbError("bad bytea hex");
}

inline std::string bytea_param(const void *data, int size) {
  return "\\x" + to_hex(static_cast<const unsigned char *>(data), static_cast<size_t>(size));
}

inline std::string decode_bytea(const std::string &text) {
  if (text.size() < 2 || text[0] != '\\' || (text[1] != 'x' && text[1] != 'X')) return text;
  std::string out;
  out.reserve((text.size() - 2) / 2);
  for (size_t i = 2; i + 1 < text.size(); i += 2) {
    out.push_back(static_cast<char>((hex_value(text[i]) << 4) | hex_value(text[i + 1])));
  }
  return out;
}

inline std::string sha256_bytes(const std::string &value) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), out);
  return std::string(reinterpret_cast<const char *>(out), SHA256_DIGEST_LENGTH);
}

inline bool pg_flag(const std::string &text) { return text == "1" || text == "t" || text == "true"; }

class Query {
 public:
  Query(PGconn *conn, const char *sql) : conn_(conn), sql_(sql) {}
  Query(PGconn *conn, const std::string &sql) : conn_(conn), owned_(sql), sql_(owned_.c_str()) {}
  ~Query() { clear(); }

  Query(const Query &) = delete;
  Query &operator=(const Query &) = delete;

  void bind(int index, int64_t value) { set_param(index, std::to_string(value)); }
  void bind(int index, const std::string &value) { set_param(index, value); }
  void bind_blob(int index, const void *data, int size) { set_param(index, bytea_param(data, size)); }

  void exec() {
    clear();
    std::vector<const char *> values(params_.size());
    for (size_t i = 0; i < params_.size(); ++i) values[i] = params_[i].c_str();
    res_ = PQexecParams(conn_, sql_, static_cast<int>(params_.size()), nullptr,
                        values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    check();
    row_ = -1;
  }

  bool next() {
    if (!res_) return false;
    ++row_;
    return row_ < PQntuples(res_);
  }

  int64_t i64(int col) const { return std::stoll(text(col)); }

  std::string text(int col) const {
    if (PQgetisnull(res_, row_, col)) return {};
    return PQgetvalue(res_, row_, col);
  }

  std::string blob(int col) const { return decode_bytea(text(col)); }

  int64_t affected() const {
    if (!res_) return 0;
    const char *tuples = PQcmdTuples(res_);
    if (!tuples || tuples[0] == '\0') return 0;
    return std::stoll(tuples);
  }

 private:
  void set_param(int index, std::string value) {
    if (index < 1) throw DbError("bad parameter index");
    if (static_cast<int>(params_.size()) < index) params_.resize(static_cast<size_t>(index));
    params_[static_cast<size_t>(index - 1)] = std::move(value);
  }

  void clear() {
    if (res_) {
      PQclear(res_);
      res_ = nullptr;
    }
  }

  void check() {
    if (!res_) throw DbError(PQerrorMessage(conn_));
    ExecStatusType status = PQresultStatus(res_);
    if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK) return;
    const char *state = PQresultErrorField(res_, PG_DIAG_SQLSTATE);
    throw DbError(PQresultErrorMessage(res_), state ? state : "");
  }

  PGconn *conn_;
  std::string owned_;
  const char *sql_;
  std::vector<std::string> params_;
  PGresult *res_ = nullptr;
  int row_ = -1;
};

inline User read_user(Query &q, int col) {
  User user;
  user.id = q.i64(col);
  user.username = q.text(col + 1);
  user.display_name = q.text(col + 2);
  return user;
}

inline Message read_message(Query &q) {
  Message message;
  message.id = q.i64(0);
  message.dialog_id = q.i64(1);
  message.sender_id = q.i64(2);
  message.text = q.text(3);
  message.created_at = q.i64(4);
  message.kind = q.text(5);
  if (message.kind.empty()) message.kind = "text";
  message.reply_to = q.i64(6);
  message.quote = q.text(7);
  message.file_id = q.i64(8);
  message.silent = pg_flag(q.text(9));
  message.scheduled_at = q.i64(10);
  message.edited_at = q.i64(11);
  message.deleted = pg_flag(q.text(12));
  message.entities = q.text(13);
  message.views = q.i64(14);
  message.expires_at = q.i64(15);
  message.topic_id = q.i64(16);
  message.transcript = q.text(17);
  message.translation = q.text(18);
  message.translation_lang = q.text(19);
  message.no_forward = pg_flag(q.text(20));
  return message;
}

inline std::string message_visible_sql() {
  return "deleted = FALSE AND (scheduled_at = 0 OR sent_at > 0) AND (expires_at = 0 OR expires_at > $NOW)";
}

}
