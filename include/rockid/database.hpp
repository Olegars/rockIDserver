#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

extern "C" {
struct sqlite3;
}

namespace rockid {

struct User {
  int64_t id = 0;
  std::string username;
  std::string display_name;
};

struct Message {
  int64_t id = 0;
  int64_t dialog_id = 0;
  int64_t sender_id = 0;
  std::string text;
  int64_t created_at = 0;
};

struct Member {
  int64_t user_id = 0;
  std::string role;
};

struct DialogInfo {
  int64_t id = 0;
  std::string kind;
  std::string title;
  int64_t member_count = 0;
  std::optional<User> peer;
  std::optional<Message> last;
  int64_t unread = 0;
};

struct DialogHead {
  int64_t id = 0;
  std::string kind;
  std::string title;
};

class Database {
 public:
  explicit Database(const std::string &path);
  ~Database();

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  std::optional<User> create_user(const std::string &username, const std::string &password,
                                  const std::string &display_name);
  std::optional<User> authenticate(const std::string &username, const std::string &password);
  std::optional<User> user_by_id(int64_t id);
  std::optional<User> user_by_username(const std::string &username);
  std::vector<User> search_users(const std::string &query, int64_t exclude, int limit);

  std::string create_session(int64_t user_id);
  std::optional<int64_t> user_id_by_token(const std::string &token);
  void delete_session(const std::string &token);

  void add_contact(int64_t owner, int64_t peer);
  std::vector<User> list_contacts(int64_t owner);

  int64_t create_private(int64_t a, int64_t b);
  int64_t create_group(int64_t owner, const std::string &title, const std::vector<int64_t> &members);
  bool add_member(int64_t dialog_id, int64_t user_id);
  std::optional<DialogHead> find_dialog(int64_t dialog_id);
  std::optional<std::string> member_role(int64_t dialog_id, int64_t user_id);
  std::vector<int64_t> member_ids(int64_t dialog_id);
  std::vector<Member> members(int64_t dialog_id);
  std::vector<int64_t> peer_user_ids(int64_t user_id);
  std::optional<DialogInfo> dialog_for(int64_t viewer, int64_t dialog_id);
  std::vector<DialogInfo> list_dialogs(int64_t viewer);

  Message add_message(int64_t dialog_id, int64_t sender_id, const std::string &text);
  std::vector<Message> history(int64_t dialog_id, int64_t before_id, int limit);
  void mark_read(int64_t user_id, int64_t dialog_id, int64_t max_id);

 private:
  struct Tx;
  void exec(const char *sql);
  void migrate();
  DialogInfo load_dialog(int64_t viewer, int64_t dialog_id, const std::string &kind,
                         const std::string &title);

  sqlite3 *db_ = nullptr;
};

}
