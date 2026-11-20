#pragma once
// hand::app::detail — what the update/input/view files share: queries over the
// model and small mechanics. Internal: not part of hand's interface.

#include <hand/app.hpp>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace mayag::dsl { struct SshHost; struct CornerControlsProps; struct FilePaneProps; }

namespace hand::detail {

using mayag::Sink;
using mayag::FontFamily;
using mayag::dsl::SshHost;
using E = mayag::Edit<Model>;

inline constexpr float kMinimapRest = 14.0f, kMinimapOpen = 96.0f;

struct Risk { bool flag = false; std::string note; };

/// Seconds on the wall clock (unix), for "3h ago".
std::int64_t unix_now();
/// Milliseconds on the wall clock, for command timings.
std::int64_t wall_ms();
/// Where the terminal starts (the strip may sit above it or below it).
float term_top(const Model& m);
/// Where the terminal starts across (the file pane may dock on the left).
float term_left(const Model& m);
/// Is window x over the file pane?
bool in_pane(const Model& m, float x);
/// Something in the file pane is still glowing (it changed a moment ago).
bool pane_glowing(const Model& m);
bool in_strip(const Model& m, float y);
/// A clock for caches that must refresh even while the frame clock is idle.
double mono_s();
/// The foreground process of a session's shell (cached ~0.5 s, on the wall clock).
std::string fg_process(const Session& se);
/// The machine a tab talks to: the host hand launched it on, else the
/// destination of an `ssh ...` you typed at its shell (cached ~1 s).
const std::optional<Host>& remote_of(const Session& se);
/// A remote's environment: what hosts.vibe says, else what its name suggests.
HostEnv remote_env(const Model& m, const Host& h);
/// The book as the picker shows it, with live tabs counted per host.
std::vector<SshHost> picker_hosts(const Model& m);
/// The picker's rows: indices into m.book.hosts, best first.
std::vector<int> picker_rank(const Model& m);
/// The minimap maps the normal screen's history: a full-screen app (the
/// alternate screen: vim, htop, cmatrix) owns the whole window and gets none.
/// The command whose output is under the top of the view, when the sticky header applies.
std::optional<Term::Command> sticky_of(const Model& m);
/// "12.3s", "4m05s", "1h02m"
std::string pretty_ms(std::int64_t ms);
/// A path for a header: $HOME as ~, long ones keep their last two parts.
std::string short_path(std::string p);
/// Does this pasted line deserve a second look?
Risk risk_of(const std::string& l);
std::vector<std::string> split_lines(const std::string& text);
bool is_shell(const std::string& p);
/// What a tab is busy with ("" = just a prompt), and for how long.
std::string busy_of(const Session& se, std::string* elapsed);
bool minimap_on(const Model& m);
/// The width the minimap (scrollbar) owns at the right edge right now (0 = none).
float minimap_w(const Model& m);
/// No strip and no system title bar: the window's buttons float in the corner,
/// just left of the minimap.
bool corner_buttons(const Model& m);
mayag::dsl::CornerControlsProps corner_props(const Model& m);
/// Is this window point on the corner buttons?
bool in_corner(const Model& m, mayag::Vec2 p);
/// The live tail shows while you read history and the live prompt is off screen.
bool tail_wanted(const Model& m);
/// The pty reader: a background stream for as long as the shell lives.
/// Coalesces bursts (cat of a big file) into one Output per wakeup, and keeps
/// ONE in flight: it reads on only once the UI has fed the last chunk
/// (pty_fed). So a firehose (`yes`) is throttled by the kernel's pty buffer
/// -- the producer blocks -- instead of piling gigabytes into the mailbox,
/// starving frames and parking ctrl-C behind seconds of queued output.
void read_pty(Sink<App::Msg> out, std::stop_token st, int fd, int pid, std::uint64_t sid);
/// The UI has fed shell `sid`'s last chunk: its reader may send the next.
void pty_fed(std::uint64_t sid);
/// How far below its row the grid is drawn (0..1): a scroll glide between rows
/// plus the glide of a freshly arrived line at the live prompt.
float draw_frac(const Model& m);
/// The viewport row under window y, through a smooth-scroll glide (the grid is
/// drawn up to a row lower mid-glide). May be -1: the row just above the view.
int view_row(const Model& m, float y);
/// Selecting into the top/bottom margin scrolls; the further out, the faster.
float edge_speed(const Model& m, float y);
int cell_of(float v, float origin, float size, int max);
FontFamily family_of(const Config& c);

// mechanics shared by several update files
/// Run ssh to `h` as `se`'s program (its terminal keeps whatever it shows).
void start_ssh(Model& m, Session& se, const Host& h);
/// Park the active session in its slot, bring slot `i` into the Model.
void activate(Model& m, std::size_t i);
/// Bytes to the active pty (snaps the view live, wakes the cursor).
std::optional<App::Msg> send(std::string bytes);
/// The file pane's props, from the model (input and view both read them).
mayag::dsl::FilePaneProps pane_props(const Model& m);

}  // namespace hand::detail
