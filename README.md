# abliterated.cli

a clean, sleek imgui-based launcher that runs your claude / opencode cli as a chat app, with a jailbreak built in.

works on most models — opus 4.7 and below, and most opencode models.

---

## what it is

pick a folder, pick a tool, pick a model. the agent starts in that folder and the window grows to show the chat.

the jailbreak lives in `assets/jb.md` and is wired in through the tool's own config (opencode's `instructions`, claude's system prompt entry point) so every turn goes through it.

native windows app, black and white, square, borderless, animated. built in c++ on dear imgui + d3d11 with a small widget layer on top (`velo`).

## supported

- **claude code** — opus 4.7 and below, sonnet, haiku. uses the stream-json sdk protocol.
- **opencode** — most provider/model ids opencode exposes. uses acp (agent client protocol).
- **oh-my-pi** — same acp wire, swappable.

live model switching, live permission-mode switching, slash commands, and session resume all work through the same ui.

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
assets/jb.md        the jailbreak
```

## credits

- **dear imgui** — mit, omar cornut and contributors
- **poppins** — sil open font license 1.1
