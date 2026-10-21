#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern "C" {
struct pg_conn;
}

namespace rockid {

struct User {
  int64_t id = 0;
  std::string username;
  std::string display_name;
};

struct Profile : User {
  std::string phone;
  std::string bio;
  std::string privacy_phone = "everybody";
  std::string privacy_last_seen = "everybody";
  std::string privacy_photo = "everybody";
  std::string preferences;
  int64_t photo_file_id = 0;
  int64_t premium_until = 0;
  int64_t last_seen_at = 0;
  bool is_bot = false;
  bool cloud_password = false;
};

struct ReactionCount {
  std::string emoji;
  int64_t count = 0;
  bool mine = false;
};

struct Message {
  int64_t id = 0;
  int64_t dialog_id = 0;
  int64_t sender_id = 0;
  std::string text;
  int64_t created_at = 0;
  std::string kind = "text";
  int64_t reply_to = 0;
  std::string quote;
  int64_t file_id = 0;
  bool silent = false;
  int64_t scheduled_at = 0;
  int64_t edited_at = 0;
  bool deleted = false;
  std::string entities;
  int64_t views = 0;
  int64_t expires_at = 0;
  int64_t topic_id = 0;
  std::string transcript;
  std::string translation;
  std::string translation_lang;
  bool no_forward = false;
  std::vector<ReactionCount> reactions;
};

struct Outgoing {
  std::string kind = "text";
  std::string text;
  std::string quote;
  std::string entities;
  int64_t reply_to = 0;
  int64_t file_id = 0;
  bool silent = false;
  int64_t scheduled_at = 0;
  int64_t expires_at = 0;
  int64_t topic_id = 0;
  bool no_forward = false;
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
  bool archived = false;
  int pin_pos = 0;
  int64_t muted_until = 0;
  bool no_forwards = false;
  bool forum = false;
  int ttl_seconds = 0;
  int64_t linked_id = 0;
};

struct DialogHead {
  int64_t id = 0;
  std::string kind;
  std::string title;
  bool no_forwards = false;
  bool forum = false;
  int ttl_seconds = 0;
  int64_t linked_id = 0;
};

struct Membership {
  std::string role;
  bool can_post = false;
  bool can_delete = false;
  bool can_ban = false;
  bool can_pin = false;
  bool can_manage = false;
};

struct SessionInfo {
  int64_t id = 0;
  std::string device;
  std::string platform;
  std::string local_lock;
  int64_t created_at = 0;
  int64_t last_seen_at = 0;
  bool current = false;
  std::string token;
};

struct FileInfo {
  int64_t id = 0;
  int64_t owner_id = 0;
  std::string name;
  std::string mime;
  int64_t size = 0;
  int64_t received = 0;
  bool ready = false;
};

struct Topic {
  int64_t id = 0;
  int64_t dialog_id = 0;
  std::string title;
  int64_t created_by = 0;
};

struct Story {
  int64_t id = 0;
  int64_t user_id = 0;
  int64_t file_id = 0;
  std::string caption;
  std::string privacy;
  bool no_screenshots = false;
  int64_t expires_at = 0;
  int64_t created_at = 0;
  int64_t views = 0;
};

struct StickerPack {
  int64_t id = 0;
  int64_t owner_id = 0;
  std::string title;
  std::string short_name;
  bool premium = false;
};

struct Sticker {
  int64_t id = 0;
  int64_t pack_id = 0;
  int64_t file_id = 0;
  std::string emoji;
};

struct Folder {
  int64_t id = 0;
  std::string title;
  int position = 0;
  std::vector<int64_t> dialog_ids;
};

struct CallInfo {
  int64_t id = 0;
  int64_t dialog_id = 0;
  int64_t creator_id = 0;
  bool active = false;
  bool recording = false;
};

struct CallPeer {
  int64_t user_id = 0;
  bool muted = false;
  bool video = false;
  bool screen = false;
};

struct Invoice {
  int64_t id = 0;
  int64_t bot_id = 0;
  int64_t user_id = 0;
  std::string title;
  int64_t amount = 0;
  std::string currency;
  std::string purpose;
  std::string status;
  std::string provider_ref;
  std::string pay_url;
};

struct MiniApp {
  int64_t id = 0;
  int64_t owner_id = 0;
  std::string title;
  std::string short_name;
  std::string url;
  std::string content;
};

struct SavedTab {
  int64_t id = 0;
  std::string title;
};

struct DueBatch {
  std::vector<Message> ready;
  std::vector<Message> expired;
};

class Database {
 public:
  explicit Database(const std::string &conninfo);
  ~Database();

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  std::optional<User> create_user(const std::string &username, const std::string &password,
                                  const std::string &display_name);
  std::optional<User> authenticate(const std::string &username, const std::string &password);
  std::optional<User> user_by_id(int64_t id);
  std::optional<User> user_by_username(const std::string &username);
  std::vector<User> search_users(const std::string &query, int64_t exclude, int limit);

  std::string create_session(int64_t user_id, const std::string &device = {},
                             const std::string &platform = {});
  std::optional<int64_t> user_id_by_token(const std::string &token);
  void delete_session(const std::string &token);
  void delete_session_id(int64_t user_id, int64_t session_id);
  std::vector<SessionInfo> list_sessions(int64_t user_id, const std::string &current_token);
  void set_local_lock(const std::string &token, const std::string &kind);
  std::optional<std::string> token_by_session(int64_t user_id, int64_t session_id);

  void add_contact(int64_t owner, int64_t peer);
  bool is_contact(int64_t owner, int64_t peer);
  std::vector<User> list_contacts(int64_t owner);
  bool can_see(int64_t viewer, int64_t owner, const std::string &level);
  void touch_last_seen(int64_t user_id);

  std::optional<Profile> profile(int64_t id);
  void update_profile(int64_t id, const std::string &display_name, const std::string &phone,
                      const std::string &bio, int64_t photo_file_id, bool set_photo);
  void set_privacy(int64_t id, const std::string &phone, const std::string &last_seen,
                   const std::string &photo);
  void set_preferences(int64_t id, const std::string &preferences);
  bool premium(int64_t id);
  void grant_premium_days(int64_t id, int days);
  int64_t channel_subscriptions(int64_t user_id);
  int64_t folder_count(int64_t user_id);
  int64_t pin_count(int64_t user_id);

  void set_cloud_password(int64_t id, const std::string &password);
  void clear_cloud_password(int64_t id, const std::string &password);
  bool has_cloud_password(int64_t id);
  bool check_cloud_password(int64_t id, const std::string &password);
  std::string create_challenge(int64_t user_id);
  std::optional<int64_t> take_challenge(const std::string &token);

  int64_t create_private(int64_t a, int64_t b);
  int64_t create_group(int64_t owner, const std::string &title, const std::vector<int64_t> &members,
                       bool forum = false);
  int64_t create_channel(int64_t owner, const std::string &title);
  int64_t create_secret(int64_t a, int64_t b, int ttl_seconds);
  int64_t saved_dialog(int64_t user_id);
  bool add_member(int64_t dialog_id, int64_t user_id);
  bool add_subscriber(int64_t dialog_id, int64_t user_id);
  int64_t member_count(int64_t dialog_id);
  bool set_member_admin(int64_t dialog_id, int64_t user_id, bool can_delete, bool can_ban,
                        bool can_pin, bool can_manage, bool can_post);
  bool kick_member(int64_t dialog_id, int64_t user_id);
  bool set_banned(int64_t dialog_id, int64_t user_id, bool banned);
  void set_dialog_flags(int64_t dialog_id, int64_t user_id, std::optional<bool> archived,
                        std::optional<int64_t> muted_until, std::optional<int> pin_pos);
  void set_secret_key(int64_t dialog_id, int64_t user_id, const std::string &public_key);
  std::vector<std::pair<int64_t, std::string>> secret_keys(int64_t dialog_id);
  void set_discussion(int64_t channel_id, int64_t group_id);
  std::optional<DialogHead> find_dialog(int64_t dialog_id);
  std::optional<std::string> member_role(int64_t dialog_id, int64_t user_id);
  std::optional<Membership> membership(int64_t dialog_id, int64_t user_id);
  std::vector<int64_t> member_ids(int64_t dialog_id);
  std::vector<Member> members(int64_t dialog_id, int limit = 200, int offset = 0);
  std::vector<int64_t> peer_user_ids(int64_t user_id);
  std::vector<int64_t> presence_recipients(int64_t user_id);
  std::optional<DialogInfo> dialog_for(int64_t viewer, int64_t dialog_id);
  std::vector<DialogInfo> list_dialogs(int64_t viewer, bool archived = false);

  int64_t create_topic(int64_t dialog_id, int64_t user_id, const std::string &title);
  std::vector<Topic> list_topics(int64_t dialog_id);
  bool topic_in_dialog(int64_t dialog_id, int64_t topic_id);

  Message add_message(int64_t dialog_id, int64_t sender_id, const Outgoing &message);
  std::optional<Message> message_by_id(int64_t id);
  std::vector<Message> history(int64_t dialog_id, int64_t before_id, int limit, int64_t viewer,
                               int64_t topic_id = 0);
  void mark_read(int64_t user_id, int64_t dialog_id, int64_t max_id);
  Message edit_message(int64_t id, const std::string &text, const std::string &entities);
  void delete_message(int64_t id);
  void add_views(const std::vector<int64_t> &ids);
  bool toggle_reaction(int64_t message_id, int64_t user_id, const std::string &emoji);
  void set_translation(int64_t id, const std::string &lang, const std::string &text);
  void set_transcript(int64_t id, const std::string &text);
  DueBatch take_due(int64_t now);
  void enqueue_bot_updates(int64_t dialog_id, int64_t sender_id, const std::string &payload);
  std::vector<std::string> bot_updates(int64_t bot_id, int limit);

  FileInfo create_file(int64_t owner, const std::string &name, const std::string &mime, int64_t size);
  std::optional<FileInfo> file_info(int64_t id);
  void file_received(int64_t id, int64_t received, bool ready);
  bool can_read_file(int64_t user_id, int64_t file_id);

  Story add_story(int64_t user_id, int64_t file_id, const std::string &caption, const std::string &privacy,
                  bool no_screenshots, int64_t expires_at);
  std::vector<Story> list_stories(int64_t viewer);
  std::optional<Story> story_by_id(int64_t id);
  void view_story(int64_t story_id, int64_t user_id);
  void add_close_friend(int64_t owner, int64_t peer);
  bool is_close_friend(int64_t owner, int64_t peer);

  std::optional<StickerPack> create_pack(int64_t owner, const std::string &title,
                                        const std::string &short_name, bool premium);
  Sticker add_sticker(int64_t pack_id, int64_t file_id, const std::string &emoji);
  std::vector<StickerPack> list_packs();
  std::optional<StickerPack> pack_by_id(int64_t id);
  std::vector<Sticker> stickers_in(int64_t pack_id);
  std::optional<Sticker> sticker_by_file(int64_t file_id);

  Folder create_folder(int64_t user_id, const std::string &title);
  std::vector<Folder> list_folders(int64_t user_id);
  void folder_add(int64_t folder_id, int64_t user_id, int64_t dialog_id);
  void folder_remove(int64_t folder_id, int64_t user_id, int64_t dialog_id);
  bool delete_folder(int64_t folder_id, int64_t user_id);

  SavedTab create_tab(int64_t user_id, const std::string &title);
  std::vector<SavedTab> list_tabs(int64_t user_id);
  void tab_add(int64_t tab_id, int64_t user_id, int64_t message_id);
  void add_tag(int64_t user_id, int64_t message_id, const std::string &tag);
  std::vector<std::string> tags_for(int64_t user_id, int64_t message_id);

  CallInfo start_call(int64_t dialog_id, int64_t creator);
  std::optional<CallInfo> call_by_id(int64_t id);
  bool join_call(int64_t call_id, int64_t user_id);
  void leave_call(int64_t call_id, int64_t user_id);
  void set_call_state(int64_t call_id, int64_t user_id, bool muted, bool video, bool screen);
  void set_call_recording(int64_t call_id, bool recording);
  std::vector<CallPeer> call_peers(int64_t call_id);

  std::optional<User> create_bot(int64_t owner, const std::string &username, const std::string &display,
                                 const std::string &token);
  std::optional<int64_t> bot_by_token(const std::string &token);

  std::optional<MiniApp> create_app(int64_t owner, const std::string &title, const std::string &short_name,
                                   const std::string &url);
  std::optional<MiniApp> app_by_name(const std::string &short_name);
  std::vector<MiniApp> list_apps();

  Invoice create_invoice(int64_t bot_id, int64_t user_id, const std::string &title, int64_t amount,
                         const std::string &currency, const std::string &purpose,
                         const std::string &pay_url = {});
  std::optional<Invoice> invoice_by_id(int64_t id);
  std::vector<Invoice> list_invoices(int64_t user_id);
  bool set_invoice_status(int64_t id, int64_t bot_id, const std::string &status,
                          const std::string &provider_ref);

  bool lock_instance();
  void set_user_online(int64_t user_id, bool online);
  void queue_push(int64_t user_id, const std::string &payload);
  std::vector<std::string> take_pushes(int64_t user_id);
  int64_t claim_device(const std::string &device_id, int64_t user_id, bool premium);
  void publish_key(int64_t user_id, const std::string &public_key);
  std::vector<std::pair<int64_t, std::string>> keys_for_dialog(int64_t dialog_id);
  bool set_app_content(int64_t owner, const std::string &short_name, const std::string &html);
  void update_file_meta(int64_t id, const std::string &mime, int64_t size);
  bool call_recording(int64_t call_id);

 private:
  struct Tx {
    Database &db;
    bool open = false;
    explicit Tx(Database &database);
    void commit();
    ~Tx();
  };
  void exec(const char *sql);
  void migrate();
  DialogInfo load_dialog(int64_t viewer, int64_t dialog_id, const std::string &kind,
                         const std::string &title);

  pg_conn *db_ = nullptr;
};

}
