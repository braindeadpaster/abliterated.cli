//
// abliterated.cli host.
//
// An ordinary app window rather than the fullscreen overlay the menus use: a
// launcher wants a taskbar button, focus, and normal clicks. Borderless, square
// (Windows 11 rounding turned off), and dragged by the title bar through
// WM_NCHITTEST so Windows does the moving.
//
// Fades in on open and out on close (layered window alpha), and resizes itself
// around its centre when the UI asks for more room (the chat). The open chat is
// also user-resizable: WS_THICKFRAME with the frame removed in WM_NCCALCSIZE,
// edges reported from WM_NCHITTEST, and a frame rendered from WM_SIZE so the
// content follows the drag live. Settings persist in
// %APPDATA%\abliterated.cli\settings.txt.

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "velo.h"
#include "ui.h"
#include "chat/session.h"
#include "res/resource.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <vector>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

static ID3D11Device*             g_device = nullptr;
static ID3D11DeviceContext*      g_context = nullptr;
static IDXGISwapChain*           g_swapChain = nullptr;
static ID3D11RenderTargetView*   g_rtv = nullptr;
static ID3D11ShaderResourceView* g_blurSrv = nullptr;
static HWND                      g_hwnd = nullptr;
static bool                      g_running = true;
static bool                      g_closing = false;
static float                     g_alpha = 0.0f;       // window fade, 0..1

static const int kW = 560, kH = 404;           // launcher size; the UI asks for more later
static float g_anchorX = 0.0f, g_anchorY = 0.0f;  // centre the window grows around
static bool  g_sizing = false;                    // inside the user's size / move loop
static bool  g_selfResize = false;                // our own SetWindowPos, not the user
static bool  g_inFrame = false;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ===================================================================== d3d
static void CreateRTV()
{
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_device->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}

static void CleanupRTV()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDevice(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &fl, &g_context) != S_OK)
        return false;
    CreateRTV();
    return true;
}

static void CleanupDevice()
{
    CleanupRTV();
    if (g_blurSrv) { g_blurSrv->Release(); g_blurSrv = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ===================================================================== blur
// One box-blur pass along rows (horizontal) or columns, with a running sum so the
// cost doesn't depend on the radius. Three passes each way approximate a gaussian.
static void BoxPass(const unsigned char* src, unsigned char* dst, int w, int h, int r, bool horizontal)
{
    int n = horizontal ? w : h, lines = horizontal ? h : w;
    int step = horizontal ? 4 : w * 4;
    int div = r * 2 + 1;
    for (int line = 0; line < lines; line++)
    {
        const unsigned char* s = src + (horizontal ? line * w * 4 : line * 4);
        unsigned char* d = dst + (horizontal ? line * w * 4 : line * 4);
        for (int c = 0; c < 3; c++)
        {
            int sum = 0;
            for (int k = -r; k <= r; k++)
            {
                int i = k < 0 ? 0 : (k >= n ? n - 1 : k);
                sum += s[i * step + c];
            }
            for (int i = 0; i < n; i++)
            {
                d[i * step + c] = (unsigned char)(sum / div);
                int add = i + r + 1; if (add >= n) add = n - 1;
                int sub = i - r;     if (sub < 0)  sub = 0;
                sum += s[add * step + c] - s[sub * step + c];
            }
        }
        for (int i = 0; i < n; i++) d[i * step + 3] = 255;
    }
}

// Copy the frame just rendered, blur it on the CPU, and hand it to the UI. Runs
// once, when the launch prompt opens — the window under the prompt is frozen
// anyway, so there's no need to blur live every frame.
static void MakeBlurSnapshot()
{
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;

    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging))) { back->Release(); return; }
    g_context->CopyResource(staging, back);
    back->Release();

    const int w = (int)d.Width, h = (int)d.Height;
    std::vector<unsigned char> a((size_t)w * h * 4), b((size_t)w * h * 4);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        for (int y = 0; y < h; y++)
            memcpy(&a[(size_t)y * w * 4], (const unsigned char*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        g_context->Unmap(staging, 0);
    }
    staging->Release();

    for (int pass = 0; pass < 3; pass++)
    {
        BoxPass(a.data(), b.data(), w, h, 9, true);
        BoxPass(b.data(), a.data(), w, h, 9, false);
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { a.data(), (UINT)w * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_device->CreateTexture2D(&td, &init, &tex))) return;
    if (g_blurSrv) { g_blurSrv->Release(); g_blurSrv = nullptr; }
    g_device->CreateShaderResourceView(tex, nullptr, &g_blurSrv);
    tex->Release();
    ui::SetBlur((ImTextureID)(intptr_t)g_blurSrv, ImVec2((float)w, (float)h));
}

// ===================================================================== assets
// assets\<name> beside the exe, or two / three levels up (bin\Release builds).
static bool FindAsset(const wchar_t* name, wchar_t* out, size_t outCount)
{
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *slash = 0;
    const wchar_t* up[] = { L"", L"\\..\\..", L"\\..\\..\\.." };
    for (int i = 0; i < 3; i++)
    {
        swprintf_s(out, outCount, L"%s%s\\assets\\%s", exePath, up[i], name);
        if (::GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

static ImFont* AddFont(const wchar_t* file, bool asset, float size, const char* fallback)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.RasterizerMultiply = 1.1f;
    wchar_t full[MAX_PATH * 2] = {};
    bool found = asset ? FindAsset(file, full, MAX_PATH * 2)
                       : (swprintf_s(full, L"C:\\Windows\\Fonts\\%s", file), ::GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES);
    if (found)
    {
        char utf8[MAX_PATH * 4] = {};
        ::WideCharToMultiByte(CP_UTF8, 0, full, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(utf8, size, &cfg)) return f;
    }
    return io.Fonts->AddFontFromFileTTF(fallback, size, &cfg);
}

// ===================================================================== settings
static void SettingsPath(wchar_t* out, size_t n, bool createDir)
{
    wchar_t appdata[MAX_PATH] = {};
    ::GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    swprintf_s(out, n, L"%s\\abliterated.cli", appdata);
    if (createDir) ::CreateDirectoryW(out, nullptr);
    wcscat_s(out, n, L"\\settings.txt");
}

static void LoadSettings()
{
    wchar_t path[MAX_PATH * 2];
    SettingsPath(path, MAX_PATH * 2, false);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    ui::Settings& s = ui::GetSettings();
    char line[640];
    while (fgets(line, sizeof(line), f))
    {
        int v = 0;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (strncmp(line, "folder=", 7) == 0) snprintf(s.folder, sizeof(s.folder), "%s", line + 7);
        else if (sscanf_s(line, "tool=%d", &v) == 1) s.tool = v;
        else if (strncmp(line, "opencodeModel=", 14) == 0) snprintf(s.opencodeModel, sizeof(s.opencodeModel), "%s", line + 14);
        else if (sscanf_s(line, "planMode=%d", &v) == 1) s.planMode = v != 0;
        else if (sscanf_s(line, "claudeModel=%d", &v) == 1) s.claudeModel = v;
        else if (sscanf_s(line, "acceptEdits=%d", &v) == 1) s.acceptEdits = v != 0;
        else if (sscanf_s(line, "skipPrompt=%d", &v) == 1) s.skipPrompt = v != 0;
        else if (sscanf_s(line, "chatW=%d", &v) == 1) s.chatW = v;
        else if (sscanf_s(line, "chatH=%d", &v) == 1) s.chatH = v;
    }
    fclose(f);
}

static void SaveSettings()
{
    wchar_t path[MAX_PATH * 2];
    SettingsPath(path, MAX_PATH * 2, true);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    const ui::Settings& s = ui::GetSettings();
    fprintf(f, "tool=%d\nopencodeModel=%s\nplanMode=%d\nskipPrompt=%d\nclaudeModel=%d\nacceptEdits=%d\nchatW=%d\nchatH=%d\nfolder=%s\n",
            s.tool, s.opencodeModel, s.planMode ? 1 : 0, s.skipPrompt ? 1 : 0, s.claudeModel, s.acceptEdits ? 1 : 0, s.chatW, s.chatH, s.folder);
    fclose(f);
}

// ===================================================================== host callbacks
static void HostMinimize() { ::ShowWindow(g_hwnd, SW_MINIMIZE); }
static void HostClose()    { ::PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
static void HostOpenUrl(const char* url) { ::ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL); }

// The shell's own folder picker. Writes a UTF-8 path.
static bool HostPickFolder(char* out, int outSize)
{
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    bool ok = false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose the working folder");
    if (SUCCEEDED(dlg->Show(g_hwnd)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                ::WideCharToMultiByte(CP_UTF8, 0, path, -1, out, outSize, nullptr, nullptr);
                ::CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ===================================================================== window
static void RenderFrame();

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_NCCALCSIZE:
        // WS_THICKFRAME is only there for resizing: the client area is the whole window.
        if (wParam) return 0;
        break;
    case WM_NCHITTEST:
    {
        LRESULT hit = ::DefWindowProcW(hWnd, msg, wParam, lParam);
        if (hit == HTCLIENT)
        {
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ::ScreenToClient(hWnd, &p);
            // Edges resize the open chat; the launcher has none.
            if (ui::Resizable())
            {
                RECT rc; ::GetClientRect(hWnd, &rc);
                const int b = 6;
                bool l = p.x < b, r = p.x >= rc.right - b, t = p.y < b, d = p.y >= rc.bottom - b;
                if (t && l) return HTTOPLEFT;
                if (t && r) return HTTOPRIGHT;
                if (d && l) return HTBOTTOMLEFT;
                if (d && r) return HTBOTTOMRIGHT;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
                if (t) return HTTOP;
                if (d) return HTBOTTOM;
            }
            // The title bar drags the window; Windows does the moving.
            if (ui::HitTestCaption((float)p.x, (float)p.y)) return HTCAPTION;
        }
        return hit;
    }
    case WM_GETMINMAXINFO:
        if (ui::Resizable())
        {
            float w, h;
            ui::MinChatSize(&w, &h);
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = (LONG)w;
            mmi->ptMinTrackSize.y = (LONG)h;
        }
        return 0;
    case WM_ENTERSIZEMOVE: g_sizing = true;  return 0;
    case WM_EXITSIZEMOVE:  g_sizing = false; return 0;
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED)
        {
            CleanupRTV();
            g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRTV();
        }
        // A size we didn't set is the user's (an edge drag, or snapping).
        if (!g_selfResize && wParam == SIZE_RESTORED && ui::Resizable())
            ui::SetChatSize((float)LOWORD(lParam), (float)HIWORD(lParam));
        // The size loop is modal; draw from here so the content follows the edge.
        if (g_sizing && g_device) RenderFrame();
        return 0;
    case WM_DROPFILES:
    {
        // Drag-drop onto the window: hand image paths to the composer.
        // Non-image extensions are filtered in ui::OnFilesDropped.
        HDROP hDrop = (HDROP)wParam;
        UINT count = ::DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        paths.reserve(count);
        for (UINT i = 0; i < count; i++)
        {
            UINT need = ::DragQueryFileW(hDrop, i, nullptr, 0);
            std::wstring p(need, L'\0');
            ::DragQueryFileW(hDrop, i, p.data(), need + 1);
            paths.push_back(std::move(p));
        }
        ::DragFinish(hDrop);
        ui::OnFilesDropped(paths);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;      // no Alt menu
        break;
    case WM_CLOSE:
        g_closing = true;                                     // fade out, then quit
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// One frame: follow the size the UI asks for, build the UI, present. Also
// called from WM_SIZE while the user drags an edge.
static void RenderFrame()
{
    if (g_inFrame) return;
    g_inFrame = true;

    // Grow / shrink to what the UI wants, around a fixed centre. The centre is
    // re-read whenever the size is settled, so a drag moves it; while the size
    // is changing it stays put, so rounding can't walk the window sideways.
    {
        float tw = 0.0f, th = 0.0f;
        ui::WindowSize(&tw, &th);
        RECT rc; ::GetWindowRect(g_hwnd, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        int nw = (int)tw, nh = (int)th;
        if (g_sizing) {}                      // the user is dragging an edge; follow, don't fight
        else if (nw == cw && nh == ch)
        {
            g_anchorX = (rc.left + rc.right) * 0.5f;
            g_anchorY = (rc.top + rc.bottom) * 0.5f;
        }
        else
        {
            int nx = (int)floorf(g_anchorX - nw * 0.5f + 0.5f);
            int ny = (int)floorf(g_anchorY - nh * 0.5f + 0.5f);
            MONITORINFO mi = { sizeof(mi) };
            ::GetMonitorInfoW(::MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            if (nx + nw > mi.rcWork.right)  nx = mi.rcWork.right - nw;
            if (ny + nh > mi.rcWork.bottom) ny = mi.rcWork.bottom - nh;
            if (nx < mi.rcWork.left) nx = mi.rcWork.left;
            if (ny < mi.rcWork.top)  ny = mi.rcWork.top;
            g_selfResize = true;
            ::SetWindowPos(g_hwnd, nullptr, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
            g_selfResize = false;
        }
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    velo::NewFrame();

    ui::Draw();

    ImGui::Render();
    const float clear[4] = { 0.035f, 0.035f, 0.035f, 1.0f };
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (ui::WantsSnapshot()) MakeBlurSnapshot();

    g_swapChain->Present(1, 0);

    g_inFrame = false;
}

static int Run()
{
    // --frames N: render N frames then quit (automated captures).
    int frameLimit = 0;
    for (int i = 1; i < __argc && __wargv; i++)
        if (wcscmp(__wargv[i], L"--frames") == 0 && i + 1 < __argc) frameLimit = _wtoi(__wargv[i + 1]);

    HICON hAppIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC | CS_DROPSHADOW, WndProc, 0L, 0L,
                       ::GetModuleHandleW(nullptr), hAppIcon, ::LoadCursorW(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"abliterated_cli", hAppIcon };
    ::RegisterClassExW(&wc);

    RECT wa;
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - kW) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - kH) / 2;

    g_hwnd = ::CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, wc.lpszClassName, L"abliterated.cli",
                               WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                               x, y, kW, kH, nullptr, nullptr, wc.hInstance, nullptr);
    ::SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);

    // Square corners: Windows 11 would otherwise round them.
    DWORD corner = 1;   // DWMWCP_DONOTROUND
    ::DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));

    if (!CreateDevice(g_hwnd))
    {
        CleanupDevice();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(g_hwnd);
    ::DragAcceptFiles(g_hwnd, TRUE);   // enables WM_DROPFILES for image attachments

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImFont* body = AddFont(L"fonts\\Poppins-Medium.ttf", true, 15.0f, "C:\\Windows\\Fonts\\segoeui.ttf");
    ImFont* mono = AddFont(L"CascadiaMono.ttf", false, 13.0f, "C:\\Windows\\Fonts\\consola.ttf");
    ImFont* semi = AddFont(L"fonts\\Poppins-SemiBold.ttf", true, 15.0f, "C:\\Windows\\Fonts\\seguisb.ttf");
    ui::SetFonts(body, mono, mono, semi);
    velo::LoadIcons();                  // Lucide, embedded in velo; after the text fonts

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    velo::Apply(velo::Preset::Mono);
    velo::ApplyToImGuiStyle();

    LoadSettings();
    if (!ui::GetSettings().folder[0])
    {
        wchar_t home[MAX_PATH] = {};
        if (::GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH))
            ::WideCharToMultiByte(CP_UTF8, 0, home, -1, ui::GetSettings().folder, (int)sizeof(ui::GetSettings().folder), nullptr, nullptr);
    }
    ui::Host host;
    host.Minimize = HostMinimize;
    host.Close = HostClose;
    host.OpenUrl = HostOpenUrl;
    host.PickFolder = HostPickFolder;
    ui::SetHost(host);

    int frame = 0;
    while (g_running)
    {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        // Minimised: nothing to draw, so don't spin.
        if (::IsIconic(g_hwnd)) { ::Sleep(50); continue; }

        // Window fade: in on open, out on close, then actually close.
        float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f;
        g_alpha += (g_closing ? -dt / 0.15f : dt / 0.22f);
        if (g_alpha > 1.0f) g_alpha = 1.0f;
        if (g_closing && g_alpha <= 0.0f) { ::DestroyWindow(g_hwnd); g_closing = false; continue; }
        ::SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)(255.0f * (g_alpha < 0.0f ? 0.0f : g_alpha)), LWA_ALPHA);

        RenderFrame();

        frame++;
        if (frameLimit > 0 && frame >= frameLimit) break;
    }

    // End the Claude session: EOF on its stdin, then give it a moment to save
    // its session file before the job kills whatever's left.
    chat::Stop();
    for (DWORD t0 = ::GetTickCount(); chat::Get().running && ::GetTickCount() - t0 < 3500; )
    {
        chat::Pump();
        ::Sleep(20);
    }

    SaveSettings();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    if (::IsWindow(g_hwnd)) ::DestroyWindow(g_hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// WinMain, not main: a console-subsystem build would pop a cmd window beside it.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return Run();
}
