#pragma once
// hand::workspace — tabs, and the split tree inside each tab.
//
// A tab's panes form a binary tree: a leaf is a terminal, a branch splits
// its area between two subtrees along an axis at a ratio. Stored flat (a
// vector of nodes addressed by index) so the tree is a plain, copyable,
// comparable value -- undo and replay are copies, and a pane's identity is
// its PaneId, stable across splits and closes.

#include <mayag/mayag.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hand {

struct PaneId {
    std::uint32_t v = 0;
    friend constexpr auto operator<=>(const PaneId&, const PaneId&) = default;
};

enum class PaneState : std::uint8_t { idle, running, done, failed };

/// What a pane shows about itself. The terminal engine will fill this in;
/// for now the app does.
struct PaneInfo {
    std::string title;      ///< OSC title, or the command
    std::string cwd;        ///< "~/src/hand"
    std::string process;    ///< foreground process: "zsh", "nvim"
    PaneState   state = PaneState::idle;
    int         exit_code = 0;
    float       progress = -1.0f;   ///< OSC 9;4, 0..1; <0 none
    bool        bell = false;
};

/// A node of the split tree: a leaf (one pane) or a split.
struct Leaf { PaneId pane; };
struct Split {
    mayag::Axis   axis = mayag::Axis::horizontal;   ///< horizontal: side by side
    float         ratio = 0.5f;                     ///< share of the FIRST child
    std::uint32_t a = 0, b = 0;                     ///< child node indices
};
using TreeNode = std::variant<Leaf, Split>;

struct Tab {
    std::vector<TreeNode> nodes;   ///< nodes[root] is the root
    std::uint32_t         root = 0;
    PaneId                focus{};
    std::optional<PaneId> zoom{};  ///< a pane shown alone
    std::string           name;    ///< user-set name; empty: derived

    [[nodiscard]] static Tab single(PaneId p) {
        Tab t;
        t.nodes.push_back(Leaf{p});
        t.focus = p;
        return t;
    }

    /// Index of the leaf holding `p`, if any.
    [[nodiscard]] std::optional<std::uint32_t> leaf_of(PaneId p) const {
        for (std::uint32_t i = 0; i < nodes.size(); ++i)
            if (const auto* l = std::get_if<Leaf>(&nodes[i]); l && l->pane == p) return i;
        return std::nullopt;
    }

    /// Replace the leaf for `p` by a split of (p, fresh): the new pane takes
    /// the second half and focus. Returns false when `p` is not in the tab.
    bool split(PaneId p, PaneId fresh, mayag::Axis axis) {
        const auto at = leaf_of(p);
        if (!at) return false;
        const auto a = static_cast<std::uint32_t>(nodes.size());
        nodes.push_back(Leaf{p});
        nodes.push_back(Leaf{fresh});
        nodes[*at] = Split{axis, 0.5f, a, a + 1};
        focus = fresh;
        zoom.reset();
        return true;
    }

    /// Remove pane `p`: its sibling takes the parent's place.
    bool close(PaneId p) {
        const auto at = leaf_of(p);
        if (!at) return false;
        for (std::uint32_t i = 0; i < nodes.size(); ++i) {
            const auto* s = std::get_if<Split>(&nodes[i]);
            if (s == nullptr || (s->a != *at && s->b != *at)) continue;
            const std::uint32_t keep = s->a == *at ? s->b : s->a;
            nodes[i] = nodes[keep];   // the sibling subtree moves up (its children keep their indices)
            if (focus == p) focus = first_leaf(i);
            if (zoom == p) zoom.reset();
            return true;
        }
        return false;   // p is the root: closing the last pane closes the tab
    }

    [[nodiscard]] PaneId first_leaf(std::uint32_t from) const {
        std::uint32_t i = from;
        for (int guard = 0; guard < 64; ++guard) {
            if (const auto* l = std::get_if<Leaf>(&nodes[i])) return l->pane;
            i = std::get<Split>(nodes[i]).a;
        }
        return focus;
    }

    /// Panes in reading order.
    [[nodiscard]] std::vector<PaneId> panes() const {
        std::vector<PaneId> out;
        const auto walk = [&](const auto& self, std::uint32_t i) -> void {
            if (const auto* l = std::get_if<Leaf>(&nodes[i])) { out.push_back(l->pane); return; }
            const auto& s = std::get<Split>(nodes[i]);
            self(self, s.a);
            self(self, s.b);
        };
        if (!nodes.empty()) walk(walk, root);
        return out;
    }
};

}  // namespace hand
