#include "rockid/connection.hpp"

#include "rockid/app.hpp"

#include <zlib.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include <unistd.h>

namespace rockid {
namespace {

constexpr uint32_t kMaxFrame = 1u << 20;
constexpr size_t kMaxIn = 2u << 20;
constexpr size_t kMaxOut = 8u << 20;

std::string zlib_process(const std::string &in, bool compress) {
  z_stream strm{};
  if (compress) {
    if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) return {};
  } else if (inflateInit(&strm) != Z_OK) {
    return {};
  }
  strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
  strm.avail_in = static_cast<uInt>(in.size());
  std::string out;
  char buf[8192];
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    strm.next_out = reinterpret_cast<Bytef *>(buf);
    strm.avail_out = sizeof buf;
    rc = compress ? deflate(&strm, Z_FINISH) : inflate(&strm, Z_FINISH);
    if (rc != Z_OK && rc != Z_STREAM_END) {
      out.clear();
      break;
    }
    out.append(buf, sizeof buf - strm.avail_out);
    if (out.size() > kMaxFrame) {
      out.clear();
      break;
    }
  }
  if (compress) deflateEnd(&strm);
  else inflateEnd(&strm);
  return out;
}

void append_frame(std::string &out, const std::string &payload, bool compress) {
  std::string body = payload;
  uint32_t flags = 0;
  if (compress) {
    std::string packed = zlib_process(payload, true);
    if (!packed.empty() && packed.size() < payload.size()) {
      body = std::move(packed);
      flags = 0x80000000u;
    }
  }
  uint32_t n = static_cast<uint32_t>(body.size()) | flags;
  char hdr[4] = {static_cast<char>((n >> 24) & 0xFF), static_cast<char>((n >> 16) & 0xFF),
                 static_cast<char>((n >> 8) & 0xFF), static_cast<char>(n & 0xFF)};
  out.append(hdr, 4);
  out.append(body);
}

}  // namespace

Connection::Connection(int fd) : fd_(fd) {}

Connection::~Connection() {
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void Connection::enable_tls(SSL_CTX *ctx) {
  ssl_ = SSL_new(ctx);
  if (!ssl_) {
    closing_ = true;
    return;
  }
  SSL_set_fd(ssl_, fd_);
  SSL_set_accept_state(ssl_);
}

ssize_t Connection::read_some(char *buf, size_t n) {
  if (!ssl_) return ::read(fd_, buf, n);
  int got = SSL_read(ssl_, buf, static_cast<int>(n));
  if (got > 0) return got;
  int err = SSL_get_error(ssl_, got);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    errno = EAGAIN;
    return -1;
  }
  return 0;
}

ssize_t Connection::write_some(const char *buf, size_t n) {
  if (!ssl_) return ::write(fd_, buf, n);
  int put = SSL_write(ssl_, buf, static_cast<int>(n));
  if (put > 0) return put;
  int err = SSL_get_error(ssl_, put);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    errno = EAGAIN;
    return -1;
  }
  return -1;
}

bool Connection::handshake() {
  if (!ssl_ || SSL_is_init_finished(ssl_)) return true;
  int rc = SSL_accept(ssl_);
  if (rc == 1) return true;
  int err = SSL_get_error(ssl_, rc);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return true;
  return false;
}

void Connection::send(const Json &message) {
  if (closing_) return;
  std::string payload = dump_json(message);
  if (payload.size() > kMaxFrame) {
    closing_ = true;
    return;
  }
  append_frame(out_, payload, compress_);
  if (out_.size() > kMaxOut) closing_ = true;
  if (on_output_) on_output_(*this);
}

bool Connection::on_read() {
  if (!handshake()) return false;
  char buf[8192];
  while (true) {
    ssize_t n = read_some(buf, sizeof buf);
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
    ssize_t n = write_some(out_.data() + out_pos_, out_.size() - out_pos_);
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
    uint32_t raw = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    bool packed = (raw & 0x80000000u) != 0;
    uint32_t len = raw & 0x7fffffffu;
    if (len > kMaxFrame) return false;
    if (in_.size() < 4u + len) return true;
    std::string payload = in_.substr(4, len);
    in_.erase(0, 4u + len);
    if (packed) {
      payload = zlib_process(payload, false);
      if (payload.empty()) return false;
    }
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
