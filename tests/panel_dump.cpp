// Render the settings panel headlessly and print it as text, so the LAYOUT can
// be inspected and iterated without launching a GPU window. Colours are lost
// (this is a structural view), but alignment, density, framing and wasted space
// -- which is what makes a UI look toy vs. considered -- are all visible.
#include "hand/glyph/glyph.hpp"
#include "hand/settings_panel.hpp"

#include <cstdio>
#include <string>

using namespace glyph;

int main(int argc, char **argv) {
    const int w = (argc > 1) ? std::atoi(argv[1]) : 100;
    const int h = (argc > 2) ? std::atoi(argv[2]) : 34;
    const int section = (argc > 3) ? std::atoi(argv[3]) : 0;

    Buffer buf(w, h);

    // Drive the real panel through its public render path so what we print is
    // exactly what the app draws.
    hand::SettingsPanel panel;
    panel.open(hand::Settings{});
    bool changed = false;
    panel.render(buf, changed);
    (void)section; // input queue is private; sections are exercised in the app

    // Print with a border so trailing space (and thus alignment) is visible.
    const Cell *cells = buf.data();
    std::printf("+%s+\n", std::string(static_cast<std::size_t>(w), '-').c_str());
    for (int y = 0; y < h; ++y) {
        std::printf("|");
        for (int x = 0; x < w; ++x) {
            const char32_t cp = cells[static_cast<std::size_t>(y) * w + x].cp;
            if (cp == 0 || cp == U' ') std::printf(" ");
            else if (cp < 0x80) std::printf("%c", static_cast<char>(cp));
            else std::printf("?"); // box-drawing/glyphs: shape only
        }
        std::printf("|\n");
    }
    std::printf("+%s+\n", std::string(static_cast<std::size_t>(w), '-').c_str());
    return 0;
}
