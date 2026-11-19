#pragma once
// hand::files — the folder a terminal is in, as a lazily-read tree.
//
// Not a "project": just the directory of the focused terminal, read on
// demand. A folder's children are read when it is first expanded and kept;
// re-rooting (focus moved to a terminal in another cwd) starts over.
// Hidden entries and the usual build/vendor dirs are skipped unless asked.
//
// Plain data in the model: the rows mayag's file_tree draws are a pure
// function of it (rows()).

#include <mayag/widget/file_tree.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace hand {

namespace fs = std::filesystem;

struct Entry {
    std::string name;
    bool        dir = false;
};

struct FileTree {
    fs::path                                 root;
    std::map<std::string, std::vector<Entry>> children;   ///< by path relative to root ("" = root)
    std::set<std::string>                    open;        ///< expanded folders (relative)
    bool                                     hidden = false;
    int                                      selected = 0;
    mayag::ScrollState                       scroll{};
    std::string                              active;      ///< relative path of the file open in an editor

    /// Read one directory (sorted: folders first, then names, case-folded).
    [[nodiscard]] static std::vector<Entry> read_dir(const fs::path& p, bool hidden) {
        static const std::set<std::string> skip{".git", "node_modules", "build", ".cache", "target", "__pycache__",
                                                "build-dev", ".zig-cache", "zig-out"};
        std::vector<Entry> out;
        std::error_code ec;
        for (fs::directory_iterator it{p, fs::directory_options::skip_permission_denied, ec}, end; !ec && it != end;
             it.increment(ec)) {
            std::string n = it->path().filename().string();
            if (!hidden && (n.starts_with('.') || skip.contains(n))) continue;
            std::error_code e2;
            out.push_back({std::move(n), it->is_directory(e2)});
        }
        const auto fold = [](std::string s) {
            for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        };
        std::ranges::sort(out, [&](const Entry& a, const Entry& b) {
            if (a.dir != b.dir) return a.dir;
            return fold(a.name) < fold(b.name);
        });
        return out;
    }

    void set_root(fs::path p) {
        std::error_code ec;
        p = fs::weakly_canonical(p, ec);
        if (p == root && !children.empty()) return;
        root = std::move(p);
        children.clear();
        open.clear();
        selected = 0;
        scroll = {};
        children[""] = read_dir(root, hidden);
    }
    void refresh() {
        for (auto& [rel, kids] : children) kids = read_dir(rel.empty() ? root : root / rel, hidden);
    }

    /// One flattened row, and the relative path it stands for.
    struct Flat { mayag::dsl::TreeRow row; std::string rel; };

    [[nodiscard]] std::vector<Flat> flat() const {
        std::vector<Flat> out;
        const auto walk = [&](const auto& self, const std::string& rel, int depth) -> void {
            const auto it = children.find(rel);
            if (it == children.end()) return;
            for (const Entry& e : it->second) {
                const std::string path = rel.empty() ? e.name : rel + "/" + e.name;
                const bool is_open = e.dir && open.contains(path);
                out.push_back({{.name = e.name, .depth = depth, .dir = e.dir, .open = is_open, .active = path == active},
                               path});
                if (is_open) self(self, path, depth + 1);
            }
        };
        walk(walk, "", 0);
        return out;
    }
    [[nodiscard]] std::vector<mayag::dsl::TreeRow> rows() const {
        std::vector<mayag::dsl::TreeRow> r;
        for (auto& f : flat()) r.push_back(std::move(f.row));
        return r;
    }

    /// Expand/collapse the folder at relative path `rel`, reading it once.
    void toggle(const std::string& rel) {
        if (open.erase(rel) > 0) return;
        open.insert(rel);
        if (!children.contains(rel)) children[rel] = read_dir(root / rel, hidden);
    }
    void set_open(const std::string& rel, bool on) {
        if (on != open.contains(rel)) toggle(rel);
    }

    /// "~/src/hand" for display.
    [[nodiscard]] std::string display_root() const {
        std::string s = root.string();
        if (const char* home = std::getenv("HOME"); home && s.starts_with(home)) s = "~" + s.substr(std::strlen(home));
        return s;
    }
};

}  // namespace hand
