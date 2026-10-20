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
};

}
