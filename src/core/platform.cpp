// hand::plat — the parts that are the same everywhere.
//
// glob_paths is expressed in std::filesystem on top of glob_match, so the
// ssh_config `Include` directive behaves identically on every platform
// (POSIX glob() sorts and drops non-existent paths; so does this).

#include <hand/core/platform.hpp>

#include <algorithm>
#include <filesystem>
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

}  // namespace hand::plat
