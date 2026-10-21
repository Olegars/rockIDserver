#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rockid {

struct LivePeer {
  int64_t user_id = 0;
  bool muted = false;
  bool video = false;
  bool screen = false;
};

struct LiveRoom {
  int64_t id = 0;
  int64_t dialog_id = 0;
  int64_t creator_id = 0;
  int64_t recorder_id = 0;
  bool active = false;
  bool recording = false;
  std::vector<LivePeer> peers;
};

// Состав эфира в памяти процесса. Звук группы не смешивается:
// каждому участнику уходит чужая дорожка как есть.
class CallBoard {
 public:
  CallBoard();
  ~CallBoard();

  CallBoard(const CallBoard &) = delete;
  CallBoard &operator=(const CallBoard &) = delete;

  void open(int64_t id, int64_t dialog_id, int64_t creator, int64_t now_ms);
  bool join(int64_t id, int64_t user_id);
  bool set_state(int64_t id, int64_t user_id, bool muted, bool video, bool screen);
  // false ждёт, пока поток записи допишет файл.
  bool set_recording(int64_t id, int64_t user_id, bool on);
  LiveRoom leave(int64_t id, int64_t user_id);
  LiveRoom take(int64_t id);
  LiveRoom view(int64_t id) const;

  enum class Admit { Ok, Absent, Muted, Limited };
  // record_kind: 1 звук, 2 видео, 3 экран. Кусок на диск ставится в очередь, не пишется здесь.
  Admit admit(int64_t id, int64_t user_id, int record_kind, const std::string &payload, uint32_t &seq,
              int64_t &ts_ms);

  std::vector<int64_t> calls_of(int64_t user_id) const;
  std::vector<int64_t> expired(int64_t now_ms) const;
  void sync();

 private:
  struct PeerState {
    bool muted = false;
    bool video = false;
    bool screen = false;
    uint32_t seq = 0;
    int64_t audio_window = 0;
    int audio_frames = 0;
    int64_t audio_bytes = 0;
    int64_t video_window = 0;
    int video_frames = 0;
    int64_t video_bytes = 0;
  };
  struct Room {
    int64_t dialog_id = 0;
    int64_t creator_id = 0;
    int64_t recorder_id = 0;
    bool recording = false;
    int64_t ring_until = 0;
    std::vector<std::pair<int64_t, PeerState>> peers;
  };
  struct Job {
    int64_t call_id = 0;
    int kind = 0;
    std::string bytes;
    uint64_t barrier = 0;
  };

  LiveRoom snap(int64_t id, const Room &room, bool active) const;
  void unindex(int64_t user_id, int64_t call_id);
  void enqueue(Job job);
  void write_loop();
  void write_job(const Job &job);

  struct State;
  State *state_;
};

}
