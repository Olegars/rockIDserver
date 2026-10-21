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
#include <cstring>
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

Server::Server(int port) : port_(port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error("socket() failed");

  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (set_nonblock(listen_fd_) < 0) throw std::runtime_error("fcntl() failed");

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0)
    throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
  if (::listen(listen_fd_, 128) < 0) throw std::runtime_error("listen() failed");

  epoll_fd_ = ::epoll_create1(0);
  if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1() failed");

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd_;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0)
    throw std::runtime_error("epoll_ctl() failed");
}

Server::~Server() {
  conns_.clear();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

void Server::request_stop() { stop_.store(true); }

void Server::run() {
  log_info("listening on 0.0.0.0:" + std::to_string(port_));
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
        accept_all();
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

void Server::accept_all() {
  while (true) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
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

    auto conn = std::make_unique<Connection>(fd);
    Connection *raw = conn.get();
    raw->set_app(app_);
    raw->set_on_output([this](Connection &c) { update_events(c); });
    conns_.emplace(fd, std::move(conn));

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
      log_error("epoll_ctl add failed");
      conns_.erase(fd);
      continue;
    }

    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
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
  if (it == online_.end()) return;
  /* Copy: send() may close other connections only by flag, but be safe if the vector changes. */
  std::vector<Connection *> targets = it->second;
  for (Connection *conn : targets) {
    if (conn != except) conn->send(message);
  }
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
