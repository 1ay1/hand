// SPDX-License-Identifier: LGPL-2.0-or-later
//
// config_roundtrip_test — guards that EVERY settings-pane field survives the
// full loop: Settings -> HandConfig -> save VIBE -> load VIBE -> Settings.
// This is the safety net for "everything is configurable": if a field is added
// to the pane but not to config.cpp's parse/save (or to_toe/from/into), it
// fails here instead of silently not persisting.
#include "hand/config/config.hpp"
#include "hand/settings_panel.hpp"
#include <cstdio>
int main(){
  // Start from defaults, mutate a Settings across every section, fold into a
  // HandConfig, save, reload, and confirm the values survived.
  hand::HandConfig c;
  auto s = hand::Settings::from(c);
  s.font_size = 22; s.ligatures = true; s.font_fallback = "Noto";
  s.font_bold = "/f/B.ttf"; s.font_italic = "/f/I.ttf"; s.font_bold_italic = "/f/BI.ttf";
  s.cursor_style = 1; s.blink_ms = 700; s.animate_ms = 33; s.animate_trail = false;
  s.fg = "#aabbcc"; s.selection = "#112233"; s.cursor_color = "#ff00ff";
  s.scrollback = 50000; s.scroll_mult = 7; s.scroll_on_output = true;
  s.audible_bell = true; s.confirm_close = true;
  s.title = "myterm"; s.padding = 12; s.opacity = 0.85f; s.decorations = false;
  s.overlay_panel_opacity = 0.80f; s.overlay_scrim_opacity = 0.10f;
  s.shell = "/bin/zsh"; s.term_env = "xterm-kitty";
  s.into(c);
  hand::save_hand_config(c, "/tmp/rt.vibe");
  auto d = hand::load_hand_config("/tmp/rt.vibe");
  auto r = hand::Settings::from(d);
  int fails=0;
  auto ck=[&](bool ok,const char*n){ if(!ok){std::printf("FAIL %s\n",n);++fails;} };
  ck(r.font_size==22,"font_size"); ck(r.ligatures,"ligatures"); ck(r.font_fallback=="Noto","fallback");
  ck(r.cursor_style==1,"cursor_style"); ck(r.blink_ms==700,"blink_ms"); ck(r.animate_ms==33,"animate_ms"); ck(!r.animate_trail,"trail");
  ck(r.fg=="#aabbcc","fg"); ck(r.selection=="#112233","selection"); ck(r.cursor_color=="#ff00ff","cursor_color");
  ck(r.scrollback==50000,"scrollback"); ck(r.scroll_mult==7,"scroll_mult"); ck(r.scroll_on_output,"scroll_on_output");
  ck(r.audible_bell,"audible_bell"); ck(r.confirm_close,"confirm_close");
  ck(r.padding==12,"padding"); ck(r.opacity>0.84f&&r.opacity<0.86f,"opacity"); ck(!r.decorations,"decorations");
  ck(r.font_bold=="/f/B.ttf","font_bold"); ck(r.font_italic=="/f/I.ttf","font_italic");
  ck(r.font_bold_italic=="/f/BI.ttf","font_bold_italic");
  ck(r.title=="myterm","title"); ck(r.shell=="/bin/zsh","shell"); ck(r.term_env=="xterm-kitty","term");
  ck(r.overlay_panel_opacity>0.79f&&r.overlay_panel_opacity<0.81f,"overlay_panel_opacity");
  ck(r.overlay_scrim_opacity>0.09f&&r.overlay_scrim_opacity<0.11f,"overlay_scrim_opacity");

  // And it must actually reach the ENGINE, not just persist: the once-dead
  // behavior knobs now project into toe::Config so the EventRouter/drain honor
  // them. (A field that persists but doesn't project is still a dead knob.)
  auto tc = d.to_toe();
  ck(tc.scroll_on_output, "scroll_on_output -> toe::Config");
  ck(tc.wheel_lines==r.scroll_mult, "wheel_lines -> toe::Config");
  ck(tc.selection_bg.r==0x11 && tc.selection_bg.g==0x22 && tc.selection_bg.b==0x33,
     "selection colour -> toe::Config");
  ck(tc.cursor_blink_ms==700, "blink_ms -> toe::Config (blink on)");
  ck(tc.font_file_bold=="/f/B.ttf", "font_bold -> toe::Config");
  ck(tc.font_file_italic=="/f/I.ttf", "font_italic -> toe::Config");
  ck(tc.font_file_bold_italic=="/f/BI.ttf", "font_bold_italic -> toe::Config");

  std::printf(fails? "%d FIELD(S) FAILED ROUND-TRIP\n":"ALL FIELDS ROUND-TRIP CLEAN\n", fails);
  return fails?1:0;
}
