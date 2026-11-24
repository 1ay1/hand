#pragma once
// hand::fx — every effect hand performs, and nothing else.
//
// update() never touches the world: it returns a Cmd naming one of these
// (Cmd::run for a quick one on the loop, Cmd::task for one that may block),
// with the values it chose as arguments. They are the ONLY code under
// src/app that may write a pty, read or write a file, start a process or ask
// the OS anything (tests/purity.cmake); each is a plain function of its
// arguments, so the runtime can run it after the step that asked.
//
//   pty.cpp     spawn a shell / ssh, write, resize, probe what runs in a tab
//   store.cpp   hand.vibe and hosts.vibe: save, load, the host book
//   os.cpp      open a link, reveal a file, notify, pick files, list ports
//   files.cpp   the file pane: list a folder (and git), read a preview
//   remote.cpp  sftp: list, get, put

#include <hand/app.hpp>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace hand::fx {

using Sink = mayag::Sink<App::Msg>;

// ── ptys (src/app/fx/pty.cpp) ──────────────────────────────────────────────
/// Start the session's Launch on a new pty and say so (Spawned; an empty
/// pty: it did not start).
App::Msg spawn(SessionId sid, Launch l);
void pty_write(mayag::CoOwned<Pty> pty, std::string bytes);
void pty_resize(mayag::CoOwned<Pty> pty, int cols, int rows, int px_w, int px_h);
/// What runs in each tab now: the foreground process, and the destination of
/// an `ssh ...` typed at a shell. One Probed, only when something changed.
struct ProbeTab { SessionId sid; std::optional<mayag::CoOwned<Pty>> pty; std::string was; bool watch_ssh = false; bool had_remote = false; };
std::optional<App::Msg> probe(std::vector<ProbeTab> tabs);
/// The shell `shell` names, the way the pty would resolve it ($SHELL, ...).
std::string resolved_shell(std::string shell);
/// The pty reader: a background stream for as long as the shell lives.
/// Coalesces bursts (cat of a big file) into one Output per wakeup, and keeps
/// ONE in flight: it reads on only once the UI has fed the last chunk (the
/// Output carries an OutputCredit; update gives it back). So a firehose
/// (`yes`) is throttled by the kernel's pty buffer -- the producer blocks --
/// instead of piling gigabytes into the mailbox, starving frames and parking
/// ctrl-C behind seconds of queued output.
void read_pty(Sink out, std::stop_token st, mayag::CoOwned<Pty> pty, SessionId sid);

// ── hand.vibe and hosts.vibe (src/app/fx/store.cpp) ───────────────────────
void save_config(Config c, std::string path);
/// The host book, read fresh (~/.ssh/config may have changed); with PuTTY and
/// MobaXterm's sessions too when `imports`.
App::Msg load_hosts(HostSources s, bool imports);
void record_use(HostSources s, Host h, std::int64_t now_s);
void set_pinned(HostSources s, Host h, bool pinned);
void save_host(HostSources s, Host h, std::string old_alias);
void remove_host(HostSources s, std::string alias);
void save_folder(HostSources s, Folder f, std::string old_name);
void remove_folder(HostSources s, std::string name);

// ── the desktop (src/app/fx/os.cpp) ────────────────────────────────────────
void open_detached(std::vector<std::string> argv);
/// A link: `launcher` from hand.vibe, or the system's opener when it is empty.
void open_url(std::string launcher, std::string url);
void open_path(std::string path);
void reveal(std::string path);
void tunnel_scan(Sink out);
void pick_upload(Sink out, std::string alias, std::string dir);

// ── the file pane (src/app/fx/files.cpp) ───────────────────────────────────
void list_local(Sink out, std::uint64_t token, std::string dir, bool git, bool quiet);
void preview_local(Sink out, std::string path, int lines);
// ── files in editor tabs (src/app/fx/files.cpp) ────────────────────────────
/// Read a file for the editor (refuses binaries and anything over 32 MB).
void read_file(Sink out, SessionId sid, std::string path);
/// The same, for an open file that changed on disk (or "reload").
void reread_file(Sink out, SessionId sid, std::string path);
/// The file's mtime now (0: gone), as FileStat.
void stat_file(Sink out, SessionId sid, std::string path);
/// Every file under `root` worth opening (git's list when it is a repo:
/// ignored files stay out; else a bounded walk skipping build trees).
void list_project(Sink out, std::string root);
/// Search the project (from `root`'s repo top) for `query`: git grep in a
/// repo, grep elsewhere; plain text, case-insensitive unless it has capitals.
void search_project(Sink out, std::uint64_t token, std::string root, std::string query);
// ── the same, on another machine (src/app/fx/remote_files.cpp) ───────────────────
// One command each over the host's shared connection (hand::remote).
void read_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path);
void reread_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path);
void stat_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path);
void write_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path, std::string text, std::uint64_t point);
void list_remote_project(Sink out, Host h, HostBook book, std::string dir);
void search_remote_project(Sink out, std::uint64_t token, Host h, HostBook book, std::string dir, std::string query);
/// What each of those does, as plain functions returning the message (the
/// opt-in self-test drives them against a real host).
namespace remote_ops {
App::FileLoaded read_file(SessionId sid, const Host& h, const HostBook& book, const std::string& path, bool reload);
std::optional<App::FileStat> stat_file(SessionId sid, const Host& h, const HostBook& book, const std::string& path);
App::FileSaved write_file(SessionId sid, const Host& h, const HostBook& book, const std::string& path, const std::string& text, std::uint64_t point);
App::QuickListed list_project(const Host& h, const HostBook& book, const std::string& dir);
App::SearchAllDone search_project(std::uint64_t token, const Host& h, const HostBook& book, const std::string& dir, const std::string& query);
}  // namespace remote_ops
/// Write `text` to `path` atomically (a sibling temp file, renamed over it,
/// keeping the file's permissions), then FileSaved{sid, point}.
void write_file(Sink out, SessionId sid, std::string path, std::string text, std::uint64_t point);

// ── sftp (src/app/fx/remote.cpp) ───────────────────────────────────────────
void list_remote_pane(Sink out, std::uint64_t token, Host h, HostBook book, std::string dir, bool quiet);
void list_remote_files(Sink out, Host h, HostBook book, std::string dir);
void download(Sink out, Host h, HostBook book, std::string remote, std::string name);
void upload(Sink out, Host h, HostBook book, std::string dir, std::vector<std::string> files);
/// The host's shared connection and one kept-open channel, ahead of need
/// (Cmd::task: it blocks on ssh; answers nothing).
void warm_remote(Sink out, Host h, HostBook book);

}  // namespace hand::fx
