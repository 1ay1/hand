#pragma once
// hand::demo — sample screens, until toe drives the panes.
//
// Written in mayag's term_row markup and resolved against the CURRENT
// scheme's palette, so they recolour live like a real terminal would.
// This file is the only place that knows the panes are fake.

#include "core/scheme.hpp"

#include <mayag/widget/terminal_view.hpp>

#include <initializer_list>
#include <string_view>

namespace hand::demo {

using mayag::dsl::TermGrid;
using mayag::dsl::TermMark;

enum class Screen : std::uint8_t { shell, editor, tests, build, logs };

namespace detail {
struct Line { std::string_view markup; TermMark mark = TermMark::none; };
[[nodiscard]] inline TermGrid grid(std::initializer_list<Line> lines, const Scheme& s, int cur_row, int cur_col,
                                   int cols = 120) {
    TermGrid g;
    g.cols = cols;
    g.rows = static_cast<int>(lines.size());
    for (const auto& l : lines) g.lines.push_back(mayag::dsl::term_row(l.markup, cols, l.mark, s.ansi));
    g.cursor_row = cur_row;
    g.cursor_col = cur_col;
    return g;
}
}  // namespace detail

[[nodiscard]] inline TermGrid screen(Screen which, const Scheme& s) {
    using detail::grid;
    using M = TermMark;
    switch (which) {
        case Screen::shell:
            return grid({
                {"{b}{B}~/src/hand{0} {d}on{0} {m}\u2387 main{0} {d}[!2]{0}", M::ok},
                {"{g}{B}\u276F{0} git log --oneline -6"},
                {"{y}a1f3c9e{0} {c}(HEAD -> main){0} settings view generated from the typed table"},
                {"{y}7be20d4{0} workspace: mayag pane_split + pane_frame + terminal_view"},
                {"{y}33c81aa{0} mayag: font family roles through the shaper"},
                {"{y}e09d215{0} theme: Appearance -> Seed -> mayag::looks"},
                {"{y}5d11b70{0} scheme: sixteen slots, contrast checked at compile time"},
                {"{y}0c4e8a2{0} initial commit"},
                {""},
                {"{b}{B}~/src/hand{0} {d}on{0} {m}\u2387 main{0} {d}[!2]{0}", M::ok},
                {"{g}{B}\u276F{0} eza -l --git include/hand"},
                {"{b}{B}d{0}{y}r{0}{r}w{0}{g}x{0}{y}r{0}{d}-{0}{g}x{0}{y}r{0}{d}-{0}{g}x{0}  {d}-{0}  {y}ayush{0} {d}19 Nov 07:12{0}  {d}--{0} {b}{B}core{0}"},
                {"{b}{B}d{0}{y}r{0}{r}w{0}{g}x{0}{y}r{0}{d}-{0}{g}x{0}{y}r{0}{d}-{0}{g}x{0}  {d}-{0}  {y}ayush{0} {d}19 Nov 07:40{0}  {g}-M{0} {b}{B}widget{0}"},
                {".{y}r{0}{r}w{0}{d}-{0}{y}r{0}{d}--{0}{y}r{0}{d}--{0}  {G}6.1k{0} {y}ayush{0} {d}19 Nov 07:41{0}  {g}-M{0} app.hpp"},
                {".{y}r{0}{r}w{0}{d}-{0}{y}r{0}{d}--{0}{y}r{0}{d}--{0}  {G}3.4k{0} {y}ayush{0} {d}19 Nov 07:38{0}  {g}N-{0} demo.hpp"},
                {""},
                {"{b}{B}~/src/hand{0} {d}on{0} {m}\u2387 main{0} {d}[!2]{0}", M::ok},
                {"{g}{B}\u276F{0} cmake --build build -j"},
                {"{d}[42/118]{0} Building CXX object {c}src/main.cpp.o{0}"},
                {"{d}[43/118]{0} Building CXX object {c}tests/theme_test.cpp.o{0}"},
                {"{Y}{B}warning:{0} unused variable {B}'gutter'{0} {d}[-Wunused-variable]{0}"},
                {"{d}[44/118]{0} Linking CXX executable {g}{B}hand{0}"},
                {""},
                {"{b}{B}~/src/hand{0} {d}on{0} {m}\u2387 main{0} {d}took 41s{0}", M::prompt},
                {"{g}{B}\u276F{0} hand --dev"},
            }, s, 24, 13);
        case Screen::editor:
            return grid({
                {"{d}  1 {0}{m}#pragma once{0}"},
                {"{d}  2 {0}"},
                {"{d}  3 {0}{d}{I}// the whole configuration, as one value{0}"},
                {"{d}  4 {0}{m}struct{0} {Y}Appearance{0} {{"},
                {"{d}  5 {0}    {Y}SchemeId{0} scheme {c}={0} SchemeId::obsidian;"},
                {"{d}  6 {0}    {Y}Chrome{0}   chrome {c}={0} Chrome::terminal;"},
                {"{d}  7 {0}    {Y}Bounded{0}<{m}float{0}, {r}9.0f{0}, {r}22.0f{0}> font_size{{{r}13.0f{0}}};"},
                {"{d}  8 {0}    {Y}Bounded{0}<{m}float{0}, {r}0.0f{0}, {r}1.6f{0}>  roundness{{{r}0.0f{0}}};"},
                {"{d}  9 {0}    {m}friend constexpr bool{0} {b}operator=={0}(...) = {m}default{0};"},
                {"{d} 10 {0}}};"},
                {"{d} 11 {0}"},
                {"{d} 12 {0}{d}[[nodiscard]]{0} {m}inline{0} mayag::Theme {b}theme_of{0}({m}const{0} Appearance& a) {{"},
                {"{d} 13 {0}    {m}const{0} mayag::Seed seed {c}={0} {b}seed_of{0}(a);"},
                {"{d} 14 {0}    {m}switch{0} (a.chrome) {{"},
                {"{d} 15 {0}        {m}case{0} Chrome::glass: {m}return{0} mayag::looks::{b}glass{0}(seed);"},
                {"{d} 16 {0}        {m}default{0}:            {m}return{0} mayag::looks::{b}terminal{0}(seed);"},
                {"{d} 17 {0}    }"},
                {"{d} 18 {0}}"},
                {"{d}  ~{0}"}, {"{d}  ~{0}"}, {"{d}  ~{0}"}, {"{d}  ~{0}"},
                {"{V}{B} NORMAL {0}{V} theme.hpp {0}{d}  cpp \u00B7 utf-8 \u00B7 LF {0}                              {d}13:24  72%{0}"},
            }, s, 12, 30);
        case Screen::tests:
            return grid({
                {"{b}{B}~/src/hand{0} {g}{B}\u276F{0} ctest --output-on-failure", M::error},
                {"{d}Test project /home/ayush/src/hand/build{0}"},
                {"1/5 Test #1: scheme ...........   {g}{B}Passed{0}  {d}0.01 sec{0}"},
                {"2/5 Test #2: theme ............   {g}{B}Passed{0}  {d}0.02 sec{0}"},
                {"3/5 Test #3: settings .........   {g}{B}Passed{0}  {d}0.01 sec{0}"},
                {"4/5 Test #4: workspace ........   {g}{B}Passed{0}  {d}0.01 sec{0}"},
                {"5/5 Test #5: app ..............{r}***{B}Failed{0}  {d}0.40 sec{0}"},
                {"  {d}expected{0} {B}focus == pane.2{0}"},
                {"  {d}     got{0} {r}focus == pane.1{0}"},
                {""},
                {"{Y}{B}80% tests passed{0}, 1 tests failed out of 5"},
                {""},
                {"{b}{B}~/src/hand{0} {r}{B}\u276F{0} ", M::prompt},
            }, s, 12, 13);
        case Screen::build:
            return grid({
                {"{b}{B}~/src/toe{0} {g}{B}\u276F{0} cmake --build build -j12", M::running},
                {"{d}[ 81/203]{0} Building CXX object src/term/screen.cpp.o"},
                {"{d}[ 82/203]{0} Building CXX object src/term/update.cpp.o"},
                {"{d}[ 83/203]{0} Building CXX object src/vt/parser.cpp.o"},
                {"{d}[ 84/203]{0} Building CXX object tests/reflow_test.cpp.o"},
                {"{d}[ 85/203]{0} Building CXX object tests/sgr_test.cpp.o"},
            }, s, 5, 50);
        case Screen::logs:
            return grid({
                {"{d}deploy@prod-01{0}:{b}/var/log{0}$ journalctl -fu api", M::ok},
                {"{d}07:41:02{0} {g}INFO {0} api: listening on :8443"},
                {"{d}07:41:07{0} {g}INFO {0} GET /v1/sessions {d}200 3.1ms{0}"},
                {"{d}07:41:09{0} {Y}WARN {0} pool: 92% connections in use"},
                {"{d}07:41:12{0} {g}INFO {0} POST /v1/events {d}201 8.4ms{0}"},
                {"{d}07:41:15{0} {r}{B}ERROR{0} upstream timed out after {B}30s{0} {d}(req 7f3a){0}"},
                {"{d}07:41:16{0} {g}INFO {0} GET /healthz {d}200 0.2ms{0}"},
            }, s, 6, 0);
    }
    return {};
}

}  // namespace hand::demo
