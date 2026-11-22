// hand::fx — the desktop around hand: links, the file manager,
// notifications, the file picker, who listens on which port.

#include "fx.hpp"

#include <hand/core/proc.hpp>

namespace hand::fx {

void open_detached(std::vector<std::string> argv) { spawn_detached(argv); }
void open_path(std::string path) { proc::open_path(path); }
void reveal(std::string path) { proc::reveal(path); }

void tunnel_scan(Sink out) {
    bool known = true;
    App::TunnelScan s;
    for (const auto& l : proc::tcp_listeners(&known)) { s.ports.push_back(l.port); s.owners.push_back(l.process); }
    s.known = known;
    out.send(App::Msg{std::move(s)});
}

void pick_upload(Sink out, std::string alias, std::string dir) {
    out.send(App::Msg{App::UploadPicked{alias, dir, proc::pick_files("Upload to " + dir)}});
}

}  // namespace hand::fx
