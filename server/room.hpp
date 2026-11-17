#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "member.hpp"

namespace shhterm::server {

// A chat room: a set of uniquely-named members reachable by a shared key.
// Not thread-safe by design; all access happens on the server's single
// io_context thread.
class Room {
 public:
  Room(boost::asio::io_context& io, std::string display_key,
       std::string norm_key);

  const std::string& display_key() const { return display_key_; }
  const std::string& norm_key() const { return norm_key_; }

  // Add a member. Returns false if the name is already taken in this room.
  bool add(const std::shared_ptr<Member>& m);

  // Remove a member by name (no-op if absent).
  void remove(const std::string& name);

  // Rename a member. Returns false if `from` is absent or `to` is taken.
  bool rename(const std::string& from, const std::string& to);

  // Deliver `frame` to a single member by name. Returns false if absent.
  bool deliver_to(const std::string& name, const std::string& frame);

  bool has(const std::string& name) const;
  std::size_t size() const { return members_.size(); }
  bool empty() const { return members_.empty(); }
  std::vector<std::string> names() const;

  // Deliver `frame` to every member whose name != `except_name`.
  void broadcast(const std::string& frame, const std::string& except_name = "");

  // Expiry timer, owned by the room, managed by RoomManager.
  boost::asio::steady_timer& timer() { return expiry_timer_; }

 private:
  std::string display_key_;
  std::string norm_key_;
  std::map<std::string, std::shared_ptr<Member>> members_;
  boost::asio::steady_timer expiry_timer_;
};

// Owns the set of live rooms and their expiry lifecycle. A room is deleted
// after it has been empty continuously for `ttl`; any join before the timer
// fires cancels the deletion (the reconnection window).
class RoomManager {
 public:
  explicit RoomManager(
      boost::asio::io_context& io,
      std::chrono::milliseconds ttl = std::chrono::minutes(5));

  // Create a new room with a freshly generated unique key.
  std::shared_ptr<Room> create();

  // Look up a room by a normalized key (see proto::normalize_key). Returns
  // nullptr if not found.
  std::shared_ptr<Room> find(const std::string& norm_key);

  // Call after a member joins `room`: cancels any pending expiry.
  void note_join(const std::shared_ptr<Room>& room);

  // Call after a member leaves `room`: if it is now empty, schedule deletion.
  void note_leave(const std::shared_ptr<Room>& room);

  std::size_t room_count() const { return rooms_.size(); }
  std::chrono::milliseconds ttl() const { return ttl_; }

 private:
  void schedule_expiry(const std::shared_ptr<Room>& room);

  boost::asio::io_context& io_;
  std::chrono::milliseconds ttl_;
  std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;  // norm_key -> room
};

}  // namespace shhterm::server
