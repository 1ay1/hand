// hand::hosts — ~/.ssh/config, known_hosts and hosts.vibe.

#include <hand/ssh/hosts.hpp>
#include <hand/config/config.hpp>

#include <vibe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fnmatch.h>
#include <fstream>
#include <glob.h>
#include <map>
#include <set>
#include <sstream>

namespace hand {

namespace {

namespace fs = std::filesystem;

std::string home() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "";
}

std::string expand_tilde(std::string p) {
    if (!p.empty() && p[0] == '~') p = home() + p.substr(1);
    return p;
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string{s.substr(a, b - a)};
}

/// ssh_config tokenising: `Key value`, `Key=value`, `Key "a b"`; # comments.
std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && ch == '#') break;
        if (!quoted && (std::isspace(static_cast<unsigned char>(ch)) || (ch == '=' && out.empty()))) {
            if (any) { out.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur.push_back(ch);
        any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

bool has_glob(std::string_view s) { return s.find_first_of("*?[") != std::string_view::npos; }

/// ssh's Host matching: any positive pattern matches and no !negated one does.
bool host_matches(const std::vector<std::string>& pats, const std::string& name) {
    bool hit = false;
    for (const auto& p : pats) {
        const bool neg = !p.empty() && p[0] == '!';
        const std::string pat = lower(neg ? p.substr(1) : p);
        if (::fnmatch(pat.c_str(), lower(name).c_str(), 0) == 0) {
            if (neg) return false;
            hit = true;
        }
    }
    return hit;
}

struct Block {
    std::vector<std::string> patterns;   ///< empty: the global section before any Host
    bool match = false;                  ///< a Match block: hand cannot evaluate it, skips it
    std::vector<std::pair<std::string, std::string>> opts;   ///< lowercased key, value
};

void read_config(const std::string& path, std::vector<Block>& blocks, int depth, std::set<std::string>& seen) {
    if (depth > 8 || !seen.insert(path).second) return;
    std::ifstream in{path};
    if (!in) return;
    if (blocks.empty()) blocks.push_back(Block{});
    std::string line;
    while (std::getline(in, line)) {
        auto w = words(line);
        if (w.empty()) continue;
        const std::string key = lower(w[0]);
        if (key == "host") {
            Block b;
            b.patterns.assign(w.begin() + 1, w.end());
            blocks.push_back(std::move(b));
        } else if (key == "match") {
            Block b;
            b.match = true;
            blocks.push_back(std::move(b));
        } else if (key == "include") {
            for (std::size_t i = 1; i < w.size(); ++i) {
                std::string p = expand_tilde(w[i]);
                if (!p.empty() && p[0] != '/') p = home() + "/.ssh/" + p;
                glob_t g{};
                if (::glob(p.c_str(), 0, nullptr, &g) == 0)
                    for (std::size_t k = 0; k < g.gl_pathc; ++k) read_config(g.gl_pathv[k], blocks, depth + 1, seen);
                ::globfree(&g);
            }
        } else if (w.size() >= 2) {
            std::string val = w[1];
            for (std::size_t i = 2; i < w.size(); ++i) val += " " + w[i];
            blocks.back().opts.emplace_back(key, val);
        }
    }
}

/// The first value for `key` that applies to `alias`, ssh's first-wins order.
std::string resolve_opt(const std::vector<Block>& blocks, const std::string& alias, std::string_view key) {
    for (const auto& b : blocks) {
        if (b.match) continue;
        if (!b.patterns.empty() && !host_matches(b.patterns, alias)) continue;
        for (const auto& [k, v] : b.opts)
            if (k == key) return v;
    }
    return {};
}

std::string strip_brackets(std::string h, int* port) {
    // known_hosts "[host]:2222"
    if (!h.empty() && h[0] == '[') {
        const auto e = h.find(']');
        if (e != std::string::npos) {
            if (port && e + 2 < h.size() && h[e + 1] == ':') *port = std::atoi(h.c_str() + e + 2);
            h = h.substr(1, e - 1);
        }
    }
    return h;
}

struct Doc {
    VibeValue* v = nullptr;
    explicit Doc(VibeValue* x) : v(x) {}
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    ~Doc() { if (v) vibe_value_free(v); }
};

VibeObject* child(VibeObject* o, const char* key) {
    if (VibeValue* v = vibe_object_get(o, key); v && vibe_is_object(v)) return vibe_value_object(v);
    VibeValue* n = vibe_value_new_object();
    vibe_object_set(o, key, n);
    return vibe_value_object(n);
}

VibeValue* read_store(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return nullptr;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    VibeParser* p = vibe_parser_new();
    VibeValue* v = vibe_parse_string(p, text.c_str());
    vibe_parser_free(p);
    return v;
}

bool write_store(const std::string& path, VibeValue* doc) {
    std::error_code ec;
    fs::create_directories(fs::path{path}.parent_path(), ec);
    char* body = vibe_emit(doc);
    if (!body) return false;
    const std::string text = std::string{"# hosts.vibe -- hand's notes on your ssh hosts (how to connect stays in ~/.ssh/config).\n"
                                         "# Per host: pinned, env (dev|staging|prod), tags [..]; uses/last are kept by hand.\n\n"} + body;
    vibe_free(body);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary};
        out << text;
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

template <class F>
void edit_store(const HostSources& s, F&& f) {
    VibeValue* v = read_store(s.store);
    Doc doc{v && vibe_is_object(v) ? v : vibe_value_new_object()};
    if (v && !vibe_is_object(v)) vibe_value_free(v);
    VibeObject* hosts = child(vibe_value_object(doc.v), "hosts");
    f(hosts);
    (void)write_store(s.store, doc.v);
}

HostEnv env_of(std::string_view w) {
    const std::string s = lower(std::string{w});
    if (s == "prod" || s == "production") return HostEnv::prod;
    if (s == "staging" || s == "stage") return HostEnv::staging;
    if (s == "dev" || s == "development" || s == "test") return HostEnv::dev;
    return HostEnv::none;
}

}  // namespace

const Host* HostBook::find(std::string_view alias) const {
    for (const auto& h : hosts) if (h.alias == alias) return &h;
    return nullptr;
}

HostSources resolve(HostSources s) {
    if (s.ssh_config.empty()) s.ssh_config = home() + "/.ssh/config";
    if (s.known_hosts.empty()) s.known_hosts = home() + "/.ssh/known_hosts";
    if (s.store.empty()) s.store = (fs::path{config_path()}.parent_path() / "hosts.vibe").string();
    return s;
}

HostEnv guess_env(std::string_view alias, std::string_view hostname) {
    // whole words only: "product" is not prod, "devon" is not dev
    const auto has = [](std::string s, std::initializer_list<const char*> ws) {
        s = lower(s);
        for (auto& ch : s) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = ' ';
        std::istringstream in{s};
        std::string w;
        while (in >> w)
            for (const char* x : ws) if (w == x) return true;
        return false;
    };
    const std::string both = std::string{alias} + " " + std::string{hostname};
    if (has(both, {"prod", "production", "prd", "live"})) return HostEnv::prod;
    if (has(both, {"staging", "stage", "stg", "preprod", "uat"})) return HostEnv::staging;
    if (has(both, {"dev", "development", "test", "testing", "local", "sandbox", "lab"})) return HostEnv::dev;
    return HostEnv::none;
}

const char* env_word(HostEnv e) {
    switch (e) {
        case HostEnv::prod: return "prod";
        case HostEnv::staging: return "staging";
        case HostEnv::dev: return "dev";
        default: return "";
    }
}

HostBook hosts_load(const HostSources& s) {
    HostBook book;
    book.config_label = s.ssh_config;
    if (const std::string h = home(); !h.empty() && book.config_label.rfind(h, 0) == 0)
        book.config_label = "~" + book.config_label.substr(h.size());

    // ── ~/.ssh/config: every concrete Host name, resolved like ssh does ──
    std::vector<Block> blocks;
    std::set<std::string> seen_files;
    read_config(s.ssh_config, blocks, 0, seen_files);
    std::set<std::string> have;
    for (const auto& b : blocks) {
        if (b.match) continue;
        for (const auto& p : b.patterns) {
            if (p.empty() || p[0] == '!' || has_glob(p) || !have.insert(p).second) continue;
            Host h;
            h.alias = p;
            h.hostname = resolve_opt(blocks, p, "hostname");
            if (h.hostname.empty()) h.hostname = p;
            else if (h.hostname.find("%h") != std::string::npos) h.hostname.replace(h.hostname.find("%h"), 2, p);
            h.user = resolve_opt(blocks, p, "user");
            if (const auto port = resolve_opt(blocks, p, "port"); !port.empty()) h.port = std::max(1, std::atoi(port.c_str()));
            h.jump = resolve_opt(blocks, p, "proxyjump");
            if (h.jump == "none") h.jump.clear();
            h.identity = resolve_opt(blocks, p, "identityfile");
            if (const std::string hm = home(); !hm.empty() && h.identity.rfind(hm, 0) == 0) h.identity = "~" + h.identity.substr(hm.size());
            h.source = HostSource::config;
            book.hosts.push_back(std::move(h));
        }
    }
    for (const auto& b : blocks)
        for (const auto& [k, v] : b.opts) {
            if (k == "controlmaster" || k == "controlpath" || k == "controlpersist") book.sets_control = true;
            if (k == "serveraliveinterval") book.sets_alive = true;
        }

    // ── hosts.vibe: notes on those, plus ad-hoc hosts you connected to ──
    if (Doc doc{read_store(s.store)}; doc.v && vibe_is_object(doc.v)) {
        if (VibeValue* hv = vibe_object_get(vibe_value_object(doc.v), "hosts"); hv && vibe_is_object(hv)) {
            VibeObject* ho = vibe_value_object(hv);
            for (std::size_t i = 0; i < vibe_object_size(ho); ++i) {
                const char* key = vibe_object_key_at(ho, i);
                VibeValue* ev = vibe_object_value_at(ho, i);
                if (!key || !ev || !vibe_is_object(ev)) continue;
                VibeObject* e = vibe_value_object(ev);
                const auto str = [&](const char* k) {
                    VibeValue* x = vibe_object_get(e, k);
                    return x && vibe_is_string(x) ? std::string{vibe_value_string(x)} : std::string{};
                };
                const auto num = [&](const char* k) -> std::int64_t {
                    VibeValue* x = vibe_object_get(e, k);
                    if (!x) return 0;
                    if (vibe_is_integer(x)) return vibe_value_int(x);
                    if (vibe_is_float(x)) return static_cast<std::int64_t>(vibe_value_float(x));
                    return 0;
                };
                Host* h = nullptr;
                for (auto& x : book.hosts) if (x.alias == key) { h = &x; break; }
                if (!h) {
                    // not in ~/.ssh/config: an ad-hoc destination remembered from history
                    std::optional<Host> d = parse_destination(key);
                    if (!d) continue;
                    if (auto v = str("user"); !v.empty()) d->user = v;
                    if (auto v = str("hostname"); !v.empty()) d->hostname = v;
                    if (auto p = num("port"); p > 0) d->port = static_cast<int>(p);
                    d->source = HostSource::history;
                    book.hosts.push_back(std::move(*d));
                    h = &book.hosts.back();
                }
                if (VibeValue* x = vibe_object_get(e, "pinned"); x && vibe_is_boolean(x)) h->pinned = vibe_value_bool(x);
                if (auto v = str("env"); !v.empty()) { h->env = env_of(v); h->env_set = true; }
                if (VibeValue* x = vibe_object_get(e, "tags"); x && vibe_is_array(x)) {
                    VibeArray* a = vibe_value_array(x);
                    for (std::size_t k = 0; k < vibe_array_size(a); ++k)
                        if (VibeValue* t = vibe_array_get(a, k); t && vibe_is_string(t)) h->tags.emplace_back(vibe_value_string(t));
                }
                h->uses = static_cast<int>(num("uses"));
                h->last = num("last");
            }
        }
    }

    // ── known_hosts: machines you have reached, not already listed ──
    if (s.read_known) {
        std::set<std::string> names;
        for (const auto& h : book.hosts) { names.insert(lower(h.alias)); names.insert(lower(h.hostname)); }
        std::ifstream in{s.known_hosts};
        std::string line;
        int added = 0;
        while (std::getline(in, line) && added < 400) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == '|' || line[0] == '@') continue;   // hashed / markers
            const auto sp = line.find(' ');
            if (sp == std::string::npos) continue;
            std::stringstream hosts{line.substr(0, sp)};
            std::string first;
            if (!std::getline(hosts, first, ',')) continue;
            int port = 22;
            first = strip_brackets(first, &port);
            if (first.empty() || has_glob(first) || first == "localhost" || first == "127.0.0.1" || first == "::1") continue;
            const std::string key = port != 22 ? first + ":" + std::to_string(port) : first;
            if (!names.insert(lower(first)).second) continue;
            Host h;
            h.alias = key;
            h.hostname = first;
            h.port = port;
            h.source = HostSource::known;
            book.hosts.push_back(std::move(h));
            ++added;
        }
    }
    for (auto& h : book.hosts)
        if (!h.env_set) h.env = guess_env(h.alias, h.hostname);
    return book;
}

void hosts_record_use(const HostSources& s, const Host& h, std::int64_t now) {
    edit_store(s, [&](VibeObject* hosts) {
        VibeObject* e = child(hosts, h.alias.c_str());
        std::int64_t uses = 0;
        if (VibeValue* x = vibe_object_get(e, "uses"); x && vibe_is_integer(x)) uses = vibe_value_int(x);
        vibe_object_set_int(e, "uses", uses + 1);
        vibe_object_set_int(e, "last", now);
        if (h.source != HostSource::config) {   // remember how to reach an ad-hoc host
            if (!h.user.empty()) vibe_object_set_string(e, "user", h.user.c_str());
            if (h.hostname != h.alias) vibe_object_set_string(e, "hostname", h.hostname.c_str());
            if (h.port != 22) vibe_object_set_int(e, "port", h.port);
        }
    });
}

void hosts_set_pinned(const HostSources& s, const Host& h, bool pinned) {
    edit_store(s, [&](VibeObject* hosts) {
        VibeObject* e = child(hosts, h.alias.c_str());
        vibe_object_set_bool(e, "pinned", pinned);
        if (h.source != HostSource::config) {
            if (!h.user.empty()) vibe_object_set_string(e, "user", h.user.c_str());
            if (h.hostname != h.alias) vibe_object_set_string(e, "hostname", h.hostname.c_str());
            if (h.port != 22) vibe_object_set_int(e, "port", h.port);
        }
    });
}

std::optional<Host> parse_destination(std::string_view typed) {
    std::string s = trim(typed);
    if (s.rfind("ssh://", 0) == 0) s = s.substr(6);
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.empty() || s.find_first_of(" \t'\"`$;|&<>") != std::string::npos) return std::nullopt;
    Host h;
    h.alias = s;
    std::string rest = s;
    if (const auto at = rest.rfind('@'); at != std::string::npos) {
        h.user = rest.substr(0, at);
        rest = rest.substr(at + 1);
        if (h.user.empty()) return std::nullopt;
    }
    // host:port, [v6]:port; a bare v6 address (two colons) has no port
    if (!rest.empty() && rest[0] == '[') {
        rest = strip_brackets(rest, &h.port);
    } else if (const auto c = rest.rfind(':'); c != std::string::npos && rest.find(':') == c) {
        const std::string p = rest.substr(c + 1);
        if (p.empty() || !std::all_of(p.begin(), p.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
            return std::nullopt;
        h.port = std::atoi(p.c_str());
        rest = rest.substr(0, c);
    }
    if (rest.empty() || h.port <= 0 || h.port > 65535) return std::nullopt;
    h.hostname = rest;
    h.source = HostSource::history;
    return h;
}

std::optional<Host> ssh_destination(const std::vector<std::string>& argv) {
    if (argv.empty()) return std::nullopt;
    const std::string prog = argv[0].substr(argv[0].find_last_of('/') + 1);
    if (prog != "ssh" && prog != "mosh" && prog != "autossh") return std::nullopt;
    // options that take an argument (ssh(1))
    static const std::string with_arg = "BbcDEeFIiJLlmOoPpQRSWw";
    std::string user;
    int port = 22;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (a == "--") { if (i + 1 < argv.size()) { auto h = parse_destination(argv[i + 1]); if (h) return h; } return std::nullopt; }
        if (a.size() >= 2 && a[0] == '-') {
            if (a[1] == '-') continue;   // mosh's --long options
            // a cluster like -tt or -vp 22: the first char with an argument consumes the rest
            for (std::size_t k = 1; k < a.size(); ++k) {
                if (with_arg.find(a[k]) == std::string::npos) continue;
                const std::string val = k + 1 < a.size() ? a.substr(k + 1) : (i + 1 < argv.size() ? argv[++i] : "");
                if (a[k] == 'l') user = val;
                if (a[k] == 'p') port = std::max(1, std::atoi(val.c_str()));
                break;
            }
            continue;
        }
        auto h = parse_destination(a);
        if (!h) return std::nullopt;
        if (h->user.empty() && !user.empty()) h->user = user;
        if (h->port == 22) h->port = port;
        return h;
    }
    return std::nullopt;
}

std::vector<std::string> ssh_argv(const Host& h, const HostBook& book, SshFlags f) {
    std::vector<std::string> a{"ssh"};
    if (f.multiplex && !book.sets_control) {
        // one TCP+auth per host: a second tab opens instantly, and only one
        // password / 2FA prompt. Sockets live in a private per-user dir.
        std::string dir = home() + "/.cache/hand/ssh";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
        a.insert(a.end(), {"-o", "ControlMaster=auto", "-o", "ControlPath=" + dir + "/%C", "-o", "ControlPersist=10m"});
    }
    if (f.keepalive && !book.sets_alive)
        a.insert(a.end(), {"-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4"});
    if (h.source == HostSource::config) {
        a.push_back(h.alias);   // ssh applies the config itself
    } else {
        if (h.port != 22) a.insert(a.end(), {"-p", std::to_string(h.port)});
        a.push_back(h.user.empty() ? h.hostname : h.user + "@" + h.hostname);
    }
    return a;
}

std::string ssh_command(const Host& h) {
    if (h.source == HostSource::config) return "ssh " + h.alias;
    std::string s = "ssh ";
    if (h.port != 22) s += "-p " + std::to_string(h.port) + " ";
    return s + (h.user.empty() ? h.hostname : h.user + "@" + h.hostname);
}

std::string ago(std::int64_t then, std::int64_t now) {
    if (then <= 0) return {};
    const std::int64_t d = std::max<std::int64_t>(now - then, 0);
    if (d < 60) return "now";
    if (d < 3600) return std::to_string(d / 60) + "m";
    if (d < 86400) return std::to_string(d / 3600) + "h";
    if (d < 86400 * 14) return std::to_string(d / 86400) + "d";
    if (d < 86400 * 120) return std::to_string(d / (86400 * 7)) + "w";
    return std::to_string(d / (86400 * 30)) + "mo";
}

}  // namespace hand
