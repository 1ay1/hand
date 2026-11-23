#pragma once
// hand::remote — one connection per host, shared by everything hand does there.
//
// An SSH handshake is several round trips (TCP, key exchange, auth); over a
// real link it is the slow part of every remote action. So hand opens ONE
// connection per host (OpenSSH's ControlMaster) and runs every terminal,
// listing, file read, save, search and finder over it as another channel:
//
//   measured on this machine:  a fresh sftp listing 210 ms, over the shared
//   connection 60 ms; across a network, a fresh one costs every extra RTT.
//
//   control_dir()        ~/.cache/hand/ssh, owner-only: the sockets
//   ssh_args / sftp_args argv for a host with the shared connection, batch
//                        mode (a host wanting a password says so, never hangs)
//   warm(h)              make sure the connection is up (cheap when it is)
//   exec(h, cmd, in)     run a shell command on the host, stdin piped in
//   sh_quote(s)          a word for the remote shell
//
// Windows' OpenSSH has no ControlMaster: there everything still works, each
// action just pays its own handshake. A ~/.ssh/config that sets its own
// ControlPath (HostBook::sets_control) is used as is.
//
// Blocking: the app calls these from tasks.

#include <hand/core/proc.hpp>
#include <hand/ssh/hosts.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hand::remote {

/// Where the shared connections' sockets live ("" where there are none).
[[nodiscard]] std::string control_dir();
/// The options that put a connection on the shared one ({} when the
/// platform or the user's own config says otherwise).
[[nodiscard]] std::vector<std::string> control_opts(const HostBook& book);

/// `ssh [options] destination` for `h` (append the remote command).
[[nodiscard]] std::vector<std::string> ssh_args(const Host& h, const HostBook& book);
/// `sftp [options] -b batch destination`.
[[nodiscard]] std::vector<std::string> sftp_args(const Host& h, const HostBook& book, const std::string& batch_file);

/// Bring the shared connection up ("" fine, else why, for a person).
/// A live one answers in a few ms; a new one costs one handshake.
[[nodiscard]] std::string warm(const Host& h, const HostBook& book);

/// Close the host's shared connection (a dead one: the next action reconnects).
void drop(const Host& h, const HostBook& book);

/// Run `command` (a /bin/sh line) on the host; `in` goes to its stdin.
[[nodiscard]] proc::Result exec(const Host& h, const HostBook& book, const std::string& command,
                                std::string_view in = {}, int timeout_ms = 20000);

/// `s` as one word for a POSIX shell: 'it'\''s'.
[[nodiscard]] std::string sh_quote(std::string_view s);

/// ssh / sftp stderr and exit code -> a sentence ("" when it is fine).
[[nodiscard]] std::string explain(std::string_view err, int code);

}  // namespace hand::remote
