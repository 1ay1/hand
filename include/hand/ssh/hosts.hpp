#pragma once
// hand::hosts — the SSH host catalogue. No UI here: app.cpp ranks and draws.
//
//   ~/.ssh/config     the source of truth for HOW to connect. Read the way
//                     ssh reads it: Host blocks matched by glob (with !negation),
//                     first value wins, `Host *` defaults, Include (globs,
//                     relative to ~/.ssh), Match blocks skipped. Never written.
//   ~/.ssh/known_hosts  hosts you have reached before (unhashed entries).
//   hosts.vibe        hand's own notes per host: pinned, environment, tags,
//                     how often / when you last connected. Beside hand.vibe.
//
//   hosts.vibe:
//     hosts {
//       "prod-db" { pinned true  env prod  tags [db eu]  uses 14  last 1731999999 }
//       "deploy@10.0.3.4" { user deploy  hostname 10.0.3.4  uses 1  last 1731990000 }
//     }

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hand {

enum class HostEnv : std::uint8_t { none, dev, staging, prod };
/// Where a host comes from. `saved`: created in hand's host manager, every
/// field lives in hosts.vibe and can be edited there.
enum class HostSource : std::uint8_t { config, known, history, saved };

struct Host {
    std::string alias;            ///< what you type after `ssh` (a Host name, or user@host for ad-hoc)
    std::string hostname;         ///< HostName (else the alias)
    std::string user;
    int         port = 22;
    std::string jump;             ///< ProxyJump
    std::string identity;         ///< IdentityFile (the first)
    HostSource  source = HostSource::config;
    // from hosts.vibe
    HostEnv     env = HostEnv::none;
    bool        env_set = false;  ///< env came from hosts.vibe, not guessed from the name
    bool        pinned = false;
    std::vector<std::string> tags;
    int         uses = 0;
    std::int64_t last = 0;        ///< unix seconds of the last connection, 0 = never
    // from hosts.vibe, for ANY host (config ones too): what hand adds to ssh
    std::vector<std::string> forwards;   ///< "L5432:localhost:5432", "R8080:localhost:80", "D1080"
    std::string command;          ///< run on connect (`ssh -t host CMD`): "tmux new -A -s main"
    std::string folder;           ///< "Work/EU": where it sits in the manager; inherits that folder's defaults
    std::uint8_t inherited = 0;   ///< effective_host: which fields came from a folder (Inherit bits)
    [[nodiscard]] bool editable() const noexcept { return source != HostSource::config && source != HostSource::known; }
};

/// A folder of hosts and what they share. A host's empty user / jump /
/// key, and a port left at 22, come from its folder, then the folder's
/// parents ("Work/EU" -> "Work"). Kept in hosts.vibe `folders { ... }`.
struct Folder {
    std::string name;
    std::string user, jump, identity;
    int         port = 0;         ///< 0: not set
};
enum Inherit : std::uint8_t { inherit_user = 1, inherit_jump = 2, inherit_identity = 4, inherit_port = 8 };

struct HostSources {
    std::string ssh_config;       ///< "" = ~/.ssh/config
    std::string known_hosts;      ///< "" = ~/.ssh/known_hosts
    std::string store;            ///< hosts.vibe (the app puts it beside hand.vibe)
    bool        read_known = true;
};

struct HostBook {
    std::vector<Host> hosts;      ///< config (file order), then history, then known_hosts
    bool sets_control = false;    ///< the user's config chose ControlMaster/ControlPath: leave it alone
    bool sets_alive = false;      ///< ... ServerAliveInterval
    std::string config_label;     ///< "~/.ssh/config"
    std::vector<Folder> folders;  ///< defaults per folder (folders with none are implied by their hosts)
    [[nodiscard]] const Host* find(std::string_view alias) const;
    [[nodiscard]] const Folder* folder(std::string_view name) const;
};

[[nodiscard]] HostSources resolve(HostSources s);
[[nodiscard]] HostBook hosts_load(const HostSources& s);

/// hosts.vibe edits (read-modify-write; the file is small).
void hosts_record_use(const HostSources& s, const Host& h, std::int64_t now);
void hosts_set_pinned(const HostSources& s, const Host& h, bool pinned);
/// Write `h` to hosts.vibe: every field for a host hand owns (saved,
/// history), only hand's notes (env, tags, pin, forwards, command) for one
/// from ~/.ssh/config or known_hosts. `old_alias`: the entry it replaces
/// (a rename), "" for a new one.
void hosts_save(const HostSources& s, const Host& h, std::string_view old_alias);
/// Forget a host hand owns; for any other, drop hand's notes on it.
void hosts_remove(const HostSources& s, std::string_view alias);
/// Write a folder's defaults (`old_name`: a rename moves every host in it).
void folder_save(const HostSources& s, const Folder& f, std::string_view old_name);
void folder_remove(const HostSources& s, std::string_view name);
/// `h` with its folder's defaults filled in (hosts hand owns only: a
/// ~/.ssh/config host has its config). Sets Host::inherited.
[[nodiscard]] Host effective_host(const Host& h, const HostBook& book);

/// Sessions saved in other SSH clients, as hosts hand can own. Each comes
/// with its folder ("PuTTY", "MobaXterm/Servers") and its tunnels.
struct ImportSource {
    std::string       name;       ///< "PuTTY", "MobaXterm"
    std::string       where;      ///< the registry key or file read
    std::vector<Host> hosts;
};
/// PuTTY (registry / ~/.putty/sessions) and MobaXterm (MobaXterm.ini).
/// `putty_root` / `moba_ini`: "" = their usual places (tests pass fixtures).
[[nodiscard]] std::vector<ImportSource> import_sources(const std::string& putty_root = {}, const std::string& moba_ini = {});
/// A MobaXterm.ini's SSH bookmarks.
[[nodiscard]] std::vector<Host> parse_mobaxterm_ini(std::string_view text);
/// A forward as typed ("L5432:db:5432", "-L 5432:db:5432", "D1080",
/// "5432" = L5432:localhost:5432): the ssh flag and its argument, or
/// nullopt with why in `err`.
[[nodiscard]] std::optional<std::pair<std::string, std::string>> parse_forward(std::string_view s, std::string* err = nullptr);

/// "prod-db", "api.staging.example" -> prod / staging; dev/test/local -> dev.
[[nodiscard]] HostEnv guess_env(std::string_view alias, std::string_view hostname);
[[nodiscard]] const char* env_word(HostEnv e);   ///< "prod", "staging", "dev", ""

/// A typed destination: "host", "user@host", "user@host:2222", "ssh://user@host:22".
[[nodiscard]] std::optional<Host> parse_destination(std::string_view typed);

/// The ssh destination of a running `ssh ...` command line (options skipped).
[[nodiscard]] std::optional<Host> ssh_destination(const std::vector<std::string>& argv);

struct SshFlags { bool multiplex = true; bool keepalive = true; };
/// The argv hand runs for `h` (argv[0] = "ssh").
[[nodiscard]] std::vector<std::string> ssh_argv(const Host& h, const HostBook& book, SshFlags f);
/// What a person would type: "ssh prod-db", "ssh -p 2222 deploy@10.0.3.4".
[[nodiscard]] std::string ssh_command(const Host& h);

/// "now", "5m", "3h", "2d", "6w", "" for never.
[[nodiscard]] std::string ago(std::int64_t then, std::int64_t now);

}  // namespace hand
