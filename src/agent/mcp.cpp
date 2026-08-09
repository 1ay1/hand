// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand-agent-mcp — an MCP (Model Context Protocol) server that gives an AI agent
// a real, drivable terminal. It is JSON-RPC 2.0 over stdio (the MCP stdio
// transport), wrapping the same headless driver as hand-agent. Drop it into any
// MCP client (Claude Code, Codex, OpenCode, …):
//
//   { "mcpServers": { "terminal": {
//       "command": "hand-agent-mcp",
//       "args": ["--redact", "--", "/bin/bash"] } } }
//
// It exposes ONE persistent PTY session per server process and these tools:
//   terminal_snapshot  — the settled screen as clean text (token-frugal)
//   terminal_blocks    — the last N OSC 133 command blocks as JSON
//   terminal_send      — type text or vim-notation keys
//   terminal_wait      — wait until idle, or until the screen shows a pattern
//   terminal_resize    — resize the grid + PTY
//
// Why this beats the agent's built-in shell tool: a real TTY (vim/htop work),
// persistent state across calls (answer a prompt, sit in a REPL), and a clean,
// structured, redacted view instead of raw ANSI noise. See docs/AI_TERMINAL.md.

#include "hand/agent_core.hpp"

#include <cstdio>
#include <memory>
#include <string>

using namespace hand::agent;

namespace {

constexpr const char *kProtocol = "2025-06-18"; // MCP revision we speak
constexpr const char *kServerName = "hand-agent-mcp";
constexpr const char *kServerVersion = "0.1.0";

// One line of JSON-RPC out.
void emit(const std::string &json) {
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// A JSON-RPC result envelope. `id_raw` is the request's raw id token (number or
// quoted string) copied verbatim so we echo the client's exact id.
void reply_result(std::string_view id_raw, const std::string &result) {
    emit("{\"jsonrpc\":\"2.0\",\"id\":" + std::string{id_raw} + ",\"result\":" + result + "}");
}
void reply_error(std::string_view id_raw, int code, std::string_view msg) {
    emit("{\"jsonrpc\":\"2.0\",\"id\":" + std::string{id_raw} + ",\"error\":{\"code\":" +
         std::to_string(code) + ",\"message\":\"" + json_escape(msg) + "\"}}");
}

// A tools/call result whose content is one text block. `is_error` marks a tool-
// level failure (still a valid JSON-RPC result, per MCP).
std::string tool_text(const std::string &text, bool is_error = false) {
    return "{\"content\":[{\"type\":\"text\",\"text\":\"" + json_escape(text) +
           "\"}],\"isError\":" + (is_error ? "true" : "false") + "}";
}

// Extract the raw `id` token from a JSON-RPC line (number or "string"), verbatim.
// Returns "null" if absent (a notification).
std::string raw_id(std::string_view j) {
    auto k = j.find("\"id\"");
    if (k == std::string_view::npos) return "null";
    auto c = j.find(':', k + 4);
    if (c == std::string_view::npos) return "null";
    std::size_t i = c + 1;
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t')) ++i;
    if (i >= j.size()) return "null";
    if (j[i] == '"') { // quoted string id
        std::size_t e = i + 1;
        while (e < j.size() && j[e] != '"') { if (j[e] == '\\') ++e; ++e; }
        return std::string{j.substr(i, e - i + 1)};
    }
    std::size_t e = i;
    while (e < j.size() && (std::isdigit((unsigned char)j[e]) || j[e] == '-')) ++e;
    return std::string{j.substr(i, e - i)};
}

// The advertised tool catalogue (tools/list). Kept terse — descriptions are
// tokens the model reads on every turn.
std::string tools_catalogue() {
    auto tool = [](const char *name, const char *desc, const char *props, const char *req) {
        std::string s = "{\"name\":\"";
        s += name;
        s += "\",\"description\":\"";
        s += desc;
        s += "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{";
        s += props;
        s += "}";
        if (req && *req) { s += ",\"required\":["; s += req; s += "]"; }
        s += "}}";
        return s;
    };
    std::string t = "{\"tools\":[";
    t += tool("terminal_snapshot",
              "The visible terminal screen as clean text (trailing blanks trimmed). Token-frugal: prefer this over re-reading raw output.",
              "", "");
    t += ",";
    t += tool("terminal_blocks",
              "Recent shell commands as structured JSON {command,output,cwd,exitCode,durationMs}. Requires OSC 133 shell integration. Use 'last' to limit; often cheaper than a full snapshot.",
              "\"last\":{\"type\":\"integer\",\"description\":\"newest N blocks (0=all)\"}", "");
    t += ",";
    t += tool("terminal_send",
              "Type into the terminal. 'text' is literal (add \\r to run a command); 'keys' is vim-notation like <C-c> <Esc> <Up> <CR>.",
              "\"text\":{\"type\":\"string\"},\"keys\":{\"type\":\"string\"}", "");
    t += ",";
    t += tool("terminal_wait",
              "Wait for the terminal to settle. for='idle' waits until output is quiet; for='pattern' waits until the screen contains 're'. Returns the settled snapshot.",
              "\"for\":{\"type\":\"string\",\"enum\":[\"idle\",\"pattern\"]},\"re\":{\"type\":\"string\"},\"ms\":{\"type\":\"integer\"}", "");
    t += ",";
    t += tool("terminal_resize",
              "Resize the terminal grid (and send SIGWINCH to the child).",
              "\"cols\":{\"type\":\"integer\"},\"rows\":{\"type\":\"integer\"}", "\"cols\",\"rows\"");
    t += "]}";
    return t;
}

// Dispatch a tools/call. `args` is the raw JSON of the call's "arguments" object.
std::string dispatch_tool(Driver &drv, std::string_view name, std::string_view args) {
    drv.drain();
    if (name == "terminal_snapshot") {
        return tool_text(drv.redact(drv.snapshot()));
    }
    if (name == "terminal_blocks") {
        return tool_text(drv.blocks_json(json_int(args, "last").value_or(0)));
    }
    if (name == "terminal_send") {
        if (auto t = json_str(args, "text")) drv.send(*t);
        else if (auto k = json_str(args, "keys")) drv.send(encode_keys(*k, drv.app_cursor()));
        else return tool_text("terminal_send needs 'text' or 'keys'", true);
        // A send is most useful when the caller then sees the result: settle
        // briefly and return the fresh screen so the model needs one call, not two.
        drv.wait_idle(120, 1500);
        return tool_text(drv.redact(drv.snapshot()));
    }
    if (name == "terminal_wait") {
        auto what = json_str(args, "for").value_or("idle");
        long ms = json_int(args, "ms").value_or(4000);
        if (what == "pattern") {
            bool hit = drv.wait_pattern(json_str(args, "re").value_or(""), ms);
            std::string body = drv.redact(drv.snapshot());
            return tool_text((hit ? "[matched]\n" : "[timeout]\n") + body);
        }
        drv.wait_idle(json_int(args, "quiet").value_or(150), ms);
        return tool_text(drv.redact(drv.snapshot()));
    }
    if (name == "terminal_resize") {
        drv.resize((int)json_int(args, "cols").value_or(drv.cols()),
                   (int)json_int(args, "rows").value_or(drv.rows()));
        return tool_text("resized to " + std::to_string(drv.cols()) + "x" +
                         std::to_string(drv.rows()));
    }
    return tool_text(std::string{"unknown tool: "} + std::string{name}, true);
}

// Slice out the raw JSON object that is the value of "key" (brace-matched).
std::optional<std::string> json_object(std::string_view j, std::string_view key) {
    std::string needle = "\"" + std::string{key} + "\"";
    auto k = j.find(needle);
    if (k == std::string_view::npos) return std::nullopt;
    auto b = j.find('{', k);
    if (b == std::string_view::npos) return std::nullopt;
    int depth = 0;
    bool in_str = false;
    for (std::size_t i = b; i < j.size(); ++i) {
        char c = j[i];
        if (in_str) { if (c == '\\') ++i; else if (c == '"') in_str = false; continue; }
        if (c == '"') in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) return std::string{j.substr(b, i - b + 1)}; }
    }
    return std::nullopt;
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
    if (!drv) {
        std::fprintf(stderr, "hand-agent-mcp: %s\n", err.c_str());
        return 1;
    }

    std::string line;
    int ch;
    while ((ch = std::getchar()) != EOF) {
        if (ch != '\n') { line.push_back(static_cast<char>(ch)); continue; }
        std::string_view j = line;

        // method
        auto method = json_str(j, "method").value_or("");
        std::string id = raw_id(j);
        const bool is_notification = (id == "null");

        if (method == "initialize") {
            std::string caps = "{\"tools\":{\"listChanged\":false}}";
            std::string info = std::string("{\"name\":\"") + kServerName + "\",\"version\":\"" +
                               kServerVersion + "\"}";
            reply_result(id, std::string("{\"protocolVersion\":\"") + kProtocol +
                                 "\",\"capabilities\":" + caps + ",\"serverInfo\":" + info +
                                 ",\"instructions\":\"A real terminal you can drive. Prefer "
                                 "terminal_blocks for command results and terminal_snapshot for "
                                 "the current screen; both are far cheaper in tokens than re-reading "
                                 "raw output.\"}");
        } else if (method == "notifications/initialized" || method == "notifications/cancelled") {
            // no reply for notifications
        } else if (method == "ping") {
            reply_result(id, "{}");
        } else if (method == "tools/list") {
            reply_result(id, tools_catalogue());
        } else if (method == "tools/call") {
            auto params = json_object(j, "params").value_or("{}");
            auto name = json_str(params, "name").value_or("");
            auto args = json_object(params, "arguments").value_or("{}");
            std::string result = dispatch_tool(*drv, name, args);
            reply_result(id, result);
            if (drv->hung_up()) {
                // Tell the client the terminal died; then exit cleanly.
                emit("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\",\"params\":{\"level\":"
                     "\"info\",\"data\":\"terminal child exited\"}}");
                break;
            }
        } else if (!is_notification) {
            reply_error(id, -32601, std::string{"method not found: "} + method);
        }
        line.clear();
    }
    return 0;
}
