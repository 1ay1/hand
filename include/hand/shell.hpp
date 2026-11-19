#pragma once
// hand::shell_integration — teach the user's shell to mark its prompts.
//
// Like kitty and ghostty, hand injects a tiny script at launch, without
// touching the user's dotfiles:
//
//   fish  XDG_DATA_DIRS gets a dir with fish/vendor_conf.d/hand.fish (auto-loaded)
//   zsh   ZDOTDIR points at a .zshenv that restores the real ZDOTDIR first
//   bash  --rcfile with a file that sources ~/.bashrc first
//
// The script emits OSC 133 (A prompt, B input, C output, D;exit) and OSC 7
// (cwd). toe folds those into its CommandLog: prompt marks, exit codes,
// durations, jump-to-prompt, copy-last-output, notifications.

#include <string>
#include <utility>
#include <vector>

namespace hand {

struct ShellLaunch {
    std::vector<std::pair<std::string, std::string>> env;   ///< set in the child
    std::vector<std::string>                         args;  ///< after argv[0]
};

/// What to add to the launch of `shell` (a path or name). Empty when the
/// shell is not one hand knows, or the scripts could not be written.
[[nodiscard]] ShellLaunch shell_integration(const std::string& shell);

}  // namespace hand
