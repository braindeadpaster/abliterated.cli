# abliterated.cli

A small native launcher for coding-agent CLIs — **Claude Code**, **OpenCode**, and **oh-my-pi** — rendered as a chat inside the app.

Borderless, square, black-and-white. Built in C++ on Dear ImGui + Direct3D 11, with a custom widget layer (`velo`) sitting on top of ImGui.

![platform](https://img.shields.io/badge/platform-Windows%2010%2F11-black) ![cxx](https://img.shields.io/badge/C%2B%2B-17-black) ![renderer](https://img.shields.io/badge/renderer-D3D11-black)

---

## What it does

Pick a working folder, pick a tool, pick a model — the agent starts as a child process and the window grows to show the chat. The chat renders assistant turns as a mix of text, tool calls, and (where supported) thinking blocks, with inline permission prompts for tools the agent wants to run.

Two protocols drive one chat view:

| Backend     | Transport                                                                  | Verified against     |
|-------------|----------------------------------------------------------------------------|----------------------|
| Claude Code | `claude -p --output-format stream-json --input-format stream-json ...`     | Claude Code 2.1.310  |
| OpenCode    | `opencode acp` — Agent Client Protocol (JSON-RPC 2.0, line-delimited)      | OpenCode 2.0.32      |
| oh-my-pi    | `omp acp` — same ACP protocol                                              | OpenCode 2.0.32      |

Live model switching, live permission-mode switching, slash-command discovery, past-session resume (`--resume` for Claude; `session/load` for ACP), and session deletion all work through the same UI.

Settings persist to `%APPDATA%\abliterated.cli\settings.txt`.

## Features

- **Native window, not a web shell** — borderless, layered-alpha fade in/out, Windows-11 corner rounding turned off, dragged via `WM_NCHITTEST`.
- **User-resizable chat** — `WS_THICKFRAME` with the non-client frame erased in `WM_NCCALCSIZE`, edges reported from `WM_NCHITTEST`, content re-rendered live during the drag through `WM_SIZE`.
- **Self-resizing launcher** — the window grows around its own centre when the UI asks for more room (chat open vs. closed).
- **Two protocols, one chat** — Claude Code's stream-json SDK and ACP (OpenCode / oh-my-pi) feed the same `chat::State`.
- **Lock-free UI thread** — reader threads only queue raw lines; parsing and every state change happen in `Pump()` on the UI thread.
- **Live config** — model, permission mode, and plan mode can be changed mid-session via `set_model` / `set_permission_mode` (Claude) or `session/set_config_option` (ACP).
- **History** — reads Claude's on-disk transcripts and ACP agents' `session/list`, with per-row delete.

## Building

Requirements:

- Windows 10 or 11
- Visual Studio 2022 (Desktop development with C++)
- Windows 10 SDK or newer
- `claude`, `opencode`, and/or `omp` on `PATH` to actually run anything

Steps:

```
git clone https://github.com/<you>/abliterated.git
cd abliterated
```

Open `abliterated.sln` in Visual Studio 2022, select **Release / x64**, and build. The output lands at `bin/Release/abliterated.exe`.

No vcpkg, no NuGet, no CMake. Dear ImGui is bundled under `backend/imgui/`; everything else is Win32 + D3D11.

## Running

Launch `abliterated.exe`. Pick a folder, pick a tool, pick a model (or let the agent default), hit the star.

The first time an agent asks for a tool permission, the chat shows the option list the agent sent — `allow_once`, `allow_always`, `reject_once`, `reject_always` for ACP, or the Claude Code equivalents. Click one and the agent continues.

## Project layout

```
abliterated/
├── main.cpp              Win32 host: window, D3D11 device, message loop, fades
├── ui/
│   ├── ui.h              UI surface + Host callbacks + persisted Settings
│   └── ui.cpp            launcher screen and chat screen
├── chat/
│   ├── session.h         chat::State + the public API (Start/Send/Pump/...)
│   ├── session.cpp       Claude stream-json + ACP drivers
│   └── json.h            single-header JSON used by both drivers
├── backend/
│   ├── imgui/            bundled Dear ImGui (docking-free, DX11 + Win32 impls)
│   └── velo/             custom widget layer on top of ImGui
│       ├── velo.h
│       ├── velo_core.cpp
│       ├── velo_controls.cpp
│       ├── velo_widgets.cpp
│       ├── velo_internal.h
│       └── velo_lucide.h
└── assets/
    └── fonts/            Poppins (Medium + SemiBold) under OFL
```

## Licenses

The project source is yours to license as you like. Third-party pieces bundled in-tree:

- **Dear ImGui** (`backend/imgui/`) — MIT, © Omar Cornut and contributors
- **Poppins** (`assets/fonts/`) — SIL Open Font License 1.1 (see `assets/fonts/OFL.txt`)

## Notes

- The agent binaries themselves are not bundled. Install them separately (`npm i -g @anthropic-ai/claude-code`, `npm i -g opencode-ai`, etc.) and make sure they're on `PATH`.
- When npm installs a `.cmd` shim, the launcher resolves it to the real `.exe` behind it — `cmd.exe` mangles argument quoting and would corrupt the JSON-RPC stream.
