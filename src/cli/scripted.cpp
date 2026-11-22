// hand::cli — the headless runtime for the scripted modes (--headless, --png):
// the demo screen, then the self-test or one shot. Its own file because
// instantiating the headless host is half the cost of main.cpp; apart, the
// two compile in parallel.

#include "cli.hpp"

#include <string>
#include <string_view>

namespace hand::cli {

namespace {
constexpr std::string_view kDemo =
    "\x1b[1;32m~/src/hand\x1b[0m \x1b[35mmain\x1b[0m \x1b[2m+2\x1b[0m\r\n"
    "\x1b[1;34m❯\x1b[0m ls --color\r\n"
    "\x1b[1;34minclude\x1b[0m  \x1b[1;34msrc\x1b[0m  CMakeLists.txt  \x1b[32mbuild.sh\x1b[0m\r\n"
    "\x1b[1;34m❯\x1b[0m printf colours\r\n"
    "\x1b[30;40m  \x1b[41m  \x1b[42m  \x1b[43m  \x1b[44m  \x1b[45m  \x1b[46m  \x1b[47m  \x1b[0m\r\n"
    "\x1b[1mbold\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m \x1b[4:3;58:5:1mcurly\x1b[0m "
    "\x1b[9mstrike\x1b[0m \x1b[7mreverse\x1b[0m \x1b[38;2;255;121;198mtruecolor\x1b[0m 漢字 ✓\r\n"
    "\x1b[1;34m❯\x1b[0m ";
}  // namespace

int run_scripted(mayag::AppConfig ac, App::Flags flags, mayag::Vec2 size, std::optional<Shot> shot) {
    return mayag::run_headless<App>(std::move(ac), std::move(flags), [&](auto& rt) {
        rt.send(App::Msg{App::Output{std::string{kDemo}}});
        rt.tick();
        if (!shot) return selftest(rt, size);
        return shoot(rt, size, shot->name, shot->out);
    });
}

}  // namespace hand::cli
