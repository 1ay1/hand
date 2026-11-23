#pragma once
// hand::shellword — text for a shell to read, quoted for THAT shell.
//
//   shell_dialect("/usr/bin/fish")     -> Dialect::posix
//   shell_dialect("pwsh.exe")          -> Dialect::powershell
//   quote_word(Dialect::cmd, path)     -> "C:\My Files"
//   cd_line(d, path)                   -> cd 'x' / cd /d "x"
//
// The dialect is a property of the SHELL, not of the OS hand was built for:
// pwsh on macOS quotes like pwsh, bash under Git Bash on Windows quotes like
// bash. One implementation, chosen by data, tested on every machine
// (src/cli/checks/ssh.cpp). Every place hand types a path or a command into
// a shell -- the file pane's cd and paste, the host manager's "edit
// config", "open a terminal here", every remote command -- goes through here.

#include <string>
#include <string_view>

namespace hand {

enum class Dialect { posix, powershell, cmd };

/// What `shell` (a path or a name, any case, with or without .exe) speaks.
/// Anything not PowerShell or cmd is POSIX (sh, bash, zsh, fish, dash, ...).
[[nodiscard]] Dialect shell_dialect(std::string_view shell);

/// `s` as ONE word for that shell, whatever it contains.
///   posix       'it'\''s'        (fish reads it the same)
///   powershell  'it''s'
///   cmd         "C:\Program Files"   (cmd has no escape for "; none are in paths)
[[nodiscard]] std::string quote_word(Dialect d, std::string_view s);

/// The line that changes the shell's directory to `dir` (cmd needs /d to
/// change drive too).
[[nodiscard]] std::string cd_line(Dialect d, std::string_view dir);

}  // namespace hand
