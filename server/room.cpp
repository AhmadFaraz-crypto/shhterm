#include "room.hpp"

#include <utility>

#include "protocol.hpp"

namespace shhterm::server {

namespace asio = boost::asio;

Room::Room(asio::io_context& io, std::string display_key, std::string norm_key)
    : display_key_(std::move(display_key)),
      norm_key_(std::move(norm_key)),
      expiry_timer_(io) {}

bool Room::add(const std::shared_ptr<Member>& m) {
  if (members_.count(m->name())) return false;
  members_.emplace(m->name(), m);
  return true;
}

void Room::remove(const std::string& name) { members_.erase(name); }

bool Room::rename(const std::string& from, const std::string& to) {
  if (from == to) return false;
  auto it = members_.find(from);
  if (it == members_.end()) return false;
  if (members_.count(to)) return false;  // name taken
  auto member = it->second;
  members_.erase(it);
  members_.emplace(to, member);
  return true;
}

bool Room::deliver_to(const std::string& name, const std::string& frame) {
  auto it = members_.find(name);
  if (it == members_.end()) return false;
  it->second->deliver(frame);
  return true;
}

bool Room::has(const std::string& name) const {
  return members_.count(name) != 0;
}

std::vector<std::string> Room::names() const {
  std::vector<std::string> out;
  out.reserve(members_.size());
  for (const auto& [name, _] : members_) out.push_back(name);
  return out;
}

void Room::broadcast(const std::string& frame, const std::string& except_name) {
  for (auto& [name, member] : members_) {
    if (name == except_name) continue;
    member->deliver(frame);
  }
}

// ---- RoomManager -----------------------------------------------------------

RoomManager::RoomManager(asio::io_context& io, std::chrono::milliseconds ttl)
    : io_(io), ttl_(ttl) {}

std::shared_ptr<Room> RoomManager::create() {
  // Loop in the astronomically unlikely event of a key collision.
  for (;;) {
    std::string display = proto::generate_room_key();
    std::string norm = proto::normalize_key(display);
    if (rooms_.count(norm)) continue;
    auto room = std::make_shared<Room>(io_, display, norm);
    rooms_.emplace(norm, room);
    return room;
  }
}

std::shared_ptr<Room> RoomManager::find(const std::string& norm_key) {
  auto it = rooms_.find(norm_key);
  return it == rooms_.end() ? nullptr : it->second;
}

void RoomManager::note_join(const std::shared_ptr<Room>& room) {
  // A member is present again; cancel any scheduled deletion.
  room->timer().cancel();
}

void RoomManager::note_leave(const std::shared_ptr<Room>& room) {
  if (room->empty()) schedule_expiry(room);
}

void RoomManager::schedule_expiry(const std::shared_ptr<Room>& room) {
  room->timer().expires_after(ttl_);
  std::weak_ptr<Room> weak = room;
  room->timer().async_wait([this, weak](const boost::system::error_code& ec) {
    if (ec) return;  // cancelled (someone rejoined) or aborted
    auto r = weak.lock();
    if (!r) return;
    // Re-check under the single-threaded invariant: only delete if still empty.
    if (r->empty()) rooms_.erase(r->norm_key());
  });
}

}  // namespace shhterm::server
