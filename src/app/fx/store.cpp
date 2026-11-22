// hand::fx — what hand keeps on disk: hand.vibe, and the host book
// (~/.ssh/config, known_hosts, hosts.vibe; PuTTY and MobaXterm to import).

#include "fx.hpp"

namespace hand::fx {

void save_config(Config c, std::string path) { (void)config_save(c, path); }

App::Msg load_hosts(HostSources s, bool imports) {
    App::HostsLoaded r{.book = hosts_load(resolve(std::move(s)))};
    if (imports) r.imports = import_sources();   // PuTTY's registry and MobaXterm.ini: a few ms
    return App::Msg{std::move(r)};
}

void record_use(HostSources s, Host h, std::int64_t now_s) { hosts_record_use(s, h, now_s); }
void set_pinned(HostSources s, Host h, bool pinned) { hosts_set_pinned(s, h, pinned); }
void save_host(HostSources s, Host h, std::string old_alias) { hosts_save(s, h, old_alias); }
void remove_host(HostSources s, std::string alias) { hosts_remove(s, alias); }
void save_folder(HostSources s, Folder f, std::string old_name) { folder_save(s, f, old_name); }
void remove_folder(HostSources s, std::string name) { folder_remove(s, name); }

}  // namespace hand::fx
