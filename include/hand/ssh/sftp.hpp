#pragma once
// hand::sftp — a remote file browser's back end, on OpenSSH's own sftp.
//
// Every call runs `sftp -b <batch>` non-interactively (BatchMode: a host
// that wants a password says so instead of hanging) with the host's
// effective options (folder defaults, port, jump, key). Blocking: the app
// runs these in tasks. Output parsing is pure and tested on fixtures.

#include <hand/ssh/hosts.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hand::sftp {

struct Entry {
    std::string   name;
    bool          dir = false, link = false;
    std::uint64_t size = 0;
    std::string   perms;       ///< "drwxr-xr-x"
    std::string   when;        ///< "Nov 18 22:01" as the server lists it
    std::string   target;      ///< a link's target
};

struct Listing {
    std::string        path;   ///< absolute, as the server resolved it
    std::vector<Entry> entries;   ///< directories first, then by name
    std::string        error;  ///< "" = fine; else for a person
};

/// List `path` ("" = the login directory).
[[nodiscard]] Listing list(const Host& h, const HostBook& book, const std::string& path);
/// Download remote paths (files or directories, recursively) into `local_dir`.
[[nodiscard]] std::string get(const Host& h, const HostBook& book, const std::vector<std::string>& remote, const std::string& local_dir);
/// Upload local paths into `remote_dir`.
[[nodiscard]] std::string put(const Host& h, const HostBook& book, const std::vector<std::string>& local, const std::string& remote_dir);
/// Make a directory.
[[nodiscard]] std::string mkdir(const Host& h, const HostBook& book, const std::string& remote);

// pure pieces, exposed for tests
[[nodiscard]] std::vector<std::string> argv(const Host& h, const HostBook& book, const std::string& batch_file);
[[nodiscard]] Listing parse_listing(std::string_view out);
[[nodiscard]] std::string quote(std::string_view path);   ///< for a batch line
[[nodiscard]] std::string explain(std::string_view err, int code);   ///< stderr -> a sentence
[[nodiscard]] std::string join(std::string_view dir, std::string_view name);
[[nodiscard]] std::string parent(std::string_view path);

}  // namespace hand::sftp
