// hand::fx — sftp: list a remote folder, download, upload. Tasks: a slow
// link never stalls a frame.

#include "fx.hpp"

#include <hand/core/proc.hpp>
#include <hand/ssh/remote.hpp>
#include <hand/ssh/sftp.hpp>

#include <mayag/widget/file_pane.hpp>

namespace hand::fx {

void list_remote_pane(Sink out, std::uint64_t token, Host h, HostBook book, std::string dir, bool quiet) {
    const auto l = sftp::list(h, book, dir);
    App::PaneListed r{token, l.path.empty() ? dir : l.path, {}, l.error, quiet ? App::Notice::quiet : App::Notice::loud};
    for (const auto& e : l.entries) {
        mayag::dsl::FileEntry f;
        f.name = e.name; f.dir = e.dir; f.link = e.link; f.size = e.size; f.when = e.when; f.target = e.target;
        r.entries.push_back(std::move(f));
    }
    out.send(App::Msg{std::move(r)});
}

void list_remote_files(Sink out, Host h, HostBook book, std::string dir) {
    const auto l = sftp::list(h, book, dir);
    App::FilesListed r{h.alias, l.path.empty() ? dir : l.path, {}, l.error};
    for (const auto& e : l.entries) r.entries.push_back({e.name, e.when, e.target, e.dir, e.link, e.size});
    out.send(App::Msg{std::move(r)});
}

void download(Sink out, Host h, HostBook book, std::string remote, std::string name) {
    const std::string dir = proc::downloads_dir();
    const std::string err = sftp::get(h, book, {remote}, dir);
    out.send(App::Msg{App::FilesDone{h.alias, "", "Downloaded " + name, err, dir + "/" + name}});
}

void upload(Sink out, Host h, HostBook book, std::string dir, std::vector<std::string> files) {
    const std::string err = sftp::put(h, book, files, dir);
    out.send(App::Msg{App::FilesDone{h.alias, dir, "Uploaded " + std::to_string(files.size()) + (files.size() == 1 ? " file" : " files"), err, ""}});
}

void warm_remote(Sink, Host h, HostBook book) {
    // ahead of need: the shared connection, then one kept-open channel, so the
    // first listing or file read after a tab reaches a host costs a round
    // trip, not a login. Silent: a failure here is reported by the action itself.
    if (remote::warm(h, book).empty()) (void)remote::warm_channel(h, book);
}

}  // namespace hand::fx
