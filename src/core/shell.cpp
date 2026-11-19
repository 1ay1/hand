// hand::shell_integration — the scripts, written to a private runtime dir.

#include <hand/core/shell.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace hand {

namespace {

namespace fs = std::filesystem;

constexpr const char* kFish = R"FISH(# hand shell integration (injected; your config is untouched)
status is-interactive; or exit
if set -q HAND_ORIG_XDG_DATA_DIRS
    set -gx XDG_DATA_DIRS $HAND_ORIG_XDG_DATA_DIRS
    set -e HAND_ORIG_XDG_DATA_DIRS
else
    set -e XDG_DATA_DIRS
end
set -q __hand_si; and exit
set -g __hand_si 1

# hand reflows the screen itself on resize (like kitty / VTE): fish must not
# also repaint the prompt by counting rows up from the cursor, or the two
# disagree and stale prompt rows / blank gaps pile up on every drag.
set -g fish_handle_reflow 0

function __hand_osc7 --on-variable PWD
    printf '\e]7;file://%s%s\a' $hostname (string escape --style=url -- $PWD)
end
function __hand_prompt_start --on-event fish_prompt
    printf '\e]133;A\a'
    # mark where the typed command begins: after whatever prompt is set
    # (wrapped lazily, so prompts defined later in config.fish are kept)
    if not functions -q __hand_orig_prompt; and functions -q fish_prompt
        functions -c fish_prompt __hand_orig_prompt
        function fish_prompt
            __hand_orig_prompt
            printf '\e]133;B\a'
        end
    end
end
function __hand_preexec --on-event fish_preexec
    printf '\e]133;C\a'
end
function __hand_postexec --on-event fish_postexec
    printf '\e]133;D;%s\a' $status
end
__hand_osc7
)FISH";

constexpr const char* kZsh = R"ZSH(# hand shell integration (injected; your config is untouched)
if [[ -n "$HAND_ORIG_ZDOTDIR" ]]; then ZDOTDIR="$HAND_ORIG_ZDOTDIR"; unset HAND_ORIG_ZDOTDIR; else unset ZDOTDIR; fi
[[ -f "${ZDOTDIR:-$HOME}/.zshenv" ]] && source "${ZDOTDIR:-$HOME}/.zshenv"
if [[ -o interactive && -z "$__hand_si" ]]; then
  __hand_si=1
  autoload -Uz add-zsh-hook
  __hand_precmd() {
    local s=$?
    (( ${+__hand_ran} )) && print -n "\e]133;D;$s\a"
    unset __hand_ran
    print -n "\e]7;file://$HOST$PWD\a\e]133;A\a"
    [[ "$PS1" == *$'\e]133;B'* ]] || PS1="$PS1%{"$'\e]133;B\a'"%}"
  }
  __hand_preexec() { __hand_ran=1; print -n "\e]133;C\a"; }
  add-zsh-hook precmd __hand_precmd
  add-zsh-hook preexec __hand_preexec
fi
)ZSH";

constexpr const char* kBash = R"BASH(# hand shell integration (injected; your config is untouched)
[[ -f ~/.bashrc ]] && source ~/.bashrc
if [[ $- == *i* && -z "$__hand_si" ]]; then
  __hand_si=1
  __hand_prompt() {
    local s=$?
    printf '\e]133;D;%s\a\e]7;file://%s%s\a\e]133;A\a' "$s" "$HOSTNAME" "$PWD"
  }
  PROMPT_COMMAND="__hand_prompt${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
  PS1="$PS1\[\e]133;B\a\]"
  PS0="\[\e]133;C\a\]$PS0"
fi
)BASH";

fs::path runtime_dir() {
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return fs::path{x} / "hand" / "shell";
    return fs::temp_directory_path() / ("hand-shell-" + std::to_string(::getuid()));
}

bool write_file(const fs::path& p, const char* text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
    return static_cast<bool>(out);
}

}  // namespace

ShellLaunch shell_integration(const std::string& shell) {
    ShellLaunch out;
    const auto slash = shell.find_last_of('/');
    const std::string name = slash == std::string::npos ? shell : shell.substr(slash + 1);
    const fs::path dir = runtime_dir();
    if (name == "fish") {
        if (!write_file(dir / "fish" / "vendor_conf.d" / "hand-shell-integration.fish", kFish)) return {};
        const char* x = std::getenv("XDG_DATA_DIRS");
        const std::string orig = x && *x ? x : "";
        if (!orig.empty()) out.env.emplace_back("HAND_ORIG_XDG_DATA_DIRS", orig);
        out.env.emplace_back("XDG_DATA_DIRS", dir.string() + ":" + (orig.empty() ? "/usr/local/share:/usr/share" : orig));
        out.reflow_aware = true;
    } else if (name == "zsh") {
        if (!write_file(dir / "zsh" / ".zshenv", kZsh)) return {};
        if (const char* z = std::getenv("ZDOTDIR"); z && *z) out.env.emplace_back("HAND_ORIG_ZDOTDIR", z);
        out.env.emplace_back("ZDOTDIR", (dir / "zsh").string());
    } else if (name == "bash") {
        if (!write_file(dir / "bash" / "hand.bash", kBash)) return {};
        out.args = {"--rcfile", (dir / "bash" / "hand.bash").string()};
    }
    return out;
}

}  // namespace hand
