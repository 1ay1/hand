// hand::cli::checks — Tabs keeps its invariants: never empty, and the tab in
// front stays in front while others come, go and move around it.

#include "suites.hpp"

#include <hand/app/model/tabs.hpp>

#include <string>

namespace hand::cli::checks {

namespace {
/// A session told apart by its title, with an id of its own.
Session named(const char* t) {
    static SessionIds ids;
    Session s;
    s.sid = ids.next();
    s.title = t;
    return s;
}
/// The strip, left to right, with the front tab in [brackets].
std::string strip(const Tabs& t) {
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (!out.empty()) out += ' ';
        out += i == t.index() ? "[" + t[i].title + "]" : t[i].title;
    }
    return out;
}
}  // namespace

void tab_list([[maybe_unused]] Runtime& rt, [[maybe_unused]] mayag::Vec2 size, Check& ok) {
    Tabs t;
    ok(t.size() == 1 && t.index() == 0, "tabs: a window starts with one tab, in front");
    t.active() = named("a");
    t.push_back(named("b"));
    t.push_back(named("c"));
    ok(strip(t) == "[a] b c", "tabs: appending leaves the front tab in front");
    t.select(1);
    t.insert(0, named("z"));
    ok(strip(t) == "z a [b] c", "tabs: inserting before the front tab keeps it in front");
    t.insert(t.index() + 1, named("s"));
    ok(strip(t) == "z a [b] s c", "tabs: an ssh tab opens beside the front one");
    t.erase(0);
    ok(strip(t) == "a [b] s c", "tabs: closing one to the left keeps the front tab");
    t.erase(t.index());
    ok(strip(t) == "a [s] c", "tabs: closing the front tab brings its right neighbour forward");
    t.select(2);
    t.erase(2);
    ok(strip(t) == "a [s]", "tabs: closing the last tab in front brings its left neighbour forward");
    ok(t.move_active(Step::backward) && strip(t) == "[s] a", "tabs: moving the front tab carries it, still in front");
    ok(!t.move_active(Step::backward) && strip(t) == "[s] a", "tabs: it does not move past the end");
    ok(t.find(t[1].sid).value_or(9) == 1 && !t.find(SessionId{424242}), "tabs: a session is found by its id, and only then");
}

}  // namespace hand::cli::checks
