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
enum class HostSource : std::uint8_t { config, known, history };

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
};

struct HostSources {
    std::string ssh_config;       ///< "" = ~/.ssh/config
    std::string known_hosts;      ///< "" = ~/.ssh/known_hosts
    std::string store;            ///< "" = hosts.vibe beside hand.vibe
    bool        read_known = true;
};

struct HostBook {
    std::vector<Host> hosts;      ///< config (file order), then history, then known_hosts
    bool sets_control = false;    ///< the user's config chose ControlMaster/ControlPath: leave it alone
    bool sets_alive = false;      ///< ... ServerAliveInterval
    std::string config_label;     ///< "~/.ssh/config"
    [[nodiscard]] const Host* find(std::string_view alias) const;
};

[[nodiscard]] HostSources resolve(HostSources s);
[[nodiscard]] HostBook hosts_load(const HostSources& s);

/// hosts.vibe edits (read-modify-write; the file is small).
void hosts_record_use(const HostSources& s, const Host& h, std::int64_t now);
void hosts_set_pinned(const HostSources& s, const Host& h, bool pinned);

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
