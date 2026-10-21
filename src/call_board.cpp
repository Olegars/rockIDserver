#include "call_board.hpp"

#include "paths.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rockid {
namespace {

constexpr int64_t kRingMs = 45000;
constexpr int64_t kMaxQueue = 8 * 1024 * 1024;
constexpr int kAudioFrames = 50;
constexpr int64_t kAudioBytes = 48 * 1024;
constexpr int kVideoFrames = 8;
constexpr int64_t kVideoBytes = 256 * 1024;

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool allow(int64_t &window, int &frames, int64_t &bytes, int64_t now, int max_frames, int64_t max_bytes,
           size_t incoming) {
  if (window == 0 || now - window >= 1000) {
    window = now;
    frames = 0;
    bytes = 0;
  }
  if (frames + 1 > max_frames || bytes + static_cast<int64_t>(incoming) > max_bytes) return false;
  ++frames;
  bytes += static_cast<int64_t>(incoming);
  return true;
}

std::string record_path(int64_t call_id) {
  return files_dir() + "/call-" + std::to_string(call_id) + ".ridm";
}

}  // namespace

struct CallBoard::State {
  std::unordered_map<int64_t, Room> rooms;
  std::unordered_map<int64_t, std::unordered_set<int64_t>> by_user;
  std::mutex mu;
  std::condition_variable cv;
  std::condition_variable done_cv;
  std::deque<Job> queue;
  int64_t queued_bytes = 0;
  uint64_t barrier_seq = 0;
  uint64_t done_id = 0;
  bool stop = false;
  std::thread writer;
};

CallBoard::CallBoard() : state_(new State) {
  state_->writer = std::thread([this] { write_loop(); });
}

CallBoard::~CallBoard() {
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->stop = true;
  }
  state_->cv.notify_all();
  if (state_->writer.joinable()) state_->writer.join();
  delete state_;
}

LiveRoom CallBoard::snap(int64_t id, const Room &room, bool active) const {
  LiveRoom out;
  out.id = id;
  out.dialog_id = room.dialog_id;
  out.creator_id = room.creator_id;
  out.recorder_id = room.recorder_id;
  out.active = active;
  out.recording = room.recording;
  out.peers.reserve(room.peers.size());
  for (const auto &peer : room.peers) {
    LivePeer row;
    row.user_id = peer.first;
    row.muted = peer.second.muted;
    row.video = peer.second.video;
    row.screen = peer.second.screen;
    out.peers.push_back(row);
  }
  return out;
}

void CallBoard::unindex(int64_t user_id, int64_t call_id) {
  auto it = state_->by_user.find(user_id);
  if (it == state_->by_user.end()) return;
  it->second.erase(call_id);
  if (it->second.empty()) state_->by_user.erase(it);
}

void CallBoard::open(int64_t id, int64_t dialog_id, int64_t creator, int64_t now_ms_value) {
  Room room;
  room.dialog_id = dialog_id;
  room.creator_id = creator;
  room.ring_until = now_ms_value + kRingMs;
  room.peers.push_back({creator, PeerState{}});
  state_->rooms[id] = std::move(room);
  state_->by_user[creator].insert(id);
}

bool CallBoard::join(int64_t id, int64_t user_id) {
  auto it = state_->rooms.find(id);
  if (it == state_->rooms.end()) return false;
  for (const auto &peer : it->second.peers) {
    if (peer.first == user_id) return true;
  }
  it->second.peers.push_back({user_id, PeerState{}});
  state_->by_user[user_id].insert(id);
  if (it->second.peers.size() >= 2) it->second.ring_until = 0;
  return true;
}

bool CallBoard::set_state(int64_t id, int64_t user_id, bool muted, bool video, bool screen) {
  auto it = state_->rooms.find(id);
  if (it == state_->rooms.end()) return false;
  for (auto &peer : it->second.peers) {
    if (peer.first != user_id) continue;
    peer.second.muted = muted;
    peer.second.video = video;
    peer.second.screen = screen;
    return true;
  }
  return false;
}

bool CallBoard::set_recording(int64_t id, int64_t user_id, bool on) {
  auto it = state_->rooms.find(id);
  if (it == state_->rooms.end()) return false;
  bool member = false;
  for (const auto &peer : it->second.peers) {
    if (peer.first == user_id) member = true;
  }
  if (!member) return false;
  it->second.recording = on;
  if (on) {
    it->second.recorder_id = user_id;
    Job job;
    job.call_id = id;
    job.kind = 0;
    enqueue(std::move(job));
    return true;
  }
  sync();
  return true;
}

LiveRoom CallBoard::leave(int64_t id, int64_t user_id) {
  auto it = state_->rooms.find(id);
  if (it == state_->rooms.end()) return {};
  auto &peers = it->second.peers;
  bool found = false;
  for (auto peer = peers.begin(); peer != peers.end(); ++peer) {
    if (peer->first != user_id) continue;
    peers.erase(peer);
    found = true;
    break;
  }
  if (!found) return {};
  unindex(user_id, id);
  bool ended = peers.empty();
  LiveRoom out = snap(id, it->second, !ended);
  if (ended) state_->rooms.erase(it);
  else out.active = true;
  return out;
}

LiveRoom CallBoard::take(int64_t id) {
  auto it = state_->rooms.find(id);
  if (it == state_->rooms.end()) return {};
  LiveRoom out = snap(id, it->second, false);
  for (const auto &peer : it->second.peers) unindex(peer.first, id);
  state_->rooms.erase(it);
  return out;
}

LiveRoom CallBoard::view(int64_t id) const {
  auto it = state_->rooms.find(id);
  if (it == state_->rooms.end()) return {};
  return snap(id, it->second, true);
}

CallBoard::Admit CallBoard::admit(int64_t id, int64_t user_id, int record_kind, const std::string &payload,
                                  uint32_t &seq, int64_t &ts_ms) {
  auto it = state_->rooms.find(id);
  if (it == state_->rooms.end()) return Admit::Absent;
  bool audio = record_kind == 1;
  for (auto &peer : it->second.peers) {
    if (peer.first != user_id) continue;
    if (audio && peer.second.muted) return Admit::Muted;
    int64_t now = now_ms();
    bool ok = audio ? allow(peer.second.audio_window, peer.second.audio_frames, peer.second.audio_bytes, now,
                            kAudioFrames, kAudioBytes, payload.size())
                    : allow(peer.second.video_window, peer.second.video_frames, peer.second.video_bytes, now,
                            kVideoFrames, kVideoBytes, payload.size());
    if (!ok) return Admit::Limited;
    seq = ++peer.second.seq;
    ts_ms = now;
    if (it->second.recording && !payload.empty()) {
      Job job;
      job.call_id = id;
      job.kind = record_kind;
      job.bytes = payload;
      enqueue(std::move(job));
    }
    return Admit::Ok;
  }
  return Admit::Absent;
}

std::vector<int64_t> CallBoard::calls_of(int64_t user_id) const {
  std::vector<int64_t> ids;
  auto it = state_->by_user.find(user_id);
  if (it == state_->by_user.end()) return ids;
  ids.assign(it->second.begin(), it->second.end());
  return ids;
}

std::vector<int64_t> CallBoard::expired(int64_t now_ms_value) const {
  std::vector<int64_t> ids;
  for (const auto &room : state_->rooms) {
    if (room.second.ring_until > 0 && now_ms_value >= room.second.ring_until && room.second.peers.size() < 2)
      ids.push_back(room.first);
  }
  return ids;
}

void CallBoard::enqueue(Job job) {
  std::lock_guard<std::mutex> lock(state_->mu);
  if (job.kind > 0 && state_->queued_bytes > kMaxQueue) return;
  state_->queued_bytes += static_cast<int64_t>(job.bytes.size());
  state_->queue.push_back(std::move(job));
  state_->cv.notify_one();
}

void CallBoard::sync() {
  uint64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    id = ++state_->barrier_seq;
    Job job;
    job.kind = -1;
    job.barrier = id;
    state_->queue.push_back(std::move(job));
  }
  state_->cv.notify_one();
  std::unique_lock<std::mutex> lock(state_->mu);
  state_->done_cv.wait_for(lock, std::chrono::seconds(2), [&] { return state_->done_id >= id || state_->stop; });
}

void CallBoard::write_loop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(state_->mu);
      state_->cv.wait(lock, [&] { return state_->stop || !state_->queue.empty(); });
      if (state_->stop && state_->queue.empty()) return;
      job = std::move(state_->queue.front());
      state_->queue.pop_front();
      state_->queued_bytes -= static_cast<int64_t>(job.bytes.size());
      if (state_->queued_bytes < 0) state_->queued_bytes = 0;
    }
    if (job.kind < 0) {
      std::lock_guard<std::mutex> lock(state_->mu);
      state_->done_id = job.barrier;
      state_->done_cv.notify_all();
      continue;
    }
    write_job(job);
  }
}

void CallBoard::write_job(const Job &job) {
  std::error_code ec;
  std::filesystem::create_directories(files_dir(), ec);
  std::string path = record_path(job.call_id);
  if (job.kind == 0) {
    std::filesystem::remove(path, ec);
    return;
  }
  if (job.bytes.empty()) return;
  bool fresh = !std::filesystem::exists(path);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return;
  if (fresh) {
    char magic[4] = {'R', 'I', 'D', 'M'};
    out.write(magic, 4);
  }
  unsigned char tag = static_cast<unsigned char>(job.kind);
  uint32_t len = static_cast<uint32_t>(job.bytes.size());
  unsigned char hdr[5] = {tag, static_cast<unsigned char>((len >> 24) & 0xFF),
                          static_cast<unsigned char>((len >> 16) & 0xFF),
                          static_cast<unsigned char>((len >> 8) & 0xFF), static_cast<unsigned char>(len & 0xFF)};
  out.write(reinterpret_cast<char *>(hdr), 5);
  out.write(job.bytes.data(), static_cast<std::streamsize>(job.bytes.size()));
}

}
