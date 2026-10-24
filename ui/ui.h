//
// abliterated.cli — a small launcher for OpenCode and Claude CLI, built on velo.
//
// Black and white, square, animated. Either tool opens as a chat inside the app
// (see chat/session.h), and the window grows to make room for it. Platform
// work — links, the folder picker, moving and closing the window — goes
// through the callbacks in Host.

#pragma once
#include "velo.h"
#include <string>
#include <vector>

namespace ui
{
    enum Tool { Tool_OpenCode = 0, Tool_Claude = 1, Tool_Omp = 2 };

    // Platform callbacks the host fills in.
    struct Host
    {
        void (*Minimize)() = nullptr;
        void (*Close)() = nullptr;
        void (*OpenUrl)(const char* url) = nullptr;
        // Folder picker; writes a UTF-8 path and returns true if one was chosen.
        bool (*PickFolder)(char* out, int outSize) = nullptr;
    };

    // What persists between runs. The host loads and saves it.
    struct Settings
    {
        int  tool = Tool_OpenCode;
        char opencodeModel[160] = ""; // provider/model id; "" = OpenCode's own default
        bool planMode = false;        // OpenCode's read-only "plan" mode
        bool skipPrompt = false;      // "don't ask again" on the star / follow prompt
        char folder[520] = "";        // working folder for either tool (UTF-8)
        int  claudeModel = 0;         // index into chat::ClaudeModels()
        bool acceptEdits = false;     // --permission-mode acceptEdits
        int  chatW = 0, chatH = 0;    // the chat's size, once the user has resized it
    };

    void SetHost(const Host& host);
    Settings& GetSettings();

    // Body, monospace, monospace title, and semibold (chat headings).
    void SetFonts(ImFont* body, ImFont* mono, ImFont* monoTitle, ImFont* semibold);

    // Build the whole app. Fills the client area; call once per frame between
    // ImGui::NewFrame() and ImGui::Render(). The first call starts a background
    // `opencode models` to fill the model list.
    void Draw();

    // The client size the UI wants this frame. It animates between the launcher
    // and the chat; the host resizes the window to match, around its centre.
    void WindowSize(float* w, float* h);

    // The open chat can be resized by its edges. While Resizable() the host
    // reports user resizes through SetChatSize(); the launcher stays fixed.
    bool Resizable();
    void SetChatSize(float w, float h);
    void MinChatSize(float* w, float* h);

    // True on the frame the launch prompt opens. The host should copy that
    // frame's back buffer, blur it, and hand it back with SetBlur().
    bool WantsSnapshot();
    void SetBlur(ImTextureID tex, ImVec2 size);

    // For WM_NCHITTEST: true where a press should drag the window (the top bar,
    // minus its buttons). Client coordinates.
    bool HitTestCaption(float x, float y);

    // Called from main.cpp's WM_DROPFILES. Any supported image file in the drop
    // (png / jpg / jpeg / webp / gif) is loaded and queued as a pending
    // attachment on the composer; non-image files are ignored.
    void OnFilesDropped(const std::vector<std::wstring>& paths);
}
