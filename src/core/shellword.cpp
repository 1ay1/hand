// hand::shellword — see shellword.hpp.

#include <hand/core/shellword.hpp>

#include <cctype>

namespace hand {

Dialect shell_dialect(std::string_view shell) {
    // the program's name: after the last separator, before an .exe
    const auto slash = shell.find_last_of("/\\");
    std::string_view base = slash == std::string_view::npos ? shell : shell.substr(slash + 1);
    std::string name;
    for (char ch : base) name += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (name.size() > 4 && name.ends_with(".exe")) name.resize(name.size() - 4);
    if (name == "pwsh" || name == "powershell" || name.starts_with("pwsh-")) return Dialect::powershell;
    if (name == "cmd") return Dialect::cmd;
    return Dialect::posix;
}

std::string quote_word(Dialect d, std::string_view s) {
    switch (d) {
        case Dialect::cmd: return "\"" + std::string{s} + "\"";
        case Dialect::powershell: {
            std::string q = "'";
            for (char ch : s) q += ch == '\'' ? std::string{"''"} : std::string(1, ch);
            return q + "'";
        }
        case Dialect::posix: break;
    }
    std::string q = "'";
    for (char ch : s) q += ch == '\'' ? std::string{"'\\''"} : std::string(1, ch);
    return q + "'";
}

std::string cd_line(Dialect d, std::string_view dir) {
    return (d == Dialect::cmd ? "cd /d " : "cd ") + quote_word(d, dir);
}

}  // namespace hand
