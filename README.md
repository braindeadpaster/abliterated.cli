<p align="center">
  <img src="assets/logo.png" alt="abliterated.cli" width="640">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-windows%2010%20%2F%2011-000000?style=for-the-badge" alt="platform">
  <img src="https://img.shields.io/badge/c%2B%2B-17-000000?style=for-the-badge&logo=cplusplus&logoColor=white" alt="c++17">
  <img src="https://img.shields.io/badge/renderer-d3d11-000000?style=for-the-badge" alt="d3d11">
  <img src="https://img.shields.io/badge/ui-dear%20imgui-000000?style=for-the-badge" alt="dear imgui">
  <img src="https://img.shields.io/badge/jailbreak-built--in-000000?style=for-the-badge" alt="jailbreak">
</p>

<p>
  <img src="https://img.shields.io/badge/claude%20code-opus%204.7%20and%20below-ffffff?style=flat-square&labelColor=000000" alt="claude code">
  <img src="https://img.shields.io/badge/opencode-acp-ffffff?style=flat-square&labelColor=000000" alt="opencode acp">
  <img src="https://img.shields.io/badge/oh--my--pi-acp-ffffff?style=flat-square&labelColor=000000" alt="omp acp">
</p>

a clean, sleek imgui-based launcher that runs your claude / opencode cli as a chat app, with a jailbreak built in.

works on most models — opus 4.7 and below, and most opencode models.

---

## what it is

pick a folder, pick a tool, pick a model. the agent starts in that folder and the window grows to show the chat.

the jailbreak is embedded in the exe as a win32 resource (`assets/jb.md` → `IDR_JB_MD`). at launch it's extracted to `%LOCALAPPDATA%\abliterated.cli\jb.md` and wired in through each tool's own config — claude gets `--append-system-prompt-file`, opencode gets a workspace `opencode.jsonc` with `instructions`. dev builds that still have `assets/jb.md` next to the exe use that copy instead, so you can edit without a rebuild.

native windows app, black and white, square, borderless, animated. built in c++ on dear imgui + d3d11 with a small widget layer on top (`velo`).

## supported

- **claude code** — opus 4.7 and below, sonnet, haiku. uses the stream-json sdk protocol.
- **opencode** — most provider/model ids opencode exposes. uses acp (agent client protocol).
- **oh-my-pi** — same acp wire, swappable.

live model switching, live permission-mode switching, slash commands, and session resume all work through the same ui.

attach images with **Ctrl+V** (clipboard, including Snipping Tool) or by **dragging image files** (png / jpg / jpeg / webp / gif) onto the window. attached images show as chips above the composer and are sent inline — claude as `image` content blocks, acp as `image` content blocks.

## build

- windows 10 / 11
- visual studio 2022, desktop c++ workload
- windows sdk

open `abliterated.sln`, pick **release / x64**, build. output lands at `bin/release/abliterated.exe`.

no vcpkg, no cmake. dear imgui ships in-tree under `backend/imgui/`.

## run

`abliterated.exe`. pick a folder, pick a tool, pick a model, hit the star.

the agent binaries aren't bundled — install them yourself:

```
npm i -g @anthropic-ai/claude-code
npm i -g opencode-ai
```

make sure `claude` / `opencode` are on `PATH`. settings persist to `%APPDATA%\abliterated.cli\settings.txt`.

## layout

```
main.cpp            win32 + d3d11 host
ui/                 launcher and chat screens
chat/               claude stream-json + acp drivers, one shared state
backend/velo/       custom widget layer over imgui
backend/imgui/      bundled dear imgui
assets/fonts/       poppins (ofl)
assets/logo.png     banner (readme header)
assets/icon.png     star glyph (source for the exe icon)
assets/icon.ico     multi-size app icon (256/128/64/48/32/16)
assets/jb.md        the jailbreak (embedded into the exe via res/abliterated.rc)
res/                resource script and ids
```

## credits

- **dear imgui** — mit, omar cornut and contributors
- **poppins** — sil open font license 1.1
