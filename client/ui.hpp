#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/color.hpp>

#include "net_client.hpp"

namespace shhterm::client {

// A UI color theme. Presets live in ui.cpp; users switch with /theme or --theme.
struct Theme {
  std::string name;
  ftxui::Color accent;    // primary accent (title badge, prompt, delivered ✓)
  ftxui::Color accent2;   // secondary accent (keys, links)
  ftxui::Color dim;       // system/secondary text
  ftxui::Color self;      // your own messages
  ftxui::Color dm;        // direct messages
  ftxui::Color border;    // frame + separators
  ftxui::Color titleFg;   // text on the title badge
  ftxui::Color barBg;     // status/input bar background
  bool hasBarBg;          // whether to paint the bar background
};

// FTXUI front end: a status bar, a scrolling message area, and an input field.
// Incoming network events mutate the message history only; the input field's
// contents are never touched, so a message arriving mid-typing does not disturb
// what the user is writing.
class ChatUI {
 public:
  ChatUI(NetClient& net, ClientConfig cfg);

  // Blocks running the terminal loop until the user quits.
  void run();

  // Thread-safe: invoked from the network thread for every NetClient event.
  void on_net_event(Event ev);

 private:
  enum class LineKind { Chat, SelfMsg, System, DM };
  enum class Marker { None, Pending, Delivered, Failed };
  struct Line {
    std::string stamp;  // "HH:MM" (empty for system lines)
    std::string who;    // sender name for coloring (empty if none)
    std::string text;   // message body / system text
    LineKind kind;
    Marker marker = Marker::None;  // delivery status for our own messages
  };

  void add_line(const std::string& text, LineKind kind);
  void add_chat(const std::string& who, const std::string& text, LineKind kind);
  void submit();                         // handle the current input buffer
  void handle_command(const std::string& line);
  std::string status_text() const;       // caller holds the lock
  void maybe_send_typing();              // throttled typing notification
  std::string build_invite(const std::string& name_placeholder) const;
  bool set_theme(const std::string& name);  // returns false if unknown

  NetClient& net_;
  ClientConfig cfg_;
  ftxui::ScreenInteractive screen_;

  std::string input_;

  mutable std::mutex mu_;
  Theme theme_;
  std::vector<Line> history_;
  ConnState state_ = ConnState::Connecting;
  std::string status_detail_ = "starting";
  std::string room_key_;
  std::string self_name_;
  std::vector<std::string> users_;
  // Outstanding (unacknowledged) outbound messages: id -> history line index.
  std::map<long long, std::size_t> pending_;
  // Peers currently typing -> steady_clock time of their last typing event.
  std::map<std::string, std::chrono::steady_clock::time_point> typing_;
  // Last time we sent a typing notification (for client-side throttling).
  std::chrono::steady_clock::time_point last_typing_sent_{};
};

}  // namespace shhterm::client
