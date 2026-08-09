// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Config loading: a VIBE file -> toe::Config, expressed with typed extractors
// and RAII over the C parser handle instead of a wall of nested null-checks.
//
// Parse errors are non-fatal by design: a terminal that refuses to open because
// one hex colour is malformed is not a friend. Every field that fails to parse
// simply keeps its default.

#ifndef HAND_CONFIG_HPP
#define HAND_CONFIG_HPP

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "toe/terminal.hpp"

#include "hand/config/handconfig.hpp"

#include "vibe.h"

namespace hand {

// A parsed "#rrggbb" colour. The only way to build one is `parse()`, so an
// invalid string can never masquerade as a colour — it's a std::nullopt.
class HexColor {
public:
    [[nodiscard]] static std::optional<HexColor> parse(std::string_view s) noexcept {
        if (s.size() != 7 || s.front() != '#') return std::nullopt;
        std::uint8_t rgb[3];
        for (int i = 0; i < 3; ++i) {
            const std::string_view byte = s.substr(1 + static_cast<std::size_t>(i) * 2, 2);
            unsigned v{};
            const auto [ptr, ec] = std::from_chars(byte.data(), byte.data() + byte.size(), v, 16);
            if (ec != std::errc{} || ptr != byte.data() + byte.size()) return std::nullopt;
            rgb[i] = static_cast<std::uint8_t>(v);
        }
        return HexColor{toe::rgb(rgb[0], rgb[1], rgb[2])};
    }

    [[nodiscard]] toe::Rgb rgb() const noexcept { return value_; }

private:
    explicit constexpr HexColor(toe::Rgb v) noexcept : value_(v) {}
    toe::Rgb value_{};
};

// Locate the config path: -c/--config, then $XDG_CONFIG_HOME (or ~/.config)
// + /hand/config.vibe. Returns nullopt when no home and no flag is given.
[[nodiscard]] std::optional<std::string> find_config(std::span<char *> args);

// Load `path` into a HandConfig, layering parsed values over the defaults.
// Missing keys and parse failures keep the default (a diagnostic is written to
// stderr only for a hard parse error). Never throws.
[[nodiscard]] HandConfig load_hand_config(std::string_view path);

// Serialize a HandConfig to `path` in canonical VIBE. Creates parent dirs.
// Returns true on success. Never throws.
[[nodiscard]] bool save_hand_config(const HandConfig &cfg, std::string_view path);

// Convenience: find + load a HandConfig from argv (defaults if none found).
[[nodiscard]] inline HandConfig load_hand_config(std::span<char *> args) {
    if (auto path = find_config(args)) return load_hand_config(*path);
    return HandConfig{};
}

// Legacy shim: load a toe::Config directly (used where only the engine subset
// is needed). Layers the file over `defaults`.
[[nodiscard]] toe::Config load_config(const toe::Config &defaults, std::string_view path);

[[nodiscard]] inline toe::Config load_config(std::span<char *> args) {
    if (auto path = find_config(args)) return load_hand_config(*path).to_toe();
    return toe::Config{};
}

} // namespace hand

#endif // HAND_CONFIG_HPP
