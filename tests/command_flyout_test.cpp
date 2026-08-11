// SPDX-License-Identifier: LGPL-2.0-or-later
//
// command_flyout_test — the rail-hover -> selected-command rule. As the pointer
// slides down the minimap rail, the flyout must always highlight the NEAREST
// command whose prompt sits at or above that rail row (a scrollbar-thumb feel),
// and the first command when the pointer is above them all.

#include <cstdio>
#include <vector>

#include "hand/command_flyout.hpp"
#include "hand/glyph/buffer.hpp"

static int failures = 0;
static void ck(bool cond, const char *msg) {
    if (!cond) { std::printf("FAIL: %s\n", msg); ++failures; }
}

static toe::CommandView cmd(std::uint64_t id, std::int64_t prompt_row) {
    toe::CommandView v;
    v.id = id;
    v.command = "cmd" + std::to_string(id);
    v.prompt_row = prompt_row;
    return v;
}

int main() {
    using hand::CommandFlyout;

    // Three commands at rows 0, 10, 25.
    std::vector<toe::CommandView> items{cmd(1, 0), cmd(2, 10), cmd(3, 25)};

    // Hovering exactly on a prompt row selects that command.
    ck(CommandFlyout::pick_hover(items, 0) == 0, "row 0 -> first command");
    ck(CommandFlyout::pick_hover(items, 10) == 1, "row 10 -> second command");
    ck(CommandFlyout::pick_hover(items, 25) == 2, "row 25 -> third command");

    // Between two prompts selects the earlier (nearest at-or-above).
    ck(CommandFlyout::pick_hover(items, 5) == 0, "row 5 -> still first");
    ck(CommandFlyout::pick_hover(items, 9) == 0, "row 9 -> still first");
    ck(CommandFlyout::pick_hover(items, 11) == 1, "row 11 -> second");
    ck(CommandFlyout::pick_hover(items, 24) == 1, "row 24 -> second");
    ck(CommandFlyout::pick_hover(items, 999) == 2, "far below -> last command");

    // Above the first prompt selects the first (list still tracks).
    // (No command has prompt_row < 0 here, so anything below row 0 is the same.)
    std::vector<toe::CommandView> shifted{cmd(1, 5), cmd(2, 12)};
    ck(CommandFlyout::pick_hover(shifted, 0) == 0, "above first prompt -> first");
    ck(CommandFlyout::pick_hover(shifted, 3) == 0, "just above first -> first");

    // Commands lacking a resolved row (prompt_row < 0) are skipped.
    std::vector<toe::CommandView> mixed{cmd(1, -1), cmd(2, 8), cmd(3, -1), cmd(4, 20)};
    ck(CommandFlyout::pick_hover(mixed, 9) == 1, "skip unrowed, pick rowed above");
    ck(CommandFlyout::pick_hover(mixed, 30) == 3, "skip unrowed, pick last rowed");
    ck(CommandFlyout::pick_hover(mixed, 0) == 1, "above -> first ROWED command");

    // No rowed commands at all -> -1.
    std::vector<toe::CommandView> none{cmd(1, -1), cmd(2, -1)};
    ck(CommandFlyout::pick_hover(none, 5) == -1, "no rows -> nothing selected");

    // Empty list -> -1.
    ck(CommandFlyout::pick_hover({}, 5) == -1, "empty -> nothing selected");

    // Render smoke: the card paints without going out of bounds and writes the
    // header text somewhere in the buffer.
    {
        CommandFlyout f;
        f.set_state_for_test(items, 10, 60);
        glyph::Buffer buf;
        buf.resize(80, 24);
        buf.clear(glyph::Style{});
        f.render(buf); // must not crash / write OOB (put() is bounds-checked)
        bool found_c = false;
        const glyph::Cell *d = buf.data();
        for (int i = 0; i < buf.width() * buf.height(); ++i)
            if (d[i].cp == U'C') { found_c = true; break; }
        ck(found_c, "render draws the COMMANDS header");
    }

    if (failures == 0) std::printf("command_flyout: all tests passed\n");
    return failures ? 1 : 0;
}
