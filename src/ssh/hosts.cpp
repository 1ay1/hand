// hand::hosts — ~/.ssh/config, known_hosts and hosts.vibe.

#include <hand/ssh/hosts.hpp>
#include <hand/config/config.hpp>
#include <hand/core/proc.hpp>

#include <vibe.h>
#include <mayag/widget/ssh_forward.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <hand/core/platform.hpp>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace hand {

namespace {

namespace fs = std::filesystem;

/// The home ssh resolves `~` against. On Windows that is %USERPROFILE%,
/// ALWAYS: Microsoft's OpenSSH reads %USERPROFILE%\.ssh and ignores $HOME.
/// Taking $HOME (set by MSYS2 / Git Bash, here to AppData\Roaming) read a
/// ~/.ssh/config that ssh never uses, and missed the real one.
std::string home() {
#if defined(_WIN32)
    if (const char* u = std::getenv("USERPROFILE"); u && *u) {
        std::string s = u;
        for (char& ch : s) if (ch == '\\') ch = '/';
        return s;
    }
#endif
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
        if (plat::glob_match(pat, lower(name))) {
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
                // Relative Includes resolve against ~/.ssh. "Relative" must be
                // asked the platform's way: a Windows absolute path starts
                // "C:\" or "\\server", not "/".
                if (!p.empty() && !std::filesystem::path{p}.is_absolute()) p = home() + "/.ssh/" + p;
                for (const auto& hit : plat::glob_paths(p)) read_config(hit, blocks, depth + 1, seen);
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
                const bool saved = str("source") == "saved";
                if (!h && saved) {
                    // made in hand's host manager: every field is here
                    Host d;
                    d.alias = key;
                    d.hostname = str("hostname");
                    if (d.hostname.empty()) d.hostname = key;
                    d.user = str("user");
                    if (auto p = num("port"); p > 0 && p < 65536) d.port = static_cast<int>(p);
                    d.jump = str("jump");
                    d.identity = str("identity");
                    d.source = HostSource::saved;
                    book.hosts.push_back(std::move(d));
                    h = &book.hosts.back();
                } else if (!h) {
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
                if (VibeValue* x = vibe_object_get(e, "forwards"); x && vibe_is_array(x)) {
                    VibeArray* a = vibe_value_array(x);
                    for (std::size_t k = 0; k < vibe_array_size(a); ++k)
                        if (VibeValue* t = vibe_array_get(a, k); t && vibe_is_string(t)) h->forwards.emplace_back(vibe_value_string(t));
                }
                h->command = str("command");
                h->folder = str("folder");
            }
        }
        if (VibeValue* fv = vibe_object_get(vibe_value_object(doc.v), "folders"); fv && vibe_is_object(fv)) {
            VibeObject* fo = vibe_value_object(fv);
            for (std::size_t i = 0; i < vibe_object_size(fo); ++i) {
                const char* key = vibe_object_key_at(fo, i);
                VibeValue* ev = vibe_object_value_at(fo, i);
                if (!key || !ev || !vibe_is_object(ev)) continue;
                VibeObject* e = vibe_value_object(ev);
                const auto str = [&](const char* k) {
                    VibeValue* x = vibe_object_get(e, k);
                    return x && vibe_is_string(x) ? std::string{vibe_value_string(x)} : std::string{};
                };
                Folder f;
                f.name = key;
                f.user = str("user"); f.jump = str("jump"); f.identity = str("identity");
                if (VibeValue* x = vibe_object_get(e, "port"); x && vibe_is_integer(x)) f.port = static_cast<int>(vibe_value_int(x));
                book.folders.push_back(std::move(f));
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

void hosts_save(const HostSources& s, const Host& h, std::string_view old_alias) {
    edit_store(s, [&](VibeObject* hosts) {
        // keep what hand tracks (uses, last) across an edit or a rename
        std::int64_t uses = 0, last = 0;
        const std::string from = old_alias.empty() ? h.alias : std::string{old_alias};
        if (VibeValue* o = vibe_object_get(hosts, from.c_str()); o && vibe_is_object(o)) {
            VibeObject* oe = vibe_value_object(o);
            if (VibeValue* x = vibe_object_get(oe, "uses"); x && vibe_is_integer(x)) uses = vibe_value_int(x);
            if (VibeValue* x = vibe_object_get(oe, "last"); x && vibe_is_integer(x)) last = vibe_value_int(x);
        }
        if (!old_alias.empty() && old_alias != h.alias) vibe_object_remove(hosts, from.c_str());
        vibe_object_remove(hosts, h.alias.c_str());   // written fresh: cleared fields go away
        VibeObject* e = child(hosts, h.alias.c_str());
        if (h.editable()) {
            vibe_object_set_string(e, "source", "saved");
            if (!h.hostname.empty() && h.hostname != h.alias) vibe_object_set_string(e, "hostname", h.hostname.c_str());
            if (!h.user.empty()) vibe_object_set_string(e, "user", h.user.c_str());
            if (h.port != 22) vibe_object_set_int(e, "port", h.port);
            if (!h.jump.empty()) vibe_object_set_string(e, "jump", h.jump.c_str());
            if (!h.identity.empty()) vibe_object_set_string(e, "identity", h.identity.c_str());
        }
        if (h.pinned) vibe_object_set_bool(e, "pinned", true);
        if (h.env_set && h.env != HostEnv::none) vibe_object_set_string(e, "env", env_word(h.env));
        if (!h.tags.empty()) {
            VibeValue* arr = vibe_value_new_array();
            for (const auto& t : h.tags) vibe_array_push_string(vibe_value_array(arr), t.c_str());
            vibe_object_set(e, "tags", arr);
        }
        if (!h.forwards.empty()) {
            VibeValue* arr = vibe_value_new_array();
            for (const auto& f : h.forwards) vibe_array_push_string(vibe_value_array(arr), f.c_str());
            vibe_object_set(e, "forwards", arr);
        }
        if (!h.command.empty()) vibe_object_set_string(e, "command", h.command.c_str());
        if (!h.folder.empty()) vibe_object_set_string(e, "folder", h.folder.c_str());
        if (uses > 0) vibe_object_set_int(e, "uses", uses);
        if (last > 0) vibe_object_set_int(e, "last", last);
    });
}

const Folder* HostBook::folder(std::string_view name) const {
    for (const auto& f : folders) if (f.name == name) return &f;
    return nullptr;
}

void folder_save(const HostSources& s, const Folder& f, std::string_view old_name) {
    VibeValue* v = read_store(s.store);
    Doc doc{v && vibe_is_object(v) ? v : vibe_value_new_object()};
    if (v && !vibe_is_object(v)) vibe_value_free(v);
    VibeObject* root = vibe_value_object(doc.v);
    VibeObject* folders = child(root, "folders");
    if (!old_name.empty()) vibe_object_remove(folders, std::string{old_name}.c_str());
    vibe_object_remove(folders, f.name.c_str());
    VibeObject* e = child(folders, f.name.c_str());
    if (!f.user.empty()) vibe_object_set_string(e, "user", f.user.c_str());
    if (!f.jump.empty()) vibe_object_set_string(e, "jump", f.jump.c_str());
    if (!f.identity.empty()) vibe_object_set_string(e, "identity", f.identity.c_str());
    if (f.port > 0) vibe_object_set_int(e, "port", f.port);
    // a rename moves the hosts in it, and in its sub-folders
    if (!old_name.empty() && old_name != f.name) {
        const std::string from{old_name};
        VibeObject* hosts = child(root, "hosts");
        for (std::size_t i = 0; i < vibe_object_size(hosts); ++i) {
            VibeValue* hv = vibe_object_value_at(hosts, i);
            if (!hv || !vibe_is_object(hv)) continue;
            VibeObject* he = vibe_value_object(hv);
            VibeValue* x = vibe_object_get(he, "folder");
            if (!x || !vibe_is_string(x)) continue;
            const std::string cur = vibe_value_string(x);
            if (cur == from) vibe_object_set_string(he, "folder", f.name.c_str());
            else if (cur.rfind(from + "/", 0) == 0) vibe_object_set_string(he, "folder", (f.name + cur.substr(from.size())).c_str());
        }
    }
    (void)write_store(s.store, doc.v);
}

void folder_remove(const HostSources& s, std::string_view name) {
    VibeValue* v = read_store(s.store);
    if (!v) return;
    Doc doc{v};
    if (!vibe_is_object(v)) return;
    VibeObject* folders = child(vibe_value_object(doc.v), "folders");
    vibe_object_remove(folders, std::string{name}.c_str());
    (void)write_store(s.store, doc.v);
}

Host effective_host(const Host& h, const HostBook& book) {
    Host e = h;
    e.inherited = 0;
    if (!h.editable() || h.folder.empty()) return e;
    // nearest folder first: "Work/EU", then "Work"
    std::string f = h.folder;
    while (!f.empty()) {
        if (const Folder* d = book.folder(f)) {
            if (e.user.empty() && !d->user.empty()) { e.user = d->user; e.inherited |= inherit_user; }
            if (e.jump.empty() && !d->jump.empty()) { e.jump = d->jump; e.inherited |= inherit_jump; }
            if (e.identity.empty() && !d->identity.empty()) { e.identity = d->identity; e.inherited |= inherit_identity; }
            if (e.port == 22 && d->port > 0 && d->port != 22 && !(e.inherited & inherit_port)) { e.port = d->port; e.inherited |= inherit_port; }
        }
        const auto slash = f.rfind('/');
        f = slash == std::string::npos ? std::string{} : f.substr(0, slash);
    }
    return e;
}

// ── importing other clients' sessions ───────────────────────────────────────────────────

namespace {
/// An alias ssh and a person can both type: spaces and odd characters to '-'.
std::string alias_of(std::string_view name) {
    std::string a;
    for (char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        a += (std::isalnum(u) || ch == '.' || ch == '_' || ch == '-' || ch == '@' || u >= 0x80) ? ch : '-';
    }
    while (a.find("--") != std::string::npos) a.erase(a.find("--"), 1);
    while (!a.empty() && a.back() == '-') a.pop_back();
    while (!a.empty() && a.front() == '-') a.erase(a.begin());
    return a;
}
/// PuTTY's "L5432=db:5432,4D1080,R8080=localhost:80" as ssh forward specs.
std::vector<std::string> putty_forwards(std::string_view v) {
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&] {
        std::string s = cur;
        cur.clear();
        if (!s.empty() && (s[0] == '4' || s[0] == '6')) s.erase(0, 1);   // address family
        if (s.empty()) return;
        if (const auto eq = s.find('='); eq != std::string::npos) s[eq] = ':';
        if (parse_forward(s)) out.push_back(mayag::dsl::ssh_forward_parse(s)->spec());
    };
    for (char ch : v) { if (ch == ',') flush(); else cur += ch; }
    flush();
    return out;
}
}  // namespace

std::vector<Host> parse_mobaxterm_ini(std::string_view text) {
    // [Bookmarks] / [Bookmarks_N] sections: SubRep=Folder\Sub, then
    //   name=#icon#type%host%port%user%...   (type 0 = SSH)
    std::vector<Host> out;
    std::string section, subrep;
    std::size_t p = 0;
    const auto decode = [](std::string s) {
        for (const auto& [from, to] : {std::pair<const char*, const char*>{"__PIPE__", "|"}, {"__DIEZE__", "#"}, {"__PTVIRG__", ";"}, {"__DBLQUO__", "\""}}) {
            for (std::size_t k; (k = s.find(from)) != std::string::npos;) s.replace(k, std::strlen(from), to);
        }
        return s;
    };
    while (p < text.size()) {
        std::size_t e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        std::string line = trim(text.substr(p, e - p));
        p = e + 1;
        if (line.empty()) continue;
        if (line.front() == '[') { section = line; subrep.clear(); continue; }
        if (section.rfind("[Bookmarks", 0) != 0) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "SubRep") { subrep = val; for (char& ch : subrep) if (ch == '\\') ch = '/'; continue; }
        if (key == "ImgNum" || val.empty() || val[0] != '#') continue;
        const auto h2 = val.find('#', 1);
        if (h2 == std::string::npos) continue;
        std::vector<std::string> f;
        {
            std::string cur;
            for (std::size_t k = h2 + 1; k < val.size() && val[k] != '#'; ++k) {
                if (val[k] == '%') { f.push_back(cur); cur.clear(); }
                else cur += val[k];
            }
            f.push_back(cur);
        }
        if (f.size() < 4 || f[0] != "0" || f[1].empty()) continue;   // SSH sessions only
        Host h;
        h.alias = alias_of(decode(key));
        h.hostname = decode(f[1]);
        if (const int port = std::atoi(f[2].c_str()); port > 0 && port < 65536) h.port = port;
        h.user = decode(f[3]);
        // the key file is one of the later fields: the one that looks like one
        for (std::size_t k = 4; k < f.size(); ++k) {
            const std::string v = decode(f[k]);
            if (v.find("_ProfileDir_") != std::string::npos || v.find("_CurrentDrive_") != std::string::npos) continue;
            const bool looks = v.find(".ssh") != std::string::npos || v.ends_with(".pem") || v.find("id_") != std::string::npos;
            if (looks && !v.ends_with(".ppk")) { h.identity = v; break; }
        }
        h.source = HostSource::saved;
        h.folder = subrep.empty() ? std::string{"MobaXterm"} : "MobaXterm/" + subrep;
        if (!h.alias.empty()) out.push_back(std::move(h));
    }
    return out;
}

std::vector<ImportSource> import_sources(const std::string& putty_root, const std::string& moba_ini) {
    std::vector<ImportSource> out;
    {
        ImportSource src{"PuTTY", putty_root.empty() ? std::string{"PuTTY saved sessions"} : putty_root, {}};
        for (const auto& rs : proc::putty_sessions(putty_root)) {
            const auto get = [&](const char* k) { auto it = rs.values.find(k); return it == rs.values.end() ? std::string{} : it->second; };
            if (rs.name == "Default Settings" || get("Protocol") != "ssh" || get("HostName").empty()) continue;
            Host h;
            h.alias = alias_of(rs.name);
            std::string host = get("HostName");
            if (const auto at = host.find('@'); at != std::string::npos) { h.user = host.substr(0, at); host = host.substr(at + 1); }
            h.hostname = host;
            if (h.user.empty()) h.user = get("UserName");
            if (const int port = std::atoi(get("PortNumber").c_str()); port > 0 && port < 65536) h.port = port;
            // a .ppk is PuTTY's own format, which OpenSSH cannot read; an
            // OpenSSH key with the same name beside it is used if present
            if (std::string k = get("PublicKeyFile"); !k.empty()) {
                if (k.ends_with(".ppk")) {
                    const std::string bare = k.substr(0, k.size() - 4);
                    std::error_code ec;
                    if (fs::exists(bare, ec)) h.identity = bare;
                } else h.identity = k;
            }
            h.forwards = putty_forwards(get("PortForwardings"));
            h.source = HostSource::saved;
            h.folder = "PuTTY";
            if (!h.alias.empty()) src.hosts.push_back(std::move(h));
        }
        if (!src.hosts.empty()) out.push_back(std::move(src));
    }
    {
        std::vector<std::string> paths;
        if (!moba_ini.empty()) paths.push_back(moba_ini);
        else {
            for (const char* env : {"APPDATA", "USERPROFILE"}) {
                if (const char* b = std::getenv(env); b && *b) {
                    paths.push_back(std::string{b} + "/MobaXterm/MobaXterm.ini");
                    paths.push_back(std::string{b} + "/Documents/MobaXterm/MobaXterm.ini");
                }
            }
        }
        for (const auto& path : paths) {
            std::ifstream in{path, std::ios::binary};
            if (!in) continue;
            std::ostringstream ss;
            ss << in.rdbuf();
            ImportSource src{"MobaXterm", path, parse_mobaxterm_ini(ss.str())};
            if (!src.hosts.empty()) { out.push_back(std::move(src)); break; }
        }
    }
    return out;
}

void hosts_remove(const HostSources& s, std::string_view alias) {
    edit_store(s, [&](VibeObject* hosts) { vibe_object_remove(hosts, std::string{alias}.c_str()); });
}

std::optional<std::pair<std::string, std::string>> parse_forward(std::string_view in, std::string* err) {
    // one parser for the UI and the argv: mayag's (widget/ssh_forward.hpp)
    const auto f = mayag::dsl::ssh_forward_parse(in, err);
    if (!f) return std::nullopt;
    return std::pair{std::string{f->flag()}, f->arg()};
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

std::vector<std::string> ssh_argv(const Host& host, const HostBook& book, SshFlags f) {
    const Host h = effective_host(host, book);   // folder defaults: user, jump, key, port
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
    // the host manager's forwards: -L / -R / -D, each checked on save and again here
    for (const auto& fw : h.forwards)
        if (auto p = parse_forward(fw)) a.insert(a.end(), {p->first, p->second});
    if (h.source == HostSource::config) {
        a.push_back(h.alias);   // ssh applies the config itself
    } else {
        if (h.port != 22) a.insert(a.end(), {"-p", std::to_string(h.port)});
        if (!h.jump.empty()) a.insert(a.end(), {"-J", h.jump});
        if (!h.identity.empty()) {
            std::string id = h.identity;
            if (id.rfind("~/", 0) == 0) id = home() + id.substr(1);
            a.insert(a.end(), {"-i", id});
        }
        a.push_back(h.user.empty() ? h.hostname : h.user + "@" + h.hostname);
    }
    // a command on connect needs a tty (tmux, a shell with flags)
    if (!h.command.empty()) {
        a.insert(a.begin() + 1, "-t");
        a.push_back(h.command);
    }
    return a;
}

std::string ssh_command(const Host& h) {
    std::string s = "ssh ";
    for (const auto& fw : h.forwards)
        if (auto p = parse_forward(fw)) s += p->first + " " + p->second + " ";
    if (h.source == HostSource::config) s += h.alias;
    else {
        if (h.port != 22) s += "-p " + std::to_string(h.port) + " ";
        if (!h.jump.empty()) s += "-J " + h.jump + " ";
        if (!h.identity.empty()) s += "-i " + h.identity + " ";
        s += h.user.empty() ? h.hostname : h.user + "@" + h.hostname;
    }
    if (!h.command.empty()) s += " -t '" + h.command + "'";
    return s;
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
