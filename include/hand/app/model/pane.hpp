#pragma once
// hand::model — the file pane beside the terminal (ctrl+shift+e): the active
// tab's directory, local or over SFTP.

#include <hand/ssh/hosts.hpp>

#include <mayag/widget/file_pane.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hand {

struct PaneModel {
    bool                                   open = false;
    float                                  width = 300.0f;
    float                                  grab_width = 300.0f;   ///< the width when its edge was pressed
    bool                                   dragging = false;      ///< its edge is held: grids wait (fit())
    mayag::dsl::FilePaneState              ui;
    std::string                            where;          ///< "" local, else the ssh host's alias
    std::optional<Host>                    host;           ///< the host it lists over SFTP
    std::string                            path;           ///< the folder shown
    std::string                            followed;       ///< the shell's directory it last followed
    bool                                   follow = true;
    std::vector<mayag::dsl::FileEntry>     entries;
    bool                                   loading = false;
    std::string                            error, busy, note;
    std::uint64_t                          token = 0;      ///< the listing awaited (older ones are dropped)
    std::map<std::string, double>          fresh_at;       ///< name -> when a re-read found it new (the glow)
    std::optional<mayag::dsl::FilePreview> preview;
    std::string                            preview_path;   ///< what preview shows (or is being read)
};

}  // namespace hand
