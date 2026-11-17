#pragma once

#include <string>

namespace shhterm::server {

// A participant in a room. The real implementation is a TLS Session; tests use
// a lightweight fake. Rooms depend only on this interface so routing and
// lifecycle can be exercised without any sockets.
class Member {
 public:
  virtual ~Member() = default;

  // The member's display name (unique within its room).
  virtual const std::string& name() const = 0;

  // Enqueue an already-framed message (length prefix + JSON) for delivery.
  // Must be non-blocking.
  virtual void deliver(const std::string& frame) = 0;
};

}  // namespace shhterm::server
