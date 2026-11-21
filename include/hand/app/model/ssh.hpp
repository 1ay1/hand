#pragma once
// hand::model — SSH: where hosts come from, the picker (ctrl+shift+s) and the
// host manager (ctrl+shift+h). Both read the same HostBook.

#include <hand/ssh/hosts.hpp>

#include <mayag/mayag.hpp>
#include <mayag/widget/ssh_manager.hpp>

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
};

struct SshModel {
    HostSources  sources;
    HostBook     book;      ///< loaded when the picker or manager opens (files can change)
    PickerModel  picker;
    ManagerModel manager;
};

}  // namespace hand
