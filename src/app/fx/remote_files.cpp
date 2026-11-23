// hand::fx — files on another machine, for the editor, ⌘P and ⌘⇧F.
//
// Every call is ONE remote command over the host's shared connection
// (hand::remote): a read is `stat; cat`, a save is `cat > tmp; mv tmp file`,
// a search is git grep where it runs. ~60 ms on a warm link, plus the
// network's own round trip -- no handshake per action.
//
// The scripts are POSIX sh, run as `sh -c SCRIPT sh ARGS...` so the user's
// login shell (bash, zsh, fish) only ever sees quoted words, and contain no
// backslashes (fish would fold `\\` inside single quotes). File names travel
// as arguments, never spliced into the script.

#include "fx.hpp"

#include <hand/ssh/remote.hpp>

#include <mayag/widget/file_pane.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hand::fx {

void parse_grep(const std::string& out, const std::string& root, const std::string& query, App::SearchAllDone& r);

namespace {

constexpr std::uint64_t kMaxBytes = 32u * 1024u * 1024u;

/// `sh -c script sh arg...`, every word quoted for the login shell.
std::string sh_line(const std::string& script, const std::vector<std::string>& args) {
    std::string c = "sh -c " + remote::sh_quote(script) + " sh";
    for (const auto& a : args) c += " " + remote::sh_quote(a);
    return c;
}

// A file's identity for change detection: "mtime:size" from GNU or BSD stat.
constexpr const char* kStat = "stat -c %Y:%s \"$f\" 2>/dev/null || stat -f %m:%z \"$f\" 2>/dev/null";

/// "mtime:size" -> a non-zero token (0 is "gone").
std::int64_t token_of(const std::string& s) {
    if (s.empty() || s == "gone") return 0;
    const auto h = static_cast<std::int64_t>(std::hash<std::string>{}(s) & 0x7FFFFFFFFFFFFFFFull);
    return h == 0 ? 1 : h;
}

std::string first_line(const std::string& out, std::size_t* rest = nullptr) {
    const auto nl = out.find('\n');
    if (rest) *rest = nl == std::string::npos ? out.size() : nl + 1;
    std::string l = out.substr(0, nl);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    return l;
}

}  // namespace

namespace remote_ops {

App::FileLoaded read_file(SessionId sid, const Host& h, const HostBook& book, const std::string& path, bool reload) {
    App::FileLoaded r{.sid = sid, .path = path, .reload = reload};
    const std::string script = std::string{"f=$1; [ -f \"$f\" ] || { echo 'no such file' >&2; exit 3; }; m=$("} + kStat +
                               "); s=${m#*:}; [ \"${s:-0}\" -gt " + std::to_string(kMaxBytes) +
                               " ] && { echo \"too large to edit ($s bytes)\" >&2; exit 4; }; echo \"$m\"; exec cat -- \"$f\"";
    const auto x = remote::exec(h, book, sh_line(script, {path}), {}, 60000);
    if (!x.started || x.code != 0) {
        r.error = x.timed_out ? "the host did not answer in time" : remote::explain(x.err, x.code);
        if (r.error.empty()) r.error = "could not read it";
        return r;
    }
    std::size_t at = 0;
    r.mtime = token_of(first_line(x.out, &at));
    r.text = x.out.substr(at);
    if (!reload && r.text.find('\0') < 8192) { r.binary = true; r.text.clear(); }
    return r;
}

std::optional<App::FileStat> stat_file(SessionId sid, const Host& h, const HostBook& book, const std::string& path) {
    const auto x = remote::exec(h, book, sh_line(std::string{"f=$1; "} + kStat + " || echo gone", {path}), {}, 10000);
    if (!x.started || x.code != 0) return std::nullopt;   // the link blinked: ask again next time, never report "gone" for it
    return App::FileStat{sid, token_of(first_line(x.out))};
}

App::FileSaved write_file(SessionId sid, const Host& h, const HostBook& book, const std::string& path, const std::string& text, std::uint64_t point) {
    // the same discipline as a local save: a sibling, its permissions copied,
    // renamed over the file -- a dropped link mid-write leaves the old file whole
    App::FileSaved r{.sid = sid, .point = point};
    const std::string script = std::string{"f=$1; t=\"$f.hand-save~\"; cat > \"$t\" || { rm -f \"$t\"; exit 5; }; "
                                           "if [ -e \"$f\" ]; then chmod \"$(stat -c %a \"$f\" 2>/dev/null || stat -f %Lp \"$f\")\" \"$t\" 2>/dev/null; fi; "
                                           "mv -f \"$t\" \"$f\" || { rm -f \"$t\"; exit 6; }; "} + kStat;
    const auto x = remote::exec(h, book, sh_line(script, {path}), text, 120000);
    if (!x.started || x.code != 0) {
        r.error = x.timed_out ? "the host did not answer in time; the file on it is unchanged"
                : x.code == 5 ? "cannot write in that folder on " + h.alias
                : x.code == 6 ? "cannot replace the file on " + h.alias
                              : remote::explain(x.err, x.code);
        if (r.error.empty()) r.error = "the save failed";
    } else {
        r.mtime = token_of(first_line(x.out));
    }
    return r;
}

App::QuickListed list_project(const Host& h, const HostBook& book, const std::string& dir) {
    App::QuickListed r{.root = dir};
    const std::string script =
        "cd \"$1\" 2>/dev/null || exit 3; top=$(git rev-parse --show-toplevel 2>/dev/null) && cd \"$top\"; pwd; "
        "if [ -n \"$top\" ]; then git ls-files --cached --others --exclude-standard | head -50000; "
        "else find . -type f ! -path '*/.git/*' ! -path '*/node_modules/*' ! -path '*/build/*' ! -path '*/target/*' 2>/dev/null "
        "| sed 's|^[.]/||' | head -50000; fi";
    const auto x = remote::exec(h, book, sh_line(script, {dir}), {}, 20000);
    if (x.started && x.code == 0) {
        std::size_t at = 0;
        r.root = first_line(x.out, &at);
        for (std::size_t i = at, a = at; i <= x.out.size(); ++i)
            if (i == x.out.size() || x.out[i] == '\n') { if (i > a) r.files.emplace_back(x.out.substr(a, i - a)); a = i + 1; }
        r.truncated = r.files.size() >= 50000;
    }
    return r;
}

App::SearchAllDone search_project(std::uint64_t token, const Host& h, const HostBook& book, const std::string& dir, const std::string& query) {
    App::SearchAllDone r{.token = token, .root = dir};
    const bool fold = std::none_of(query.begin(), query.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    const std::string script =
        "cd \"$1\" 2>/dev/null || exit 3; top=$(git rev-parse --show-toplevel 2>/dev/null) && cd \"$top\"; pwd; "
        "if [ -n \"$top\" ]; then git grep -n -I --no-color -F $3 -e \"$2\" -- | head -2000; "
        "else grep -rnIF $3 --exclude-dir=.git --exclude-dir=node_modules --exclude-dir=build -e \"$2\" . 2>/dev/null | head -2000; fi";
    const auto x = remote::exec(h, book, sh_line(script, {dir, query, fold ? "-i" : ""}), {}, 20000);
    if (!x.started || (x.code != 0 && x.code != 1)) {
        r.error = x.timed_out ? "the search took too long on " + h.alias : remote::explain(x.err, x.code);
    } else {
        std::size_t at = 0;
        r.root = first_line(x.out, &at);
        parse_grep(x.out.substr(at), r.root, query, r);
    }
    return r;
}

}  // namespace remote_ops

// ── the effects: each one remote_ops call, its answer sent ─────────────────

void read_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path) {
    auto r = remote_ops::read_file(sid, h, book, path, false);
    if (r.binary) r.error = "a binary file: download it from the file pane (d)";   // no app to hand it to here
    r.binary = false;
    out.send(App::Msg{std::move(r)});
}

void reread_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path) {
    out.send(App::Msg{remote_ops::read_file(sid, h, book, path, true)});
}

void stat_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path) {
    if (auto r = remote_ops::stat_file(sid, h, book, path)) out.send(App::Msg{std::move(*r)});
}
void write_remote_file(Sink out, SessionId sid, Host h, HostBook book, std::string path, std::string text, std::uint64_t point) {
    out.send(App::Msg{remote_ops::write_file(sid, h, book, path, text, point)});
}
void list_remote_project(Sink out, Host h, HostBook book, std::string dir) {
    out.send(App::Msg{remote_ops::list_project(h, book, dir)});
}
void search_remote_project(Sink out, std::uint64_t token, Host h, HostBook book, std::string dir, std::string query) {
    out.send(App::Msg{remote_ops::search_project(token, h, book, dir, query)});
}

}  // namespace hand::fx
