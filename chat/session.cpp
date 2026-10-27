//
// Coding-agent sessions over stdio. See session.h for the two protocols.

#include "session.h"
#include "json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include "../res/resource.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <stdio.h>

namespace chat
{
    namespace
    {
        // ============================================================ process
        // Owned jointly by the session and its reader threads, so a thread still
        // draining a pipe after Stop() never touches freed memory.
        struct Proc
        {
            HANDLE inW = nullptr;
            HANDLE proc = nullptr;
            HANDLE job = nullptr;
            std::mutex m;
            std::deque<std::string> lines;
            std::string errTail;
            std::atomic<bool> outDone{ false };
        };

        std::shared_ptr<Proc> g_p;
        State   g_s;
        Options g_opt;
        bool    g_stopping = false;
        DWORD   g_stopTick = 0;
        int     g_reqCounter = 0;

        // Tool inputs for pending Claude permission requests, keyed by request id:
        // an allow has to echo the input back as `updatedInput`.
        std::vector<std::pair<std::string, json::Value>> g_permInputs;

        // ACP bookkeeping.
        int         g_rpcId = 0;
        int         g_idInit = -1, g_idNew = -1, g_idPrompt = -1, g_idList = -1;
        std::vector<int> g_configIds;             // set_config_option calls in flight
        std::vector<int> g_deleteIds;             // session/delete calls in flight
        std::string g_effortId;                   // the agent's configId for effort
        int         g_startupConfigs = 0;         // of those, how many gate "ready"
        bool        g_canLoad = false;            // agent advertised loadSession
        bool        g_loading = false;            // session/load replaying history
        std::string g_acpSession;

        std::string Narrow(const std::wstring& w)
        {
            if (w.empty()) return std::string();
            int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s((size_t)n, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
            return s;
        }

        void Reader(std::shared_ptr<Proc> p, HANDLE h, bool isErr)
        {
            std::string buf;
            char tmp[8192];
            DWORD n = 0;
            while (::ReadFile(h, tmp, sizeof(tmp), &n, nullptr) && n > 0)
            {
                std::lock_guard<std::mutex> lock(p->m);
                if (isErr)
                {
                    p->errTail.append(tmp, n);
                    if (p->errTail.size() > 4000) p->errTail.erase(0, p->errTail.size() - 4000);
                    continue;
                }
                buf.append(tmp, n);
                size_t pos;
                while ((pos = buf.find('\n')) != std::string::npos)
                {
                    std::string line = buf.substr(0, pos);
                    buf.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) p->lines.push_back(std::move(line));
                }
            }
            ::CloseHandle(h);
            if (!isErr)
            {
                std::lock_guard<std::mutex> lock(p->m);
                if (!buf.empty()) p->lines.push_back(buf);
                p->outDone = true;
            }
        }

        void WriteLine(const std::string& s)
        {
            if (!g_p || !g_p->inW) return;
            std::string line = s;
            line += '\n';
            DWORD w = 0;
            ::WriteFile(g_p->inW, line.data(), (DWORD)line.size(), &w, nullptr);
        }

        // ============================================================ chat model
        void Notice(const std::string& text, bool err)
        {
            Message m;
            m.role = Message::Notice;
            m.text = text;
            m.error = err;
            g_s.msgs.push_back(m);
        }

        // The assistant message the current turn writes into; a new one starts
        // after anything that isn't assistant output.
        Message& Assistant()
        {
            if (g_s.msgs.empty() || g_s.msgs.back().role != Message::Assistant)
            {
                Message m;
                m.role = Message::Assistant;
                g_s.msgs.push_back(m);
            }
            return g_s.msgs.back();
        }

        Block* FindBlock(const std::string& msgId, int index)
        {
            for (auto it = g_s.msgs.rbegin(); it != g_s.msgs.rend(); ++it)
                for (auto& b : it->blocks)
                    if (b.msgId == msgId && b.index == index) return &b;
            return nullptr;
        }

        Block* FindTool(const std::string& toolId)
        {
            if (toolId.empty()) return nullptr;
            for (auto it = g_s.msgs.rbegin(); it != g_s.msgs.rend(); ++it)
                for (auto& b : it->blocks)
                    if (b.kind == Block::Tool && b.toolId == toolId) return &b;
            return nullptr;
        }

        Block* FindPerm(const std::string& permId)
        {
            for (auto it = g_s.msgs.rbegin(); it != g_s.msgs.rend(); ++it)
                for (auto& b : it->blocks)
                    if (b.permId == permId) return &b;
            return nullptr;
        }

        Block& NewTool(const std::string& id, const std::string& name)
        {
            Block nb;
            nb.kind = Block::Tool;
            nb.toolId = id;
            nb.toolName = name;
            Assistant().blocks.push_back(nb);
            return Assistant().blocks.back();
        }

        void EndTurn()
        {
            g_s.busy = false;
            g_s.thinking = false;
            // A turn that ended (interrupted, say) leaves no tool spinning.
            for (auto& msg : g_s.msgs)
                for (auto& b : msg.blocks)
                    if (b.kind == Block::Tool && !b.done)
                    {
                        b.done = true;
                        if (b.perm == Perm::Pending) b.perm = Perm::Denied;
                    }
        }

        std::string Clip(const std::string& s, size_t n)
        {
            if (s.size() <= n) return s;
            return s.substr(0, n) + "\n...";
        }

        std::string BaseName(const std::string& path)
        {
            size_t p = path.find_last_of("\\/");
            return p == std::string::npos ? path : path.substr(p + 1);
        }

        std::string FirstLine(const std::string& s)
        {
            size_t nl = s.find('\n');
            return nl == std::string::npos ? s : s.substr(0, nl) + " ...";
        }

        std::string TextOf(const json::Value& c)
        {
            if (c.IsString()) return c.Str();
            std::string out;
            for (const auto& item : c.arr)
                if (item["type"].Str() == "text") { if (!out.empty()) out += "\n"; out += item["text"].Str(); }
            return out;
        }

        // ============================================================ Claude
        void DescribeClaude(Block& b, const json::Value& in)
        {
            const std::string& n = b.toolName;
            std::string full;
            json::Write(in, full);

            if (n == "Bash" || n == "PowerShell")
            {
                b.summary = in["command"].Str();
                b.detail = in["command"].Str();
            }
            else if (n == "Read" || n == "Write" || n == "Edit" || n == "MultiEdit" || n == "NotebookEdit")
            {
                const std::string& path = in["file_path"].IsString() ? in["file_path"].Str() : in["notebook_path"].Str();
                b.summary = BaseName(path);
                if (n == "Write")      b.detail = path + "\n\n" + Clip(in["content"].Str(), 2400);
                else if (n == "Edit")  b.detail = path + "\n\n- " + Clip(in["old_string"].Str(), 1200) + "\n+ " + Clip(in["new_string"].Str(), 1200);
                else                   b.detail = path;
            }
            else if (n == "Glob" || n == "Grep") { b.summary = in["pattern"].Str(); b.detail = Clip(full, 1500); }
            else if (n == "WebFetch")  { b.summary = in["url"].Str();   b.detail = Clip(full, 1500); }
            else if (n == "WebSearch") { b.summary = in["query"].Str(); b.detail = Clip(full, 1500); }
            else if (n == "Task" || n == "Agent") { b.summary = in["description"].Str(); b.detail = Clip(in["prompt"].Str(), 1500); }
            else if (n == "TodoWrite") { b.summary = "update the to-do list"; b.detail = Clip(full, 1500); }
            else
            {
                for (const auto& kv : in.obj)
                    if (kv.second.IsString()) { b.summary = kv.second.Str(); break; }
                b.detail = Clip(full, 1500);
            }
            b.summary = FirstLine(b.summary);
        }

        void HandleClaude(const json::Value& m)
        {
            const std::string& type = m["type"].Str();

            // The reply to our initialize request is the "process is up" signal.
            // system/init only arrives once the first user message is in, so
            // waiting for it before allowing a send would deadlock.
            if (type == "control_response")
            {
                const json::Value& r = m["response"];
                if (r["request_id"].Str().find("_init") != std::string::npos)
                {
                    if (r["subtype"].Str() == "success")
                    {
                        g_s.ready = true;
                        g_s.commands.clear();
                        for (const auto& c : r["response"]["commands"].arr)
                            g_s.commands.push_back({ c["name"].Str(), c["description"].Str(), c["argumentHint"].Str() });
                    }
                    else Notice("claude rejected the session setup: " + r["error"].Str(), true);
                }
                return;
            }

            if (type == "system")
            {
                if (m["subtype"].Str() == "init")
                {
                    g_s.ready = true;
                    g_s.sessionId = m["session_id"].Str();
                    g_s.model = m["model"].Str();
                    g_s.cwd = m["cwd"].Str();
                }
                return;
            }

            // Sub-agent internals carry a parent tool id; the chat shows only the
            // main thread, with the sub-agent as one tool card.
            if (!m["parent_tool_use_id"].IsNull()) return;

            if (type == "stream_event")
            {
                const json::Value& e = m["event"];
                const std::string& et = e["type"].Str();
                const std::string& msgId = m["api_message_id"].Str();
                int idx = (int)e["index"].Num(-1);

                if (et == "content_block_start")
                {
                    const json::Value& cb = e["content_block"];
                    const std::string& ct = cb["type"].Str();
                    Block b;
                    b.msgId = msgId;
                    b.index = idx;
                    if (ct == "text") { b.kind = Block::Text; b.text = cb["text"].Str(); }
                    else if (ct == "tool_use") { b.kind = Block::Tool; b.toolId = cb["id"].Str(); b.toolName = cb["name"].Str(); }
                    else if (ct == "thinking") { b.kind = Block::Thinking; g_s.thinking = true; }
                    else return;
                    Assistant().blocks.push_back(b);
                }
                else if (et == "content_block_delta")
                {
                    Block* b = FindBlock(msgId, idx);
                    if (b && e["delta"]["type"].Str() == "text_delta") b->text += e["delta"]["text"].Str();
                }
                else if (et == "content_block_stop")
                {
                    Block* b = FindBlock(msgId, idx);
                    if (b && b->kind == Block::Thinking) g_s.thinking = false;
                }
                return;
            }

            if (type == "assistant")
            {
                // The finished block(s), authoritative over what streamed.
                const json::Value& msg = m["message"];
                const std::string& msgId = msg["id"].Str();
                for (const auto& c : msg["content"].arr)
                {
                    const std::string& ct = c["type"].Str();
                    if (ct == "tool_use")
                    {
                        Block* b = FindTool(c["id"].Str());
                        if (!b) { b = &NewTool(c["id"].Str(), c["name"].Str()); b->msgId = msgId; }
                        DescribeClaude(*b, c["input"]);
                    }
                    else if (ct == "text")
                    {
                        const std::string& txt = c["text"].Str();
                        Block* match = nullptr;
                        for (auto& b : Assistant().blocks)
                            if (b.kind == Block::Text && b.msgId == msgId && txt.compare(0, b.text.size(), b.text) == 0)
                                match = &b;
                        if (match) match->text = txt;
                        else
                        {
                            Block nb;
                            nb.kind = Block::Text;
                            nb.text = txt;
                            nb.msgId = msgId;
                            Assistant().blocks.push_back(nb);
                        }
                    }
                }
                return;
            }

            if (type == "user")
            {
                for (const auto& c : m["message"]["content"].arr)
                {
                    if (c["type"].Str() != "tool_result") continue;
                    Block* b = FindTool(c["tool_use_id"].Str());
                    if (!b) continue;
                    b->done = true;
                    b->error = c["is_error"].Truthy();
                    b->result = Clip(TextOf(c["content"]), 3000);
                    if (b->perm == Perm::Pending) b->perm = Perm::None;
                }
                return;
            }

            if (type == "control_request")
            {
                const std::string& id = m["request_id"].Str();
                const json::Value& req = m["request"];
                if (req["subtype"].Str() == "can_use_tool")
                {
                    Block* b = FindTool(req["tool_use_id"].Str());
                    if (!b)
                    {
                        b = &NewTool(req["tool_use_id"].Str(), req["tool_name"].Str());
                        DescribeClaude(*b, req["input"]);
                    }
                    b->perm = Perm::Pending;
                    b->permId = id;
                    b->permText = req["description"].Str();
                    b->permOptions = { { "allow", "Allow", "allow_once" }, { "deny", "Deny", "reject_once" } };
                    b->expanded = true;
                    g_permInputs.emplace_back(id, req["input"]);
                }
                else
                {
                    // Anything else we don't host (hooks, MCP): say so, so the CLI
                    // doesn't wait on an answer that never comes.
                    WriteLine("{\"type\":\"control_response\",\"response\":{\"subtype\":\"error\",\"request_id\":" +
                              json::Quote(id) + ",\"error\":\"not supported by this host\"}}");
                }
                return;
            }

            if (type == "control_cancel_request")
            {
                if (Block* b = FindPerm(m["request_id"].Str()))
                    if (b->perm == Perm::Pending) b->perm = Perm::Denied;
                return;
            }

            if (type == "result")
            {
                if (!m["total_cost_usd"].IsNull()) g_s.cost = m["total_cost_usd"].Num(g_s.cost);
                if (m["is_error"].Truthy())
                {
                    std::string why = m["result"].Str();
                    if (why.empty()) why = m["subtype"].Str();
                    Notice(why.empty() ? "the turn ended with an error" : why, true);
                }
                EndTurn();
                return;
            }
        }

        // ---- Claude history: transcripts in ~/.claude/projects/<folder>/<id>.jsonl,
        // where <folder> is the working folder with every non-alphanumeric
        // character replaced by '-'.
        std::wstring ClaudeProjectDir(const std::wstring& cwd)
        {
            wchar_t base[MAX_PATH] = {};
            std::wstring root;
            if (::GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", base, MAX_PATH)) root = base;
            else if (::GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH)) root = std::wstring(base) + L"\\.claude";
            else return std::wstring();
            std::wstring enc = cwd;
            for (auto& c : enc)
                if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))) c = L'-';
            return root + L"\\projects\\" + enc;
        }

        // A user line's own text, or "" for tool results, system-injected
        // lines and command echoes ("<command-name>...", "<local-command-...").
        std::string UserText(const json::Value& line)
        {
            if (line["isMeta"].Truthy() || line["isSidechain"].Truthy()) return std::string();
            const json::Value& c = line["message"]["content"];
            std::string t;
            if (c.IsString()) t = c.Str();
            else for (const auto& b : c.arr) if (b["type"].Str() == "text") { t = b["text"].Str(); break; }
            if (t.empty() || t[0] == '<') return std::string();
            return t;
        }

        // Calls `fn` with each user / assistant line of a transcript. Other lines
        // (attachments, request blobs) can be large and are skipped unparsed.
        template <class F>
        void ForEachTranscriptLine(const std::wstring& path, size_t maxBytes, F fn)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
            std::string line;
            size_t total = 0;
            char buf[65536];
            size_t n;
            bool stop = false;
            while (!stop && (n = fread(buf, 1, sizeof(buf), f)) > 0)
            {
                for (size_t i = 0; i < n && !stop; i++)
                {
                    if (buf[i] != '\n') { line += buf[i]; continue; }
                    if (line.find("\"type\":\"user\"") != std::string::npos || line.find("\"type\":\"assistant\"") != std::string::npos)
                    {
                        json::Value v = json::Parse(line);
                        if (v.IsObject() && !fn(v)) stop = true;
                    }
                    line.clear();
                }
                total += n;
                if (maxBytes && total >= maxBytes) break;
            }
            fclose(f);
        }

        void ScanClaudeHistory(const std::wstring& cwd)
        {
            g_s.history.clear();
            std::wstring dir = ClaudeProjectDir(cwd);
            WIN32_FIND_DATAW fd;
            HANDLE h = dir.empty() ? INVALID_HANDLE_VALUE : ::FindFirstFileW((dir + L"\\*.jsonl").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    SessionInfo si;
                    std::wstring name = fd.cFileName;
                    si.id = Narrow(name.substr(0, name.size() - 6));
                    // FILETIME is 100-ns ticks since 1601-01-01 UTC;
                    // 11644473600 = seconds between 1601 and 1970.
                    {
                        ULARGE_INTEGER u;
                        u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                        u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                        si.sortKey = (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
                    }
                    SYSTEMTIME ut, lt;
                    ::FileTimeToSystemTime(&fd.ftLastWriteTime, &ut);
                    ::SystemTimeToTzSpecificLocalTime(nullptr, &ut, &lt);
                    char when[32];
                    snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute);
                    si.when = when;
                    // The title is the first thing the user typed; the first prompt
                    // sits near the top, so only the head of the file is read.
                    ForEachTranscriptLine(dir + L"\\" + name, 1 << 20, [&](const json::Value& v) {
                        if (v["type"].Str() != "user") return true;
                        std::string t = UserText(v);
                        if (t.empty()) return true;
                        si.title = FirstLine(t);
                        return false;
                    });
                    if (!si.title.empty()) g_s.history.push_back(si);   // no prompt = nothing to reopen
                } while (::FindNextFileW(h, &fd));
                ::FindClose(h);
            }
            std::sort(g_s.history.begin(), g_s.history.end(),
                      [](const SessionInfo& a, const SessionInfo& b) { return a.sortKey > b.sortKey; });
            if (g_s.history.size() > 200) g_s.history.resize(200);
            g_s.historyRev++;
        }

        void HandleClaude(const json::Value& m);

        // Replays a saved transcript into the chat through the live handler: its
        // assistant / tool_result lines have the same shapes as the stream.
        void LoadClaudeTranscript(const std::wstring& cwd, const std::string& id)
        {
            std::wstring path = ClaudeProjectDir(cwd) + L"\\" + std::wstring(id.begin(), id.end()) + L".jsonl";
            ForEachTranscriptLine(path, 0, [&](const json::Value& v) {
                if (v["isSidechain"].Truthy()) return true;
                if (v["type"].Str() == "user")
                {
                    std::string t = UserText(v);
                    if (!t.empty())
                    {
                        Message mu;
                        mu.role = Message::User;
                        mu.text = t;
                        mu.appear = 1.0f;
                        g_s.msgs.push_back(mu);
                        return true;
                    }
                }
                HandleClaude(v);
                return true;
            });
            for (auto& msg : g_s.msgs) msg.appear = 1.0f;
            EndTurn();
        }

        // ============================================================ ACP (OpenCode, oh-my-pi)
        int Rpc(const char* method, const std::string& paramsJson)
        {
            int id = ++g_rpcId;
            char head[96];
            snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":", id);
            WriteLine(std::string(head) + json::Quote(method) + ",\"params\":" + paramsJson + "}");
            return id;
        }

        void SetConfig(const char* configId, const std::string& value)
        {
            g_configIds.push_back(Rpc("session/set_config_option", "{\"sessionId\":" + json::Quote(g_acpSession) +
                ",\"configId\":" + json::Quote(configId) + ",\"value\":" + json::Quote(value) + "}"));
        }

        // Reads the model, mode and effort lists, and what's selected, out of a
        // configOptions array.
        void TakeConfig(const json::Value& opts)
        {
            if (!opts.IsArray() || opts.arr.empty()) return;
            bool sawEffort = false;
            for (const auto& co : opts.arr)
            {
                const std::string& id = co["id"].Str();
                const std::string& cat = co["category"].Str();
                bool isModel = (id == "model" || cat == "model");
                bool isMode = (id == "mode" || cat == "mode");
                bool isEffort = (id == "effort" || cat == "thought_level");
                if (!isModel && !isMode && !isEffort) continue;
                std::vector<Choice> list;
                for (const auto& o : co["options"].arr)
                    list.push_back({ o["value"].Str(), o["name"].Str().empty() ? o["value"].Str() : o["name"].Str() });
                if (isModel)
                {
                    g_s.models = list;
                    g_s.modelValue = co["currentValue"].Str();
                    g_s.model = g_s.modelValue;
                }
                else if (isMode)
                {
                    g_s.modes = list;
                    g_s.modeValue = co["currentValue"].Str();
                }
                else
                {
                    sawEffort = true;
                    g_effortId = id;
                    g_s.efforts = list;
                    g_s.effortValue = co["currentValue"].Str();
                }
            }
            // The list is per model: a model without effort levels drops the option.
            if (!sawEffort) { g_s.efforts.clear(); g_s.effortValue.clear(); }
        }

        // True (and forgets it) if `rid` answers one of our session/delete calls.
        bool TakeDeleteReply(int rid)
        {
            for (size_t i = 0; i < g_deleteIds.size(); i++)
                if (g_deleteIds[i] == rid) { g_deleteIds.erase(g_deleteIds.begin() + i); return true; }
            return false;
        }

        // The reply to a set_config_option call (or its error).
        bool ConfigReply(int rid, const json::Value* result)
        {
            for (size_t i = 0; i < g_configIds.size(); i++)
            {
                if (g_configIds[i] != rid) continue;
                g_configIds.erase(g_configIds.begin() + i);
                if (result) TakeConfig((*result)["configOptions"]);
                if (g_startupConfigs > 0 && --g_startupConfigs == 0) g_s.ready = true;
                return true;
            }
            return false;
        }

        std::string WhenFromIso(const std::string& iso)
        {
            // "2026-10-24T01:29:42.685Z" -> "2026-10-24 01:29"
            if (iso.size() < 16) return iso;
            std::string s = iso.substr(0, 16);
            s[10] = ' ';
            return s;
        }

        // "2026-10-24T01:29:42.685Z" -> unix seconds. UTC; the bucket pass
        // compares against the local day's start, so timezone is handled there.
        long long IsoToUnixSec(const std::string& iso)
        {
            if (iso.size() < 19) return 0;
            SYSTEMTIME st = {};
            st.wYear   = (WORD)atoi(iso.substr(0, 4).c_str());
            st.wMonth  = (WORD)atoi(iso.substr(5, 2).c_str());
            st.wDay    = (WORD)atoi(iso.substr(8, 2).c_str());
            st.wHour   = (WORD)atoi(iso.substr(11, 2).c_str());
            st.wMinute = (WORD)atoi(iso.substr(14, 2).c_str());
            st.wSecond = (WORD)atoi(iso.substr(17, 2).c_str());
            FILETIME ft;
            if (!::SystemTimeToFileTime(&st, &ft)) return 0;
            ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
            // FILETIME is 100-ns ticks since 1601-01-01 UTC; 11644473600 = seconds
            // between 1601 and 1970.
            return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
        }

        long long FileTimeToUnixSec(const FILETIME& ft)
        {
            ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
            return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
        }

        // An ACP agent runs only the commands it advertises (OpenCode 2.0.32:
        // init, review, compact); anything else typed as "/x" reaches the model
        // as plain text. OpenCode's TUI commands (/models, /themes, /share...)
        // live in its terminal client, not the agent, so they can't be offered
        // here. The two below are handled by this app (IsLocalCommand).
        void AddLocalCommands(std::vector<Command>& out)
        {
            static const Command kLocal[] = {
                { "new",      "start a new chat in this folder", "" },
                { "sessions", "show this folder's past chats", "" },
            };
            for (const auto& c : kLocal)
            {
                bool have = false;
                for (const auto& e : out) if (e.name == c.name) { have = true; break; }
                if (!have) out.push_back(c);
            }
        }

        std::string Capitalised(const std::string& s)
        {
            std::string o = s;
            if (!o.empty() && o[0] >= 'a' && o[0] <= 'z') o[0] = (char)(o[0] - 'a' + 'A');
            return o;
        }

        // Card text for an ACP tool call, from whatever the update carries.
        void DescribeAcp(Block& b, const json::Value& u)
        {
            const json::Value& in = u["rawInput"];
            std::string path = u["locations"][(size_t)0]["path"].Str();
            if (path.empty()) path = in["path"].IsString() ? in["path"].Str() : in["filePath"].Str();

            if (in["command"].IsString() && !in["command"].Str().empty())
            {
                b.summary = in["command"].Str();
                b.detail = in["command"].Str();
            }
            else if (!path.empty())
            {
                b.summary = BaseName(path);
                b.detail = path;
                if (in["content"].IsString()) b.detail += "\n\n" + Clip(in["content"].Str(), 2400);
                if (in["oldString"].IsString())
                    b.detail += "\n\n- " + Clip(in["oldString"].Str(), 1200) + "\n+ " + Clip(in["newString"].Str(), 1200);
            }
            else if (in["pattern"].IsString()) b.summary = in["pattern"].Str();
            else if (in["url"].IsString())     b.summary = in["url"].Str();
            else if (in["query"].IsString())   b.summary = in["query"].Str();
            else if (in["description"].IsString()) b.summary = in["description"].Str();
            else if (!u["title"].Str().empty() && b.summary.empty()) b.summary = u["title"].Str();

            // A permission request carries the proposed change as a diff.
            for (const auto& c : u["content"].arr)
                if (c["type"].Str() == "diff")
                {
                    b.detail = c["path"].Str();
                    if (c["oldText"].IsString()) b.detail += "\n\n- " + Clip(c["oldText"].Str(), 1200) + "\n+ " + Clip(c["newText"].Str(), 1200);
                    else b.detail += "\n\n" + Clip(c["newText"].Str(), 2400);
                }
            b.summary = FirstLine(b.summary);
        }

        void AcpUpdate(const json::Value& u)
        {
            const std::string& kind = u["sessionUpdate"].Str();

            if (kind == "agent_message_chunk")
            {
                g_s.thinking = false;
                const std::string& id = u["messageId"].Str();
                const std::string& txt = u["content"]["text"].Str();
                Message& a = Assistant();
                Block* b = nullptr;
                for (auto& blk : a.blocks)
                    if (blk.kind == Block::Text && blk.msgId == id) b = &blk;
                // Text after a tool call starts a fresh block, so it reads in order.
                if (!b || (&a.blocks.back() != b))
                {
                    Block nb;
                    nb.kind = Block::Text;
                    nb.msgId = id;
                    a.blocks.push_back(nb);
                    b = &a.blocks.back();
                }
                b->text += txt;
            }
            else if (kind == "agent_thought_chunk")
            {
                g_s.thinking = true;
            }
            else if (kind == "tool_call" || kind == "tool_call_update")
            {
                g_s.thinking = false;
                const std::string& id = u["toolCallId"].Str();
                Block* b = FindTool(id);
                if (!b)
                {
                    std::string name = !u["name"].Str().empty() ? u["name"].Str() : u["title"].Str();
                    b = &NewTool(id, Capitalised(name));
                }
                DescribeAcp(*b, u);
                const std::string& st = u["status"].Str();
                if (st == "completed") { b->done = true; b->error = false; }
                else if (st == "failed") { b->done = true; b->error = true; }
                std::string out;
                for (const auto& c : u["content"].arr)
                    if (c["type"].Str() == "content")
                    {
                        const json::Value& inner = c["content"];
                        if (!out.empty()) out += "\n";
                        out += inner.IsObject() ? inner["text"].Str() : TextOf(inner);
                    }
                if (out.empty() && u["rawOutput"]["output"].IsString()) out = u["rawOutput"]["output"].Str();
                if (!out.empty()) b->result = Clip(out, 3000);
                if (b->done && b->perm == Perm::Pending) b->perm = b->error ? Perm::Denied : Perm::None;
            }
            else if (kind == "usage_update")
            {
                g_s.ctxUsed = (long long)u["used"].Num(0);
                g_s.ctxSize = (long long)u["size"].Num(0);
                if (!u["cost"]["amount"].IsNull()) g_s.cost = u["cost"]["amount"].Num(g_s.cost);
            }
            else if (kind == "config_option_update" || kind == "config_options_update")
            {
                TakeConfig(u["configOptions"]);
            }
            else if (kind == "current_mode_update")
            {
                g_s.modeValue = u["currentModeId"].Str();
            }
            else if (kind == "available_commands_update")
            {
                g_s.commands.clear();
                for (const auto& c : u["availableCommands"].arr)
                    g_s.commands.push_back({ c["name"].Str(), c["description"].Str(), c["input"]["hint"].Str() });
                AddLocalCommands(g_s.commands);
            }
            else if (kind == "user_message_chunk" && g_loading)
            {
                // Only while session/load replays history; a live turn's own
                // message is already in the chat.
                const std::string& id = u["messageId"].Str();
                if (g_s.msgs.empty() || g_s.msgs.back().role != Message::User || g_s.msgs.back().id != id)
                {
                    Message m;
                    m.role = Message::User;
                    m.id = id;
                    g_s.msgs.push_back(m);
                }
                g_s.msgs.back().text += u["content"]["text"].Str();
            }
        }

        void HandleAcp(const json::Value& m)
        {
            const json::Value& method = m["method"];
            const json::Value& id = m["id"];

            // ---- responses to our requests
            if (method.IsNull() && !id.IsNull())
            {
                int rid = (int)id.Num(-1);
                const json::Value& err = m["error"];
                std::string who = BackendName(g_s.backend);
                if (!err.IsNull())
                {
                    std::string msg = err["message"].Str();
                    if (rid == g_idList) return;            // no history; the list just stays empty
                    if (TakeDeleteReply(rid))
                    {
                        Notice(who + " couldn't delete that chat: " + msg, true);
                        RefreshHistory();                   // bring the row back
                        return;
                    }
                    if (rid == g_idPrompt) EndTurn();
                    if (rid == g_idNew || rid == g_idInit) msg = who + " couldn't start a session: " + msg;
                    // A model the agent rejects still leaves a usable session on its default.
                    ConfigReply(rid, nullptr);
                    Notice(msg.empty() ? who + " reported an error" : msg, true);
                    return;
                }
                const json::Value& r = m["result"];
                if (rid == g_idInit)
                {
                    g_canLoad = r["agentCapabilities"]["loadSession"].Truthy();
                    std::string common = "\"cwd\":" + json::Quote(Narrow(g_opt.cwd)) + ",\"mcpServers\":[]";
                    if (!g_opt.resumeId.empty() && g_canLoad)
                    {
                        // The agent replays the session as updates before it answers.
                        g_loading = true;
                        g_acpSession = g_opt.resumeId;
                        g_idNew = Rpc("session/load", "{\"sessionId\":" + json::Quote(g_opt.resumeId) + "," + common + "}");
                    }
                    else
                    {
                        if (!g_opt.resumeId.empty()) Notice(who + " can't reopen past sessions; starting a new one", true);
                        g_idNew = Rpc("session/new", "{" + common + "}");
                    }
                }
                else if (rid == g_idNew)
                {
                    g_loading = false;
                    if (r["sessionId"].IsString()) g_acpSession = r["sessionId"].Str();
                    g_s.sessionId = g_acpSession;
                    g_s.cwd = Narrow(g_opt.cwd);
                    TakeConfig(r["configOptions"]);
                    if (!g_opt.model.empty() && g_opt.model != g_s.modelValue) { SetConfig("model", g_opt.model); g_startupConfigs++; }
                    if (!g_opt.mode.empty() && g_opt.mode != g_s.modeValue)    { SetConfig("mode", g_opt.mode);   g_startupConfigs++; }
                    if (g_startupConfigs == 0) g_s.ready = true;
                }
                else if (rid == g_idPrompt)
                {
                    const std::string& why = r["stopReason"].Str();
                    if (why == "cancelled") Notice("turn stopped", false);
                    else if (why == "max_tokens") Notice("stopped: hit the output limit", true);
                    else if (why == "refusal") Notice("the model declined to continue", true);
                    EndTurn();
                }
                else if (rid == g_idList)
                {
                    g_s.history.clear();
                    for (const auto& se : r["sessions"].arr)
                    {
                        SessionInfo si;
                        si.id = se["sessionId"].Str();
                        si.title = se["title"].Str();
                        const std::string& iso = se["updatedAt"].Str();
                        si.when = WhenFromIso(iso);
                        si.sortKey = IsoToUnixSec(iso);
                        if (si.title.empty() || si.title.compare(0, 12, "New session ") == 0) si.title = "(untitled)";
                        g_s.history.push_back(si);
                    }
                    std::sort(g_s.history.begin(), g_s.history.end(),
                              [](const SessionInfo& a, const SessionInfo& b) { return a.sortKey > b.sortKey; });
                    g_s.historyRev++;
                }
                else if (TakeDeleteReply(rid)) RefreshHistory();
                else ConfigReply(rid, &r);
                return;
            }

            const std::string& meth = method.Str();

            // ---- notifications
            if (id.IsNull())
            {
                if (meth == "session/update") AcpUpdate(m["params"]["update"]);
                return;
            }

            // ---- requests from the agent
            std::string rawId;
            json::Write(id, rawId);
            if (meth == "session/request_permission")
            {
                const json::Value& p = m["params"];
                const json::Value& tc = p["toolCall"];
                Block* b = FindTool(tc["toolCallId"].Str());
                if (!b)
                {
                    std::string name = !tc["name"].Str().empty() ? tc["name"].Str() : tc["title"].Str();
                    b = &NewTool(tc["toolCallId"].Str(), Capitalised(name));
                }
                DescribeAcp(*b, tc);
                b->perm = Perm::Pending;
                b->permId = rawId;
                b->permText = tc["title"].Str();
                b->permOptions.clear();
                for (const auto& o : p["options"].arr)
                    b->permOptions.push_back({ o["optionId"].Str(), o["name"].Str(), o["kind"].Str() });
                if (b->permOptions.empty())
                    b->permOptions = { { "allow", "Allow", "allow_once" }, { "reject", "Deny", "reject_once" } };
                b->expanded = true;
                return;
            }

            // We advertised no file-system or terminal capabilities, so anything
            // else is unexpected: decline it rather than leave the agent waiting.
            WriteLine("{\"jsonrpc\":\"2.0\",\"id\":" + rawId + ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
        }

        void Cleanup()
        {
            if (!g_p) return;
            if (g_p->inW) { ::CloseHandle(g_p->inW); g_p->inW = nullptr; }
            if (g_p->proc) { ::CloseHandle(g_p->proc); g_p->proc = nullptr; }
            // Closing the job kills anything still in it (kill-on-close).
            if (g_p->job) { ::CloseHandle(g_p->job); g_p->job = nullptr; }
            g_p.reset();
            g_permInputs.clear();
        }

        bool Exists(const std::wstring& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

        // Writes the embedded IDR_JB_MD resource to
        // %LOCALAPPDATA%\abliterated.cli\jb.md and returns that path. Overwrites
        // every launch so a newer exe always wins over an older extracted copy.
        bool ExtractJb(std::wstring& out)
        {
            HRSRC hRes = ::FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_JB_MD), RT_RCDATA);
            if (!hRes) return false;
            HGLOBAL hGlobal = ::LoadResource(nullptr, hRes);
            if (!hGlobal) return false;
            const void* data = ::LockResource(hGlobal);
            DWORD size = ::SizeofResource(nullptr, hRes);
            if (!data || !size) return false;

            PWSTR appData = nullptr;
            if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) || !appData)
                return false;
            std::wstring dir = appData;
            ::CoTaskMemFree(appData);
            dir += L"\\abliterated.cli";
            ::CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring path = dir + L"\\jb.md";

            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
            fwrite(data, 1, size, f);
            fclose(f);
            out = path;
            return true;
        }

        // Resolve the jailbreak. For dev builds (running out of bin\Release) an
        // on-disk assets\jb.md next to the exe or a few levels up wins, so edits
        // land without a rebuild. For a shipped exe with nothing beside it, the
        // embedded RCDATA copy is extracted to %LOCALAPPDATA%.
        bool FindJb(std::wstring& out)
        {
            wchar_t exePath[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            wchar_t* slash = wcsrchr(exePath, L'\\');
            if (slash) *slash = 0;
            const wchar_t* up[] = { L"", L"\\..", L"\\..\\..", L"\\..\\..\\.." };
            for (const wchar_t* u : up)
            {
                wchar_t buf[MAX_PATH * 2];
                swprintf_s(buf, L"%s%s\\assets\\jb.md", exePath, u);
                if (Exists(buf)) { out = buf; return true; }
            }
            return ExtractJb(out);
        }

        // Doubles every backslash so a Windows path can be embedded in a JSON string.
        std::string JsonPath(const std::wstring& w)
        {
            int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s((size_t)n, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
            std::string o;
            o.reserve(s.size() + 8);
            for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '"') o += "\\\""; else o += c; }
            return o;
        }

        // GLM 5.3's trick: a workspace opencode.jsonc that pins the model and loads
        // jb.md as an instruction file. Written once per working folder if the user
        // hasn't dropped one of their own.
        void EnsureOpenCodeConfig(const std::wstring& cwd, const std::wstring& jb)
        {
            if (cwd.empty()) return;
            std::wstring path = cwd + L"\\opencode.jsonc";
            if (Exists(path)) return;             // respect any config the user placed
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
            std::string jbStr = JsonPath(jb);
            fprintf(f,
                "{\n"
                "  \"$schema\": \"https://opencode.ai/config.json\",\n"
                "  \"model\": \"opencode/glm-5.3-flash-free\",\n"
                "  \"instructions\": [\"%s\"]\n"
                "}\n",
                jbStr.c_str());
            fclose(f);
        }

        // Spawns `cmd` hidden, with stdin/stdout/stderr on pipes, in a kill-on-close
        // job. The child's environment drops CLAUDECODE (nested-session marker).
        bool Spawn(const std::wstring& exe, const std::wstring& cmd, const std::wstring& cwd,
                   HANDLE* inW, HANDLE* outR, HANDLE* errR, HANDLE* proc, HANDLE* job, std::string& err)
        {
            SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
            HANDLE inR = nullptr, outW = nullptr, errW = nullptr;
            *inW = *outR = *errR = nullptr;
            if (!::CreatePipe(&inR, inW, &sa, 1 << 20) || !::CreatePipe(outR, &outW, &sa, 1 << 20) ||
                !::CreatePipe(errR, &errW, &sa, 1 << 16))
            {
                err = "couldn't create pipes";
                return false;
            }
            ::SetHandleInformation(*inW, HANDLE_FLAG_INHERIT, 0);
            ::SetHandleInformation(*outR, HANDLE_FLAG_INHERIT, 0);
            ::SetHandleInformation(*errR, HANDLE_FLAG_INHERIT, 0);

            std::wstring env;
            if (wchar_t* block = ::GetEnvironmentStringsW())
            {
                for (wchar_t* e = block; *e; e += wcslen(e) + 1)
                    if (_wcsnicmp(e, L"CLAUDECODE=", 11) != 0) { env += e; env += L'\0'; }
                ::FreeEnvironmentStringsW(block);
            }
            env += L'\0';

            STARTUPINFOW si = { sizeof(si) };
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = inR;
            si.hStdOutput = outW;
            si.hStdError = errW;
            PROCESS_INFORMATION pi = {};
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(0);
            BOOL ok = ::CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                       (void*)env.c_str(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
            ::CloseHandle(inR);
            ::CloseHandle(outW);
            ::CloseHandle(errW);
            if (!ok)
            {
                ::CloseHandle(*inW); ::CloseHandle(*outR); ::CloseHandle(*errR);
                char buf[96]; snprintf(buf, sizeof(buf), "couldn't start the agent (error %lu)", ::GetLastError());
                err = buf;
                return false;
            }
            *job = ::CreateJobObjectW(nullptr, nullptr);
            if (*job)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
                li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                ::SetInformationJobObject(*job, JobObjectExtendedLimitInformation, &li, sizeof(li));
                ::AssignProcessToJobObject(*job, pi.hProcess);
            }
            ::ResumeThread(pi.hThread);
            ::CloseHandle(pi.hThread);
            *proc = pi.hProcess;
            return true;
        }
    }

    State& Get() { return g_s; }

    bool FindExe(const wchar_t* name, std::wstring& exe)
    {
        wchar_t buf[MAX_PATH];
        if (::SearchPathW(nullptr, name, L".exe", MAX_PATH, buf, nullptr)) { exe = buf; return true; }

        // npm's .cmd shim runs `"%dp0%\<relative path>.exe" %*`; read that path.
        if (::SearchPathW(nullptr, name, L".cmd", MAX_PATH, buf, nullptr))
        {
            std::wstring shim = buf;
            std::wstring dir = shim.substr(0, shim.find_last_of(L"\\/"));
            FILE* f = nullptr;
            if (_wfopen_s(&f, shim.c_str(), L"rb") == 0 && f)
            {
                char text[4096] = {};
                size_t n = fread(text, 1, sizeof(text) - 1, f);
                fclose(f);
                std::string s(text, n);
                size_t a = s.find("%dp0%\\");
                size_t e = (a == std::string::npos) ? std::string::npos : s.find(".exe\"", a);
                if (e != std::string::npos)
                {
                    std::string rel = s.substr(a + 6, e + 4 - (a + 6));
                    std::wstring wrel(rel.begin(), rel.end());
                    std::wstring c = dir + L"\\" + wrel;
                    if (Exists(c)) { exe = c; return true; }
                }
            }
        }

        // Per-user install folders: the native installers', and bun's globals.
        wchar_t home[MAX_PATH];
        if (::GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH))
        {
            const wchar_t* dirs[] = { L"\\.local\\bin\\", L"\\.bun\\bin\\", L"\\.omp\\bin\\" };
            for (const wchar_t* d : dirs)
            {
                std::wstring c = std::wstring(home) + d + name + L".exe";
                if (Exists(c)) { exe = c; return true; }
            }
        }
        return false;
    }

    bool ListOpenCodeModels(std::vector<std::string>& out)
    {
        std::wstring exe;
        if (!FindExe(L"opencode", exe)) return false;
        HANDLE inW, outR, errR, proc, job;
        std::string err;
        if (!Spawn(exe, L"\"" + exe + L"\" models", L"", &inW, &outR, &errR, &proc, &job, err)) return false;
        ::CloseHandle(inW);
        ::CloseHandle(errR);

        std::string all;
        char tmp[4096];
        DWORD n = 0;
        while (::ReadFile(outR, tmp, sizeof(tmp), &n, nullptr) && n > 0) all.append(tmp, n);
        ::CloseHandle(outR);
        ::WaitForSingleObject(proc, 20000);
        ::CloseHandle(proc);
        if (job) ::CloseHandle(job);

        size_t i = 0;
        while (i < all.size())
        {
            size_t nl = all.find('\n', i);
            if (nl == std::string::npos) nl = all.size();
            std::string line = all.substr(i, nl - i);
            i = nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            // provider/model ids only: no spaces, at least one slash.
            if (!line.empty() && line.find('/') != std::string::npos && line.find(' ') == std::string::npos)
                out.push_back(line);
        }
        return !out.empty();
    }

    const char* BackendName(Backend b)
    {
        return b == Backend::Claude ? "Claude" : b == Backend::OpenCode ? "OpenCode" : "oh-my-pi";
    }

    const wchar_t* BackendExe(Backend b)
    {
        return b == Backend::Claude ? L"claude" : b == Backend::OpenCode ? L"opencode" : L"omp";
    }

    const std::vector<Choice>& ClaudeModels()
    {
        static const std::vector<Choice> list = {
            { "claude-opus-4-7",   "Opus 4.7" },
            { "claude-opus-4-6",   "Opus 4.6" },
            { "claude-opus-4-5",   "Opus 4.5" },
            { "claude-sonnet-4-6", "Sonnet 4.6" },
            { "claude-sonnet-4-5", "Sonnet 4.5" },
            { "claude-haiku-4-5",  "Haiku 4.5" },
        };
        return list;
    }

    // No bypass mode: every tool the agent runs either asks, or is an edit the
    // user chose to auto-accept.
    const std::vector<Choice>& ClaudeModes()
    {
        static const std::vector<Choice> list = {
            { "default",     "Ask permission" },
            { "acceptEdits", "Auto-accept edits" },
            { "plan",        "Plan mode" },
        };
        return list;
    }

    bool Start(const Options& opt, std::string& err)
    {
        Stop();
        Cleanup();

        std::wstring exe;
        if (!FindExe(BackendExe(opt.backend), exe))
        {
            err = std::string(Narrow(BackendExe(opt.backend))) + " not found (install " + BackendName(opt.backend) + " first)";
            return false;
        }

        // Ids are restricted to the characters they actually use; nothing else
        // from the UI reaches the command line.
        auto Clean = [](const std::string& in) {
            std::string o;
            for (char c : in)
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '/' || c == '_' || c == ':' || c == '#' || c == '[' || c == ']')
                    o += c;
            return o;
        };
        std::string model = Clean(opt.model), mode = Clean(opt.mode), resume = Clean(opt.resumeId);
        auto W = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };

        // Shipped jailbreak system prompt — appended to Claude's system prompt
        // and dropped into opencode.jsonc for OpenCode. Matches the bypasses in
        // Clawd\Launch_Jailbreak.bat and "GLM 5.3"\workspace\opencode.jsonc.
        std::wstring jb;
        bool haveJb = FindJb(jb);

        std::wstring cmd = L"\"" + exe + L"\"";
        if (opt.backend == Backend::Claude)
        {
            cmd += L" -p --output-format stream-json --verbose --include-partial-messages";
            cmd += L" --dangerously-skip-permissions --effort max";
            if (haveJb) cmd += L" --append-system-prompt-file \"" + jb + L"\"";
            if (!model.empty()) cmd += L" --model " + W(model);
            if (!resume.empty()) cmd += L" --resume " + W(resume);
            cmd += L" --input-format stream-json";
        }
        else
            cmd += L" acp";            // model and mode go through session/set_config_option instead

        // No folder chosen means the home folder (ACP needs an absolute path).
        std::wstring cwd = opt.cwd;
        if (cwd.empty())
        {
            wchar_t home[MAX_PATH];
            if (::GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) cwd = home;
        }

        // OpenCode/omp: drop a workspace opencode.jsonc that pins the GLM model
        // and loads jb.md if the folder has no config of its own.
        if ((opt.backend == Backend::OpenCode || opt.backend == Backend::Omp) && haveJb)
            EnsureOpenCodeConfig(cwd, jb);

        auto p = std::make_shared<Proc>();
        HANDLE outR, errR;
        if (!Spawn(exe, cmd, cwd, &p->inW, &outR, &errR, &p->proc, &p->job, err)) return false;
        g_p = p;
        std::thread(Reader, p, outR, false).detach();
        std::thread(Reader, p, errR, true).detach();

        g_opt = opt;
        g_opt.cwd = cwd;
        g_opt.model = model;
        g_opt.mode = mode;
        g_opt.resumeId = resume;
        g_s = State();
        g_s.backend = opt.backend;
        g_s.running = true;
        g_s.cwd = Narrow(cwd);
        g_stopping = false;
        g_rpcId = 0;
        g_idInit = g_idNew = g_idPrompt = g_idList = -1;
        g_configIds.clear();
        g_deleteIds.clear();
        g_effortId.clear();
        g_startupConfigs = 0;
        g_canLoad = g_loading = false;
        g_acpSession.clear();

        if (opt.backend == Backend::Claude)
        {
            g_s.models = ClaudeModels();
            g_s.modes = ClaudeModes();
            g_s.modelValue = model;
            g_s.modeValue = mode.empty() ? "default" : mode;
            if (!resume.empty())
            {
                LoadClaudeTranscript(cwd, resume);
                g_s.sessionId = resume;
            }
        }
        else
        {
            // The agent's own commands arrive in available_commands_update;
            // until then the popup offers the app's.
            g_s.commands.clear();
            AddLocalCommands(g_s.commands);
        }

        std::string who = BackendName(opt.backend);
        Notice((resume.empty() ? "starting " : "reopening a session with ") + who + " in " + Narrow(cwd), false);

        if (opt.backend == Backend::Claude)
        {
            char rid[48]; snprintf(rid, sizeof(rid), "req_%d_init", ++g_reqCounter);
            WriteLine("{\"type\":\"control_request\",\"request_id\":" + json::Quote(rid) +
                      ",\"request\":{\"subtype\":\"initialize\",\"hooks\":null}}");
        }
        else
        {
            // No file-system or terminal capabilities: the agent does its own I/O.
            g_idInit = Rpc("initialize", "{\"protocolVersion\":1,\"clientCapabilities\":{\"fs\":{\"readTextFile\":false,"
                                         "\"writeTextFile\":false},\"terminal\":false}}");
        }
        return true;
    }

    void SetModel(const std::string& value)
    {
        if (!g_p || value.empty() || value == g_s.modelValue) return;
        g_s.modelValue = value;
        if (g_s.backend == Backend::Claude)
        {
            char rid[48]; snprintf(rid, sizeof(rid), "req_%d_model", ++g_reqCounter);
            WriteLine("{\"type\":\"control_request\",\"request_id\":" + json::Quote(rid) +
                      ",\"request\":{\"subtype\":\"set_model\",\"model\":" + json::Quote(value) + "}}");
            g_s.model = value;
        }
        else if (!g_acpSession.empty())
            SetConfig("model", value);
    }

    void SetMode(const std::string& value)
    {
        if (!g_p || value.empty() || value == g_s.modeValue) return;
        g_s.modeValue = value;
        if (g_s.backend == Backend::Claude)
        {
            char rid[48]; snprintf(rid, sizeof(rid), "req_%d_mode", ++g_reqCounter);
            WriteLine("{\"type\":\"control_request\",\"request_id\":" + json::Quote(rid) +
                      ",\"request\":{\"subtype\":\"set_permission_mode\",\"mode\":" + json::Quote(value) + "}}");
        }
        else if (!g_acpSession.empty())
            SetConfig("mode", value);
    }

    void SetEffort(const std::string& value)
    {
        if (!g_p || value.empty() || value == g_s.effortValue) return;
        if (g_s.backend == Backend::Claude || g_acpSession.empty() || g_effortId.empty()) return;
        g_s.effortValue = value;
        SetConfig(g_effortId.c_str(), value);
    }

    void RefreshHistory()
    {
        if (g_s.backend == Backend::Claude)
        {
            std::wstring cwd = g_opt.cwd;
            if (cwd.empty()) return;
            ScanClaudeHistory(cwd);
        }
        else if (g_p && g_s.ready)
            g_idList = Rpc("session/list", "{\"cwd\":" + json::Quote(Narrow(g_opt.cwd)) + "}");
    }

    void DeleteSession(const std::string& id)
    {
        // The open session can't be deleted out from under the running agent.
        if (id.empty() || id == g_s.sessionId) return;

        // Session ids are UUIDs (Claude) or ses_... (OpenCode); anything else
        // is not one of ours, and never becomes part of a path.
        for (char c : id)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return;

        if (g_s.backend == Backend::Claude)
        {
            std::wstring dir = g_opt.cwd.empty() ? std::wstring() : ClaudeProjectDir(g_opt.cwd);
            if (dir.empty()) return;
            std::wstring path = dir + L"\\" + std::wstring(id.begin(), id.end()) + L".jsonl";
            if (!::DeleteFileW(path.c_str()))
            {
                Notice("couldn't delete that chat's transcript", true);
                return;
            }
        }
        else
        {
            // ACP: the agent owns its sessions (verified: OpenCode 2.0.32 answers
            // session/delete {sessionId} with {} and drops it from session/list).
            // The list is re-read when the reply lands, not before: a list
            // request sent alongside could still include the deleted session.
            if (!g_p || !g_s.ready) return;
            g_deleteIds.push_back(Rpc("session/delete", "{\"sessionId\":" + json::Quote(id) + "}"));
        }

        // Drop the row now so it vanishes without waiting for a refresh.
        for (auto it = g_s.history.begin(); it != g_s.history.end(); ++it)
            if (it->id == id) { g_s.history.erase(it); break; }
        g_s.historyRev++;
        g_s.revision++;
    }

    // Standard base64, no line breaks. Used for inlining image bytes into the
    // Claude / ACP content blocks.
    static std::string Base64(const unsigned char* data, size_t n)
    {
        static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((n + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < n; i += 3)
        {
            unsigned v = (unsigned)data[i] << 16 | (unsigned)data[i + 1] << 8 | (unsigned)data[i + 2];
            out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
            out += T[(v >> 6) & 63];  out += T[v & 63];
        }
        if (i < n)
        {
            unsigned v = (unsigned)data[i] << 16 | (i + 1 < n ? (unsigned)data[i + 1] << 8 : 0);
            out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
            out += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    void Send(const std::string& text) { Send(text, {}); }

    void Send(const std::string& text, const std::vector<Attachment>& atts)
    {
        if (!g_p || !g_s.ready || g_s.busy) return;
        if (text.empty() && atts.empty()) return;

        Message m;
        m.role = Message::User;
        m.text = text;
        m.attachments = atts;
        g_s.msgs.push_back(m);
        g_s.busy = true;
        g_s.revision++;

        if (g_s.backend == Backend::Claude)
        {
            // Claude SDK: user message content is either a plain string (old
            // path) or an array of blocks. Switch to the array shape as soon
            // as any attachment is present so images ride alongside the text.
            std::string content;
            if (atts.empty())
            {
                content = json::Quote(text);
            }
            else
            {
                content = "[";
                bool first = true;
                if (!text.empty())
                {
                    content += "{\"type\":\"text\",\"text\":" + json::Quote(text) + "}";
                    first = false;
                }
                for (const auto& a : atts)
                {
                    if (!first) content += ",";
                    first = false;
                    std::string mime = a.mime.empty() ? "image/png" : a.mime;
                    content += "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":" +
                               json::Quote(mime) + ",\"data\":\"" +
                               Base64(a.bytes.data(), a.bytes.size()) + "\"}}";
                }
                content += "]";
            }
            WriteLine("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":" + content +
                      "},\"parent_tool_use_id\":null,\"session_id\":\"default\"}");
        }
        else
        {
            // ACP: prompt is always an array of content blocks. Text first,
            // then one image block per attachment.
            std::string prompt = "[";
            bool first = true;
            if (!text.empty())
            {
                prompt += "{\"type\":\"text\",\"text\":" + json::Quote(text) + "}";
                first = false;
            }
            for (const auto& a : atts)
            {
                if (!first) prompt += ",";
                first = false;
                std::string mime = a.mime.empty() ? "image/png" : a.mime;
                prompt += "{\"type\":\"image\",\"mimeType\":" + json::Quote(mime) +
                          ",\"data\":\"" + Base64(a.bytes.data(), a.bytes.size()) + "\"}";
            }
            prompt += "]";
            g_idPrompt = Rpc("session/prompt", "{\"sessionId\":" + json::Quote(g_acpSession) +
                             ",\"prompt\":" + prompt + "}");
        }
    }

    void Interrupt()
    {
        if (!g_p || !g_s.busy) return;
        if (g_s.backend == Backend::Claude)
        {
            char rid[48]; snprintf(rid, sizeof(rid), "req_%d_interrupt", ++g_reqCounter);
            WriteLine("{\"type\":\"control_request\",\"request_id\":" + json::Quote(rid) +
                      ",\"request\":{\"subtype\":\"interrupt\"}}");
            return;
        }
        // ACP: answer any open permission request as cancelled, then cancel the turn.
        for (auto& msg : g_s.msgs)
            for (auto& b : msg.blocks)
                if (b.perm == Perm::Pending)
                {
                    WriteLine("{\"jsonrpc\":\"2.0\",\"id\":" + b.permId + ",\"result\":{\"outcome\":{\"outcome\":\"cancelled\"}}}");
                    b.perm = Perm::Denied;
                }
        WriteLine("{\"jsonrpc\":\"2.0\",\"method\":\"session/cancel\",\"params\":{\"sessionId\":" + json::Quote(g_acpSession) + "}}");
    }

    void Respond(const std::string& permId, int optionIndex)
    {
        Block* b = FindPerm(permId);
        if (!g_p || !b || b->perm != Perm::Pending) return;
        if (optionIndex < 0 || optionIndex >= (int)b->permOptions.size()) return;
        const PermOption& o = b->permOptions[optionIndex];
        bool allow = o.kind.compare(0, 5, "allow") == 0;

        if (g_s.backend == Backend::Claude)
        {
            std::string resp;
            if (allow)
            {
                std::string input = "{}";
                for (const auto& kv : g_permInputs)
                    if (kv.first == permId) { input.clear(); json::Write(kv.second, input); break; }
                resp = "{\"behavior\":\"allow\",\"updatedInput\":" + input + "}";
            }
            else
                resp = "{\"behavior\":\"deny\",\"message\":\"The user denied this request.\"}";
            WriteLine("{\"type\":\"control_response\",\"response\":{\"subtype\":\"success\",\"request_id\":" +
                      json::Quote(permId) + ",\"response\":" + resp + "}}");
        }
        else
            WriteLine("{\"jsonrpc\":\"2.0\",\"id\":" + permId + ",\"result\":{\"outcome\":{\"outcome\":\"selected\",\"optionId\":" +
                      json::Quote(o.id) + "}}}");

        b->perm = allow ? Perm::Allowed : Perm::Denied;
        if (!allow) b->expanded = false;
        g_s.revision++;
    }

    void Stop()
    {
        if (!g_p || g_stopping) return;
        // Both agents exit on stdin EOF; Pump() kills them if they linger.
        if (g_p->inW) { ::CloseHandle(g_p->inW); g_p->inW = nullptr; }
        g_stopping = true;
        g_stopTick = ::GetTickCount();
    }

    void Pump()
    {
        if (!g_p) return;

        std::deque<std::string> lines;
        std::string errTail;
        bool done;
        {
            std::lock_guard<std::mutex> lock(g_p->m);
            lines.swap(g_p->lines);
            done = g_p->outDone;
            errTail = g_p->errTail;
        }
        for (const auto& line : lines)
        {
            if (line.empty() || line[0] != '{') continue;
            json::Value v = json::Parse(line);
            if (!v.IsObject()) continue;
            if (g_s.backend == Backend::Claude) HandleClaude(v);
            else HandleAcp(v);
            g_s.revision++;
        }

        if (g_stopping && ::GetTickCount() - g_stopTick > 3000 && g_p->job)
            ::TerminateJobObject(g_p->job, 1);

        if (done && ::WaitForSingleObject(g_p->proc, 0) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            ::GetExitCodeProcess(g_p->proc, &code);
            g_s.running = false;
            g_s.ready = false;
            g_s.busy = false;
            g_s.thinking = false;
            if (!g_stopping)
            {
                // The last line on stderr is usually the reason.
                std::string why;
                size_t e = errTail.find_last_not_of("\r\n ");
                if (e != std::string::npos)
                {
                    size_t s = errTail.find_last_of('\n', e);
                    size_t from = (s == std::string::npos) ? 0 : s + 1;
                    why = errTail.substr(from, e - from + 1);
                }
                char buf[64];
                snprintf(buf, sizeof(buf), "%s exited (code %lu)", BackendName(g_s.backend), code);
                Notice(why.empty() ? std::string(buf) : std::string(buf) + ": " + why, code != 0);
            }
            g_s.revision++;
            Cleanup();
            g_stopping = false;
        }
    }
}
