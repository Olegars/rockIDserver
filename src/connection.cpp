#include "rockid/connection.hpp"

#include "rockid/app.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#include <unistd.h>

namespace rockid {
namespace {

constexpr uint32_t kMaxFrame = 1u << 20;
constexpr size_t kMaxIn = 2u << 20;
constexpr size_t kMaxOut = 8u << 20;

void append_frame(std::string &out, const std::string &payload) {
  uint32_t n = static_cast<uint32_t>(payload.size());
  char hdr[4] = {static_cast<char>((n >> 24) & 0xFF), static_cast<char>((n >> 16) & 0xFF),
                 static_cast<char>((n >> 8) & 0xFF), static_cast<char>(n & 0xFF)};
  out.append(hdr, 4);
  out.append(payload);
}

}  // namespace

Connection::Connection(int fd) : fd_(fd) {}

Connection::~Connection() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void Connection::send(const Json &message) {
  if (closing_) return;
  std::string payload = dump_json(message);
  if (payload.size() > kMaxFrame) {
    closing_ = true;
    return;
  }
  append_frame(out_, payload);
  if (out_.size() > kMaxOut) closing_ = true;
  if (on_output_) on_output_(*this);
}

bool Connection::on_read() {
  char buf[8192];
  while (true) {
    ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n > 0) {
      in_.append(buf, static_cast<size_t>(n));
      if (in_.size() > kMaxIn) return false;
      continue;
    }
    if (n == 0) return false;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    return false;
  }
  return pull_frames();
}

bool Connection::on_write() {
  while (out_pos_ < out_.size()) {
    ssize_t n = ::write(fd_, out_.data() + out_pos_, out_.size() - out_pos_);
    if (n > 0) {
      out_pos_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    return false;
  }
  out_.clear();
  out_pos_ = 0;
  return true;
}

bool Connection::pull_frames() {
  while (in_.size() >= 4) {
    uint32_t b0 = static_cast<unsigned char>(in_[0]);
    uint32_t b1 = static_cast<unsigned char>(in_[1]);
    uint32_t b2 = static_cast<unsigned char>(in_[2]);
    uint32_t b3 = static_cast<unsigned char>(in_[3]);
    uint32_t len = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    if (len > kMaxFrame) return false;
    if (in_.size() < 4u + len) return true;
    std::string payload = in_.substr(4, len);
    in_.erase(0, 4u + len);
    handle_payload(payload);
    if (closing_) return false;
  }
  return true;
}

void Connection::handle_payload(const std::string &payload) {
  Json req;
  try {
    req = parse_json(payload);
  } catch (const JsonError &) {
    send(error_response(Json::nul(), 400, "BAD_JSON"));
    return;
  }
  if (!app_) return;
  app_->handle(*this, req);
}

}
