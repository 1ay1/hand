// hand::plat — the parts that are the same everywhere.
//
// glob_paths is expressed in std::filesystem on top of glob_match, so the
// ssh_config `Include` directive behaves identically on every platform
// (POSIX glob() sorts and drops non-existent paths; so does this).

#include <mayag/core/target.hpp>
#include <hand/core/platform.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace hand::plat {

std::vector<std::string> glob_paths(const std::string& pattern) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    if (pattern.empty()) return out;

    // No wildcard: it is a plain path, included if it exists.
    if (pattern.find_first_of("*?[") == std::string::npos) {
        std::error_code ec;
        if (fs::exists(pattern, ec)) out.push_back(pattern);
        return out;
    }

    // Split off the directory; ssh only globs the final component.
    const std::size_t slash = pattern.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string{"."} : pattern.substr(0, slash + 1);
    const std::string leaf = slash == std::string::npos ? pattern : pattern.substr(slash + 1);

    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;   // glob() hides dotfiles
        if (glob_match(leaf, name)) out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string find_program(std::string_view name) {
    namespace fs = std::filesystem;
    // the OS's differences, as data: the PATH separator, the suffix a bare
    // name gets, and what "runnable" means (an exec bit; on Windows, existing)
    constexpr char sep = MAYAG_OS_WINDOWS ? ';' : ':';
    constexpr std::string_view suffix = MAYAG_OS_WINDOWS ? ".exe" : "";
    const auto runnable = [](const fs::path& p) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return false;
        if constexpr (MAYAG_OS_WINDOWS) return true;
        const auto pm = fs::status(p, ec).permissions();
        return (pm & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
    };
    if (name.empty()) return {};
    if (name.find_first_of("/\\") != std::string_view::npos)   // a path, not a name
        return runnable(fs::path{std::string{name}}) ? std::string{name} : std::string{};
    std::string file{name};
    if (!suffix.empty() && !file.ends_with(suffix)) file += suffix;
    const char* path = std::getenv("PATH");
    for (std::string_view p = path ? path : ""; !p.empty();) {
        const auto c = p.find(sep);
        const std::string_view dir = p.substr(0, c);
        if (!dir.empty())
            if (const fs::path cand = fs::path{std::string{dir}} / file; runnable(cand)) return cand.string();
        if (c == std::string_view::npos) break;
        p.remove_prefix(c + 1);
    }
    return {};
}

}  // namespace hand::plat
