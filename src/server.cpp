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

Server::Server(int port, int http_port) : port_(port), http_port_(http_port) {
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
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (http_fd_ >= 0) ::close(http_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  if (tls_) SSL_CTX_free(tls_);
}

void Server::request_stop() { stop_.store(true); }

void Server::run() {
  log_info(std::string("listening on ") + (ipv6_ ? "[::]:" : "0.0.0.0:") + std::to_string(port_) +
           (tls_ ? " tls" : ""));
  if (http_fd_ >= 0) log_info("http on " + std::to_string(http_port_));
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
        accept_all(listen_fd_, false);
        continue;
      }
      if (fd == http_fd_) {
        accept_all(http_fd_, true);
        continue;
      }
      if (http_buf_.count(fd)) {
        on_http(fd);
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

void Server::accept_all(int listen_fd, bool http) {
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
    if (http) {
      http_buf_[fd] = {};
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        http_buf_.erase(fd);
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
