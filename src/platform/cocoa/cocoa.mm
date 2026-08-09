// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Cocoa + NSOpenGL backend for the toe::App contract — the macOS frontend.
//
// This is hand's native window on macOS, the peer of the Wayland and X11
// backends. It brings up an NSWindow with an NSOpenGLView (a 3.3 core-profile
// context — toe's GLSL is #version 330, inside macOS's 4.1 ceiling), translates
// NSEvent keyboard/mouse input into toe's platform-neutral win::Event sum type,
// and satisfies the same structural `App` concept every other backend does — so
// toe::run<CocoaApp> is the same fully-monomorphic loop, just inlined to Cocoa
// calls. No engine code changes; toe never learns the word "Cocoa".
//
// ── the readiness wait, without a window fd ────────────────────────────────
// X11/Wayland hand toe a pollable connection fd; Cocoa has none — native events
// land on the main thread's NSApp event queue. So event_fd() is -1 and
// wait_readable() bridges the two worlds: it drains the Cocoa queue first (any
// event => window-ready), else it poll()s the PTY fd but CAPS the block at a few
// ms so input that can't wake poll() is still serviced promptly. One tiny idle
// wakeup is the price of not having a window fd; it's invisible at 60fps and
// zero while the child floods output (the pty fd wakes us immediately).

#include "hand/app.hpp"
#include "hand/platform/surface.hpp"

#include "toe/run.hpp" // toe::run<App>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <poll.h>
#include <time.h>
#include <mach/mach_time.h>

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
// NOTE: we deliberately do NOT include <OpenGL/gl3.h> here. toe's headers pull
// in epoxy/gl.h (its GL loader), and including gl3.h alongside it trips a
// "gl.h and gl3.h both included" warning. We issue no GL calls in this TU
// anyway — all rendering is toe's; we only manage the NSOpenGLContext.

// ───────────────────────── Objective-C view/window ─────────────────────────
//
// The view owns the GL context and records input into a plain C++ inbox the
// C++ surface drains. Keeping ObjC objects thin (no C++ types in their ivars
// beyond a raw back-pointer) keeps the ARC/■ boundary clean.

namespace hand::platform {
struct CocoaInbox; // fwd: the C++ event inbox the view writes into
}

using hand::platform::CocoaInbox;

// A pending native event, pre-translated to toe's vocabulary at capture time so
// poll_events() is a trivial drain.
namespace hand::platform {

struct PendingKey {
    toe::KeyEvent ev;
};
struct PendingText {
    std::string utf8;
};
struct PendingResize {
    toe::PixelSize size;
};
struct PendingMouse {
    enum class Kind { down, up, move, wheel } kind;
    toe::win::MouseButton button{};
    toe::Modifiers mods{};
    bool button_down = false; // for move: is a button held (a drag)?
    int x = 0, y = 0;
    double dx = 0, dy = 0;
};
struct PendingFocus {
    bool focused = false;
};
struct PendingZoom {
    int delta = 0;     // +1 / -1 notch
    int absolute = -1; // exact px when >= 0 (used for reset-to-default)
};

// The inbox: a queue of translated events plus latched lifecycle flags. The
// ObjC view appends; the C++ surface drains in poll_events().
struct CocoaInbox {
    std::vector<std::variant<PendingKey, PendingText, PendingResize, PendingMouse, PendingFocus,
                             PendingZoom>>
        events;
    bool close_requested = false;
    bool saw_event = false; // any native event since the last wait (window-ready)
    // The font size the app launched with, so Cmd-0 can reset to it.
    int default_font_px = 0;
    // Fractional scroll accumulators: trackpad deltas are sub-"line"; we bank
    // them and emit one discrete wheel step per threshold crossed so small
    // two-finger scrolls aren't silently dropped by integer truncation.
    double scroll_ax = 0.0, scroll_ay = 0.0;
};

} // namespace hand::platform

// ── keyboard translation ───────────────────────────────────────────────────
// Map an NSEvent keyDown into either a SpecialKey or committed text, plus mods.
namespace {

toe::Modifiers mods_of(NSEventModifierFlags f) {
    return toe::Modifiers{
        .ctrl = (f & NSEventModifierFlagControl) != 0,
        // Treat Option as Alt (meta) so terminal apps get ESC-prefixed input.
        .alt = (f & NSEventModifierFlagOption) != 0,
        .shift = (f & NSEventModifierFlagShift) != 0,
    };
}

// macOS virtual keycodes for the keys that map to toe::SpecialKey. These are
// layout-independent (physical), which is exactly right for arrows/function keys.
std::optional<toe::SpecialKey> special_of_keycode(unsigned short kc) {
    switch (kc) {
    case 0x24: return toe::SpecialKey::Enter;      // Return
    case 0x4C: return toe::SpecialKey::KpEnter;    // Keypad Enter
    case 0x33: return toe::SpecialKey::Backspace;  // Delete (backspace)
    case 0x30: return toe::SpecialKey::Tab;
    case 0x35: return toe::SpecialKey::Escape;
    case 0x7E: return toe::SpecialKey::Up;
    case 0x7D: return toe::SpecialKey::Down;
    case 0x7B: return toe::SpecialKey::Left;
    case 0x7C: return toe::SpecialKey::Right;
    case 0x73: return toe::SpecialKey::Home;
    case 0x77: return toe::SpecialKey::End;
    case 0x74: return toe::SpecialKey::PageUp;
    case 0x79: return toe::SpecialKey::PageDown;
    case 0x75: return toe::SpecialKey::Delete;     // Forward Delete
    case 0x72: return toe::SpecialKey::Insert;     // Help/Insert
    case 0x7A: return toe::SpecialKey::F1;
    case 0x78: return toe::SpecialKey::F2;
    case 0x63: return toe::SpecialKey::F3;
    case 0x76: return toe::SpecialKey::F4;
    case 0x60: return toe::SpecialKey::F5;
    case 0x61: return toe::SpecialKey::F6;
    case 0x62: return toe::SpecialKey::F7;
    case 0x64: return toe::SpecialKey::F8;
    case 0x65: return toe::SpecialKey::F9;
    case 0x6D: return toe::SpecialKey::F10;
    case 0x67: return toe::SpecialKey::F11;
    case 0x6F: return toe::SpecialKey::F12;
    default: return std::nullopt;
    }
}

} // namespace

// ───────────────────────────── the NSView ──────────────────────────────────

@interface HandGLView : NSOpenGLView {
@public
    CocoaInbox *inbox_; // borrowed; owned by the C++ surface
}
@end

@implementation HandGLView

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

// Show an I-beam over the terminal, like every other text surface on macOS.
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:[NSCursor IBeamCursor]];
}

// --- menu actions ----------------------------------------------------------
// Implemented on the view so the Edit/View menu items are ENABLED (AppKit only
// enables a menu item whose action is reachable in the responder chain). Each
// pushes the same event the keyboard shortcut would.
- (void)copy:(id)sender {
    if (!inbox_) return;
    inbox_->events.push_back(hand::platform::PendingKey{
        toe::KeyEvent{.key = toe::TextInput{"c"}, .mods = {.ctrl = true, .shift = true}}});
    inbox_->saw_event = true;
}
- (void)paste:(id)sender {
    if (!inbox_) return;
    inbox_->events.push_back(hand::platform::PendingKey{
        toe::KeyEvent{.key = toe::TextInput{"v"}, .mods = {.ctrl = true, .shift = true}}});
    inbox_->saw_event = true;
}
- (void)zoomIn:(id)sender {
    if (inbox_) { inbox_->events.push_back(hand::platform::PendingZoom{.delta = +1}); inbox_->saw_event = true; }
}
- (void)zoomOut:(id)sender {
    if (inbox_) { inbox_->events.push_back(hand::platform::PendingZoom{.delta = -1}); inbox_->saw_event = true; }
}
- (void)zoomReset:(id)sender {
    if (inbox_) {
        inbox_->events.push_back(hand::platform::PendingZoom{.absolute = inbox_->default_font_px});
        inbox_->saw_event = true;
    }
}

// Convert the view's backing-store (pixel) bounds and post a resize.
- (void)pushResize {
    if (!inbox_) return;
    const NSRect px = [self convertRectToBacking:self.bounds];
    inbox_->events.push_back(hand::platform::PendingResize{
        toe::PixelSize{std::max(1, (int)px.size.width), std::max(1, (int)px.size.height)}});
}

- (void)reshape {
    [super reshape];
    [[self openGLContext] update];
    [self pushResize];
}

- (void)keyDown:(NSEvent *)e {
    if (!inbox_) return;
    inbox_->saw_event = true;
    const toe::Modifiers m = mods_of(e.modifierFlags);

    if (auto sk = special_of_keycode(e.keyCode)) {
        inbox_->events.push_back(
            hand::platform::PendingKey{toe::KeyEvent{.key = *sk, .mods = m}});
        return;
    }

    // Ordinary typing. With Ctrl/Option held we still want the base key as a
    // KeyEvent so toe's keymap can synthesise C0 / ESC-prefixed bytes; without
    // modifiers, the produced characters are committed text.
    NSString *chars = m.ctrl ? e.charactersIgnoringModifiers : e.characters;
    if (m.ctrl || m.alt) {
        NSString *base = e.charactersIgnoringModifiers;
        if (base.length > 0) {
            const char *u = base.UTF8String;
            inbox_->events.push_back(hand::platform::PendingKey{
                toe::KeyEvent{.key = toe::TextInput{std::string{u ? u : ""}}, .mods = m}});
        }
        return;
    }
    if (chars.length > 0) {
        const char *u = chars.UTF8String;
        if (u && *u) inbox_->events.push_back(hand::platform::PendingText{std::string{u}});
    }
}

- (void)flagsChanged:(NSEvent *)e {
    if (inbox_) inbox_->saw_event = true; // wake the loop; modifiers alone do nothing
}

// Command-key shortcuts. A mac terminal must not leak ⌘C/⌘V/⌘Q to the shell as
// raw keystrokes. We translate the clipboard shortcuts into the Ctrl+Shift+C/V
// form toe's EventRouter already understands (reusing the engine's copy/paste,
// no engine change), and route ⌘Q / ⌘W to a clean close.
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    if (!inbox_) return NO;
    const NSEventModifierFlags f = e.modifierFlags;
    if ((f & NSEventModifierFlagCommand) == 0) return NO;
    // Ignore if other command-ish modifiers muddy it (e.g. ⌃⌘).
    NSString *ch = [e.charactersIgnoringModifiers lowercaseString];
    if (ch.length != 1) return NO;
    const unichar c = [ch characterAtIndex:0];
    const bool shift = (f & NSEventModifierFlagShift) != 0;

    switch (c) {
    case 'q': // Quit the app
        inbox_->close_requested = true;
        inbox_->saw_event = true;
        return YES;
    case 'w': // Close the window (single-window app: same as quit)
        inbox_->close_requested = true;
        inbox_->saw_event = true;
        return YES;
    case 'c': { // Copy selection (⌘C -> Ctrl+Shift+C for toe's router)
        toe::Modifiers m{.ctrl = true, .alt = false, .shift = true};
        inbox_->events.push_back(hand::platform::PendingKey{
            toe::KeyEvent{.key = toe::TextInput{"c"}, .mods = m}});
        inbox_->saw_event = true;
        return YES;
    }
    case 'v': { // Paste (⌘V -> Ctrl+Shift+V)
        toe::Modifiers m{.ctrl = true, .alt = false, .shift = true};
        inbox_->events.push_back(hand::platform::PendingKey{
            toe::KeyEvent{.key = toe::TextInput{"v"}, .mods = m}});
        inbox_->saw_event = true;
        return YES;
    }
    case 'k': // Clear — send Ctrl+L (form feed) which most shells treat as clear
        (void)shift;
        inbox_->events.push_back(hand::platform::PendingKey{
            toe::KeyEvent{.key = toe::TextInput{"l"}, .mods = {.ctrl = true}}});
        inbox_->saw_event = true;
        return YES;
    case '+': // Zoom in (⌘+ / ⌘=)
    case '=':
        inbox_->events.push_back(hand::platform::PendingZoom{.delta = +1});
        inbox_->saw_event = true;
        return YES;
    case '-': // Zoom out (⌘-)
        inbox_->events.push_back(hand::platform::PendingZoom{.delta = -1});
        inbox_->saw_event = true;
        return YES;
    case '0': // Reset zoom to the launch default (⌘0)
        inbox_->events.push_back(
            hand::platform::PendingZoom{.absolute = inbox_->default_font_px});
        inbox_->saw_event = true;
        return YES;
    default:
        // Let other ⌘ combos (e.g. ⌘M minimise, ⌘H hide) go to the menu/system.
        return NO;
    }
}

- (toe::win::MouseButton)btnOf:(NSEvent *)e {
    switch (e.type) {
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp: return toe::win::MouseButton::right;
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp: return toe::win::MouseButton::middle;
    default: return toe::win::MouseButton::left;
    }
}

- (void)pushMouse:(NSEvent *)e kind:(hand::platform::PendingMouse::Kind)k {
    if (!inbox_) return;
    inbox_->saw_event = true;
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const NSRect px = [self convertRectToBacking:self.bounds];
    // Cocoa's origin is bottom-left; toe wants top-left pixels.
    const NSPoint pp = [self convertPointToBacking:p];
    hand::platform::PendingMouse pm;
    pm.kind = k;
    pm.button = [self btnOf:e];
    pm.mods = mods_of(e.modifierFlags);
    // A drag (mouseDragged) means a button is held; NSEvent type tells us.
    pm.button_down = (e.type == NSEventTypeLeftMouseDragged ||
                      e.type == NSEventTypeRightMouseDragged ||
                      e.type == NSEventTypeOtherMouseDragged);
    pm.x = (int)pp.x;
    pm.y = (int)(px.size.height - pp.y);
    if (k == hand::platform::PendingMouse::Kind::wheel) {
        pm.dx = e.scrollingDeltaX;
        pm.dy = e.scrollingDeltaY;
    }
    inbox_->events.push_back(pm);
}

- (void)mouseDown:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::down]; }
- (void)mouseUp:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::up]; }
- (void)rightMouseDown:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::down]; }
- (void)rightMouseUp:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::up]; }
- (void)otherMouseDown:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::down]; }
- (void)otherMouseUp:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::up]; }
- (void)mouseDragged:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::move]; }
- (void)mouseMoved:(NSEvent *)e { [self pushMouse:e kind:hand::platform::PendingMouse::Kind::move]; }
- (void)scrollWheel:(NSEvent *)e {
    if (!inbox_) return;
    inbox_->saw_event = true;

    // Normalise to "lines": precise (trackpad) deltas are in points, coarse
    // (mouse wheel) deltas are already in lines. Bank them and emit one wheel
    // step per whole line crossed, so a gentle two-finger drag still scrolls
    // and a fast flick doesn't overshoot.
    double dx = e.scrollingDeltaX, dy = e.scrollingDeltaY;
    if (e.hasPreciseScrollingDeltas) {
        dx /= 16.0; // ~one line per 16pt of trackpad travel
        dy /= 16.0;
    }
    inbox_->scroll_ax += dx;
    inbox_->scroll_ay += dy;

    const int sx = (int)inbox_->scroll_ax;
    const int sy = (int)inbox_->scroll_ay;
    if (sx == 0 && sy == 0) return;
    inbox_->scroll_ax -= sx;
    inbox_->scroll_ay -= sy;

    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const NSPoint pp = [self convertPointToBacking:p];
    const NSRect px = [self convertRectToBacking:self.bounds];
    hand::platform::PendingMouse pm;
    pm.kind = hand::platform::PendingMouse::Kind::wheel;
    pm.mods = mods_of(e.modifierFlags);
    pm.x = (int)pp.x;
    pm.y = (int)(px.size.height - pp.y);
    pm.dx = sx;
    pm.dy = sy;
    inbox_->events.push_back(pm);
}

@end

// ─────────────────────── the window delegate (close/focus) ──────────────────

@interface HandWindowDelegate : NSObject <NSWindowDelegate> {
@public
    CocoaInbox *inbox_;
}
@end

@implementation HandWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if (inbox_) inbox_->close_requested = true;
    return NO; // let the toe loop tear down cleanly on should_close()
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    if (inbox_) inbox_->events.push_back(hand::platform::PendingFocus{true});
}
- (void)windowDidResignKey:(NSNotification *)n {
    if (inbox_) inbox_->events.push_back(hand::platform::PendingFocus{false});
}
@end

// ─────────────────────── the application delegate (quit) ────────────────────
// Routes a menu / Dock / system Quit through the toe loop's clean teardown
// (set close_requested, cancel the OS terminate) instead of a hard exit that
// would skip the child's SIGHUP + reap.
@interface HandAppDelegate : NSObject <NSApplicationDelegate> {
@public
    CocoaInbox *inbox_;
}
@end

@implementation HandAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    if (inbox_) {
        inbox_->close_requested = true;
        inbox_->saw_event = true;
    }
    return NSTerminateCancel; // the toe loop observes should_close() and exits
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}
@end

// ───────────────────────────── the C++ surface ─────────────────────────────

namespace hand::platform {

// The launch-time font pixel size, stashed by run_cocoa before toe::run opens
// the surface (which happens inside toe::run<CocoaApp>, out of run_cocoa's
// reach). Read once in CocoaSurface::open to seed Cmd-0 "reset zoom".
static int g_default_font_px = 0;

class CocoaSurface final {
public:
    static Result<std::unique_ptr<CocoaSurface>> open(std::string_view title, PixelSize initial);
    ~CocoaSurface();

    void swap();
    void swap_damaged(toe::DamageRect) { swap(); }
    [[nodiscard]] PixelSize pixel_size() const { return size_; }
    [[nodiscard]] int event_fd() const { return -1; } // no pollable window fd
    [[nodiscard]] int repeat_fd() const { return -1; }
    void flush() {}
    [[nodiscard]] bool should_close() const { return inbox_.close_requested; }

    void set_title(std::string_view t);
    void set_clipboard(std::string_view t);
    [[nodiscard]] std::string get_clipboard();
    void open_url(std::string_view u);

    void poll_events(const toe::EventSink &sink);
    [[nodiscard]] toe::Readiness wait_readable(int pty_fd, toe::WaitDeadline d);

private:
    CocoaSurface() = default;
    void pump_cocoa(bool force = false); // drain the NSApp queue into inbox_ (throttled)

    NSWindow *window_ = nil;
    HandGLView *view_ = nil;
    HandWindowDelegate *delegate_ = nil;
    HandAppDelegate *app_delegate_ = nil;
    CocoaInbox inbox_{};
    PixelSize size_{};
    std::uint64_t last_pump_ = 0; // mach ticks of the last event-queue scan
};

// --- open -------------------------------------------------------------------

Result<std::unique_ptr<CocoaSurface>> CocoaSurface::open(std::string_view title, PixelSize initial) {
    @autoreleasepool {
        // A regular (foreground, dockable) app.
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        // A minimal menu bar so the app feels native: an app menu (Hide/Quit
        // with their standard ⌘ equivalents) and an Edit menu (Copy/Paste).
        // Without this, the standard ⌘H/⌘Q/⌘M never work and there's no title.
        if (![app mainMenu]) {
            NSMenu *bar = [[NSMenu alloc] init];
            NSString *appName = @"hand";

            NSMenuItem *appItem = [[NSMenuItem alloc] init];
            [bar addItem:appItem];
            NSMenu *appMenu = [[NSMenu alloc] init];
            [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                               action:@selector(hide:)
                        keyEquivalent:@"h"];
            [appMenu addItemWithTitle:@"Hide Others"
                               action:@selector(hideOtherApplications:)
                        keyEquivalent:@"h"].keyEquivalentModifierMask =
                NSEventModifierFlagOption | NSEventModifierFlagCommand;
            [appMenu addItem:[NSMenuItem separatorItem]];
            [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                               action:@selector(terminate:)
                        keyEquivalent:@"q"];
            [appItem setSubmenu:appMenu];

            NSMenuItem *editItem = [[NSMenuItem alloc] init];
            [bar addItem:editItem];
            NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
            [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
            [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
            [editItem setSubmenu:editMenu];

            // View menu: font zoom + fullscreen. The zoom items are for
            // discoverability; performKeyEquivalent: already handles the keys.
            NSMenuItem *viewItem = [[NSMenuItem alloc] init];
            [bar addItem:viewItem];
            NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
            [viewMenu addItemWithTitle:@"Bigger" action:@selector(zoomIn:) keyEquivalent:@"+"];
            [viewMenu addItemWithTitle:@"Smaller" action:@selector(zoomOut:) keyEquivalent:@"-"];
            [viewMenu addItemWithTitle:@"Actual Size"
                                action:@selector(zoomReset:)
                         keyEquivalent:@"0"];
            [viewMenu addItem:[NSMenuItem separatorItem]];
            NSMenuItem *fs = [viewMenu addItemWithTitle:@"Enter Full Screen"
                                                 action:@selector(toggleFullScreen:)
                                          keyEquivalent:@"f"];
            fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
            [viewItem setSubmenu:viewMenu];

            // Window menu: Minimize / Zoom, and register it so macOS manages it.
            NSMenuItem *winItem = [[NSMenuItem alloc] init];
            [bar addItem:winItem];
            NSMenu *winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
            [winMenu addItemWithTitle:@"Minimize"
                               action:@selector(performMiniaturize:)
                        keyEquivalent:@"m"];
            [winMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
            [winItem setSubmenu:winMenu];
            [app setWindowsMenu:winMenu];

            [app setMainMenu:bar];
        }

        auto s = std::unique_ptr<CocoaSurface>(new CocoaSurface());
        s->inbox_.default_font_px = g_default_font_px;

        // Content size is in POINTS; the GL drawable is in backing pixels.
        const NSRect content = NSMakeRect(0, 0, std::max(1, (int)initial.w),
                                          std::max(1, (int)initial.h));
        const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        s->window_ = [[NSWindow alloc] initWithContentRect:content
                                                 styleMask:style
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
        if (!s->window_) return fail("cocoa: NSWindow allocation failed");
        [s->window_ setReleasedWhenClosed:NO]; // we own its lifetime via ARC
        s->window_.minSize = NSMakeSize(120, 60);

        NSString *t = [[NSString alloc] initWithBytes:title.data()
                                               length:(NSUInteger)title.size()
                                             encoding:NSUTF8StringEncoding];
        [s->window_ setTitle:(t ? t : @"hand")];

        // Native feel: a dark aqua appearance so the titlebar matches a dark
        // terminal background, no window tabbing (single-window terminal), and
        // the toolbar-style titlebar so the chrome sits tight to the content.
        s->window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        if (@available(macOS 10.12, *)) [s->window_ setTabbingMode:NSWindowTabbingModeDisallowed];
        s->window_.titlebarAppearsTransparent = NO;

        // 3.2 core profile — matches toe's #version 330 GLSL, within macOS's 4.1
        // ceiling. Double-buffered; no depth/stencil needed for 2D.
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            0,
        };
        NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf) return fail("cocoa: no suitable NSOpenGLPixelFormat (need GL 3.2 core)");

        s->view_ = [[HandGLView alloc] initWithFrame:content pixelFormat:pf];
        if (!s->view_) return fail("cocoa: NSOpenGLView creation failed");
        s->view_->inbox_ = &s->inbox_;
        [s->view_ setWantsBestResolutionOpenGLSurface:YES]; // HiDPI: real pixels

        s->delegate_ = [[HandWindowDelegate alloc] init];
        s->delegate_->inbox_ = &s->inbox_;
        [s->window_ setDelegate:s->delegate_];

        s->app_delegate_ = [[HandAppDelegate alloc] init];
        s->app_delegate_->inbox_ = &s->inbox_;
        [app setDelegate:s->app_delegate_];

        [s->window_ setContentView:s->view_];
        [s->window_ setAcceptsMouseMovedEvents:YES];
        [s->window_ makeFirstResponder:s->view_];
        [s->window_ center];
        [s->window_ makeKeyAndOrderFront:nil];

        // finishLaunching wires up the run loop/app lifecycle so our manual
        // nextEventMatchingMask pump behaves like a normally-launched app.
        [app finishLaunching];
        [app activateIgnoringOtherApps:YES];

        // Make the GL context current for toe's Renderer::create (called right
        // after open()). vsync (swap-interval) is left OFF so flushBuffer never
        // BLOCKS the single render/drain loop on the display refresh — under a
        // flood that stall dominated the profile (~17% of samples in
        // NSWaitUntilHostTime). Instead swap() software-throttles presents to
        // ~1/refresh below, giving vsync-off throughput with a capped, smooth
        // on-screen update rate and no tearing artefacts that matter for text.
        [[s->view_ openGLContext] makeCurrentContext];
        GLint zero = 0;
        [[s->view_ openGLContext] setValues:&zero
                              forParameter:NSOpenGLContextParameterSwapInterval];

        NSRect px = [s->view_ convertRectToBacking:s->view_.bounds];
        if (px.size.width < 1 || px.size.height < 1) {
            // Backing bounds can read 0 before the first layout; fall back to the
            // requested size scaled by the screen factor.
            const CGFloat sc = s->window_.screen ? s->window_.screen.backingScaleFactor : 1.0;
            px.size.width = std::max<CGFloat>(1, initial.w * sc);
            px.size.height = std::max<CGFloat>(1, initial.h * sc);
        }
        s->size_ = PixelSize{std::max(1, (int)px.size.width), std::max(1, (int)px.size.height)};

        return s;
    }
}

CocoaSurface::~CocoaSurface() {
    @autoreleasepool {
        if (window_) {
            [window_ setDelegate:nil];
            [window_ close];
        }
    }
}

// --- present ----------------------------------------------------------------

void CocoaSurface::swap() {
    // vsync (swap-interval) is OFF, so flushBuffer returns immediately instead of
    // stalling this single render/drain loop on the display clock — under a flood
    // that stall dominated the profile (~17% of samples in NSWaitUntilHostTime).
    // toe's run_loop already software-caps the PRESENT RATE (kFloodPresentMs)
    // while streaming and always presents the final idle frame, so we simply
    // never block here and let the engine own cadence.
    [[view_ openGLContext] flushBuffer];
}

// --- events -----------------------------------------------------------------

void CocoaSurface::pump_cocoa(bool force) {
    // nextEventMatchingMask services the CFRunLoop + mach ports each call. Under
    // a flood, run_loop calls the readiness path thousands of times/sec, and
    // profiling showed ~13% of main-thread samples burned in this scan with no
    // events pending. Human input doesn't need sub-ms polling, so unless a
    // caller FORCES it (a moment where we're about to act on input), skip the
    // scan if we did one < kPumpIntervalNs ago. Latency impact is imperceptible
    // (≤ ~3ms); the flood churn disappears.
    if (!force) {
        static mach_timebase_info_data_t tb = [] {
            mach_timebase_info_data_t t{}; mach_timebase_info(&t); return t;
        }();
        constexpr std::uint64_t kPumpIntervalNs = 3'000'000; // 3ms
        const std::uint64_t now = mach_absolute_time();
        const std::uint64_t elapsed_ns = (now - last_pump_) * tb.numer / tb.denom;
        if (elapsed_ns < kPumpIntervalNs) return;
        last_pump_ = now;
    } else {
        last_pump_ = mach_absolute_time();
    }

    @autoreleasepool {
        NSApplication *app = NSApp;
        for (;;) {
            NSEvent *e = [app nextEventMatchingMask:NSEventMaskAny
                                          untilDate:[NSDate distantPast]
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
            if (!e) break;
            [app sendEvent:e]; // routes to the view's key/mouse handlers
        }
    }
}

void CocoaSurface::poll_events(const toe::EventSink &sink) {
    pump_cocoa(/*force=*/true);

    if (inbox_.close_requested) {
        sink(toe::win::CloseRequested{});
    }

    for (auto &ev : inbox_.events) {
        std::visit(
            [&](auto &p) {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, PendingKey>) {
                    sink(toe::win::KeyPressed{p.ev});
                } else if constexpr (std::is_same_v<T, PendingText>) {
                    sink(toe::win::TextEntered{p.utf8});
                } else if constexpr (std::is_same_v<T, PendingResize>) {
                    size_ = p.size;
                    sink(toe::win::Resized{p.size});
                } else if constexpr (std::is_same_v<T, PendingFocus>) {
                    sink(toe::win::FocusChanged{p.focused});
                } else if constexpr (std::is_same_v<T, PendingZoom>) {
                    sink(toe::win::FontZoom{p.delta, p.absolute});
                } else if constexpr (std::is_same_v<T, PendingMouse>) {
                    using K = PendingMouse::Kind;
                    switch (p.kind) {
                    case K::down:
                        sink(toe::win::MouseDown{p.button, p.x, p.y, 1, p.mods});
                        break;
                    case K::up:
                        sink(toe::win::MouseUp{p.button, p.x, p.y, p.mods});
                        break;
                    case K::move:
                        sink(toe::win::MouseMove{p.x, p.y, p.button_down});
                        break;
                    case K::wheel: {
                        // dx/dy are already discrete line steps (banked in the
                        // scrollWheel handler); dy>0 = content up, matching toe.
                        const std::int32_t dx = (std::int32_t)p.dx;
                        const std::int32_t dy = (std::int32_t)p.dy;
                        if (dx != 0 || dy != 0) sink(toe::win::MouseWheel{dx, dy});
                        break;
                    }
                    }
                }
            },
            ev);
    }
    inbox_.events.clear();
}

// --- readiness wait ---------------------------------------------------------

toe::Readiness CocoaSurface::wait_readable(int pty_fd, toe::WaitDeadline d) {
    // Any Cocoa event already queued => window-ready, don't block. Throttled:
    // under a flood this runs thousands of times/sec and a stale ~3ms is fine.
    pump_cocoa(/*force=*/false);
    if (inbox_.saw_event || inbox_.close_requested || !inbox_.events.empty()) {
        inbox_.saw_event = false;
        return toe::Readiness{.pty = false, .window = true};
    }

    // No window fd to poll on, so cap the block so window input that can't wake
    // poll() is still serviced within a frame. 8ms ≈ 120Hz idle poll.
    constexpr int kCapMs = 8;
    int timeout_ms = kCapMs;
    if (!d.blocks_forever()) {
        const long ms = (long)(d.ns / 1'000'000);
        timeout_ms = (int)std::min<long>(ms, kCapMs);
    }

    struct pollfd pfd {
        pty_fd, POLLIN, 0
    };
    const int n = ::poll(&pfd, (pty_fd >= 0 ? 1 : 0), timeout_ms);

    toe::Readiness r{};
    if (n > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) r.pty = true;

    // Re-drain Cocoa: an event may have arrived during the poll window. FORCE it
    // — we just blocked, so a keystroke that woke us must be seen immediately.
    pump_cocoa(/*force=*/true);
    if (inbox_.saw_event || inbox_.close_requested || !inbox_.events.empty()) {
        inbox_.saw_event = false;
        r.window = true;
    }
    return r;
}

// --- title & clipboard ------------------------------------------------------

void CocoaSurface::set_title(std::string_view t) {
    @autoreleasepool {
        NSString *s = [[NSString alloc] initWithBytes:t.data()
                                               length:(NSUInteger)t.size()
                                             encoding:NSUTF8StringEncoding];
        if (s) [window_ setTitle:s];
    }
}

void CocoaSurface::set_clipboard(std::string_view t) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString *s = [[NSString alloc] initWithBytes:t.data()
                                               length:(NSUInteger)t.size()
                                             encoding:NSUTF8StringEncoding];
        if (s) [pb setString:s forType:NSPasteboardTypeString];
    }
}

std::string CocoaSurface::get_clipboard() {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (!s) return {};
        const char *u = s.UTF8String;
        return u ? std::string{u} : std::string{};
    }
}

void CocoaSurface::open_url(std::string_view u) {
    @autoreleasepool {
        NSString *s = [[NSString alloc] initWithBytes:u.data()
                                               length:(NSUInteger)u.size()
                                             encoding:NSUTF8StringEncoding];
        if (!s) return;
        NSURL *url = [NSURL URLWithString:s];
        // Only open web/file-ish schemes NSURL could parse; ignore garbage so a
        // stray OSC 8 payload can't ask the OS to launch something unexpected.
        if (url && url.scheme) [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

} // namespace hand::platform

// ─────────────────────── the App handle + backend entry ─────────────────────

namespace hand {

// BackendApp<CocoaSurface>::open — the toe::App factory for this backend.
template <>
toe::Result<CocoaApp> CocoaApp::open(const toe::WindowConfig &win) {
    auto s = platform::CocoaSurface::open(win.title, win.size);
    if (!s) return toe::fail(s.error().message);
    return CocoaApp{s->release()};
}

// The backend entry: enter the fully-monomorphic loop for CocoaApp. toe::run<>
// is instantiated HERE, where CocoaSurface is complete — direct calls inline,
// no NS* type leaks into the engine.
int run_cocoa(const toe::Config &cfg_in, const toe::WindowConfig &win) {
    toe::Config cfg = cfg_in;

    // toe renders in BACKING PIXELS, so a fixed pixel font looks tiny on a 2x
    // Retina panel and huge on a 1x one. Scale the requested size (interpreted
    // as logical/point px) by the display's backing scale factor so the on-
    // screen size is consistent everywhere — exactly how Terminal.app behaves.
    @autoreleasepool {
        CGFloat scale = 1.0;
        if (NSScreen *scr = [NSScreen mainScreen]) scale = scr.backingScaleFactor;
        if (scale < 1.0) scale = 1.0;
        // Round DOWN: a deterministic, slightly-conservative size beats a
        // half-pixel that rounds up to the next point.
        cfg.font_pixel_size = (int)((CGFloat)cfg.font_pixel_size * scale);
    }
    // Seed Cmd-0 "reset zoom" with the launch size (read in CocoaSurface::open).
    platform::g_default_font_px = cfg.font_pixel_size;
    return toe::run<CocoaApp>(cfg, win);
}

} // namespace hand
