// hand::proc — what every OS does the same way (one implementation).

#include <hand/core/platform.hpp>
#include <hand/core/proc.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

namespace hand::proc {

std::string temp_path(std::string_view stem) {
    // No shared counter: sftp batches run on worker threads at the same time
    // (a pane listing and a preview), and a plain `static unsigned n; ++n` is
    // a data race that handed two of them the same file -- one batch then
    // overwrote the other's. A fresh draw from the OS's entropy per call,
    // with the pid and the clock, shares nothing between threads.
    std::random_device rd;
    const auto t = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    char tail[64];
    std::snprintf(tail, sizeof tail, "-%d-%llx-%08x%08x.txt", plat::self_pid(), t, rd(), rd());
    return (std::filesystem::temp_directory_path() / (std::string{stem} + tail)).string();
}

}  // namespace hand::proc
