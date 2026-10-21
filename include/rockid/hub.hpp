#pragma once

#include "rockid/json.hpp"

#include <cstdint>
#include <vector>

namespace rockid {

class Connection;

/* Online sessions and server pushes. All calls run on the server thread. */
class Hub {
 public:
  virtual ~Hub() = default;
  virtual void bind_user(Connection *connection, int64_t user_id) = 0;
  virtual void unbind(Connection *connection) = 0;
  virtual void push_user(int64_t user_id, const Json &message, Connection *except) = 0;
  virtual void push_many(const std::vector<int64_t> &user_ids, const Json &message,
                         Connection *except) = 0;
  virtual bool online(int64_t user_id) const = 0;
  /* Drop live sockets that presented this exact session token. */
  virtual void close_token(int64_t user_id, const std::string &token) = 0;
  /* token_hash is the SHA-256 of the session token, the same bytes stored in sessions. */
  virtual void close_hash(int64_t user_id, const std::string &token_hash) = 0;
};

}
