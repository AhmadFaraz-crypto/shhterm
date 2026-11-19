#include "ui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <cstdio>
#include <ctime>
#include <utility>

namespace shhterm::client {

using namespace ftxui;

namespace {

// Copy text to the system clipboard. Tries the common macOS/Linux/Termux tools
// in turn and returns the name of whichever succeeded, or "" if none worked.
std::string copy_to_clipboard(const std::string& text) {
  static const char* tools[] = {
      "pbcopy",                               // macOS
      "wl-copy",                              // Wayland
      "xclip -selection clipboard",           // X11
      "xsel --clipboard --input",             // X11 (alt)
      "termux-clipboard-set",                 // Android/Termux
  };
  for (const char* tool : tools) {
    FILE* pipe = popen(tool, "w");
    if (!pipe) continue;
    std::fwrite(text.data(), 1, text.size(), pipe);
    int rc = pclose(pipe);
    if (rc == 0) {
      std::string name = tool;
      return name.substr(0, name.find(' '));  // first word = program name
    }
  }
  return "";
}

// Basename of a path (for showing the cert file name in an invite).
std::string basename_of(const std::string& path) {
  auto p = path.find_last_of('/');
  return p == std::string::npos ? path : path.substr(p + 1);
}

// ---- Themes ----------------------------------------------------------------
// Truecolor presets (fall back gracefully on 256-color terminals).

using ftxui::Color;

std::vector<Theme> all_themes() {
  return {
      // "dark" — matches the web client: navy panels, green + blue accents.
      {"dark", Color::RGB(57, 217, 138), Color::RGB(74, 168, 255),
       Color::RGB(125, 141, 163), Color::RGB(125, 211, 255),
       Color::RGB(217, 140, 255), Color::RGB(34, 48, 66),
       Color::RGB(4, 45, 27), Color::RGB(18, 24, 33), true},
      // "ocean" — cool blues and cyan.
      {"ocean", Color::RGB(80, 200, 220), Color::RGB(120, 160, 255),
       Color::RGB(130, 150, 170), Color::RGB(150, 220, 255),
       Color::RGB(200, 160, 255), Color::RGB(40, 60, 80),
       Color::RGB(6, 30, 45), Color::RGB(12, 28, 42), true},
      // "matrix" — green-on-black hacker vibe.
      {"matrix", Color::RGB(0, 255, 90), Color::RGB(0, 200, 70),
       Color::RGB(0, 120, 40), Color::RGB(120, 255, 150),
       Color::RGB(0, 230, 120), Color::RGB(0, 90, 30),
       Color::RGB(0, 20, 0), Color::RGB(0, 15, 0), true},
      // "sunset" — warm oranges/pinks.
      {"sunset", Color::RGB(255, 150, 80), Color::RGB(255, 100, 140),
       Color::RGB(170, 140, 130), Color::RGB(255, 200, 120),
       Color::RGB(255, 130, 200), Color::RGB(80, 50, 50),
       Color::RGB(40, 20, 15), Color::RGB(35, 22, 20), true},
      // "light" — for light terminals; no painted bar background.
      {"light", Color::RGB(0, 150, 80), Color::RGB(30, 110, 220),
       Color::RGB(110, 120, 135), Color::RGB(20, 110, 190),
       Color::RGB(150, 60, 200), Color::RGB(200, 205, 215),
       Color::RGB(255, 255, 255), Color::RGB(235, 238, 242), false},
      // "mono" — minimal, uses the terminal's own palette.
      {"mono", Color::Green, Color::Cyan, Color::GrayDark, Color::CyanLight,
       Color::Magenta, Color::GrayDark, Color::Black, Color::Default, false},
  };
}

std::string theme_names() {
  std::string s;
  for (auto& t : all_themes()) s += (s.empty() ? "" : ", ") + t.name;
  return s;
}

// Split a multi-line string into individual lines for display.
std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n') {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

// Stable per-name color so each participant reads consistently. Simple hash
// over a palette chosen to be legible on both light and dark terminals.
Color name_color(const std::string& name) {
  static const Color palette[] = {
      Color::Green,     Color::Cyan,      Color::Magenta, Color::Yellow,
      Color::Blue,      Color::Red,       Color::GreenLight, Color::CyanLight,
      Color::MagentaLight, Color::YellowLight};
  std::size_t h = 1469598103934665603ULL % 131;  // seed
  for (char c : name) h = (h * 131 + static_cast<unsigned char>(c)) % 100000007;
  return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

// Local wall-clock "HH:MM".
std::string hhmm() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[6];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

}  // namespace

ChatUI::ChatUI(NetClient& net, ClientConfig cfg)
    : net_(net),
      cfg_(std::move(cfg)),
      screen_(ScreenInteractive::Fullscreen()) {
  self_name_ = cfg_.name;
  if (!set_theme(cfg_.theme)) set_theme("dark");
}

bool ChatUI::set_theme(const std::string& name) {
  for (auto& t : all_themes()) {
    if (t.name == name) {
      theme_ = t;
      return true;
    }
  }
  return false;
}

void ChatUI::add_line(const std::string& text, LineKind kind) {
  history_.push_back({"", "", text, kind, Marker::None});
}

void ChatUI::add_chat(const std::string& who, const std::string& text,
                      LineKind kind) {
  history_.push_back({hhmm(), who, text, kind, Marker::None});
}

std::string ChatUI::status_text() const {
  switch (state_) {
    case ConnState::Connected: return "connected";
    default: return status_detail_;
  }
}

void ChatUI::on_net_event(Event ev) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    switch (ev.type) {
      case Event::Type::Status: {
        state_ = ev.state;
        if (!ev.text.empty()) status_detail_ = ev.text;
        if (ev.state == ConnState::Reconnecting ||
            ev.state == ConnState::Failed || ev.state == ConnState::Closed) {
          // Any unacknowledged messages cannot be considered delivered.
          for (auto& [id, idx] : pending_) history_[idx].marker = Marker::Failed;
          pending_.clear();
          typing_.clear();
          if (!ev.text.empty()) add_line("[" + ev.text + "]", LineKind::System);
        }
        break;
      }
      case Event::Type::Welcome: {
        room_key_ = ev.key;
        self_name_ = ev.from;
        users_ = ev.users;
        state_ = ConnState::Connected;
        status_detail_ = "connected";
        if (cfg_.create)
          add_line("Room created. Share this key to invite others:",
                   LineKind::System);
        else
          add_line("Joined the room (messages are end-to-end encrypted).",
                   LineKind::System);
        add_line("    " + room_key_, LineKind::System);
        std::string who = "In room: ";
        for (std::size_t i = 0; i < users_.size(); ++i)
          who += (i ? ", " : "") + users_[i];
        add_line(who, LineKind::System);
        break;
      }
      case Event::Type::Chat:
        typing_.erase(ev.from);  // they clearly finished typing
        add_chat(ev.from, ev.text, ev.dm ? LineKind::DM : LineKind::Chat);
        break;
      case Event::Type::Presence:
        typing_.erase(ev.from);
        add_line("* " + ev.text, LineKind::System);
        break;
      case Event::Type::Nick: {
        typing_.erase(ev.from);
        if (ev.from == self_name_) self_name_ = ev.text;  // our own rename
        add_line("* " + ev.from + " is now " + ev.text, LineKind::System);
        break;
      }
      case Event::Type::Typing:
        typing_[ev.from] = std::chrono::steady_clock::now();
        break;
      case Event::Type::Users: {
        users_ = ev.users;
        std::string s = "Users (" + std::to_string(users_.size()) + "): ";
        for (std::size_t i = 0; i < users_.size(); ++i)
          s += (i ? ", " : "") + users_[i];
        add_line(s, LineKind::System);
        break;
      }
      case Event::Type::Ack: {
        auto it = pending_.find(ev.id);
        if (it != pending_.end()) {
          history_[it->second].marker = Marker::Delivered;
          pending_.erase(it);
        }
        break;
      }
      case Event::Type::Notice:
        add_line("[" + ev.text + "]", LineKind::System);
        break;
    }
  }
  screen_.PostEvent(ftxui::Event::Custom);  // request a redraw
}

void ChatUI::handle_command(const std::string& line) {
  // Split "/cmd arg rest..." into cmd and the remainder.
  std::string cmd = line, rest;
  if (auto sp = line.find(' '); sp != std::string::npos) {
    cmd = line.substr(0, sp);
    rest = line.substr(sp + 1);
  }

  if (cmd == "/help") {
    add_line("Commands:", LineKind::System);
    add_line("  /invite [name] - copy a ready-to-send join command",
             LineKind::System);
    add_line("  /copy          - copy just the room key", LineKind::System);
    add_line("  /clear         - clear your chat view", LineKind::System);
    add_line("  /theme [name]  - change the color theme", LineKind::System);
    add_line("  /users         - list people in this room", LineKind::System);
    add_line("  /nick <name>   - change your display name", LineKind::System);
    add_line("  /msg <user> <text> - send a private message", LineKind::System);
    add_line("  /help          - show this help", LineKind::System);
    add_line("  /quit          - leave and exit", LineKind::System);
  } else if (cmd == "/invite") {
    if (room_key_.empty()) {
      add_line("No room yet — wait until you're connected.", LineKind::System);
      return;
    }
    std::string placeholder = rest.empty() ? "YOURNAME" : rest;
    std::string invite = build_invite(placeholder);
    std::string tool = copy_to_clipboard(invite);
    add_line("Share this with your friend (they just change the name):",
             LineKind::System);
    for (auto& l : split_lines(invite)) add_line("  " + l, LineKind::System);
    if (!tool.empty())
      add_line("✓ Copied to clipboard (" + tool + "). Paste and send it.",
               LineKind::System);
    else
      add_line("(Couldn't auto-copy — select the text above to copy it.)",
               LineKind::System);
  } else if (cmd == "/copy") {
    if (room_key_.empty()) {
      add_line("No room key yet.", LineKind::System);
      return;
    }
    std::string tool = copy_to_clipboard(room_key_);
    if (!tool.empty())
      add_line("✓ Room key copied to clipboard (" + tool + ").",
               LineKind::System);
    else
      add_line("Room key: " + room_key_ + "  (copy it manually)",
               LineKind::System);
  } else if (cmd == "/clear") {
    history_.clear();
    pending_.clear();  // on-screen markers are gone; keep state consistent
    add_line("— cleared —", LineKind::System);
  } else if (cmd == "/theme") {
    if (rest.empty()) {
      add_line("Current theme: " + theme_.name, LineKind::System);
      add_line("Available: " + theme_names(), LineKind::System);
      add_line("Usage: /theme <name>", LineKind::System);
    } else if (set_theme(rest)) {
      add_line("Theme set to '" + rest + "'.", LineKind::System);
    } else {
      add_line("Unknown theme '" + rest + "'. Available: " + theme_names(),
               LineKind::System);
    }
  } else if (cmd == "/users") {
    net_.request_users();
  } else if (cmd == "/nick") {
    if (rest.empty())
      add_line("Usage: /nick <name>", LineKind::System);
    else
      net_.send_nick(rest);
  } else if (cmd == "/msg") {
    auto sp = rest.find(' ');
    if (sp == std::string::npos || sp == 0) {
      add_line("Usage: /msg <user> <message>", LineKind::System);
      return;
    }
    std::string to = rest.substr(0, sp);
    std::string body = rest.substr(sp + 1);
    long long id = net_.send_chat(body, to);
    if (id == 0) {
      add_line("Not connected — DM not sent.", LineKind::System);
      return;
    }
    add_chat(self_name_ + " → " + to, body, LineKind::DM);
    history_.back().marker = Marker::Pending;
    pending_[id] = history_.size() - 1;
  } else if (cmd == "/quit") {
    add_line("Leaving…", LineKind::System);
    net_.stop();
    screen_.Exit();
  } else {
    add_line("Unknown command: " + cmd + "  (try /help)", LineKind::System);
  }
}

std::string ChatUI::build_invite(const std::string& name_placeholder) const {
  // A ready-to-run join command carrying the full key (which includes the
  // decryption secret). The friend only edits the name.
  std::string cmd = "shhterm join " + room_key_ + " --name " +
                    name_placeholder + " --server " + cfg_.host + ":" +
                    cfg_.port;
  std::string note;
  if (cfg_.insecure_skip_verify) {
    cmd += " --insecure";
  } else if (!cfg_.ca_file.empty()) {
    std::string cert = basename_of(cfg_.ca_file);
    cmd += " --ca " + cert + " --verify-host " + cfg_.verify_host;
    note = "(" + cert + " must be in the same folder — send them that file too)";
  }
  std::string invite = "Join my encrypted shhterm room:\n" + cmd;
  if (!note.empty()) invite += "\n" + note;
  return invite;
}

void ChatUI::maybe_send_typing() {
  auto now = std::chrono::steady_clock::now();
  if (now - last_typing_sent_ < std::chrono::milliseconds(1500)) return;
  last_typing_sent_ = now;
  net_.send_typing();
}

void ChatUI::submit() {
  std::string line = input_;
  input_.clear();
  if (line.empty()) return;

  std::lock_guard<std::mutex> lk(mu_);
  if (line[0] == '/') {
    handle_command(line);
    return;
  }
  long long id = net_.send_chat(line);
  if (id == 0) {
    add_chat(self_name_, line + "  (not connected — not sent)",
             LineKind::System);
    return;
  }
  add_chat(self_name_, line, LineKind::SelfMsg);
  history_.back().marker = Marker::Pending;
  pending_[id] = history_.size() - 1;
}

void ChatUI::run() {
  InputOption opt = InputOption::Default();
  opt.placeholder = "Type a message, or /help";
  opt.on_enter = [this] { submit(); };
  opt.on_change = [this] {
    // Notify peers we're typing (throttled), but not while composing a command.
    if (!input_.empty() && input_[0] != '/') maybe_send_typing();
  };
  opt.multiline = false;
  Component input = Input(&input_, opt);

  Component root = Renderer(input, [this, input] {
    std::lock_guard<std::mutex> lk(mu_);
    const Theme& th = theme_;

    Color dot;
    switch (state_) {
      case ConnState::Connected: dot = th.accent; break;
      case ConnState::Failed:
      case ConnState::Closed: dot = Color::Red; break;
      default: dot = Color::Yellow; break;
    }

    // Title badge + status bar.
    Element badge = text(" shhterm ") | bold | color(th.titleFg) |
                    bgcolor(th.accent);
    Element status = hbox({
        badge,
        text(" "),
        text("●") | color(dot),
        text(" " + status_text()) | bold,
        text("  🔒 e2e") | color(th.accent),
        filler(),
        text("you: ") | color(th.dim),
        text(self_name_) | color(name_color(self_name_)) | bold,
        text("  users: " + std::to_string(users_.size()) + " ") | color(th.dim),
    });
    if (th.hasBarBg) status = status | bgcolor(th.barBg);

    Elements lines;
    for (const auto& l : history_) {
      if (l.kind == LineKind::System) {
        lines.push_back(text(l.text) | color(th.dim));
        continue;
      }
      // Split the body on newlines so multi-paragraph messages (e.g. from the
      // web/mobile clients) render across multiple rows instead of garbled.
      std::vector<std::string> body_lines;
      {
        std::string cur;
        for (char ch : l.text) {
          if (ch == '\n') { body_lines.push_back(cur); cur.clear(); }
          else cur.push_back(ch);
        }
        body_lines.push_back(cur);
      }
      auto color_body = [&](Element e) {
        if (l.kind == LineKind::DM) return e | color(th.dm);
        if (l.kind == LineKind::SelfMsg) return e | color(th.self);
        return e;
      };
      auto marker_el = [&]() -> Element {
        if (l.marker == Marker::Pending) return text("  ·") | color(th.dim);
        if (l.marker == Marker::Delivered) return text("  ✓") | color(th.accent);
        if (l.marker == Marker::Failed) return text("  ✗ not delivered") | color(Color::Red);
        return text("");
      };
      // Indent continuation rows to line up under the message text.
      std::string indent(l.stamp.size() + 1 + l.who.size() + 2, ' ');
      Elements rows;
      for (std::size_t i = 0; i < body_lines.size(); ++i) {
        Elements parts;
        if (i == 0) {
          parts.push_back(text(l.stamp + " ") | color(th.dim));
          parts.push_back(text(l.who + ": ") | color(name_color(l.who)) | bold);
        } else {
          parts.push_back(text(indent));
        }
        parts.push_back(color_body(text(body_lines[i])));
        if (i + 1 == body_lines.size()) parts.push_back(marker_el());
        rows.push_back(hbox(std::move(parts)));
      }
      lines.push_back(rows.size() == 1 ? std::move(rows[0]) : vbox(std::move(rows)));
    }
    Element msg_area = vbox(std::move(lines)) |
                       focusPositionRelative(0, 1) | yframe | flex;

    // Typing indicator: names seen typing within the last 3 seconds.
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> active;
    for (const auto& [name, ts] : typing_)
      if (now - ts < std::chrono::seconds(3)) active.push_back(name);
    std::string typing_line;
    if (active.size() == 1)
      typing_line = active[0] + " is typing…";
    else if (active.size() > 1)
      typing_line = std::to_string(active.size()) + " people are typing…";
    Element typing_el = text(typing_line.empty() ? " " : "  " + typing_line) |
                        color(th.dim) | dim;

    Element input_line = hbox({
        text(" ❯ ") | bold | color(th.accent),
        input->Render() | flex,
    });
    if (th.hasBarBg) input_line = input_line | bgcolor(th.barBg);

    return vbox({
               status,
               separator() | color(th.border),
               msg_area,
               typing_el,
               separator() | color(th.border),
               input_line,
           }) |
           borderStyled(ftxui::ROUNDED, th.border);
  });

  // Ctrl-C arrives as a raw control character; treat it (and Ctrl-D) as /quit.
  Component app = CatchEvent(root, [this](ftxui::Event e) {
    if (e.is_character() &&
        (e.character() == std::string(1, '\x03') ||   // Ctrl-C
         e.character() == std::string(1, '\x04'))) {  // Ctrl-D
      net_.stop();
      screen_.Exit();
      return true;
    }
    return false;
  });

  add_line("Welcome to shhterm. Messages are end-to-end encrypted.",
           LineKind::System);
  add_line("Type /help for commands.", LineKind::System);
  screen_.Loop(app);
  net_.stop();
}

}  // namespace shhterm::client
