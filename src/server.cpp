#include "rockid/server.hpp"

#include "rockid/app.hpp"
#include "rockid/connection.hpp"
#include "rockid/log.hpp"

#include <openssl/sha.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace rockid {
namespace {

int set_nonblock(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

int Server::open_listen(int port) {
  int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  bool v6 = fd >= 0;
  if (fd < 0) fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket() failed");
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (v6) {
    int off = 0;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
      ::close(fd);
      v6 = false;
      fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) throw std::runtime_error("socket() failed");
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    }
  }
  if (!v6) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
      int err = errno;
      ::close(fd);
      throw std::runtime_error(std::string("bind() failed: ") + std::strerror(err));
    }
  }
  if (set_nonblock(fd) < 0 || ::listen(fd, 128) < 0) {
    int err = errno;
    ::close(fd);
    throw std::runtime_error(std::string("listen() failed: ") + std::strerror(err));
  }
  ipv6_ = v6;
  return fd;
}

Server::Server(int port, int http_port, int media_port)
    : port_(port), http_port_(http_port), media_port_(media_port) {
  listen_fd_ = open_listen(port_);
  epoll_fd_ = ::epoll_create1(0);
  if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1() failed");
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd_;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0)
    throw std::runtime_error("epoll_ctl() failed");
  if (http_port_ > 0) {
    try {
      http_fd_ = open_listen(http_port_);
      ev.data.fd = http_fd_;
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, http_fd_, &ev) < 0)
        throw std::runtime_error("epoll_ctl http failed");
    } catch (const std::exception &ex) {
      log_error(std::string("http listen failed: ") + ex.what());
      http_fd_ = -1;
    }
  }
  if (media_port_ > 0) {
    try {
      media_fd_ = open_listen(media_port_);
      ev.data.fd = media_fd_;
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, media_fd_, &ev) < 0)
        throw std::runtime_error("epoll_ctl media failed");
    } catch (const std::exception &ex) {
      log_error(std::string("media listen failed: ") + ex.what());
      if (media_fd_ >= 0) ::close(media_fd_);
      media_fd_ = -1;
    }
  }
}

bool Server::enable_tls(const std::string &cert, const std::string &key) {
  tls_ = SSL_CTX_new(TLS_server_method());
  if (!tls_) return false;
  if (SSL_CTX_use_certificate_file(tls_, cert.c_str(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_PrivateKey_file(tls_, key.c_str(), SSL_FILETYPE_PEM) != 1) {
    SSL_CTX_free(tls_);
    tls_ = nullptr;
    return false;
  }
  return true;
}

Server::~Server() {
  conns_.clear();
  for (const auto &link : media_) ::close(link.first);
  media_.clear();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (http_fd_ >= 0) ::close(http_fd_);
  if (media_fd_ >= 0) ::close(media_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  if (tls_) SSL_CTX_free(tls_);
}

void Server::request_stop() { stop_.store(true); }

void Server::run() {
  log_info(std::string("listening on ") + (ipv6_ ? "[::]:" : "0.0.0.0:") + std::to_string(port_) +
           (tls_ ? " tls" : ""));
  if (http_fd_ >= 0) log_info("http on " + std::to_string(http_port_));
  if (media_fd_ >= 0) log_info("media on " + std::to_string(media_port_));
  while (!stop_.load()) {
    epoll_event events[64];
    int n = ::epoll_wait(epoll_fd_, events, 64, 500);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("epoll_wait() failed: ") + std::strerror(errno));
    }
    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      uint32_t ev = events[i].events;
      if (fd == listen_fd_) {
        accept_all(listen_fd_, 0);
        continue;
      }
      if (fd == http_fd_) {
        accept_all(http_fd_, 1);
        continue;
      }
      if (fd == media_fd_) {
        accept_all(media_fd_, 2);
        continue;
      }
      if (http_buf_.count(fd)) {
        on_http(fd);
        continue;
      }
      if (media_.count(fd)) {
        on_media(fd, ev);
        continue;
      }
      auto it = conns_.find(fd);
      if (it == conns_.end()) continue;
      Connection *conn = it->second.get();
      bool ok = true;
      if (ev & EPOLLIN) ok = conn->on_read();
      if (ok && conn->want_write()) ok = conn->on_write();
      if (!ok || conn->closing() || (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)))
        close_conn(fd);
      else
        update_events(*conn);
    }
    std::vector<int> dead;
    for (const auto &kv : conns_) {
      if (kv.second->closing()) dead.push_back(kv.first);
    }
    for (int fd : dead) close_conn(fd);
    if (app_) app_->flush_due();
  }
  log_info("server stopped");
}

void Server::accept_all(int listen_fd, int kind) {
  while (true) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    int fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&addr), &len);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      log_error(std::string("accept() failed: ") + std::strerror(errno));
      return;
    }
    if (set_nonblock(fd) < 0) {
      ::close(fd);
      continue;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (kind == 1) {
      http_buf_[fd] = {};
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        http_buf_.erase(fd);
        ::close(fd);
      }
      continue;
    }
    if (kind == 2) {
      media_[fd] = MediaLink{};
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        media_.erase(fd);
        ::close(fd);
      }
      continue;
    }

    auto conn = std::make_unique<Connection>(fd);
    Connection *raw = conn.get();
    raw->set_app(app_);
    raw->set_on_output([this](Connection &c) { update_events(c); });
    if (tls_) raw->enable_tls(tls_);
    conns_.emplace(fd, std::move(conn));
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
      log_error("epoll_ctl add failed");
      conns_.erase(fd);
      continue;
    }

    char ip[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET6) {
      ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(&addr)->sin6_addr, ip, sizeof ip);
    } else {
      ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&addr)->sin_addr, ip, sizeof ip);
    }
    log_info(std::string("connected fd=") + std::to_string(fd) + " from " + ip);
  }
}

void Server::update_events(Connection &connection) {
  if (conns_.find(connection.fd()) == conns_.end()) return;
  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLRDHUP;
  if (connection.want_write()) ev.events |= EPOLLOUT;
  ev.data.fd = connection.fd();
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, connection.fd(), &ev);
}

void Server::close_conn(int fd) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  Connection *conn = it->second.get();
  unbind(conn);
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  log_info("closed fd=" + std::to_string(fd));
  conns_.erase(it);
}

void Server::bind_user(Connection *connection, int64_t user_id) {
  if (!connection) return;
  if (connection->user_id() == user_id) return;
  if (connection->user_id() != 0) unbind(connection);
  bool was = online(user_id);
  connection->set_user_id(user_id);
  online_[user_id].push_back(connection);
  if (!was && presence_) presence_(user_id, true);
}

void Server::unbind(Connection *connection) {
  if (!connection) return;
  int64_t user_id = connection->user_id();
  if (user_id == 0) return;
  auto it = online_.find(user_id);
  if (it != online_.end()) {
    auto &list = it->second;
    list.erase(std::remove(list.begin(), list.end(), connection), list.end());
    if (list.empty()) online_.erase(it);
  }
  connection->set_user_id(0);
  connection->set_token({});
  if (!online(user_id) && presence_) presence_(user_id, false);
}

void Server::push_user(int64_t user_id, const Json &message, Connection *except) {
  auto it = online_.find(user_id);
  if (it == online_.end() || it->second.empty()) {
    if (offline_) offline_(user_id, message);
    return;
  }
  /* Copy: send() may close other connections only by flag, but be safe if the vector changes. */
  std::vector<Connection *> targets = it->second;
  int sent = 0;
  for (Connection *conn : targets) {
    if (conn == except) continue;
    conn->send(message);
    ++sent;
  }
  if (sent == 0 && offline_ && !(targets.size() == 1 && targets[0] == except)) offline_(user_id, message);
}

void Server::push_many(const std::vector<int64_t> &user_ids, const Json &message, Connection *except) {
  for (int64_t user_id : user_ids) push_user(user_id, message, except);
}

bool Server::online(int64_t user_id) const {
  auto it = online_.find(user_id);
  return it != online_.end() && !it->second.empty();
}

void Server::close_token(int64_t user_id, const std::string &token) {
  auto it = online_.find(user_id);
  if (it == online_.end()) return;
  for (Connection *conn : it->second) {
    if (conn && conn->token() == token) conn->request_close();
  }
}

void Server::on_http(int fd) {
  char buf[4096];
  ssize_t n = ::read(fd, buf, sizeof buf);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    http_buf_.erase(fd);
    return;
  }
  if (n < 0) return;
  std::string &in = http_buf_[fd];
  in.append(buf, static_cast<size_t>(n));
  if (in.size() > 262144) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    http_buf_.erase(fd);
    return;
  }
  auto header_end = in.find("\r\n\r\n");
  if (header_end == std::string::npos) return;
  size_t length = 0;
  auto cl = in.find("Content-Length:");
  if (cl == std::string::npos) cl = in.find("content-length:");
  if (cl != std::string::npos && cl < header_end) length = std::strtoul(in.c_str() + cl + 15, nullptr, 10);
  if (in.size() < header_end + 4 + length) return;
  auto line_end = in.find("\r\n");
  std::string http_method;
  std::string path;
  std::string version;
  std::istringstream line(in.substr(0, line_end));
  line >> http_method >> path >> version;
  auto query = path.find('?');
  if (query != std::string::npos) path.erase(query);
  std::string req_body = in.substr(header_end + 4, length);
  std::string response = app_ ? app_->handle_http(http_method, path, req_body)
                              : "HTTP/1.1 500 Internal\r\nConnection: close\r\n\r\n";
  size_t off = 0;
  while (off < response.size()) {
    ssize_t wrote = ::write(fd, response.data() + off, response.size() - off);
    if (wrote > 0) off += static_cast<size_t>(wrote);
    else if (wrote < 0 && errno == EINTR) continue;
    else break;
  }
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  ::close(fd);
  http_buf_.erase(fd);
}

namespace {

void append_be16(std::string &out, uint16_t value) {
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void append_be32(std::string &out, uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void append_be64(std::string &out, int64_t value) {
  uint64_t bits = static_cast<uint64_t>(value);
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>((bits >> shift) & 0xFF));
}

uint16_t read_be16(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t read_be32(const unsigned char *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

int64_t read_be64(const unsigned char *p) {
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
  return static_cast<int64_t>(bits);
}

}  // namespace

bool Server::write_media(int fd) {
  auto it = media_.find(fd);
  if (it == media_.end()) return false;
  MediaLink &link = it->second;
  while (link.out_pos < link.out.size()) {
    ssize_t n = ::send(fd, link.out.data() + link.out_pos, link.out.size() - link.out_pos, MSG_NOSIGNAL);
    if (n > 0) {
      link.out_pos += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    return false;
  }
  if (link.out_pos >= link.out.size()) {
    link.out.clear();
    link.out_pos = 0;
  } else if (link.out_pos > 4096) {
    link.out.erase(0, link.out_pos);
    link.out_pos = 0;
  }
  return true;
}

void Server::close_media(int fd) {
  auto it = media_.find(fd);
  if (it == media_.end()) return;
  if (it->second.ready) {
    auto user = media_user_.find(it->second.user_id);
    if (user != media_user_.end() && user->second == fd) media_user_.erase(user);
  }
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  ::close(fd);
  media_.erase(it);
}

void Server::relay_audio(const std::vector<int64_t> &users, int64_t from, uint32_t seq, int64_t ts,
                         const std::string &bytes) {
  std::string frame;
  frame.push_back(2);
  append_be32(frame, static_cast<uint32_t>(8 + 4 + 8 + bytes.size()));
  append_be64(frame, from);
  append_be32(frame, seq);
  append_be64(frame, ts);
  frame.append(bytes);
  for (int64_t user_id : users) {
    auto who = media_user_.find(user_id);
    if (who == media_user_.end()) continue;
    auto link = media_.find(who->second);
    if (link == media_.end() || !link->second.ready) continue;
    if (link->second.out.size() - link->second.out_pos > 256 * 1024) continue;
    link->second.out.append(frame);
    if (!write_media(who->second)) close_media(who->second);
    else {
      epoll_event ev{};
      ev.events = EPOLLIN | EPOLLRDHUP;
      if (link->second.out_pos < link->second.out.size()) ev.events |= EPOLLOUT;
      ev.data.fd = who->second;
      ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, who->second, &ev);
    }
  }
}

bool Server::read_media(int fd) {
  auto it = media_.find(fd);
  if (it == media_.end()) return false;
  char buf[8192];
  ssize_t n = ::recv(fd, buf, sizeof buf, 0);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) return false;
  if (n < 0) return true;
  MediaLink &link = it->second;
  link.in.append(buf, static_cast<size_t>(n));
  if (link.in.size() > 256 * 1024) return false;
  int handled = 0;
  while (link.in.size() >= 5 && handled < 64) {
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(link.in.data());
    uint32_t len = read_be32(raw + 1);
    if (len > 8192) return false;
    if (link.in.size() < 5 + len) break;
    uint8_t op = raw[0];
    std::string payload(link.in.data() + 5, len);
    link.in.erase(0, 5 + len);
    ++handled;
    if (op == 1) {
      if (link.ready || payload.size() < 10) return false;
      const unsigned char *body = reinterpret_cast<const unsigned char *>(payload.data());
      uint16_t token_len = read_be16(body);
      if (token_len < 1 || token_len > 128 || payload.size() != static_cast<size_t>(2 + token_len + 8))
        return false;
      std::string token(payload.data() + 2, token_len);
      int64_t call_id = read_be64(body + 2 + token_len);
      int64_t user_id = 0;
      int code = app_ ? app_->accept_media(token, call_id, user_id) : 404;
      if (code != 0) {
        std::string err;
        err.push_back(static_cast<char>(255));
        append_be32(err, 2);
        append_be16(err, static_cast<uint16_t>(code));
        link.out.append(err);
        write_media(fd);
        return false;
      }
      auto previous = media_user_.find(user_id);
      if (previous != media_user_.end() && previous->second != fd) close_media(previous->second);
      it = media_.find(fd);
      if (it == media_.end()) return false;
      it->second.ready = true;
      it->second.user_id = user_id;
      it->second.call_id = call_id;
      media_user_[user_id] = fd;
      std::string ok;
      ok.push_back(1);
      append_be32(ok, 8);
      append_be64(ok, user_id);
      it->second.out.append(ok);
      if (!write_media(fd)) return false;
      continue;
    }
    if (!link.ready || !app_) return false;
    if (op == 3) return false;
    if (op != 2) return false;
    if (payload.empty() || payload.size() > 8192) return false;
    uint32_t seq = 0;
    int64_t ts = 0;
    std::vector<int64_t> peers;
    int fate = app_->accept_audio(link.user_id, link.call_id, payload, seq, ts, peers);
    if (fate == 2) return false;
    if (fate == 0) relay_audio(peers, link.user_id, seq, ts, payload);
  }
  return true;
}

void Server::on_media(int fd, uint32_t ev) {
  bool dead = (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
  if (!dead && (ev & EPOLLIN)) dead = !read_media(fd);
  if (!dead && media_.count(fd) && media_[fd].out_pos < media_[fd].out.size()) dead = !write_media(fd);
  if (dead || !media_.count(fd)) {
    if (media_.count(fd)) close_media(fd);
    return;
  }
  epoll_event next{};
  next.events = EPOLLIN | EPOLLRDHUP;
  if (media_[fd].out_pos < media_[fd].out.size()) next.events |= EPOLLOUT;
  next.data.fd = fd;
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &next);
}

void Server::close_hash(int64_t user_id, const std::string &token_hash) {
  auto it = online_.find(user_id);
  if (it == online_.end()) return;
  for (Connection *conn : it->second) {
    if (!conn) continue;
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(conn->token().data()), conn->token().size(), out);
    if (token_hash.size() == sizeof out &&
        std::memcmp(out, token_hash.data(), sizeof out) == 0)
      conn->request_close();
  }
}

}
