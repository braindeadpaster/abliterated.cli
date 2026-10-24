//
// abliterated.cli, built on velo.
//
// Two views in one window:
//   - the launcher: title bar, two tool cards, an options panel that animates
//     between the tools, the launch button, and a typed status line
//   - the chat, for either agent: messages, tool cards with the agent's
//     permission choices, and a composer
// Launching grows the window into the chat (the host resizes it around its
// centre each frame from WindowSize()); Back shrinks it again. Once open, the
// chat can be resized by its edges (SetChatSize). Its sidebar lists the working
// folder and the folder's past sessions; the strip above the messages switches
// model and permission mode live; typing "/" in the composer lists the agent's
// slash commands.
//
// Most of it is hand-drawn with ImDrawList: this app's own furniture. The
// options panel uses velo pages and controls, so the tool switch gets velo's
// staggered transition for free.

#include "velo.h"
#include "ui.h"
#include "../chat/session.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ctype.h>

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <float.h>
#include <time.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace velo;

namespace
{
    // ---- links shown in the launch prompt — point these at your own
    const char* kRepoUrl    = "https://github.com/YOUR_NAME/abliterated.cli";
    const char* kProfileUrl = "https://github.com/YOUR_NAME";
    const char* kVersion    = "v1.1";

    // OpenCode's models come from `opencode models`, run on a worker thread the
    // first time the UI draws. The fetch state is heap-held and shared with the
    // thread, so closing the app mid-fetch never frees it under the thread.
    struct ModelFetch
    {
        std::mutex m;
        std::vector<std::string> list;
        bool done = false;
    };
    std::shared_ptr<ModelFetch> g_fetch;
    bool g_modelsReady = false;
    std::vector<std::string> g_models;          // [0] = "Default": OpenCode's own choice
    std::vector<const char*> g_modelPtrs;

    // Labels for velo dropdowns, rebuilt from chat::Choice lists.
    std::vector<const char*> Names(const std::vector<chat::Choice>& list)
    {
        std::vector<const char*> v;
        for (const auto& c : list) v.push_back(c.name.c_str());
        return v;
    }

    // Is each tool's binary installed? Checked once, on the first frame.
    bool g_installed[3] = { true, true, true };

    ui::Host     g_host;
    ui::Settings g_set;
    ImFont* g_body = nullptr;
    ImFont* g_mono = nullptr;
    ImFont* g_monoTitle = nullptr;
    ImFont* g_semi = nullptr;

    // ---- layout
    const float kBarH  = 40.0f;
    const float kPad   = 20.0f;
    const float kCardH = 92.0f;
    const float kBtnH  = 44.0f;
    const float kSideW = 248.0f;        // chat sidebar
    const ImVec2 kLauncherSize(560.0f, 404.0f);
    const ImVec2 kChatMin(780.0f, 500.0f);
    ImVec2 g_chatSize(960.0f, 680.0f);  // the user can resize the chat; persisted

    // Caption hit-test, recomputed each frame.
    float g_barBottom = 0.0f, g_barButtonsLeft = 0.0f;

    // ---- view switch: 0 = launcher, 1 = chat. Linear progress, eased where used.
    bool  g_chatMode = false;
    float g_expand = 0.0f;
    float g_viewAlpha = 1.0f;           // multiplies every Fade() while a view draws

    // ---- launch prompt
    bool  g_promptOpen = false;
    bool  g_wantSnapshot = false;
    float g_prompt = 0.0f;
    ImTextureID g_blur = 0;
    bool  g_openedStar = false, g_openedFollow = false;

    // ---- status line: typed out a character at a time
    char  g_status[192] = "ready";
    float g_statusT = 0.0f;
    bool  g_statusErr = false;

    // ---- chat
    int     g_chatTool = 0;             // the tool the open chat runs
    int     g_startCount = 0;           // bumps per Start(), to key per-session work
    char    g_input[8192] = "";
    bool    g_focusInput = false;
    bool    g_forceScroll = false;
    int     g_lastRev = -1;
    ImGuiID g_copiedId = 0;
    double  g_copiedAt = -10.0;

    // Composer text set from outside the input (slash completion, file
    // mentions). Written to the buffer directly and again through the input's
    // callback, because an active InputText owns its buffer until it lets go.
    std::string g_pending;
    bool        g_pendingSet = false;

    // Slash-command popup.
    int  g_slashSel = 0, g_slashTop = 0;
    bool g_slashDismissed = false;

    // Sidebar: 0 = files, 1 = chats.
    struct FileNode
    {
        std::wstring path;
        std::string name;
        bool dir = false, open = false, loaded = false;
        std::vector<FileNode> kids;
    };
    int      g_sideTab = 0;
    FileNode g_root;
    std::string g_rootFor;
    int  g_doneTools = -1;              // finished tool calls, to refresh after edits
    int  g_historyFor = -1;             // g_startCount the history was fetched for
    bool g_wasBusy = false;

    double g_boot = -1.0;

    // ================================================================ helpers
    float Dt() { return VMin(ImGui::GetIO().DeltaTime, 1.0f / 20.0f); }

    // Startup choreography: each element arrives `delayMs` after launch.
    float Enter(float delayMs, float durMs = 560.0f)
    {
        if (!theme().Animations) return 1.0f;
        float t = (float)(ImGui::GetTime() - g_boot) * 1000.0f - delayMs;
        return EaseF(Ease::OutQuint, Clamp01(t / durMs));
    }

    ImU32 Fade(ImU32 c, float a) { return WithAlpha(c, a * g_viewAlpha); }

    float TextW(ImFont* f, float size, const char* s, const char* e = nullptr)
    {
        return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s, e).x;
    }

    void Text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* s, const char* e = nullptr)
    {
        dl->AddText(f, size, ImVec2(floorf(p.x + 0.5f), floorf(p.y + 0.5f)), col, s, e);
    }

    // Wrapped text; returns the height it took.
    float Wrapped(ImDrawList* dl, ImFont* f, float size, ImVec2 p, float wrapW, ImU32 col, const char* s, const char* e = nullptr)
    {
        if (!s || s == e || !*s) return 0.0f;
        ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, wrapW, s, e);
        dl->AddText(f, size, ImVec2(floorf(p.x + 0.5f), floorf(p.y + 0.5f)), col, s, e, wrapW);
        return sz.y;
    }

    // Cut `s` with "..." so it fits `maxW`.
    std::string Ellipsize(ImFont* f, float size, const std::string& s, float maxW)
    {
        if (TextW(f, size, s.c_str()) <= maxW) return s;
        std::string out = s;
        while (!out.empty() && TextW(f, size, (out + "...").c_str()) > maxW) out.pop_back();
        return out + "...";
    }

    // Small uppercase monospace label — the CLI-ish section heading.
    void Caps(ImDrawList* dl, ImVec2 p, const char* s, float a)
    {
        Text(dl, g_mono, 11.0f, p, Fade(theme().TextInactive, a), s);
    }

    // A soft square blink for the block cursors.
    float Blink(float rate = 1.1f)
    {
        float ph = fmodf((float)ImGui::GetTime() * rate, 1.0f);
        float e = ph < 0.5f ? ph / 0.5f : (1.0f - ph) / 0.5f;
        return Clamp01((e - 0.25f) * 3.0f);
    }

    void Spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
    {
        float t = (float)ImGui::GetTime() * 7.0f;
        dl->PathArcTo(c, r, t, t + 4.4f, 18);
        dl->PathStroke(col, 0, 1.6f);
    }

    void SetStatus(bool err, const char* fmt, ...)
    {
        va_list ap; va_start(ap, fmt);
        vsnprintf(g_status, sizeof(g_status), fmt, ap);
        va_end(ap);
        g_statusErr = err;
        g_statusT = 0.0f;
    }

    std::wstring Widen(const char* s)
    {
        std::wstring w;
        // Plain UTF-8 -> UTF-16; enough for paths.
        for (const unsigned char* p = (const unsigned char*)s; *p; )
        {
            unsigned cp = *p++;
            if (cp >= 0xF0 && p[0] && p[1] && p[2]) { cp = ((cp & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
            else if (cp >= 0xE0 && p[0] && p[1])    { cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
            else if (cp >= 0xC0 && p[0])            { cp = ((cp & 31) << 6) | (p[0] & 63); p += 1; }
            if (cp >= 0x10000) { cp -= 0x10000; w += (wchar_t)(0xD800 + (cp >> 10)); w += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
            else w += (wchar_t)cp;
        }
        return w;
    }

    // ================================================================ launch
    // Once a frame: start the fetch, adopt its list when it lands, and keep the
    // saved model in the list even if this install no longer offers it.
    void SyncModels()
    {
        if (!g_fetch)
        {
            g_fetch = std::make_shared<ModelFetch>();
            std::shared_ptr<ModelFetch> f = g_fetch;
            std::thread([f] {
                std::vector<std::string> list;
                chat::ListOpenCodeModels(list);
                std::lock_guard<std::mutex> lock(f->m);
                f->list.swap(list);
                f->done = true;
            }).detach();
        }
        if (!g_modelsReady)
        {
            std::lock_guard<std::mutex> lock(g_fetch->m);
            if (g_fetch->done)
            {
                g_modelsReady = true;
                g_models.assign(1, "Default");
                g_models.insert(g_models.end(), g_fetch->list.begin(), g_fetch->list.end());
            }
        }
        if (g_models.empty()) g_models.push_back("Default");
        std::string saved = g_set.opencodeModel;
        if (!saved.empty() && std::find(g_models.begin(), g_models.end(), saved) == g_models.end())
            g_models.push_back(saved);
        g_modelPtrs.clear();
        for (const auto& m : g_models) g_modelPtrs.push_back(m.c_str());
    }

    chat::Backend ToolBackend(int tool)
    {
        return tool == ui::Tool_Claude ? chat::Backend::Claude : tool == ui::Tool_Omp ? chat::Backend::Omp : chat::Backend::OpenCode;
    }
    bool IsClaude() { return chat::Get().backend == chat::Backend::Claude; }
    const char* AgentName() { return chat::BackendName(chat::Get().backend); }
    const char* AgentCrumb()
    {
        chat::Backend b = chat::Get().backend;
        return b == chat::Backend::Claude ? "claude" : b == chat::Backend::OpenCode ? "opencode" : "oh-my-pi";
    }

    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    // Starts the chat for the launcher's tool, or restarts the open chat's tool
    // (New chat, or reopening a past session with `resume`).
    void StartChat(const std::string& resume = std::string())
    {
        int tool = g_chatMode ? g_chatTool : g_set.tool;
        chat::Options opt;
        opt.backend = ToolBackend(tool);
        opt.cwd = Widen(g_set.folder);
        opt.resumeId = resume;
        if (opt.backend == chat::Backend::Claude)
        {
            const auto& cm = chat::ClaudeModels();
            opt.model = cm[(size_t)VMax(0, VMin(g_set.claudeModel, (int)cm.size() - 1))].value;
            opt.mode = g_set.acceptEdits ? "acceptEdits" : "default";
        }
        else if (opt.backend == chat::Backend::OpenCode)
        {
            opt.model = g_set.opencodeModel;
            if (g_set.planMode) opt.mode = "plan";
        }
        std::string err;
        if (!chat::Start(opt, err)) { SetStatus(true, "error: %s", err.c_str()); return; }
        g_chatMode = true;
        g_chatTool = tool;
        g_startCount++;
        g_input[0] = 0;
        g_focusInput = true;
        g_lastRev = -1;
        g_forceScroll = true;
        g_rootFor.clear();
        SetStatus(false, "chat open");
    }

    // Live model / mode changes from the chat strip also update the launcher's
    // saved choice, so the next chat starts the same way.
    void ChooseModel(const std::string& value)
    {
        chat::SetModel(value);
        chat::Backend b = chat::Get().backend;
        if (b == chat::Backend::Claude)
        {
            const auto& cm = chat::ClaudeModels();
            for (size_t i = 0; i < cm.size(); i++) if (cm[i].value == value) g_set.claudeModel = (int)i;
        }
        else if (b == chat::Backend::OpenCode)
            snprintf(g_set.opencodeModel, sizeof(g_set.opencodeModel), "%s", value.c_str());
    }

    void ChooseMode(const std::string& value)
    {
        chat::SetMode(value);
        chat::Backend b = chat::Get().backend;
        if (b == chat::Backend::Claude) g_set.acceptEdits = (value == "acceptEdits");
        else if (b == chat::Backend::OpenCode) g_set.planMode = (value == "plan");
    }

    void DoLaunch() { StartChat(); }

    // ================================================================ title bar
    // A square icon button the height of the bar. Returns true on click.
    bool BarButton(const char* id, ImVec2 mn, ImVec2 mx, bool invertOnHover, float a, float* outHot)
    {
        const Theme& t = theme();
        ImGui::SetCursorScreenPos(mn);
        ImGui::InvisibleButton(id, ImVec2(mx.x - mn.x, mx.y - mn.y));
        bool hov = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        char hid[48]; snprintf(hid, sizeof(hid), "%s##hot", id);
        float hot = Anim(ImGui::GetID(hid), hov ? 1.0f : 0.0f, 16.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, Fade(invertOnHover ? t.Accent : t.ElementHover, a * hot));
        *outHot = hot;
        return clicked;
    }

    void TitleBar(ImDrawList* dl, ImVec2 o, float W)
    {
        const Theme& t = theme();
        float a = Enter(0.0f);

        float size = 15.0f;
        float lh = size;
        ImVec2 p(o.x + kPad, o.y + (kBarH - lh) * 0.5f - 1.0f - (1.0f - a) * 4.0f);

        Text(dl, g_monoTitle, size, p, Fade(t.Text, a), "abliterated");
        float x = p.x + TextW(g_monoTitle, size, "abliterated");
        Text(dl, g_monoTitle, size, ImVec2(x, p.y), Fade(t.TextInactive, a), ".cli");
        x += TextW(g_monoTitle, size, ".cli");

        // In the chat, a breadcrumb fades in after the name.
        float ce = EaseF(Ease::OutCubic, Clamp01((g_expand - 0.4f) / 0.6f));
        if (ce > 0.01f)
        {
            Text(dl, g_monoTitle, size, ImVec2(x + 8.0f - (1.0f - ce) * 6.0f, p.y), Fade(t.TextInactive, a * ce * 0.7f), "/");
            const char* crumb = AgentCrumb();
            Text(dl, g_monoTitle, size, ImVec2(x + 22.0f - (1.0f - ce) * 6.0f, p.y), Fade(t.Text, a * ce), crumb);
            x += 22.0f + TextW(g_monoTitle, size, crumb);
        }
        x += 5.0f;
        dl->AddRectFilled(ImVec2(x, p.y + 2.0f), ImVec2(x + 8.0f, p.y + lh + 1.0f),
                          Fade(t.Text, a * (0.15f + 0.85f * Blink())));

        // Window buttons: square, full bar height. Close inverts to white. In the
        // chat a Back button joins them on the left.
        const float bw = 40.0f;
        int count = g_chatMode ? 3 : 2;
        g_barButtonsLeft = o.x + W - bw * count;
        g_barBottom = o.y + kBarH;
        for (int i = 0; i < count; i++)
        {
            int kind = (count == 3) ? i : i + 1;       // 0 back, 1 minimise, 2 close
            ImVec2 mn(o.x + W - bw * (count - i), o.y), mx(mn.x + bw, o.y + kBarH - 1.0f);
            char id[24]; snprintf(id, sizeof(id), "##bar%d", kind);
            float hot = 0.0f;
            bool clicked = BarButton(id, mn, mx, kind == 2, a, &hot);
            ImU32 ic = (kind == 2) ? LerpCol(t.TextInactive, t.AccentText, hot) : LerpCol(t.TextInactive, t.Text, hot);
            ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
            if (kind == 2)
            {
                float r = 4.5f;
                dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), Fade(ic, a), 1.2f);
                dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), Fade(ic, a), 1.2f);
                if (clicked && g_host.Close) g_host.Close();
            }
            else if (kind == 1)
            {
                dl->AddLine(ImVec2(c.x - 5.0f, c.y + 0.5f), ImVec2(c.x + 5.0f, c.y + 0.5f), Fade(ic, a), 1.2f);
                if (clicked && g_host.Minimize) g_host.Minimize();
            }
            else
            {
                float nudge = -2.0f * hot;
                dl->AddLine(ImVec2(c.x - 6.0f + nudge, c.y), ImVec2(c.x + 6.0f, c.y), Fade(ic, a), 1.3f);
                dl->AddLine(ImVec2(c.x - 6.0f + nudge, c.y), ImVec2(c.x - 1.5f + nudge, c.y - 4.5f), Fade(ic, a), 1.3f);
                dl->AddLine(ImVec2(c.x - 6.0f + nudge, c.y), ImVec2(c.x - 1.5f + nudge, c.y + 4.5f), Fade(ic, a), 1.3f);
                if (clicked)
                {
                    chat::Stop();
                    g_chatMode = false;
                    SetStatus(false, "session ended");
                }
            }
        }

        dl->AddLine(ImVec2(o.x, o.y + kBarH - 0.5f), ImVec2(o.x + W, o.y + kBarH - 0.5f), Fade(t.Border, a));
    }

    // ================================================================ cards
    struct CardInfo { const char* name; const char* desc; Glyph icon; };
    const CardInfo kCards[3] = {
        { "OpenCode",   "open-source agent", Glyph::Code     },
        { "Claude CLI", "Anthropic's agent", Glyph::Terminal },
        { "oh-my-pi",   "fast pi harness",   Glyph::Zap      },
    };

    float ToolCards(ImDrawList* dl, ImVec2 o, float W, float y)
    {
        const Theme& t = theme();
        Caps(dl, ImVec2(o.x + kPad, y), "TOOL", Enter(80.0f));
        y += 18.0f;

        const float gap = 12.0f;
        float cw = (W - kPad * 2.0f - gap * 2.0f) / 3.0f;
        float selX = Anim(ImGui::GetID("##selx"), o.x + kPad + g_set.tool * (cw + gap), 12.0f);
        float selA = Enter(260.0f);

        for (int i = 0; i < 3; i++)
        {
            float a = Enter(140.0f + i * 70.0f);
            ImVec2 mn(o.x + kPad + i * (cw + gap), y + (1.0f - a) * 12.0f);
            ImVec2 mx(mn.x + cw, mn.y + kCardH);

            ImGui::SetCursorScreenPos(mn);
            ImGui::PushID(i);
            ImGui::InvisibleButton("##card", ImVec2(cw, kCardH));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && g_set.tool != i) g_set.tool = i;
            float hot = Anim(ImGui::GetID("##hot"), hov ? 1.0f : 0.0f, 14.0f);
            float sel = Anim(ImGui::GetID("##sel"), g_set.tool == i ? 1.0f : 0.0f, 12.0f);
            ImGui::PopID();

            float lift = hot * 2.0f;
            mn.y -= lift; mx.y -= lift;
            dl->AddRectFilled(mn, mx, Fade(LerpCol(t.Section, t.Element, hot * 0.7f), a));
            dl->AddRect(mn, mx, Fade(LerpCol(t.Border, Lighten(t.Border, 0.22f), hot), a));

            ImVec2 imn(mn.x + 16.0f, mn.y + 16.0f), imx(imn.x + 30.0f, imn.y + 30.0f);
            dl->AddRectFilled(imn, imx, Fade(LerpCol(t.Element, t.Accent, sel), a));
            dl->AddRect(imn, imx, Fade(LerpCol(t.Border, t.Accent, sel), a));
            DrawGlyph(dl, kCards[i].icon, ImVec2(imn.x + 7.0f, imn.y + 7.0f), ImVec2(imx.x - 7.0f, imx.y - 7.0f),
                      Fade(LerpCol(t.Text, t.AccentText, sel), a));

            Text(dl, g_body, 15.0f, ImVec2(mn.x + 16.0f, mn.y + 52.0f), Fade(t.Text, a), kCards[i].name);
            Text(dl, g_body, 12.5f, ImVec2(mn.x + 16.0f, mn.y + 70.0f), Fade(g_installed[i] ? t.TextInactive : t.Risky, a),
                 g_installed[i] ? kCards[i].desc : "not installed");

            ImVec2 rmn(mx.x - 30.0f, mn.y + 16.0f), rmx(rmn.x + 14.0f, rmn.y + 14.0f);
            dl->AddRect(rmn, rmx, Fade(LerpCol(t.Border, t.Accent, VMax(sel, hot * 0.5f)), a));
            float in = 7.0f * (1.0f - EaseF(Ease::OutBack, sel));
            if (sel > 0.01f)
                dl->AddRectFilled(ImVec2(rmn.x + 3.0f + in, rmn.y + 3.0f + in),
                                  ImVec2(rmx.x - 3.0f - in, rmx.y - 3.0f - in), Fade(t.Accent, a * sel));
        }

        dl->AddRect(ImVec2(selX, y), ImVec2(selX + cw, y + kCardH), Fade(t.Accent, selA));
        return y + kCardH;
    }

    // ================================================================ buttons
    // Outlined at rest. On hover a white fill wipes across from the left and the
    // label inverts exactly under it — drawn twice, clipped either side of the
    // wipe edge. A faint shine sweeps across every few seconds while idle.
    // `primary` starts filled; `enabled = false` dims it and ignores clicks.
    bool WipeButton(const char* id, ImVec2 mn, ImVec2 mx, const char* label, float a, bool primary,
                    Glyph icon = Glyph::None, bool arrow = false, float fontSize = 15.0f, bool enabled = true)
    {
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::SetCursorScreenPos(mn);
        ImGui::InvisibleButton(id, ImVec2(VMax(1.0f, mx.x - mn.x), VMax(1.0f, mx.y - mn.y)));
        bool hov = enabled && ImGui::IsItemHovered();
        bool held = enabled && ImGui::IsItemActive();
        bool clicked = enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
        char hid[64]; snprintf(hid, sizeof(hid), "%s##hot", id);
        char pid[64]; snprintf(pid, sizeof(pid), "%s##press", id);
        float hot = Anim(ImGui::GetID(hid), hov ? 1.0f : 0.0f, 10.0f);
        float press = Anim(ImGui::GetID(pid), held ? 1.0f : 0.0f, 26.0f);
        if (!enabled) a *= 0.4f;

        float sh = press * 1.5f;
        mn.x += sh; mn.y += sh; mx.x -= sh; mx.y -= sh;

        float fill = primary ? 1.0f : EaseF(Ease::OutCubic, hot);
        float wipeX = mn.x + (mx.x - mn.x) * fill;

        ImU32 border = primary ? t.Accent : LerpCol(LerpCol(t.Border, t.Accent, 0.45f), t.Accent, hot);
        if (fill > 0.0f)
            dl->AddRectFilled(mn, ImVec2(wipeX, mx.y), Fade(primary ? LerpCol(t.Accent, Darken(t.Accent, 0.12f), hot) : t.Accent, a));
        dl->AddRect(mn, mx, Fade(border, a));

        if (!primary && enabled && hot < 0.05f && t.Animations)
        {
            float ph = fmodf((float)ImGui::GetTime(), 4.5f) / 0.9f;
            if (ph < 1.0f)
            {
                float w = 70.0f, cx = mn.x - w + (mx.x - mn.x + w * 2.0f) * EaseF(Ease::InOutCubic, ph);
                ImU32 c0 = IM_COL32(255, 255, 255, 0), c1 = Fade(IM_COL32(255, 255, 255, 26), a);
                dl->PushClipRect(mn, mx, true);
                dl->AddRectFilledMultiColor(ImVec2(cx - w * 0.5f, mn.y), ImVec2(cx, mx.y), c0, c1, c1, c0);
                dl->AddRectFilledMultiColor(ImVec2(cx, mn.y), ImVec2(cx + w * 0.5f, mx.y), c1, c0, c0, c1);
                dl->PopClipRect();
            }
        }

        float iconSz = (icon != Glyph::None) ? fontSize + 1.0f : 0.0f;
        float lw = TextW(g_body, fontSize, label);
        float arrowW = arrow ? 16.0f : 0.0f;
        float total = iconSz + (iconSz > 0 && label[0] ? 8.0f : 0.0f) + lw + (arrow ? 10.0f + arrowW : 0.0f);
        float x = (mn.x + mx.x) * 0.5f - total * 0.5f;
        float cy = (mn.y + mx.y) * 0.5f;
        float nudge = 4.0f * EaseF(Ease::OutCubic, hot);

        for (int pass = 0; pass < 2; pass++)
        {
            ImU32 col = pass == 0 ? t.Text : t.AccentText;
            ImVec2 cmn = pass == 0 ? ImVec2(wipeX, mn.y) : mn;
            ImVec2 cmx = pass == 0 ? mx : ImVec2(wipeX, mx.y);
            if (cmx.x <= cmn.x) continue;
            dl->PushClipRect(cmn, cmx, true);
            float px = x;
            if (iconSz > 0.0f)
            {
                DrawGlyph(dl, icon, ImVec2(px, cy - iconSz * 0.5f), ImVec2(px + iconSz, cy + iconSz * 0.5f), Fade(col, a));
                px += iconSz + (label[0] ? 8.0f : 0.0f);
            }
            Text(dl, g_body, fontSize, ImVec2(px, cy - fontSize * 0.5f - 1.0f), Fade(col, a), label);
            px += lw + 10.0f;
            if (arrow)
            {
                float ax = px + nudge, ay = cy;
                dl->AddLine(ImVec2(ax, ay), ImVec2(ax + arrowW, ay), Fade(col, a), 1.4f);
                dl->AddLine(ImVec2(ax + arrowW - 5.0f, ay - 5.0f), ImVec2(ax + arrowW, ay), Fade(col, a), 1.4f);
                dl->AddLine(ImVec2(ax + arrowW - 5.0f, ay + 5.0f), ImVec2(ax + arrowW, ay), Fade(col, a), 1.4f);
            }
            dl->PopClipRect();
        }
        return clicked;
    }

    // ================================================================ options
    PageOpts OptionsPage()
    {
        PageOpts o;
        o.Transition = PageTransition::FadeSlide;
        o.SlideFrom = SlideOrigin::Auto;
        o.SlideDistance = 16.0f;
        o.DurationMs = 340.0f;
        o.FadeOutMs = 120.0f;
        o.StaggerMs = 40.0f;
        o.Curve = Ease::OutQuint;
        return o;
    }

    void PageCaps(const char* s)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Caps(dl, ImGui::GetCursorScreenPos(), s, PageFade());
        ImGui::Dummy(ImVec2(1.0f, 16.0f));
    }

    // Working folder: the path in a box, Browse beside it.
    void FolderRow(float w)
    {
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float a = PageFade();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = 30.0f, bw = 92.0f;
        ImVec2 mn = p, mx(p.x + w - bw - 8.0f, p.y + h);
        dl->AddRectFilled(mn, mx, Fade(t.Element, a));
        dl->AddRect(mn, mx, Fade(t.Border, a));
        std::string shown = g_set.folder[0] ? Ellipsize(g_mono, 12.5f, g_set.folder, mx.x - mn.x - 20.0f) : "(home folder)";
        Text(dl, g_mono, 12.5f, ImVec2(mn.x + 10.0f, mn.y + 8.0f), Fade(g_set.folder[0] ? t.Text : t.TextInactive, a), shown.c_str());
        if (WipeButton("##browse", ImVec2(mx.x + 8.0f, p.y), ImVec2(p.x + w, p.y + h), "Browse", a, false,
                       Glyph::Folder, false, 13.0f) && g_host.PickFolder)
        {
            char out[520];
            if (g_host.PickFolder(out, (int)sizeof(out)))
                snprintf(g_set.folder, sizeof(g_set.folder), "%s", out);
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(w, h));
    }

    float Options(ImVec2 o, float W, float y)
    {
        const Theme& t = theme();
        float a = Enter(320.0f) * g_viewAlpha;
        float cw = W - kPad * 2.0f;
        const float h = 100.0f;    // each page: 16 + 30 + 4 + 30, plus 3 x 6px item spacing
        ImGui::SetCursorScreenPos(ImVec2(o.x + kPad, y + (1.0f - a) * 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
        ImGui::BeginChild("##options", ImVec2(cw, h), 0,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        float half = (cw - 12.0f) * 0.5f;
        if (BeginPage("p_opencode", ui::Tool_OpenCode, g_set.tool, OptionsPage()))
        {
            PageCaps("WORKING FOLDER");
            FolderRow(cw);
            ImGui::Dummy(ImVec2(1.0f, 4.0f));
            ImGui::BeginChild("##om", ImVec2(half, 30.0f), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            int idx = 0;
            for (int i = 1; i < (int)g_models.size(); i++)
                if (g_models[i] == g_set.opencodeModel) idx = i;
            if (Dropdown("##ocmodel", &idx, g_modelPtrs.data(), (int)g_modelPtrs.size()))
                snprintf(g_set.opencodeModel, sizeof(g_set.opencodeModel), "%s", idx > 0 ? g_models[idx].c_str() : "");
            ImGui::EndChild();
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::BeginChild("##pm", ImVec2(half, 30.0f), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            ImGui::SetCursorPosY(5.0f);
            Checkbox(g_modelsReady ? "Plan mode (read-only)" : "Plan mode (loading models)", &g_set.planMode);
            ImGui::EndChild();
            EndPage();
        }
        if (BeginPage("p_claude", ui::Tool_Claude, g_set.tool, OptionsPage()))
        {
            PageCaps("WORKING FOLDER");
            FolderRow(cw);
            ImGui::Dummy(ImVec2(1.0f, 4.0f));
            ImGui::BeginChild("##cm", ImVec2(half, 30.0f), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            std::vector<const char*> cm = Names(chat::ClaudeModels());
            Dropdown("##claudemodel", &g_set.claudeModel, cm.data(), (int)cm.size());
            ImGui::EndChild();
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::BeginChild("##ae", ImVec2(half, 30.0f), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            ImGui::SetCursorPosY(5.0f);
            Checkbox("Auto-accept edits", &g_set.acceptEdits);
            ImGui::EndChild();
            EndPage();
        }
        if (BeginPage("p_omp", ui::Tool_Omp, g_set.tool, OptionsPage()))
        {
            PageCaps("WORKING FOLDER");
            FolderRow(cw);
            ImGui::Dummy(ImVec2(1.0f, 4.0f));
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            if (g_installed[ui::Tool_Omp])
                Text(pdl, g_mono, 12.0f, ImVec2(p.x, p.y + 8.0f), Fade(t.TextInactive, PageFade()),
                     "model and mode are picked inside the chat");
            else
                Text(pdl, g_mono, 12.0f, ImVec2(p.x, p.y + 8.0f), Fade(t.Risky, PageFade()),
                     "not installed:  bun install -g @oh-my-pi/pi-coding-agent");
            ImGui::Dummy(ImVec2(cw, 30.0f));
            EndPage();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        (void)t;
        return y + h;
    }

    // ================================================================ status line
    void StatusLine(ImDrawList* dl, ImVec2 o, float W, float y)
    {
        const Theme& t = theme();
        float a = Enter(440.0f);
        g_statusT += Dt();

        int len = (int)strlen(g_status);
        int shown = t.Animations ? VMin(len, (int)(g_statusT * 70.0f)) : len;
        char buf[192]; memcpy(buf, g_status, shown); buf[shown] = 0;

        float size = 12.0f;
        float x = o.x + kPad;
        Text(dl, g_mono, size, ImVec2(x, y), Fade(t.TextInactive, a * 0.7f), ">");
        x += TextW(g_mono, size, "> ");
        Text(dl, g_mono, size, ImVec2(x, y), Fade(g_statusErr ? t.Risky : t.TextInactive, a), buf);
        x += TextW(g_mono, size, buf) + 2.0f;
        dl->AddRectFilled(ImVec2(x, y + 2.0f), ImVec2(x + 6.0f, y + size + 1.0f),
                          Fade(t.TextInactive, a * (0.1f + 0.7f * Blink(1.4f))));

        float vw = TextW(g_mono, size, kVersion);
        Text(dl, g_mono, size, ImVec2(o.x + W - kPad - vw, y), Fade(t.TextInactive, a * 0.6f), kVersion);
    }

    // The launcher body, laid out in a 560x404 box centred in the window so it
    // stays put while the window grows around it.
    void LauncherView(ImDrawList* dl, ImVec2 wo, float W, float H)
    {
        ImVec2 o(floorf(wo.x + (W - kLauncherSize.x) * 0.5f), floorf(wo.y + (H - kLauncherSize.y) * 0.5f));
        float LW = kLauncherSize.x, LH = kLauncherSize.y;

        float y = ToolCards(dl, o, LW, o.y + kBarH + 18.0f);
        Options(o, LW, y + 20.0f);

        float statusY = o.y + LH - 30.0f;
        ImVec2 bmn(o.x + kPad, statusY - 16.0f - kBtnH), bmx(o.x + LW - kPad, statusY - 16.0f);
        float ba = Enter(380.0f);
        bmn.y += (1.0f - ba) * 10.0f; bmx.y += (1.0f - ba) * 10.0f;
        char label[64];
        snprintf(label, sizeof(label), "Launch %s", kCards[g_set.tool].name);
        if (WipeButton("##launch", bmn, bmx, label, ba, false, Glyph::None, true) && !g_chatMode)
        {
            if (g_set.skipPrompt) DoLaunch();
            else
            {
                g_promptOpen = true;
                g_wantSnapshot = true;
                g_openedStar = g_openedFollow = false;
            }
        }
        StatusLine(dl, o, LW, statusY);
    }

    // ================================================================ chat: content
    Glyph ToolGlyph(const std::string& n)
    {
        if (n == "Bash" || n == "PowerShell") return Glyph::Terminal;
        if (n == "Read") return Glyph::Eye;
        if (n == "Write" || n == "Edit" || n == "MultiEdit" || n == "NotebookEdit") return Glyph::Code;
        if (n == "Glob" || n == "Grep") return Glyph::Search;
        if (n == "Patch") return Glyph::Code;
        if (n == "List") return Glyph::Folder;
        if (n == "WebFetch" || n == "WebSearch" || n == "Webfetch" || n == "Websearch") return Glyph::Globe;
        if (n == "Task" || n == "Agent") return Glyph::Users;
        if (n == "TodoWrite" || n == "Todowrite" || n == "Todoread") return Glyph::List;
        return Glyph::Wrench;
    }

    // Markdown-lite: headings, bullets, quotes and fenced code blocks. Inline
    // markers (**bold**, `code`) are stripped rather than styled.
    std::string CleanInline(const std::string& s)
    {
        std::string o;
        o.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '`') continue;
            if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] == s[i]) { i++; continue; }
            o += s[i];
        }
        return o;
    }

    float CodeBlock(ImDrawList* dl, ImVec2 p, float w, const std::string& code, float a, ImGuiID id)
    {
        const Theme& t = theme();
        const float pad = 12.0f, size = 12.5f;
        float th = g_mono->CalcTextSizeA(size, FLT_MAX, w - pad * 2.0f, code.c_str()).y;
        float h = th + pad * 2.0f;
        ImVec2 mn = p, mx(p.x + w, p.y + h);
        dl->AddRectFilled(mn, mx, Fade(t.Element, a));
        dl->AddRect(mn, mx, Fade(t.Border, a));
        Wrapped(dl, g_mono, size, ImVec2(mn.x + pad, mn.y + pad), w - pad * 2.0f, Fade(t.Text, a), code.c_str());

        // Copy, top right.
        bool copied = (g_copiedId == id) && (ImGui::GetTime() - g_copiedAt < 1.4);
        const char* lbl = copied ? "copied" : "copy";
        float lw = TextW(g_mono, 11.0f, lbl);
        ImVec2 bmn(mx.x - lw - 16.0f, mn.y + 6.0f), bmx(mx.x - 6.0f, mn.y + 24.0f);
        ImGui::SetCursorScreenPos(bmn);
        ImGui::PushID((int)id);
        ImGui::InvisibleButton("##copy", ImVec2(bmx.x - bmn.x, bmx.y - bmn.y));
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { ImGui::SetClipboardText(code.c_str()); g_copiedId = id; g_copiedAt = ImGui::GetTime(); }
        ImGui::PopID();
        dl->AddRectFilled(bmn, bmx, Fade(hov ? t.ElementHover : t.Section, a));
        Text(dl, g_mono, 11.0f, ImVec2(bmn.x + 5.0f, bmn.y + 2.0f), Fade(hov || copied ? t.Text : t.TextInactive, a), lbl);
        return h;
    }

    float Prose(ImDrawList* dl, ImVec2 p, float w, const std::string& text, float a, ImGuiID baseId)
    {
        const Theme& t = theme();
        float y = p.y;
        const float size = 14.0f, lineGap = 5.0f;
        size_t i = 0;
        int codeIndex = 0;
        bool first = true;
        while (i <= text.size())
        {
            size_t nl = text.find('\n', i);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(i, nl - i);
            i = nl + 1;

            if (line.compare(0, 3, "```") == 0)
            {
                // Collect until the closing fence (or the end, mid-stream).
                std::string code;
                while (i <= text.size())
                {
                    size_t n2 = text.find('\n', i);
                    if (n2 == std::string::npos) n2 = text.size();
                    std::string l2 = text.substr(i, n2 - i);
                    i = n2 + 1;
                    if (l2.compare(0, 3, "```") == 0) break;
                    if (!code.empty()) code += '\n';
                    code += l2;
                    if (n2 >= text.size()) break;
                }
                if (!first) y += 4.0f;
                y += CodeBlock(dl, ImVec2(p.x, y), w, code, a, baseId * 31u + (ImGuiID)(codeIndex++)) + 8.0f;
                first = false;
                continue;
            }

            if (line.empty()) { if (!first) y += 6.0f; if (nl >= text.size()) break; continue; }

            ImFont* f = g_body;
            float sz = size;
            float indent = 0.0f;
            ImU32 col = Fade(t.Text, a);
            std::string body = line;

            size_t hashes = 0;
            while (hashes < body.size() && body[hashes] == '#') hashes++;
            if (hashes > 0 && hashes <= 4 && hashes < body.size() && body[hashes] == ' ')
            {
                f = g_semi; sz = hashes == 1 ? 18.0f : hashes == 2 ? 16.0f : 15.0f;
                body = body.substr(hashes + 1);
                if (!first) y += 6.0f;
            }
            else if (body.size() > 2 && (body[0] == '-' || body[0] == '*') && body[1] == ' ')
            {
                Text(dl, g_body, size, ImVec2(p.x + 4.0f, y), col, "\xE2\x80\xA2");
                indent = 18.0f;
                body = body.substr(2);
            }
            else if (body.size() > 2 && body[0] == '>' && body[1] == ' ')
            {
                body = body.substr(2);
                indent = 14.0f;
                col = Fade(t.TextInactive, a);
            }
            else
            {
                // "12. " numbered lists keep their number, hung like a bullet.
                size_t d = 0;
                while (d < body.size() && body[d] >= '0' && body[d] <= '9') d++;
                if (d > 0 && d + 1 < body.size() && body[d] == '.' && body[d + 1] == ' ')
                {
                    std::string num = body.substr(0, d + 1);
                    Text(dl, g_body, size, ImVec2(p.x, y), Fade(t.TextInactive, a), num.c_str());
                    indent = 22.0f;
                    body = body.substr(d + 2);
                }
            }

            std::string clean = CleanInline(body);
            float h = Wrapped(dl, f, sz, ImVec2(p.x + indent, y), w - indent, col, clean.c_str());
            if (indent == 14.0f) dl->AddRectFilled(ImVec2(p.x + 2.0f, y), ImVec2(p.x + 4.0f, y + h), Fade(t.Border, a));
            y += h + lineGap;
            first = false;
            if (nl >= text.size()) break;
        }
        return y - p.y;
    }

    // Tool call card: one line collapsed; expands to show the input and result,
    // and opens itself with the agent's choices when it asks permission.
    float ToolCard(ImDrawList* dl, ImVec2 p, float w, chat::Block& b, float a, ImGuiID id)
    {
        const Theme& t = theme();
        const float headH = 34.0f;
        bool pending = (b.perm == chat::Perm::Pending);
        ImGui::PushID((int)id);

        // Header (click to expand).
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("##hdr", ImVec2(w, headH));
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) b.expanded = !b.expanded;
        float hot = Anim(ImGui::GetID("##hot"), hov ? 1.0f : 0.0f, 16.0f);
        float ex = Anim(ImGui::GetID("##ex"), b.expanded ? 1.0f : 0.0f, 14.0f);

        // Body content, measured first so the card can animate open to it.
        const float pad = 12.0f, csz = 12.0f;
        float innerW = w - pad * 2.0f;
        std::string detail = b.detail.empty() ? b.summary : b.detail;
        float dh = detail.empty() ? 0.0f : g_mono->CalcTextSizeA(csz, FLT_MAX, innerW, detail.c_str()).y;
        float rh = b.result.empty() ? 0.0f : g_mono->CalcTextSizeA(csz, FLT_MAX, innerW, b.result.c_str()).y;
        const float maxPart = 260.0f;
        dh = VMin(dh, maxPart); rh = VMin(rh, maxPart);
        float permH = pending ? 78.0f : 0.0f;
        float bodyH = pad + (dh > 0 ? dh + 10.0f : 0.0f) + (rh > 0 ? 16.0f + rh + 10.0f : 0.0f) + permH;
        float h = headH + bodyH * EaseF(Ease::OutCubic, ex);

        ImVec2 mn = p, mx(p.x + w, p.y + h);
        float pulse = pending ? 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.0f) : 0.0f;
        dl->AddRectFilled(mn, mx, Fade(LerpCol(t.Section, t.Element, hot * 0.6f), a));
        ImU32 border = pending ? LerpCol(Lighten(t.Border, 0.3f), t.Accent, pulse) : LerpCol(t.Border, Lighten(t.Border, 0.2f), hot);
        dl->AddRect(mn, mx, Fade(border, a));

        // Status mark.
        ImVec2 sc(mn.x + 17.0f, mn.y + headH * 0.5f);
        if (pending)                DrawGlyph(dl, Glyph::Info, ImVec2(sc.x - 7, sc.y - 7), ImVec2(sc.x + 7, sc.y + 7), Fade(t.Accent, a));
        else if (!b.done)           Spinner(dl, sc, 5.5f, Fade(t.Text, a));
        else if (b.error)           DrawGlyph(dl, Glyph::Cross, ImVec2(sc.x - 6, sc.y - 6), ImVec2(sc.x + 6, sc.y + 6), Fade(t.Risky, a));
        else                        DrawGlyph(dl, Glyph::Check, ImVec2(sc.x - 6, sc.y - 6), ImVec2(sc.x + 6, sc.y + 6), Fade(t.TextInactive, a));

        float x = mn.x + 34.0f;
        DrawGlyph(dl, ToolGlyph(b.toolName), ImVec2(x, sc.y - 7.0f), ImVec2(x + 14.0f, sc.y + 7.0f), Fade(t.TextInactive, a));
        x += 22.0f;
        Text(dl, g_body, 13.5f, ImVec2(x, sc.y - 8.0f), Fade(t.Text, a), b.toolName.c_str());
        x += TextW(g_body, 13.5f, b.toolName.c_str()) + 10.0f;

        const char* tag = b.perm == chat::Perm::Allowed ? "allowed" : b.perm == chat::Perm::Denied ? "denied" : "";
        float tagW = tag[0] ? TextW(g_mono, 11.0f, tag) + 12.0f : 0.0f;
        float chevX = mx.x - 20.0f;
        std::string sum = Ellipsize(g_mono, 12.0f, b.summary, chevX - 10.0f - tagW - x);
        Text(dl, g_mono, 12.0f, ImVec2(x, sc.y - 7.0f), Fade(t.TextInactive, a), sum.c_str());
        if (tag[0])
            Text(dl, g_mono, 11.0f, ImVec2(chevX - 10.0f - tagW + 6.0f, sc.y - 6.0f),
                 Fade(b.perm == chat::Perm::Denied ? t.Risky : t.TextInactive, a), tag);

        // Chevron: right when closed, down when open.
        float ang = ex * 1.5707963f;
        auto R = [&](float lx, float ly) { return ImVec2(chevX + lx * cosf(ang) - ly * sinf(ang), sc.y + lx * sinf(ang) + ly * cosf(ang)); };
        dl->AddLine(R(-2.5f, -4.5f), R(2.5f, 0.0f), Fade(t.TextInactive, a), 1.3f);
        dl->AddLine(R(2.5f, 0.0f), R(-2.5f, 4.5f), Fade(t.TextInactive, a), 1.3f);

        // Body, revealed as the card opens.
        if (ex > 0.01f)
        {
            ImVec2 bmn(mn.x + 1.0f, mn.y + headH), bmx(mx.x - 1.0f, mx.y - 1.0f);
            dl->PushClipRect(bmn, bmx, true);
            float by = mn.y + headH;
            dl->AddLine(ImVec2(mn.x, by), ImVec2(mx.x, by), Fade(t.Border, a));
            by += pad;
            float ba = a * Clamp01(ex * 1.4f - 0.2f);
            if (dh > 0)
            {
                dl->PushClipRect(ImVec2(mn.x, by), ImVec2(mx.x, by + dh), true);
                Wrapped(dl, g_mono, csz, ImVec2(mn.x + pad, by), innerW, Fade(t.TextInactive, ba), detail.c_str());
                dl->PopClipRect();
                by += dh + 10.0f;
            }
            if (rh > 0)
            {
                Caps(dl, ImVec2(mn.x + pad, by), b.error ? "ERROR" : "RESULT", ba);
                by += 16.0f;
                dl->PushClipRect(ImVec2(mn.x, by), ImVec2(mx.x, by + rh), true);
                Wrapped(dl, g_mono, csz, ImVec2(mn.x + pad, by), innerW, Fade(b.error ? t.Risky : t.Text, ba), b.result.c_str());
                dl->PopClipRect();
                by += rh + 10.0f;
            }
            dl->PopClipRect();

            if (pending && ex > 0.6f)
            {
                char q[160];
                snprintf(q, sizeof(q), "Allow %s to use %s?", AgentName(), b.toolName.c_str());
                Text(dl, g_body, 13.5f, ImVec2(mn.x + pad, by), Fade(t.Text, ba), q);
                if (!b.permText.empty())
                {
                    std::string pt = Ellipsize(g_body, 12.5f, b.permText, innerW);
                    Text(dl, g_body, 12.5f, ImVec2(mn.x + pad, by + 18.0f), Fade(t.TextInactive, ba), pt.c_str());
                }
                // One button per choice the agent offers; the first "allow" is primary.
                const float bh2 = 28.0f, byy = by + 40.0f;
                float bx = mn.x + pad;
                bool primaryUsed = false;
                int picked = -1;
                for (int oi = 0; oi < (int)b.permOptions.size(); oi++)
                {
                    const chat::PermOption& po = b.permOptions[oi];
                    bool allowKind = po.kind.compare(0, 5, "allow") == 0;
                    bool primary = allowKind && !primaryUsed;
                    primaryUsed |= primary;
                    float bw = VMax(84.0f, TextW(g_body, 13.0f, po.name.c_str()) + 32.0f);
                    char bid[24]; snprintf(bid, sizeof(bid), "##perm%d", oi);
                    if (WipeButton(bid, ImVec2(bx, byy), ImVec2(bx + bw, byy + bh2), po.name.c_str(), ba, primary,
                                   Glyph::None, false, 13.0f))
                        picked = oi;
                    bx += bw + 8.0f;
                }
                if (picked >= 0) chat::Respond(b.permId, picked);
            }
        }

        ImGui::PopID();
        return h;
    }

    // "working" / "thinking" with three dots that rise in turn.
    float Working(ImDrawList* dl, ImVec2 p, float a, bool thinking)
    {
        const Theme& t = theme();
        for (int i = 0; i < 3; i++)
        {
            float ph = (float)ImGui::GetTime() * 5.0f - i * 0.7f;
            float k = 0.5f + 0.5f * sinf(ph);
            dl->AddRectFilled(ImVec2(p.x + i * 9.0f, p.y + 7.0f - k * 3.0f), ImVec2(p.x + i * 9.0f + 5.0f, p.y + 12.0f - k * 3.0f),
                              Fade(t.Text, a * (0.35f + 0.65f * k)));
        }
        Text(dl, g_mono, 12.0f, ImVec2(p.x + 34.0f, p.y + 2.0f), Fade(t.TextInactive, a), thinking ? "thinking" : "working");
        return 22.0f;
    }

    void Messages(ImVec2 mn, ImVec2 mx, float viewA)
    {
        const Theme& t = theme();
        chat::State& st = chat::Get();

        ImGui::SetCursorScreenPos(mn);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(t.Border));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(Lighten(t.Border, 0.2f)));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImGui::ColorConvertU32ToFloat4(t.TextInactive));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::BeginChild("##msgs", ImVec2(mx.x - mn.x, mx.y - mn.y), 0, ImGuiWindowFlags_NoBackground);
        bool stick = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 30.0f;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x - 14.0f;     // room for the scrollbar
        ImVec2 o = ImGui::GetCursorScreenPos();
        float dt = Dt();

        ImGui::Dummy(ImVec2(w, 6.0f));
        for (size_t mi = 0; mi < st.msgs.size(); mi++)
        {
            chat::Message& m = st.msgs[mi];
            m.appear = theme().Animations ? Approach(m.appear, 1.0f, 9.0f, dt) : 1.0f;
            float e = EaseF(Ease::OutCubic, m.appear);
            float a = e * viewA;
            ImVec2 p = ImGui::GetCursorScreenPos();
            p.y += (1.0f - e) * 10.0f;
            float h = 0.0f;

            if (m.role == chat::Message::User)
            {
                const float pad = 12.0f, size = 14.0f;
                float maxW = w * 0.72f;
                ImVec2 ts = g_body->CalcTextSizeA(size, FLT_MAX, maxW - pad * 2.0f, m.text.c_str());
                float bw = ts.x + pad * 2.0f, bh = ts.y + pad * 2.0f - 2.0f;
                ImVec2 bmn(o.x + w - bw, p.y), bmx(o.x + w, p.y + bh);
                dl->AddRectFilled(bmn, bmx, Fade(t.Element, a));
                dl->AddRect(bmn, bmx, Fade(Lighten(t.Border, 0.15f), a));
                Wrapped(dl, g_body, size, ImVec2(bmn.x + pad, bmn.y + pad - 1.0f), maxW - pad * 2.0f, Fade(t.Text, a), m.text.c_str());
                h = bh;
            }
            else if (m.role == chat::Message::Notice)
            {
                float tw = TextW(g_mono, 11.5f, m.text.c_str());
                if (tw > w) tw = w;
                float x = o.x + (w - tw) * 0.5f;
                h = Wrapped(dl, g_mono, 11.5f, ImVec2(x, p.y), w, Fade(m.error ? t.Risky : t.TextInactive, a), m.text.c_str());
            }
            else
            {
                // Assistant turn: a small label, then its blocks in order.
                std::string who = AgentName();
                for (auto& c : who) c = (char)toupper((unsigned char)c);
                Caps(dl, ImVec2(o.x, p.y), who.c_str(), a);
                float y = p.y + 18.0f;
                for (size_t bi = 0; bi < m.blocks.size(); bi++)
                {
                    chat::Block& b = m.blocks[bi];
                    ImGuiID id = (ImGuiID)(mi * 131 + bi + 1);
                    if (b.kind == chat::Block::Text)
                    {
                        if (b.text.empty()) continue;
                        y += Prose(dl, ImVec2(o.x, y), w, b.text, a, id) + 4.0f;
                    }
                    else if (b.kind == chat::Block::Tool)
                        y += ToolCard(dl, ImVec2(o.x, y), w, b, a, id) + 8.0f;
                }
                h = y - p.y;
            }

            ImGui::SetCursorScreenPos(ImVec2(o.x, p.y));
            ImGui::Dummy(ImVec2(w, h + 16.0f));
        }

        // Working indicator, while nothing is visibly streaming.
        if (st.busy)
        {
            bool streamingText = !st.msgs.empty() && st.msgs.back().role == chat::Message::Assistant &&
                                 !st.msgs.back().blocks.empty() && st.msgs.back().blocks.back().kind == chat::Block::Text &&
                                 !st.thinking;
            if (!streamingText)
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                float h = Working(dl, p, viewA, st.thinking);
                ImGui::Dummy(ImVec2(w, h + 10.0f));
            }
        }
        else if (st.running && !st.ready)
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            Spinner(dl, ImVec2(p.x + 6.0f, p.y + 8.0f), 5.0f, Fade(t.TextInactive, viewA));
            char starting[64];
            snprintf(starting, sizeof(starting), "starting %s", AgentCrumb());
            Text(dl, g_mono, 12.0f, ImVec2(p.x + 20.0f, p.y + 1.0f), Fade(t.TextInactive, viewA), starting);
            ImGui::Dummy(ImVec2(w, 30.0f));
        }

        if ((st.revision != g_lastRev && stick) || g_forceScroll) ImGui::SetScrollHereY(1.0f);
        g_lastRev = st.revision;
        g_forceScroll = false;

        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
    }

    // ================================================================ chat: composer
    // Puts `text` in the composer (replacing it, or appended) and focuses it.
    void ComposerSet(const std::string& text, bool append)
    {
        std::string next = append ? std::string(g_input) + text : text;
        snprintf(g_input, sizeof(g_input), "%s", next.c_str());
        g_pending = next;
        g_pendingSet = true;
        g_focusInput = true;
    }

    int ComposeCallback(ImGuiInputTextCallbackData* d)
    {
        if (g_pendingSet)
        {
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, g_pending.c_str());
            g_pendingSet = false;
        }
        return 0;
    }

    std::string FirstLineOf(const std::string& s)
    {
        size_t nl = s.find('\n');
        return nl == std::string::npos ? s : s.substr(0, nl);
    }

    bool ContainsNoCase(const std::string& hay, const char* needle)
    {
        std::string h = hay, n = needle;
        for (auto& c : h) c = (char)tolower((unsigned char)c);
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        return h.find(n) != std::string::npos;
    }

    // Commands matching what's typed after "/": prefix matches first, then any
    // that merely contain it.
    std::vector<int> SlashMatches(const chat::State& st, const char* typed)
    {
        std::vector<int> pre, mid;
        size_t n = strlen(typed);
        for (int i = 0; i < (int)st.commands.size(); i++)
        {
            const std::string& name = st.commands[i].name;
            if (_strnicmp(name.c_str(), typed, n) == 0) pre.push_back(i);
            else if (n > 0 && ContainsNoCase(name, typed)) mid.push_back(i);
        }
        pre.insert(pre.end(), mid.begin(), mid.end());
        return pre;
    }

    // The command list, in its own top-level window above the composer so it
    // draws over the messages and takes clicks. Returns a clicked command's
    // index into st.commands, or -1.
    int SlashPopup(const chat::State& st, const std::vector<int>& m, ImVec2 anchor, float w, float a)
    {
        const Theme& t = theme();
        const int maxRows = 8;
        const float rh = 30.0f, footH = 24.0f;
        int rows = VMin((int)m.size(), maxRows);
        if (g_slashSel < g_slashTop) g_slashTop = g_slashSel;
        if (g_slashSel >= g_slashTop + rows) g_slashTop = g_slashSel - rows + 1;
        g_slashTop = VMax(0, VMin(g_slashTop, (int)m.size() - rows));

        float h = rows * rh + footH + 2.0f;
        ImGui::SetNextWindowPos(ImVec2(anchor.x, anchor.y - h - 6.0f));
        ImGui::SetNextWindowSize(ImVec2(w, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##slash", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetWindowPos();
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), Fade(t.Section, a));
        dl->AddRect(o, ImVec2(o.x + w, o.y + h), Fade(Lighten(t.Border, 0.15f), a));

        int clicked = -1;
        float nameW = 0.0f;
        for (int r = 0; r < rows; r++)
            nameW = VMax(nameW, TextW(g_mono, 12.5f, ("/" + st.commands[m[g_slashTop + r]].name).c_str()));
        nameW = VMin(nameW, w * 0.45f);

        for (int r = 0; r < rows; r++)
        {
            int k = g_slashTop + r;
            const chat::Command& c = st.commands[m[k]];
            ImVec2 rmn(o.x + 1.0f, o.y + 1.0f + r * rh), rmx(o.x + w - 1.0f, rmn.y + rh);
            ImGui::SetCursorScreenPos(rmn);
            ImGui::PushID(k);
            ImGui::InvisibleButton("##cmd", ImVec2(rmx.x - rmn.x, rh));
            if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
                g_slashSel = k;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clicked = m[k];
            ImGui::PopID();

            bool sel = (k == g_slashSel);
            if (sel)
            {
                dl->AddRectFilled(rmn, rmx, Fade(t.Element, a));
                dl->AddRectFilled(rmn, ImVec2(rmn.x + 2.0f, rmx.y), Fade(t.Accent, a));
            }
            float cy = rmn.y + rh * 0.5f;
            std::string name = Ellipsize(g_mono, 12.5f, "/" + c.name, nameW);
            Text(dl, g_mono, 12.5f, ImVec2(rmn.x + 14.0f, cy - 7.5f), Fade(sel ? t.Text : LerpCol(t.TextInactive, t.Text, 0.7f), a), name.c_str());
            float dx = rmn.x + 14.0f + nameW + 18.0f;
            std::string desc = c.hint.empty() ? c.desc : c.hint + "   " + c.desc;
            desc = Ellipsize(g_body, 12.5f, FirstLineOf(desc), rmx.x - 12.0f - dx);
            Text(dl, g_body, 12.5f, ImVec2(dx, cy - 8.0f), Fade(t.TextInactive, a), desc.c_str());
        }

        // Footer: how many, and the keys.
        float fy = o.y + 1.0f + rows * rh;
        dl->AddLine(ImVec2(o.x, fy + 0.5f), ImVec2(o.x + w, fy + 0.5f), Fade(t.Border, a));
        char info[64];
        snprintf(info, sizeof(info), "%d command%s", (int)m.size(), m.size() == 1 ? "" : "s");
        Text(dl, g_mono, 11.0f, ImVec2(o.x + 14.0f, fy + 6.0f), Fade(t.TextInactive, a * 0.8f), info);
        const char* keys = "up/down  enter to pick  esc";
        Text(dl, g_mono, 11.0f, ImVec2(o.x + w - 14.0f - TextW(g_mono, 11.0f, keys), fy + 6.0f), Fade(t.TextInactive, a * 0.8f), keys);

        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
            g_slashSel = VMax(0, VMin((int)m.size() - 1, g_slashSel - (int)ImGui::GetIO().MouseWheel));

        ImGui::End();
        ImGui::PopStyleVar(2);
        return clicked;
    }

    void Composer(ImVec2 mn, ImVec2 mx, float a)
    {
        const Theme& t = theme();
        chat::State& st = chat::Get();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float sendW = 44.0f;
        ImVec2 bmn = mn, bmx(mx.x - sendW - 8.0f, mx.y);
        bool canType = st.running;
        bool wasActive = ImGui::GetStateStorage()->GetBool(ImGui::GetID("##composeactive"));
        float focus = Anim(ImGui::GetID("##composefocus"), wasActive ? 1.0f : 0.0f, 14.0f);

        // Frame first: the input draws its own text on top (a stock ImGui widget).
        dl->AddRectFilled(bmn, bmx, Fade(t.Element, a));
        dl->AddRect(bmn, bmx, Fade(LerpCol(t.Border, t.Accent, focus * 0.8f), a));
        if (!g_input[0])
        {
            char hint[128];
            snprintf(hint, sizeof(hint), "Message %s...   (/ for commands, Enter to send, Ctrl+Enter for a new line)", AgentName());
            std::string shown = Ellipsize(g_body, 14.0f, canType ? hint : "session ended - Back to start a new one", bmx.x - bmn.x - 24.0f);
            Text(dl, g_body, 14.0f, ImVec2(bmn.x + 12.0f, bmn.y + 11.0f), Fade(t.TextInactive, a * 0.8f), shown.c_str());
        }

        // Slash commands: "/" plus a word, no space yet.
        if (g_input[0] != '/') g_slashDismissed = false;
        std::vector<int> matches;
        bool slash = false;
        if (canType && wasActive && !g_slashDismissed && g_input[0] == '/' && !strpbrk(g_input, " \t\r\n"))
        {
            matches = SlashMatches(st, g_input + 1);
            slash = !matches.empty();
        }
        if (slash)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   g_slashSel--;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) g_slashSel++;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))    g_slashDismissed = true;
            g_slashSel = VMax(0, VMin((int)matches.size() - 1, g_slashSel));
        }
        else
            g_slashSel = g_slashTop = 0;

        ImGui::SetCursorScreenPos(ImVec2(bmn.x + 10.0f, bmn.y + 9.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Fade(t.Text, a)));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushFont(g_body, 14.0f);
        if (g_focusInput && canType) { ImGui::SetKeyboardFocusHere(); g_focusInput = false; }
        ImGui::BeginDisabled(!canType);
        bool enter = ImGui::InputTextMultiline("##compose", g_input, sizeof(g_input),
                                               ImVec2(bmx.x - bmn.x - 20.0f, bmx.y - bmn.y - 18.0f),
                                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine |
                                               ImGuiInputTextFlags_CallbackAlways, ComposeCallback);
        ImGui::EndDisabled();
        ImGui::GetStateStorage()->SetBool(ImGui::GetID("##composeactive"), ImGui::IsItemActive());
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        if (slash)
        {
            int picked = SlashPopup(st, matches, bmn, VMin(620.0f, bmx.x - bmn.x), a);
            if (picked >= 0) ComposerSet("/" + st.commands[picked].name + " ", false);
            // Enter on a partly typed command completes it; on a complete one it sends.
            if (enter && std::string(g_input + 1) != st.commands[matches[g_slashSel]].name)
            {
                ComposerSet("/" + st.commands[matches[g_slashSel]].name + " ", false);
                enter = false;
                g_focusInput = true;
            }
        }

        // Send / Stop.
        bool busy = st.busy;
        bool hasText = false;
        for (const char* c = g_input; *c; c++) if (*c != ' ' && *c != '\n' && *c != '\r' && *c != '\t') { hasText = true; break; }
        bool canSend = canType && st.ready && !busy && hasText;
        ImVec2 smn(mx.x - sendW, mn.y), smx = mx;
        bool clicked = WipeButton("##send", smn, smx, "", a, canSend || busy, Glyph::None, false, 14.0f, canSend || busy);
        // Icon over the button: an up arrow to send, a square to stop.
        ImVec2 c((smn.x + smx.x) * 0.5f, (smn.y + smx.y) * 0.5f);
        ImU32 ic = Fade((canSend || busy) ? t.AccentText : t.TextInactive, a * ((canSend || busy) ? 1.0f : 0.5f));
        if (busy) dl->AddRectFilled(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), ic);
        else
        {
            dl->AddLine(ImVec2(c.x, c.y + 7.0f), ImVec2(c.x, c.y - 7.0f), ic, 1.6f);
            dl->AddLine(ImVec2(c.x - 5.0f, c.y - 2.0f), ImVec2(c.x, c.y - 7.0f), ic, 1.6f);
            dl->AddLine(ImVec2(c.x + 5.0f, c.y - 2.0f), ImVec2(c.x, c.y - 7.0f), ic, 1.6f);
        }

        if (busy && clicked) chat::Interrupt();
        else if ((enter || clicked) && canSend)
        {
            // Trim trailing newlines and spaces before sending.
            std::string msg = g_input;
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) msg.pop_back();
            ComposerSet("", false);
            // The app's own commands (offered to ACP agents, see AddLocalCommands)
            // never reach the agent.
            if (!IsClaude() && msg == "/new") { StartChat(); return; }
            if (!IsClaude() && msg == "/sessions") { g_sideTab = 1; g_historyFor = -1; return; }
            chat::Send(msg);
            g_forceScroll = true;
        }
        if (enter) g_focusInput = true;      // Enter drops focus; take it straight back
    }

    // ================================================================ chat: sidebar
    // A scrolling child with the app's thin square scrollbar.
    void BeginScroll(const char* id, ImVec2 mn, ImVec2 mx)
    {
        const Theme& t = theme();
        ImGui::SetCursorScreenPos(mn);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(t.Border));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(Lighten(t.Border, 0.2f)));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImGui::ColorConvertU32ToFloat4(t.TextInactive));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::BeginChild(id, ImVec2(VMax(1.0f, mx.x - mn.x), VMax(1.0f, mx.y - mn.y)), 0, ImGuiWindowFlags_NoBackground);
    }

    void EndScroll()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
    }

    bool LessNode(const FileNode& a, const FileNode& b)
    {
        if (a.dir != b.dir) return a.dir;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    }

    // Lists one folder. Folders that were open stay open and are re-read, so a
    // refresh keeps the tree as the user left it.
    void LoadDir(FileNode& n)
    {
        std::vector<FileNode> prev;
        prev.swap(n.kids);
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW((n.path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring nm = fd.cFileName;
                if (nm == L"." || nm == L".." || nm == L".git") continue;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                FileNode k;
                k.path = n.path + L"\\" + nm;
                k.name = Narrow(nm);
                k.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                n.kids.push_back(k);
            } while (n.kids.size() < 600 && ::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        std::sort(n.kids.begin(), n.kids.end(), LessNode);
        for (auto& k : n.kids)
            for (auto& o : prev)
                if (k.dir && o.open && o.name == k.name) { k.open = true; LoadDir(k); break; }
        n.loaded = true;
    }

    // A page with a folded corner.
    void FileIcon(ImDrawList* dl, ImVec2 c, ImU32 col)
    {
        const float w = 9.0f, h = 11.0f, f = 3.0f;
        ImVec2 p(floorf(c.x - w * 0.5f) + 0.5f, floorf(c.y - h * 0.5f) + 0.5f);
        ImVec2 pts[5] = { p, ImVec2(p.x + w - f, p.y), ImVec2(p.x + w, p.y + f), ImVec2(p.x + w, p.y + h), ImVec2(p.x, p.y + h) };
        dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, 1.0f);
        dl->AddLine(ImVec2(p.x + w - f, p.y), ImVec2(p.x + w - f, p.y + f), col);
        dl->AddLine(ImVec2(p.x + w - f, p.y + f), ImVec2(p.x + w, p.y + f), col);
    }

    void Chevron(ImDrawList* dl, ImVec2 c, float open, ImU32 col)
    {
        float ang = open * 1.5707963f;
        auto R = [&](float lx, float ly) { return ImVec2(c.x + lx * cosf(ang) - ly * sinf(ang), c.y + lx * sinf(ang) + ly * cosf(ang)); };
        dl->AddLine(R(-1.8f, -3.5f), R(1.8f, 0.0f), col, 1.2f);
        dl->AddLine(R(1.8f, 0.0f), R(-1.8f, 3.5f), col, 1.2f);
    }

    // The tree. Clicking a folder opens it; clicking a file mentions it in the
    // composer as @path, relative to the working folder.
    void FileRows(ImDrawList* dl, FileNode& n, int depth, float w, float a, const std::wstring& root)
    {
        const Theme& t = theme();
        const float rh = 24.0f;
        for (size_t i = 0; i < n.kids.size(); i++)
        {
            FileNode& k = n.kids[i];
            ImGui::PushID((int)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##row", ImVec2(w, rh));
            bool hov = ImGui::IsItemHovered();
            bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            float hot = Anim(ImGui::GetID("##hot"), hov ? 1.0f : 0.0f, 20.0f);
            if (hot > 0.01f) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), Fade(t.ElementHover, a * hot));

            float x = p.x + 10.0f + depth * 14.0f, cy = p.y + rh * 0.5f;
            ImU32 ic = Fade(LerpCol(t.TextInactive, t.Text, hot), a);
            if (k.dir)
            {
                float ex = Anim(ImGui::GetID("##ex"), k.open ? 1.0f : 0.0f, 18.0f);
                Chevron(dl, ImVec2(x + 3.0f, cy), ex, ic);
                DrawGlyph(dl, Glyph::Folder, ImVec2(x + 11.0f, cy - 6.5f), ImVec2(x + 24.0f, cy + 6.5f), ic);
            }
            else
                FileIcon(dl, ImVec2(x + 17.5f, cy), ic);
            float tx = x + 31.0f;
            std::string label = Ellipsize(g_body, 13.0f, k.name, p.x + w - tx - 8.0f);
            Text(dl, g_body, 13.0f, ImVec2(tx, cy - 8.0f),
                 Fade(k.dir ? t.Text : LerpCol(t.TextInactive, t.Text, 0.55f + 0.45f * hot), a), label.c_str());

            if (clicked)
            {
                if (k.dir) { k.open = !k.open; if (k.open) LoadDir(k); }
                else
                {
                    std::wstring rel = k.path.size() > root.size() ? k.path.substr(root.size()) : k.path;
                    while (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/')) rel.erase(0, 1);
                    for (auto& ch : rel) if (ch == L'\\') ch = L'/';
                    size_t len = strlen(g_input);
                    bool gap = len == 0 || g_input[len - 1] == ' ' || g_input[len - 1] == '\n';
                    ComposerSet(std::string(gap ? "" : " ") + "@" + Narrow(rel) + " ", true);
                }
            }
            if (k.dir && k.open) FileRows(dl, k, depth + 1, w, a, root);
            ImGui::PopID();
        }
    }

    void FilesTab(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float a)
    {
        const Theme& t = theme();
        chat::State& st = chat::Get();
        std::wstring root = Widen(st.cwd.c_str());
        if (g_rootFor != st.cwd)
        {
            g_root = FileNode();
            g_root.path = root;
            g_root.dir = true;
            LoadDir(g_root);
            g_rootFor = st.cwd;
            g_doneTools = -1;
        }
        // Re-read the tree whenever the agent finishes a tool call: it may have
        // written or moved files.
        int done = 0;
        for (const auto& m : st.msgs)
            for (const auto& b : m.blocks)
                if (b.kind == chat::Block::Tool && b.done) done++;
        if (done != g_doneTools) { if (g_doneTools >= 0) LoadDir(g_root); g_doneTools = done; }

        // Header: the folder's name, and a refresh link.
        size_t slash = st.cwd.find_last_of("\\/");
        std::string name = slash == std::string::npos ? st.cwd : st.cwd.substr(slash + 1);
        if (name.empty()) name = st.cwd;
        const char* rl = "refresh";
        float rw = TextW(g_mono, 11.0f, rl);
        ImVec2 rmn(mx.x - 14.0f - rw - 6.0f, mn.y + 6.0f), rmx(mx.x - 8.0f, mn.y + 28.0f);
        ImGui::SetCursorScreenPos(rmn);
        ImGui::InvisibleButton("##refresh", ImVec2(rmx.x - rmn.x, rmx.y - rmn.y));
        bool rh = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) LoadDir(g_root);
        Text(dl, g_mono, 11.0f, ImVec2(rmn.x + 3.0f, mn.y + 11.0f), Fade(rh ? t.Text : t.TextInactive, a), rl);
        std::string shown = Ellipsize(g_semi, 13.5f, name, rmn.x - mn.x - 22.0f);
        Text(dl, g_semi, 13.5f, ImVec2(mn.x + 14.0f, mn.y + 9.0f), Fade(t.Text, a), shown.c_str());

        BeginScroll("##files", ImVec2(mn.x, mn.y + 34.0f), mx);
        float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        if (g_root.kids.empty())
            Text(cdl, g_mono, 11.5f, ImVec2(mn.x + 14.0f, ImGui::GetCursorScreenPos().y + 6.0f), Fade(t.TextInactive, a), "empty folder");
        FileRows(cdl, g_root, 0, w, a, root);
        ImGui::Dummy(ImVec2(w, 8.0f));
        EndScroll();
    }

    // Chats sort into five buckets by when they were last touched. Returns the
    // bucket index for a sortKey in unix seconds, measured against the local
    // day's start computed in `todayStart`.
    enum { Bk_Today = 0, Bk_Yesterday, Bk_Week, Bk_Month, Bk_Older, Bk_Count };
    const char* kBucketLabel[Bk_Count] = { "TODAY", "YESTERDAY", "LAST 7 DAYS", "LAST 30 DAYS", "OLDER" };

    int BucketOf(long long when, long long todayStart)
    {
        if (when >= todayStart)                   return Bk_Today;
        if (when >= todayStart - 86400LL)         return Bk_Yesterday;
        if (when >= todayStart - 7LL  * 86400LL)  return Bk_Week;
        if (when >= todayStart - 30LL * 86400LL)  return Bk_Month;
        return Bk_Older;
    }

    long long LocalTodayStart()
    {
        time_t now = time(nullptr);
        struct tm lt;
        if (localtime_s(&lt, &now) != 0) return (long long)now;
        lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;
        return (long long)mktime(&lt);
    }

    void ChatsTab(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float a)
    {
        const Theme& t = theme();
        chat::State& st = chat::Get();
        // Claude's history is files on disk; an ACP agent answers once it's up.
        if (g_historyFor != g_startCount && (IsClaude() || st.ready))
        {
            chat::RefreshHistory();
            g_historyFor = g_startCount;
        }

        if (WipeButton("##newchat", ImVec2(mn.x + 12.0f, mn.y + 10.0f), ImVec2(mx.x - 12.0f, mn.y + 42.0f), "New chat", a, false,
                       Glyph::Plus, false, 13.0f))
            StartChat();

        BeginScroll("##chats", ImVec2(mn.x, mn.y + 52.0f), mx);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;
        if (st.history.empty())
            Text(cdl, g_mono, 11.5f, ImVec2(mn.x + 14.0f, ImGui::GetCursorScreenPos().y + 6.0f), Fade(t.TextInactive, a),
                 "no past chats in this folder");

        const float rh = 48.0f;
        const long long today = LocalTodayStart();
        int lastBucket = -1;
        std::string pendingDelete;
        static std::string armedDelete;     // the row whose trash was clicked once

        for (size_t i = 0; i < st.history.size(); i++)
        {
            const chat::SessionInfo& h = st.history[i];

            int bk = BucketOf(h.sortKey, today);
            if (bk != lastBucket)
            {
                // Bucket header: a thin uppercase label with a hairline under it.
                ImVec2 hp = ImGui::GetCursorScreenPos();
                if (lastBucket != -1) ImGui::Dummy(ImVec2(w, 8.0f));
                hp = ImGui::GetCursorScreenPos();
                Text(cdl, g_mono, 10.5f, ImVec2(hp.x + 14.0f, hp.y + 10.0f), Fade(t.TextInactive, a * 0.75f), kBucketLabel[bk]);
                cdl->AddLine(ImVec2(hp.x + 14.0f, hp.y + 26.0f), ImVec2(hp.x + w - 14.0f, hp.y + 26.0f), Fade(t.Border, a * 0.6f));
                ImGui::Dummy(ImVec2(w, 30.0f));
                lastBucket = bk;
            }

            ImGui::PushID((int)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##chat", ImVec2(w, rh));
            bool hov = ImGui::IsItemHovered();
            bool cur = !st.sessionId.empty() && h.id == st.sessionId;
            bool rowClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            float hot = Anim(ImGui::GetID("##hot"), hov ? 1.0f : 0.0f, 20.0f);
            ImVec2 rmx(p.x + w, p.y + rh);
            if (cur) cdl->AddRectFilled(p, rmx, Fade(t.Element, a));
            else if (hot > 0.01f) cdl->AddRectFilled(p, rmx, Fade(t.ElementHover, a * hot));
            if (cur) cdl->AddRectFilled(p, ImVec2(p.x + 2.0f, rmx.y), Fade(t.Accent, a));

            // Trash, revealed on hover. It's hit-tested inside the row's own
            // button: a second, overlapping button never gets the click (ImGui
            // gives hover to the item submitted first, so the row opened
            // instead), and placing one moves the layout cursor, which stacked
            // every later row 13px too high. Deleting is permanent, so the first
            // click arms it and a second deletes; leaving the row disarms. The
            // open chat has no trash.
            const float tbSz = 22.0f;
            ImVec2 tbMn(rmx.x - tbSz - 10.0f, p.y + (rh - tbSz) * 0.5f);
            ImVec2 tbMx(tbMn.x + tbSz, tbMn.y + tbSz);
            bool canDel = !cur;
            bool overDel = canDel && hov && ImGui::IsMouseHoveringRect(tbMn, tbMx);
            if (armedDelete == h.id && !hov) armedDelete.clear();
            bool armed = canDel && armedDelete == h.id;
            float delHot = Anim(ImGui::GetID("##delhot"), overDel ? 1.0f : 0.0f, 16.0f);
            float reveal = canDel ? VMax(hot, armed ? 1.0f : 0.0f) : 0.0f;
            float armW = 0.0f;
            if (reveal > 0.01f)
            {
                ImU32 box = armed ? t.Risky : LerpCol(t.ElementHover, t.Element, delHot);
                cdl->AddRectFilled(tbMn, tbMx, Fade(box, a * reveal));
                DrawGlyph(cdl, Glyph::Trash, ImVec2(tbMn.x + 4.0f, tbMn.y + 4.0f), ImVec2(tbMx.x - 4.0f, tbMx.y - 4.0f),
                          Fade(armed ? t.AccentText : LerpCol(t.TextInactive, t.Text, delHot), a * reveal));
                if (armed)
                {
                    const char* q = "delete?";
                    armW = TextW(g_mono, 11.0f, q) + 8.0f;
                    Text(cdl, g_mono, 11.0f, ImVec2(tbMn.x - armW, tbMn.y + 4.0f), Fade(t.Risky, a), q);
                }
            }
            if (rowClicked && overDel)
            {
                if (armed) { pendingDelete = h.id; armedDelete.clear(); }
                else armedDelete = h.id;
                rowClicked = false;
            }

            float titleMaxW = (reveal > 0.01f ? w - tbSz - 24.0f - armW : w - 28.0f);
            std::string title = Ellipsize(g_body, 13.0f, h.title, titleMaxW);
            Text(cdl, g_body, 13.0f, ImVec2(p.x + 14.0f, p.y + 8.0f), Fade(cur || hov ? t.Text : LerpCol(t.TextInactive, t.Text, 0.7f), a), title.c_str());
            Text(cdl, g_mono, 11.0f, ImVec2(p.x + 14.0f, p.y + 27.0f), Fade(t.TextInactive, a * 0.8f), h.when.c_str());

            ImGui::PopID();

            if (rowClicked && !cur) { StartChat(h.id); break; }
        }

        // After the loop: deleting erases from st.history, which is being
        // iterated above. The row is dropped at once; an ACP agent's list is
        // re-read when its reply lands (see chat::DeleteSession).
        if (!pendingDelete.empty()) chat::DeleteSession(pendingDelete);

        ImGui::Dummy(ImVec2(w, 8.0f));
        EndScroll();
    }

    void Sidebar(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float a)
    {
        const Theme& t = theme();
        float w = mx.x - mn.x;
        dl->AddRectFilled(mn, mx, Fade(t.Section, a));
        dl->AddLine(ImVec2(mx.x - 0.5f, mn.y), ImVec2(mx.x - 0.5f, mx.y), Fade(t.Border, a));

        // Two tabs, with an indicator that slides between them.
        const float th = 38.0f;
        const char* tabs[2] = { "FILES", "CHATS" };
        float tw = w * 0.5f;
        for (int i = 0; i < 2; i++)
        {
            ImVec2 tmn(mn.x + i * tw, mn.y);
            ImGui::SetCursorScreenPos(tmn);
            ImGui::PushID(i);
            ImGui::InvisibleButton("##sidetab", ImVec2(tw, th));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) g_sideTab = i;
            float hot = Anim(ImGui::GetID("##hot"), hov ? 1.0f : 0.0f, 16.0f);
            float on = Anim(ImGui::GetID("##on"), g_sideTab == i ? 1.0f : 0.0f, 16.0f);
            ImGui::PopID();
            float lw = TextW(g_mono, 11.5f, tabs[i]);
            Text(dl, g_mono, 11.5f, ImVec2(tmn.x + (tw - lw) * 0.5f, tmn.y + 13.0f),
                 Fade(LerpCol(t.TextInactive, t.Text, VMax(on, hot * 0.6f)), a), tabs[i]);
        }
        float ix = Anim(ImGui::GetID("##sideind"), mn.x + g_sideTab * tw, 16.0f);
        dl->AddLine(ImVec2(mn.x, mn.y + th - 0.5f), ImVec2(mx.x, mn.y + th - 0.5f), Fade(t.Border, a));
        dl->AddRectFilled(ImVec2(ix + 22.0f, mn.y + th - 2.0f), ImVec2(ix + tw - 22.0f, mn.y + th), Fade(t.Accent, a));

        ImVec2 cmn(mn.x, mn.y + th), cmx(mx.x - 1.0f, mx.y);
        if (g_sideTab == 0) FilesTab(dl, cmn, cmx, a);
        else ChatsTab(dl, cmn, cmx, a);
    }

    // ================================================================ chat: strip
    // Model and permission pickers on the left, context / cost / state on the right.
    void Strip(ImDrawList* dl, ImVec2 mn, float w, float a)
    {
        const Theme& t = theme();
        chat::State& st = chat::Get();
        float y = mn.y + 7.0f;
        float x = mn.x + kPad;

        auto Picker = [&](const char* id, const char* cap, const std::vector<chat::Choice>& list, const std::string& cur,
                          float pw, void (*choose)(const std::string&))
        {
            Caps(dl, ImVec2(x, y + 8.0f), cap, a);
            x += TextW(g_mono, 11.0f, cap) + 8.0f;
            if (list.empty())
            {
                Text(dl, g_mono, 12.0f, ImVec2(x, y + 7.0f), Fade(t.TextInactive, a), st.ready ? "default" : "...");
                x += 70.0f;
                return;
            }
            std::vector<const char*> names = Names(list);
            int idx = -1;
            for (size_t i = 0; i < list.size(); i++) if (list[i].value == cur) idx = (int)i;
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
            ImGui::BeginChild(id, ImVec2(pw, 30.0f), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            int before = idx;
            if (Dropdown(id, &idx, names.data(), (int)names.size(), cur.empty() ? "default" : cur.c_str()) && idx != before && idx >= 0)
                choose(list[(size_t)idx].value);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            x += pw + 18.0f;
        };
        Picker("##stripmodel", "MODEL", st.models, st.modelValue, 200.0f, ChooseModel);
        Picker("##stripmode", IsClaude() ? "PERMISSIONS" : "MODE", st.modes, st.modeValue, 160.0f, ChooseMode);

        char right[160], ctx[32] = "";
        const char* state = !st.running ? "ended" : !st.ready ? "starting" : st.busy ? "working" : "ready";
        if (st.ctxSize > 0) snprintf(ctx, sizeof(ctx), "ctx %d%%   ", (int)(100.0 * (double)st.ctxUsed / (double)st.ctxSize + 0.5));
        snprintf(right, sizeof(right), "%s$%.4f   %s", ctx, st.cost, state);
        float rw = TextW(g_mono, 11.5f, right);
        float rx = mn.x + w - kPad - rw;
        if (rx > x)
        {
            Text(dl, g_mono, 11.5f, ImVec2(rx, mn.y + 15.0f), Fade(t.TextInactive, a), right);
            if (st.busy)
            {
                float k = 0.4f + 0.6f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.0f));
                dl->AddRectFilled(ImVec2(rx - 12.0f, mn.y + 19.0f), ImVec2(rx - 7.0f, mn.y + 24.0f), Fade(t.Text, a * k));
            }
        }
    }

    // ================================================================ chat view
    void ChatView(ImDrawList* dl, ImVec2 o, float W, float H, float a)
    {
        const Theme& t = theme();
        chat::State& st = chat::Get();
        float top = o.y + kBarH;
        const float stripH = 44.0f;

        Sidebar(dl, ImVec2(o.x + 1.0f, top), ImVec2(o.x + kSideW, o.y + H - 1.0f), a);

        float x0 = o.x + kSideW, mw = W - kSideW;
        Strip(dl, ImVec2(x0, top), mw, a);
        dl->AddLine(ImVec2(x0, top + stripH - 0.5f), ImVec2(o.x + W, top + stripH - 0.5f), Fade(t.Border, a));

        const float compH = 64.0f;
        float rise = (1.0f - a) * 16.0f;
        Messages(ImVec2(x0 + kPad, top + stripH + 1.0f), ImVec2(o.x + W - kPad + 10.0f, o.y + H - kPad - compH - 12.0f + rise), a);
        Composer(ImVec2(x0 + kPad, o.y + H - kPad - compH + rise), ImVec2(o.x + W - kPad, o.y + H - kPad + rise), a);

        // A finished turn may have renamed or added a session.
        if (g_wasBusy && !st.busy) g_historyFor = -1;
        g_wasBusy = st.busy;
    }

    // ================================================================ prompt
    void Prompt()
    {
        const Theme& t = theme();
        ImGuiIO& io = ImGui::GetIO();

        g_prompt = t.Animations ? Approach(g_prompt, g_promptOpen ? 1.0f : 0.0f, 9.0f, Dt())
                                : (g_promptOpen ? 1.0f : 0.0f);
        if (!g_promptOpen && g_prompt < 0.003f) { g_prompt = 0.0f; return; }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##prompt", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        g_viewAlpha = 1.0f;

        float e = EaseF(Ease::OutCubic, g_prompt);
        ImVec2 disp = io.DisplaySize;
        if (g_blur)
            dl->AddImage(g_blur, ImVec2(0, 0), disp, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(255 * e)));
        dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, (int)(120 * e)));

        const float bw = 384.0f, bh = 220.0f;
        float rise = (1.0f - EaseF(Ease::OutBack, g_prompt)) * 14.0f;
        ImVec2 mn(floorf((disp.x - bw) * 0.5f), floorf((disp.y - bh) * 0.5f + rise));
        ImVec2 mx(mn.x + bw, mn.y + bh);
        dl->AddRectFilled(mn, mx, Fade(t.Section, e));
        dl->AddRect(mn, mx, Fade(t.Accent, e * 0.9f));

        auto Step = [&](float k) { return Clamp01((g_prompt - k) / (1.0f - k)); };
        float a0 = Step(0.15f), a1 = Step(0.3f), a2 = Step(0.45f), a3 = Step(0.6f);

        Caps(dl, ImVec2(mn.x + 22.0f, mn.y + 20.0f), "BEFORE YOU LAUNCH", a0);
        Text(dl, g_body, 18.0f, ImVec2(mn.x + 22.0f, mn.y + 38.0f), Fade(t.Text, a0), "Enjoying abliterated.cli?");
        Text(dl, g_body, 13.0f, ImVec2(mn.x + 22.0f, mn.y + 68.0f), Fade(t.TextInactive, a1), "A star on the repo and a follow on GitHub");
        Text(dl, g_body, 13.0f, ImVec2(mn.x + 22.0f, mn.y + 86.0f), Fade(t.TextInactive, a1), "help keep this going. Thank you.");

        bool live = g_promptOpen && g_prompt > 0.6f;

        float half = (bw - 44.0f - 10.0f) * 0.5f;
        ImVec2 s0(mn.x + 22.0f, mn.y + 118.0f), s1(s0.x + half, s0.y + 34.0f);
        ImVec2 f0(s1.x + 10.0f, s0.y), f1(f0.x + half, s1.y);
        if (WipeButton("##star", s0, s1, g_openedStar ? "Opened" : "Star the repo", a2, false,
                       g_openedStar ? Glyph::Check : Glyph::Star, false, 13.5f) && live)
        {
            if (g_host.OpenUrl) g_host.OpenUrl(kRepoUrl);
            g_openedStar = true;
        }
        if (WipeButton("##follow", f0, f1, g_openedFollow ? "Opened" : "Follow on GitHub", a2, false,
                       g_openedFollow ? Glyph::Check : Glyph::User, false, 13.5f) && live)
        {
            if (g_host.OpenUrl) g_host.OpenUrl(kProfileUrl);
            g_openedFollow = true;
        }

        float by = mx.y - 22.0f - 34.0f;
        {
            ImVec2 cmn(mn.x + 22.0f, by + 10.0f), cmx(cmn.x + 14.0f, cmn.y + 14.0f);
            const char* lbl = "Don't ask again";
            float lw = TextW(g_body, 12.5f, lbl);
            ImGui::SetCursorScreenPos(ImVec2(cmn.x, by));
            ImGui::InvisibleButton("##skip", ImVec2(14.0f + 8.0f + lw, 34.0f));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && live) g_set.skipPrompt = !g_set.skipPrompt;
            float on = Anim(ImGui::GetID("##skipon"), g_set.skipPrompt ? 1.0f : 0.0f, 16.0f);
            float hot = Anim(ImGui::GetID("##skiphot"), hov ? 1.0f : 0.0f, 16.0f);
            dl->AddRect(cmn, cmx, Fade(LerpCol(t.Border, t.Accent, VMax(on, hot * 0.6f)), a3));
            float in = 6.0f * (1.0f - EaseF(Ease::OutBack, on));
            if (on > 0.01f)
                dl->AddRectFilled(ImVec2(cmn.x + 3.0f + in, cmn.y + 3.0f + in), ImVec2(cmx.x - 3.0f - in, cmx.y - 3.0f - in),
                                  Fade(t.Accent, a3 * on));
            Text(dl, g_body, 12.5f, ImVec2(cmx.x + 8.0f, cmn.y - 1.0f), Fade(LerpCol(t.TextInactive, t.Text, hot), a3), lbl);
        }

        ImVec2 l1(mx.x - 22.0f, by + 34.0f), l0(l1.x - 118.0f, by);
        ImVec2 c1(l0.x - 8.0f, l1.y), c0(c1.x - 84.0f, by);
        bool cancel = WipeButton("##cancel", c0, c1, "Cancel", a3, false, Glyph::None, false, 13.5f) && live;
        bool go = WipeButton("##go", l0, l1, "Launch", a3, true, Glyph::None, true, 13.5f) && live;

        if (live && ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
        if (live && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) go = true;

        if (cancel) { g_promptOpen = false; SetStatus(false, "cancelled"); }
        if (go)     { g_promptOpen = false; DoLaunch(); }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

// ===================================================================== api
void ui::SetHost(const Host& host) { g_host = host; }
ui::Settings& ui::GetSettings() { return g_set; }
void ui::SetFonts(ImFont* body, ImFont* mono, ImFont* monoTitle, ImFont* semibold)
{
    g_body = body; g_mono = mono; g_monoTitle = monoTitle; g_semi = semibold ? semibold : body;
}
void ui::SetBlur(ImTextureID tex, ImVec2) { g_blur = tex; }

bool ui::Resizable() { return g_chatMode && g_expand >= 1.0f; }

void ui::SetChatSize(float w, float h)
{
    g_chatSize = ImVec2(VMax(kChatMin.x, w), VMax(kChatMin.y, h));
    g_set.chatW = (int)g_chatSize.x;
    g_set.chatH = (int)g_chatSize.y;
}

void ui::MinChatSize(float* w, float* h) { *w = kChatMin.x; *h = kChatMin.y; }

bool ui::WantsSnapshot()
{
    bool w = g_wantSnapshot;
    g_wantSnapshot = false;
    return w;
}

bool ui::HitTestCaption(float x, float y)
{
    return y >= 0.0f && y < g_barBottom && x < g_barButtonsLeft;
}

void ui::WindowSize(float* w, float* h)
{
    // Ease the size, not the progress, so the window glides and settles.
    float e = EaseF(Ease::InOutCubic, g_expand);
    *w = floorf(kLauncherSize.x + (g_chatSize.x - kLauncherSize.x) * e + 0.5f);
    *h = floorf(kLauncherSize.y + (g_chatSize.y - kLauncherSize.y) * e + 0.5f);
}

void ui::Draw()
{
    if (g_boot < 0.0)
    {
        g_boot = ImGui::GetTime();
        for (int i = 0; i < 3; i++)
        {
            std::wstring exe;
            g_installed[i] = chat::FindExe(chat::BackendExe(ToolBackend(i)), exe);
        }
        if (g_set.chatW > 0 && g_set.chatH > 0) ui::SetChatSize((float)g_set.chatW, (float)g_set.chatH);
    }
    if (g_set.tool < 0 || g_set.tool > 2) g_set.tool = 0;
    if (g_set.claudeModel < 0 || g_set.claudeModel >= (int)chat::ClaudeModels().size()) g_set.claudeModel = 0;

    chat::Pump();
    SyncModels();

    // Window growth: ~0.55s each way.
    float target = g_chatMode ? 1.0f : 0.0f;
    if (!theme().Animations) g_expand = target;
    else if (g_expand < target) g_expand = VMin(target, g_expand + Dt() / 0.55f);
    else if (g_expand > target) g_expand = VMax(target, g_expand - Dt() / 0.45f);

    const Theme& t = theme();
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(t.Content));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(t.Outline));

    // NoBringToFrontOnFocus keeps the prompt — a separate window submitted after
    // this one — on top, even when the launcher behind it is clicked.
    ImGui::Begin("##app", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetWindowPos();
        float W = ImGui::GetWindowSize().x, H = ImGui::GetWindowSize().y;

        ImGui::BeginDisabled(g_promptOpen);
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);

        g_viewAlpha = 1.0f;
        TitleBar(dl, o, W);

        // Launcher fades out over the first part of the growth, chat in over the last.
        float la = 1.0f - Clamp01(g_expand * 2.2f);
        float ca = EaseF(Ease::OutCubic, Clamp01((g_expand - 0.45f) / 0.55f));
        if (la > 0.001f)
        {
            g_viewAlpha = la;
            ImGui::BeginDisabled(g_chatMode);
            LauncherView(dl, o, W, H);
            ImGui::EndDisabled();
        }
        if (ca > 0.001f)
        {
            g_viewAlpha = 1.0f;
            ChatView(dl, o, W, H, ca);
        }
        g_viewAlpha = 1.0f;

        ImGui::PopStyleVar();
        ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    Prompt();
}
