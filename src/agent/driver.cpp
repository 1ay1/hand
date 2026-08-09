// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand-agent — the line-delimited-JSON stdio frontend to the agent driver.
// One JSON request per line in, one JSON reply per line out. See the shared
// engine in include/hand/agent_core.hpp and the protocol in docs/AGENT_DRIVER.md.

#include "hand/agent_core.hpp"

#include <cstdio>
#include <string>

using namespace hand::agent;

namespace {
void reply_ok(const std::string &body) {
    std::printf("{\"ok\":true,%s}\n", body.c_str());
    std::fflush(stdout);
}
void reply_err(std::string_view msg) {
    std::printf("{\"ok\":false,\"error\":\"%s\"}\n", json_escape(msg).c_str());
    std::fflush(stdout);
}
} // namespace

int main(int argc, char **argv) {
    int cols = 80, rows = 24;
    bool redact = false;
    hand::SpawnCommand cmd;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--cols" && i + 1 < argc) cols = std::atoi(argv[++i]);
        else if (a == "--rows" && i + 1 < argc) rows = std::atoi(argv[++i]);
        else if (a == "--redact") redact = true;
        else if (a == "--") { for (int j = i + 1; j < argc; ++j) cmd.argv.emplace_back(argv[j]); break; }
    }

    std::string err;
    auto drv = Driver::spawn(cmd, cols, rows, redact, err);
    if (!drv) { reply_err(err); return 1; }

    reply_ok("\"ready\":true,\"cols\":" + std::to_string(cols) + ",\"rows\":" + std::to_string(rows));

    std::string line;
    int ch;
    while ((ch = std::getchar()) != EOF) {
        if (ch != '\n') { line.push_back(static_cast<char>(ch)); continue; }
        std::string_view j = line;
        auto op = json_str(j, "op").value_or("");
        drv->drain(); // answer reads against the freshest state

        if (op == "snapshot") {
            reply_ok("\"text\":\"" + json_escape(drv->redact(drv->snapshot())) + "\"");
        } else if (op == "blocks") {
            reply_ok("\"blocks\":" + drv->blocks_json(json_int(j, "last").value_or(0)));
        } else if (op == "send") {
            if (auto t = json_str(j, "text")) drv->send(*t);
            else if (auto k = json_str(j, "keys")) drv->send(encode_keys(*k, drv->app_cursor()));
            reply_ok("\"sent\":true");
        } else if (op == "key") {
            drv->send(encode_keys("<" + json_str(j, "name").value_or("") + ">", drv->app_cursor()));
            reply_ok("\"sent\":true");
        } else if (op == "wait") {
            auto what = json_str(j, "for").value_or("idle");
            long ms = json_int(j, "ms").value_or(2000);
            if (what == "pattern") {
                bool hit = drv->wait_pattern(json_str(j, "re").value_or(""), ms);
                reply_ok(std::string("\"matched\":") + (hit ? "true" : "false"));
            } else {
                drv->wait_idle(json_int(j, "quiet").value_or(120), ms);
                reply_ok("\"idle\":true");
            }
        } else if (op == "resize") {
            drv->resize((int)json_int(j, "cols").value_or(drv->cols()),
                        (int)json_int(j, "rows").value_or(drv->rows()));
            reply_ok("\"resized\":true");
        } else if (op == "close") {
            reply_ok("\"bye\":true");
            break;
        } else {
            reply_err("unknown op: " + op);
        }

        if (drv->hung_up()) { reply_ok("\"exited\":true"); break; }
        line.clear();
    }
    return 0;
}
