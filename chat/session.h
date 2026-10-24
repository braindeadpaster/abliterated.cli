//
// A coding-agent session shown as a chat. Two protocols feed one chat model:
//
//   Claude    `claude -p --output-format stream-json --verbose
//              --include-partial-messages --permission-prompt-tool stdio
//              --input-format stream-json --model m [--permission-mode p] [--resume id]`
//             Claude Code's SDK protocol (stream-json + control requests).
//             The initialize reply carries the slash commands; set_model and
//             set_permission_mode switch live. Checked against Claude Code 2.1.310.
//
//   ACP       `opencode acp` / `omp acp` — Agent Client Protocol: JSON-RPC 2.0,
//             one object per line on stdio. initialize -> session/new (or
//             session/load to resume, which replays the history as updates) ->
//             session/prompt, with session/update notifications and
//             session/request_permission. Model and mode go through
//             session/set_config_option; session/list gives past sessions.
//             Checked against OpenCode 2.0.32. oh-my-pi speaks the same protocol.
//
// Threading: reader threads only queue raw lines. Parsing and every change to
// State happen on the UI thread in Pump(), so the UI reads State without locks.

#pragma once
#include <string>
#include <vector>

namespace chat
{
    enum class Backend { Claude, OpenCode, Omp };
    enum class Perm { None, Pending, Allowed, Denied };

    // One choice on a permission request. `kind` is ACP's: allow_once,
    // allow_always, reject_once, reject_always.
    struct PermOption { std::string id, name, kind; };

    // An entry in a model or mode list: the id the agent wants, and its label.
    struct Choice { std::string value, name; };

    // A slash command the agent accepts, as it advertised it.
    struct Command { std::string name, desc, hint; };

    // A past session in the working folder.
    struct SessionInfo { std::string id, title, when; long long sortKey = 0; };

    struct Block
    {
        enum Kind { Text, Tool, Thinking };
        Kind kind = Text;
        std::string text;
        // Tool
        std::string toolId, toolName, summary, detail, result;
        bool done = false, error = false;
        // Permission request attached to this tool call, if any.
        Perm perm = Perm::None;
        std::string permId, permText;
        std::vector<PermOption> permOptions;
        // Bookkeeping for matching stream events.
        std::string msgId;
        int index = -1;
        bool expanded = false;
    };

    struct Message
    {
        enum Role { User, Assistant, Notice };
        Role role = Notice;
        std::string text;              // User / Notice
        std::vector<Block> blocks;     // Assistant
        std::string id;                // ACP message id (history replay)
        bool error = false;
        float appear = 0.0f;           // UI animation state
    };

    struct State
    {
        Backend backend = Backend::Claude;
        std::vector<Message> msgs;
        bool running = false;          // process alive
        bool ready = false;            // accepting messages
        bool busy = false;             // a turn is in flight
        bool thinking = false;
        std::string sessionId, model, cwd;
        double cost = 0.0;
        long long ctxUsed = 0, ctxSize = 0;   // ACP agents report context use

        // What the chat's dropdowns offer, and what's selected. Claude's come
        // from fixed lists; an ACP agent's from its configOptions.
        std::vector<Choice> models, modes;
        std::string modelValue, modeValue;

        std::vector<Command> commands;
        std::vector<SessionInfo> history;
        int historyRev = 0;            // bumps when `history` is replaced

        int revision = 0;              // bumps on every change (auto-scroll)
    };

    struct Options
    {
        Backend backend = Backend::Claude;
        std::wstring cwd;              // working folder
        std::string model;             // empty = the agent's own default
        std::string mode;              // Claude: "default" / "acceptEdits" / "plan"; ACP: a mode value
        std::string resumeId;          // continue this session instead of starting one
    };

    State& Get();

    const char* BackendName(Backend b);    // "Claude", "OpenCode", "oh-my-pi"
    const wchar_t* BackendExe(Backend b);  // claude, opencode, omp

    // Claude models offered (4.7 and below) and Claude's permission modes.
    const std::vector<Choice>& ClaudeModels();
    const std::vector<Choice>& ClaudeModes();

    // Resolves an agent binary: <name>.exe on PATH, the .exe behind npm's
    // <name>.cmd shim (never the .cmd itself — cmd.exe mangles arguments), or
    // the usual per-user install folders.
    bool FindExe(const wchar_t* name, std::wstring& exe);

    // Runs `opencode models` and returns one provider/model id per entry.
    // Blocking; call it from a worker thread.
    bool ListOpenCodeModels(std::vector<std::string>& out);

    bool Start(const Options& opt, std::string& err);
    void Send(const std::string& text);
    void Interrupt();
    void Respond(const std::string& permId, int optionIndex);
    void SetModel(const std::string& value);   // live, mid-session
    void SetMode(const std::string& value);
    // Refills State::history: Claude's transcripts on disk, or the running ACP
    // agent's session/list.
    void RefreshHistory();
    // Deletes a past session, never the open one. Claude's transcript file is
    // removed from disk; an ACP agent is sent session/delete and the list is
    // re-read once it answers. The row leaves State::history straight away.
    void DeleteSession(const std::string& id);
    void Stop();                       // closes stdin; killed if it lingers
    void Pump();                       // once per frame on the UI thread
}
