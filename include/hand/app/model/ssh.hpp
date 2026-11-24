#pragma once
// hand::model — SSH: where hosts come from, the picker (ctrl+shift+s) and the
// host manager (ctrl+shift+h). Both read the same HostBook.

#include <hand/ssh/hosts.hpp>

#include <mayag/mayag.hpp>
#include <mayag/widget/ssh_manager.hpp>

#include <map>
#include <string>
#include <vector>

namespace hand {

/// The quick picker: type to filter, enter to connect.
struct PickerModel {
    bool                 open = false;
    mayag::TextEditState query;
    int                  sel = 0;
};

/// The host manager: every host, organised and editable.
struct ManagerModel {
    bool                                    open = false;
    float                                   t = 0.0f;   ///< 0..1, eased in and out
    mayag::dsl::SshManagerState             ui;
    mayag::dsl::SshFilesView                files;       ///< the Files tab, as shown (dotfiles filtered)
    std::vector<mayag::dsl::SshRemoteEntry> files_all;   ///< the whole listing
    std::vector<int>                        ports;       ///< listening TCP ports (tunnel status), 1 Hz while open
    std::vector<std::string>                port_owners;
    bool                                    ports_known = true;
    std::vector<ImportSource>               imports;     ///< other clients' sessions, read on open
    /// The link to each host as last probed (by alias): the selected host is
    /// probed when it is picked -- which also brings its shared connection up,
    /// so Connect is instant -- never the whole book behind your back.
    struct Link { mayag::dsl::SshHost::Link state = mayag::dsl::SshHost::Link::unknown; int rtt_ms = -1; std::string error; };
    std::map<std::string, Link>             links;
    std::string                             probed;      ///< the alias the last probe was asked for (one in flight per selection)
};

struct SshModel {
    HostSources  sources;
    HostBook     book;      ///< loaded when the picker or manager opens (files can change)
    PickerModel  picker;
    ManagerModel manager;
};

}  // namespace hand
