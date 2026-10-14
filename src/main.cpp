// =============================================================================
//  Hytale — external trainer.
//
//  A transparent, topmost DX11 + ImGui overlay covers the monitor the game is
//  on. INSERT opens the menu (the overlay then takes mouse input); with the
//  menu closed every click passes through to the game. ESP draws regardless.
//  Ctrl+END quits and restores everything the trainer changed.
// =============================================================================
#include "attach.h"
#include "camera.h"
#include "cent.h"
#include "esp_draw.h"
#include "game.h"
#include "ingame.h"
#include "log.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#ifndef TRAINER_TITLE
#define TRAINER_TITLE "HYTALE"
#endif

namespace {

HWND                    g_hwnd = nullptr;
ID3D11Device*           g_dev  = nullptr;
ID3D11DeviceContext*    g_ctx  = nullptr;
IDXGISwapChain*         g_swap = nullptr;
ID3D11RenderTargetView* g_rtv  = nullptr;
// DirectComposition path: a flip-model swap chain with premultiplied alpha,
// composed by DWM directly. The legacy path (blt-model swap chain on a layered
// window) makes DWM copy the whole monitor-sized surface through a redirection
// bitmap every frame and adds a frame of latency; it is kept only as fallback.
IDCompositionDevice*    g_dcomp   = nullptr;
IDCompositionTarget*    g_dtarget = nullptr;
IDCompositionVisual*    g_dvisual = nullptr;
HANDLE                  g_frameWait = nullptr;   // frame-latency waitable object
UINT                    g_swapFlags = 0;
bool                    g_composition = false;
bool                    g_open = true, g_interactive = false;
RECT                    g_mon{};          // overlay rect in screen coordinates

// ---------------------------------------------------------------------------
//  Window / device
// ---------------------------------------------------------------------------
void CreateRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (g_dev && wp != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, g_swapFlags);
            CreateRTV();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

HWND MakeWindow(DWORD extraEx) {
    return CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | extraEx,
        L"HytaleTrainerOverlay", L"Hytale Trainer", WS_POPUP,
        g_mon.left, g_mon.top, g_mon.right - g_mon.left, g_mon.bottom - g_mon.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool CreateComposition() {
    g_hwnd = MakeWindow(WS_EX_NOREDIRECTIONBITMAP);
    if (!g_hwnd) return false;
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);   // layered only for click-through
    ShowWindow(g_hwnd, SW_SHOW);

    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL fl;
    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* sc = nullptr;
    IDXGISwapChain2* sc2 = nullptr;
    bool ok = false;
    do {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     want, 2, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx))) break;
        if (FAILED(g_dev->QueryInterface(IID_PPV_ARGS(&dxdev)))) break;
        if (FAILED(dxdev->GetAdapter(&adapter))) break;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) break;

        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = (UINT)(g_mon.right - g_mon.left);
        d.Height = (UINT)(g_mon.bottom - g_mon.top);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;      // ImGui on a cleared target is premultiplied
        d.Scaling = DXGI_SCALING_STRETCH;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (FAILED(factory->CreateSwapChainForComposition(g_dev, &d, nullptr, &sc))) break;
        g_swapFlags = d.Flags;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc2)))) {
            // One frame queued at most: the camera is read right before the
            // frame that uses it is drawn, not two frames earlier.
            sc2->SetMaximumFrameLatency(1);
            g_frameWait = sc2->GetFrameLatencyWaitableObject();
        }

        if (FAILED(DCompositionCreateDevice(dxdev, IID_PPV_ARGS(&g_dcomp)))) break;
        if (FAILED(g_dcomp->CreateTargetForHwnd(g_hwnd, TRUE, &g_dtarget))) break;
        if (FAILED(g_dcomp->CreateVisual(&g_dvisual))) break;
        if (FAILED(g_dvisual->SetContent(sc))) break;
        if (FAILED(g_dtarget->SetRoot(g_dvisual))) break;
        if (FAILED(g_dcomp->Commit())) break;
        g_swap = sc;
        sc = nullptr;
        ok = true;
    } while (false);
    SafeRelease(sc2); SafeRelease(sc); SafeRelease(factory); SafeRelease(adapter); SafeRelease(dxdev);
    if (!ok) {
        if (g_frameWait) { CloseHandle(g_frameWait); g_frameWait = nullptr; }
        SafeRelease(g_dvisual); SafeRelease(g_dtarget); SafeRelease(g_dcomp);
        SafeRelease(g_ctx); SafeRelease(g_dev);
        g_swapFlags = 0;
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        return false;
    }
    CreateRTV();
    g_composition = true;
    return true;
}

bool CreateOverlay() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HytaleTrainerOverlay";
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_mon = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (CreateComposition()) { log_::Info("overlay: DirectComposition, flip model, latency 1"); return true; }
    log_::Info("overlay: DirectComposition unavailable, using the legacy swap chain");

    g_hwnd = MakeWindow(0);
    if (!g_hwnd) return false;
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    MARGINS m{ -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    ShowWindow(g_hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx))) return false;
    CreateRTV();
    return true;
}

// Click-through unless the menu is open.
void SetInteractive(bool on) {
    if (on == g_interactive) return;
    g_interactive = on;
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    ex = on ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
    if (on) SetForegroundWindow(g_hwnd);
}

// Keep the overlay on the monitor that shows the game.
void FollowGameMonitor(const game::Snapshot& s) {
    if (s.gameW <= 0) return;
    POINT c{ s.gameX + s.gameW / 2, s.gameY + s.gameH / 2 };
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) return;
    if (EqualRect(&mi.rcMonitor, &g_mon)) return;
    g_mon = mi.rcMonitor;
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_mon.left, g_mon.top,
        g_mon.right - g_mon.left, g_mon.bottom - g_mon.top, SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
//  Theme (crimson / slate, same palette as the other trainers)
// ---------------------------------------------------------------------------
const ImVec4 Red  (0.78f, 0.13f, 0.16f, 1.f);
const ImVec4 RedHi(0.92f, 0.22f, 0.24f, 1.f);

void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 3; s.FrameRounding = 2; s.GrabRounding = 2; s.PopupRounding = 2;
    s.ScrollbarRounding = 2; s.ChildRounding = 3; s.TabRounding = 2;
    s.WindowBorderSize = 1; s.FrameBorderSize = 1; s.ChildBorderSize = 1;
    s.WindowPadding = ImVec2(10, 10); s.ItemSpacing = ImVec2(8, 6);
    s.FramePadding = ImVec2(8, 5); s.IndentSpacing = 14;

    const ImVec4 bg(0.085f, 0.088f, 0.095f, 0.98f);
    const ImVec4 panel(0.13f, 0.135f, 0.145f, 1.f);
    const ImVec4 panelHi(0.17f, 0.175f, 0.185f, 1.f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.105f, 0.115f, 1.f);
    c[ImGuiCol_PopupBg] = bg;
    c[ImGuiCol_Border] = ImVec4(0.30f, 0.10f, 0.11f, 0.55f);
    c[ImGuiCol_Text] = ImVec4(0.85f, 0.86f, 0.88f, 1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.49f, 0.52f, 1.f);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = panelHi;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.10f, 0.11f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.10f, 0.11f, 0.75f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.13f, 0.15f, 0.85f);
    c[ImGuiCol_HeaderActive] = Red;
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.14f, 0.16f, 1.f);
    c[ImGuiCol_ButtonActive] = Red;
    c[ImGuiCol_CheckMark] = RedHi;
    c[ImGuiCol_SliderGrab] = Red;
    c[ImGuiCol_SliderGrabActive] = RedHi;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.25f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.32f, 1.f);
    c[ImGuiCol_ScrollbarGrabHovered] = Red;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.10f, 0.11f, 0.55f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.09f, 0.10f, 1.f);
}

// ---------------------------------------------------------------------------
//  Widgets bound to the worker's atomics
// ---------------------------------------------------------------------------
bool Check(const char* label, std::atomic<bool>& v) {
    bool b = v.load();
    if (ImGui::Checkbox(label, &b)) { v = b; return true; }
    return false;
}

bool SliderF(const char* label, std::atomic<float>& v, float lo, float hi, const char* fmt) {
    float f = v.load();
    if (ImGui::SliderFloat(label, &f, lo, hi, fmt)) { v = f; return true; }
    return false;
}

// Checkbox with its value slider on the same line.
void CheckSlider(const char* label, std::atomic<bool>& on, const char* id, std::atomic<float>& v, float lo, float hi, const char* fmt) {
    Check(label, on);
    ImGui::SameLine(210);
    ImGui::SetNextItemWidth(-1);
    SliderF(id, v, lo, hi, fmt);
}

void Section(const char* label) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.82f, 0.30f, 0.32f, 1.f), "%s", label);
    ImGui::Separator();
    ImGui::Spacing();
}

void Swatch(ImU32 col) {   // small colored square for the legend
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x + h * 0.6f, p.y + h * 0.8f), col);
    ImGui::Dummy(ImVec2(h * 0.6f, h));
}

void Bar(const char* label, float v, float mx, ImVec4 col) {
    float f = mx > 0 ? (std::max)(0.f, (std::min)(1.f, v / mx)) : 0.f;
    char ov[48];
    snprintf(ov, sizeof(ov), "%s  %.0f / %.0f", label, v, mx);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(f, ImVec2(-1, 16), ov);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
//  Per-frame inputs
//
//  The server sends a new picture of the world once per tick (~30 Hz). Drawn
//  as-is, every box would move in 33 ms steps however fast the overlay runs.
//  Interp blends the last two ticks over one tick interval, the same thing
//  the game client does with the entities it draws, so motion is continuous
//  at the overlay's frame rate. It costs one tick of delay on positions the
//  server reports; the camera itself is read live every frame.
// ---------------------------------------------------------------------------
struct Interp {
    std::shared_ptr<const game::Snapshot> cur, prev;
    std::unordered_map<uint64_t, game::Vec3> prevPos;
    double interval = 1.0 / 30.0;
    float  alpha = 1.f;
    game::Vec3 me{};

    static uint64_t Key(const game::Entity& e) { return ((uint64_t)(uint8_t)e.kind << 32) | (uint32_t)e.id; }
    static game::Vec3 Lerp(const game::Vec3& a, const game::Vec3& b, float t) {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    }

    void Update(std::shared_ptr<const game::Snapshot> s, double now, bool smooth) {
        if (!cur || s->seq != cur->seq || !s->havePlayer) {
            prev = cur;
            prevPos.clear();
            if (prev && prev->havePlayer && s->havePlayer && prev->world == s->world) {
                for (auto& e : prev->ents) prevPos[Key(e)] = e.pos;
                double dt = s->sampleTime - prev->sampleTime;
                if (dt > 0.008 && dt < 0.25) interval = interval * 0.8 + dt * 0.2;
            } else {
                prev = nullptr;
            }
        }
        cur = std::move(s);
        alpha = 1.f;
        if (smooth && prev) alpha = (float)std::clamp((now - cur->sampleTime) / interval, 0.0, 1.0);
        me = Pos(cur->pos, prev ? &prev->pos : nullptr);
    }
    game::Vec3 Pos(const game::Vec3& now, const game::Vec3* before) const {
        if (!before || alpha >= 1.f) return now;
        float dx = now.x - before->x, dy = now.y - before->y, dz = now.z - before->z;
        if (dx * dx + dy * dy + dz * dz > 64.f) return now;          // teleported: do not slide
        return Lerp(*before, now, alpha);
    }
    game::Vec3 Pos(const game::Entity& e) const {
        if (alpha >= 1.f) return e.pos;
        auto it = prevPos.find(Key(e));
        return Pos(e.pos, it == prevPos.end() ? nullptr : &it->second);
    }
};

Interp    g_interp;
cam::View g_view;          // read once per frame, shared by the ESP and the menu
bool      g_viewOk = false;

// Frame timing for the VISUALS tab and the bench log.
struct FrameStats {
    double last = 0, acc = 0, worst = 0;
    int    frames = 0;
    float  fps = 0, ms = 0, worstMs = 0;
    void Tick(double now) {
        if (last > 0) {
            double dt = now - last;
            acc += dt; worst = (std::max)(worst, dt); ++frames;
            if (acc >= 0.5) {
                fps = (float)(frames / acc); ms = (float)(acc / frames * 1000.0); worstMs = (float)(worst * 1000.0);
                acc = 0; worst = 0; frames = 0;
            }
        }
        last = now;
    }
} g_stats;

// Player stats overlay position (see DrawStatsHud).
ImVec2 g_hudOffset(16.f, 16.f);

std::filesystem::path HudPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path() / L"stats_overlay.txt";
}
void LoadHudOffset() {
    if (FILE* f = _wfopen(HudPath().c_str(), L"r")) {
        float x, y;
        if (fscanf(f, "%f %f", &x, &y) == 2 && x >= 0 && y >= 0 && x < 10000 && y < 10000) g_hudOffset = ImVec2(x, y);
        fclose(f);
    }
}
void SaveHudOffset() {
    if (FILE* f = _wfopen(HudPath().c_str(), L"w")) { fprintf(f, "%.0f %.0f\n", g_hudOffset.x, g_hudOffset.y); fclose(f); }
}

// How far the client camera sits behind the eye: zero in first person, a
// couple of blocks on the third-person boom arm.
float Boom(const cam::View& v, const game::Snapshot& s) {
    if (!v.valid || !s.havePlayer) return 0.f;
    // A camera-relative matrix solves to the origin until the camera position
    // itself has been found; then v.eye is the real camera.
    if (v.relative && std::fabs(v.eye[0]) + std::fabs(v.eye[1]) + std::fabs(v.eye[2]) < 1.f) return 0.f;
    float dx = v.eye[0] - s.pos.x, dy = v.eye[1] - (s.pos.y + s.eye), dz = v.eye[2] - s.pos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Shown wherever a feature is held back because the world is shared.
void SharedNote(const game::Snapshot& s) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.f, 0.72f, 0.30f, 1.f),
                       "%d players are in this world.", s.players);
    ImGui::TextWrapped("Features that change the world for everyone are held back. Turn them on "
                       "under SETTINGS if that is what you want.");
}

// ---------------------------------------------------------------------------
//  Menu
// ---------------------------------------------------------------------------
int         g_tab = 0;
const char* kTabs[] = { "PLAYER", "MOVEMENT", "COMBAT", "WORLD", "ITEMS", "VISUALS", "SETTINGS" };
bool        g_quit = false;
std::string g_cfgMsg;
float       g_hour = 12.f, g_tpX = 0, g_tpY = 100, g_tpZ = 0, g_tpDist = 20.f;
char        g_itemFilter[64] = "", g_wpName[48] = "";
int         g_itemSel = -1, g_itemQty = 1, g_weatherSel = 0;

void DrawBanner() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + 30), 0, 0, 0x33282CC8, 0x33282CC8);

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::TextColored(RedHi, TRAINER_TITLE);
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    bool on = game::Attached();
    ImVec4 col = on ? ImVec4(0.30f, 0.95f, 0.45f, 1.f) : ImVec4(0.95f, 0.65f, 0.25f, 1.f);
    ImVec2 d = ImGui::GetCursorScreenPos();
    float th = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(14, th));
    if (on) dl->AddCircleFilled(ImVec2(d.x + 9, d.y + th * 0.5f + 1), 4.f, ImGui::GetColorU32(col));
    else    dl->AddCircle(ImVec2(d.x + 9, d.y + th * 0.5f + 1), 4.f, ImGui::GetColorU32(col), 0, 1.5f);
    ImGui::SameLine();
    std::string st = on ? std::string("attached") : game::Status();
    ImGui::TextColored(col, "%s", st.c_str());
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 52);
    ImGui::TextColored(ImVec4(0.50f, 0.51f, 0.54f, 1.f), "[Insert]");
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Separator();
    ImGui::Spacing();
}

void DrawTabBar() {
    const int n = IM_ARRAYSIZE(kTabs);
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float tw = (ImGui::GetContentRegionAvail().x - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        bool active = g_tab == i;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? Red : ImVec4(0.14f, 0.145f, 0.155f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? RedHi : ImVec4(0.40f, 0.13f, 0.15f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, RedHi);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(1.f, 0.95f, 0.95f, 1.f) : ImVec4(0.62f, 0.63f, 0.66f, 1.f));
        if (ImGui::Button(kTabs[i], ImVec2(tw, 30))) g_tab = i;
        ImGui::PopStyleColor(4);
        if (i < n - 1) ImGui::SameLine();
    }
}

void TabPlayer(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("SURVIVAL");
    Check("God Mode (no damage)", c.god);
    Check("Infinite Health", c.infHp);
    Check("Infinite Stamina", c.infStam);
    Check("Infinite Mana", c.infMana);
    Check("Infinite Oxygen (breathe underwater)", c.infOxy);
    Check("No Fall Damage", c.noFall);

    Section("QUICK ACTIONS");
    if (ImGui::Button("Full Heal")) game::Action("heal");
    ImGui::SameLine();
    if (ImGui::Button("Clear Effects")) game::Action("clearfx");
    ImGui::TextDisabled("Full Heal also refills stamina, mana and oxygen.");

    Section("GAME MODE");
    bool creative = s.mode == "Creative";
    if (ImGui::RadioButton("Adventure", !creative) && creative) game::Action("gamemode adventure");
    ImGui::SameLine();
    if (ImGui::RadioButton("Creative", creative) && !creative) game::Action("gamemode creative");

    Section("EQUIPMENT");
    Check("Infinite Durability", c.infDur);
    ImGui::SameLine(220); Check("Infinite Ammo", c.infAmmo);
    if (ImGui::Button("Repair All Now")) game::Action("repair");
    ImGui::SameLine();
    ImGui::TextDisabled("tools, weapons and armour");

    if (s.havePlayer) {
        Section("STATUS");
        Bar("Health", s.hp, s.hpMax, ImVec4(0.80f, 0.20f, 0.22f, 1.f));
        Bar("Stamina", s.stam, s.stamMax, ImVec4(0.85f, 0.70f, 0.20f, 1.f));
        if (s.manaMax > 0) Bar("Mana", s.mana, s.manaMax, ImVec4(0.30f, 0.45f, 0.90f, 1.f));
        if (s.oxyMax > 0 && s.oxy < s.oxyMax) Bar("Oxygen", s.oxy, s.oxyMax, ImVec4(0.40f, 0.80f, 0.90f, 1.f));
    }
}

void TabMovement(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("MOVEMENT");
    CheckSlider("Speed", c.speedOn, "##speed", c.speed, 1.f, 6.f, "%.2fx");
    CheckSlider("Jump Height", c.jumpOn, "##jump", c.jump, 1.f, 5.f, "%.2fx");
    Check("Fly (double-tap jump)", c.fly);
    CheckSlider("Fly Speed", c.flySpeedOn, "##flyspeed", c.flySpeed, 1.f, 6.f, "%.2fx");
    Check("No Clip (walk through blocks)", c.noclip);
    ImGui::TextDisabled("No Clip is best used while flying.");

    Section("TELEPORT");
    if (ImGui::Button("Forward")) { char t[48]; snprintf(t, sizeof(t), "tpfwd %.1f", g_tpDist); game::Action(t); }
    ImGui::SameLine(); if (ImGui::Button("Up")) { char t[48]; snprintf(t, sizeof(t), "tpup %.1f", g_tpDist); game::Action(t); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(130); ImGui::SliderFloat("##tpd", &g_tpDist, 5.f, 200.f, "%.0f blocks");
    ImGui::SameLine(); if (ImGui::Button("World Spawn")) game::Action("tpspawn");

    ImGui::SetNextItemWidth(90); ImGui::InputFloat("##x", &g_tpX, 0, 0, "%.1f"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90); ImGui::InputFloat("##y", &g_tpY, 0, 0, "%.1f"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90); ImGui::InputFloat("##z", &g_tpZ, 0, 0, "%.1f"); ImGui::SameLine();
    if (ImGui::Button("Go to X Y Z")) { char t[96]; snprintf(t, sizeof(t), "tp %.2f %.2f %.2f", g_tpX, g_tpY, g_tpZ); game::Action(t); }
    if (s.havePlayer) {
        ImGui::TextDisabled("You are at  %.1f  %.1f  %.1f", s.pos.x, s.pos.y, s.pos.z);
        ImGui::SameLine();
        if (ImGui::SmallButton("copy")) { g_tpX = s.pos.x; g_tpY = s.pos.y; g_tpZ = s.pos.z; }
    }

    Section("SAVED POSITIONS");
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##wpn", "name", g_wpName, sizeof(g_wpName));
    ImGui::SameLine();
    if (ImGui::Button("Save Current Position")) { game::SaveWaypoint(g_wpName); g_wpName[0] = 0; }
    auto wps = game::Waypoints();
    for (size_t i = 0; i < wps.size(); ++i) {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("Go")) game::GotoWaypoint(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) game::DeleteWaypoint(i);
        ImGui::SameLine();
        ImGui::Text("%s", wps[i].name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f, %.0f, %.0f)", wps[i].pos.x, wps[i].pos.y, wps[i].pos.z);
        ImGui::PopID();
    }
}

void TabCombat(const game::Snapshot& s) {
    auto& c = game::Cfg();
    bool lock = game::SharedLocked();
    if (lock) SharedNote(s);

    Section("TARGETS");
    ImGui::TextDisabled("Who every combat feature on this tab may hit.");
    Check("Hostile", c.tgtHostile);
    ImGui::SameLine(150); Check("Neutral (NPCs & animals)", c.tgtNeutral);
    Check("Friendly", c.tgtFriendly);
    ImGui::SameLine(150); Check("Players", c.tgtPlayers);
    if (c.tgtPlayers.load())
        ImGui::TextDisabled("Players are only in your world when you host; combat features are\n"
                            "held back then unless shared-world features are allowed (SETTINGS).");

    Section("DAMAGE");
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 0.35f, 0.35f, 1.f));
    Check("One-Hit Kills", c.oneHit);
    ImGui::PopStyleColor();
    CheckSlider("Damage Multiplier", c.dmgOn, "##dmg", c.dmgMult, 1.f, 20.f, "%.1fx");
    ImGui::SetNextItemWidth(-130); SliderF("Hit Damage", c.aaDamage, 5.f, 500.f, "%.0f");
    ImGui::TextDisabled("Hit Damage is what Killaura, Reach and Auto Attack deal per hit; One-Hit\n"
                        "Kills and the multiplier apply on top. Everything counts as your hit.");
    if (!s.filterOk && s.attached) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Damage hooks unavailable in this game version.");

    Section("MELEE (ON SWING)");
    ImGui::TextDisabled("Fires on your own attacks (not while mining with a tool).");
    ImGui::BeginDisabled(lock);
    Check("Killaura - hit the nearest targets", c.swingAura);
    ImGui::SetNextItemWidth(-130); SliderF("Aura Range", c.aaRadius, 2.f, 16.f, "%.1f blocks");
    ImGui::SetNextItemWidth(-130); SliderF("Targets per Swing", c.auraMax, 1.f, 8.f, "%.0f");
    Check("Reach - hit what you aim at", c.swingReach);
    ImGui::SetNextItemWidth(-130); SliderF("Reach Distance", c.reachRange, 3.f, 32.f, "%.1f blocks");
    Check("Ghost Hit - pull the nearest target to you", c.ghostHit);
    ImGui::SetNextItemWidth(-130); SliderF("Ghost Range", c.ghostRange, 3.f, 32.f, "%.1f blocks");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Reach wins over Killaura when you aim at something.");
    if (s.havePlayer) ImGui::TextDisabled("swings %lld   hits %lld   pulled %lld", s.swings, s.hits, s.ghosted);

    Section("RANGED (PROJECTILES)");
    ImGui::TextDisabled("Arrows, spells and thrown items you fire. They pick the target nearest\n"
                        "their line of flight; the game's own collision makes the hit.");
    ImGui::BeginDisabled(lock);
    Check("Projectile Aimbot - steer into the target", c.projAim);
    Check("Magic Bullet - appear at the target, any distance", c.magicBullet);
    ImGui::SetNextItemWidth(-130); SliderF("Projectile Range", c.projRange, 8.f, 256.f, "%.0f blocks");
    ImGui::SetNextItemWidth(-130); SliderF("Aim Cone", c.projCone, 3.f, 180.f, "%.0f deg");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Aim Cone 180 = any target in range, wherever you shoot.");
    if (s.havePlayer && (c.projAim.load() || c.magicBullet.load()))
        ImGui::TextDisabled("steered %lld   placed %lld", s.homed, s.bullets);

    Section("AUTO ATTACK (TIMED)");
    ImGui::BeginDisabled(lock);
    Check("Auto Attack - hit the nearest target on a timer", c.autoAttack);
    Check("Only what you are facing", c.aaFrontOnly);
    ImGui::SetNextItemWidth(-130); SliderF("Hits / sec", c.aaRate, 0.5f, 10.f, "%.1f");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Uses Aura Range and Hit Damage.");
}

void TabWorld(const game::Snapshot& s) {
    auto& c = game::Cfg();
    bool lock = game::SharedLocked();
    if (lock) SharedNote(s);

    ImGui::BeginDisabled(lock);
    Section("TIME OF DAY");
    if (ImGui::Button("Dawn")) game::Action("time 5.5");
    ImGui::SameLine(); if (ImGui::Button("Noon")) game::Action("time 12");
    ImGui::SameLine(); if (ImGui::Button("Dusk")) game::Action("time 19.5");
    ImGui::SameLine(); if (ImGui::Button("Midnight")) game::Action("time 0");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("##hour", &g_hour, 0.f, 23.99f, "%.1f h");
    ImGui::SameLine();
    if (ImGui::Button("Set Time")) { char t[32]; snprintf(t, sizeof(t), "time %.2f", g_hour); game::Action(t); }
    Check("Freeze Time", c.freezeTime);
    if (s.havePlayer) {
        float h = s.dayProgress * 24.f;
        ImGui::SameLine();
        ImGui::TextDisabled("current time  %02d:%02d", (int)h, (int)((h - (int)h) * 60));
    }

    Section("WEATHER");
    auto ws = game::Weathers();
    if (ws.empty()) ImGui::TextDisabled("No weather list yet.");
    else {
        if (g_weatherSel >= (int)ws.size()) g_weatherSel = 0;
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##weather", ws[g_weatherSel].c_str())) {
            for (int i = 0; i < (int)ws.size(); ++i)
                if (ImGui::Selectable(ws[i].c_str(), i == g_weatherSel)) g_weatherSel = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Force")) game::Action("weather " + ws[g_weatherSel]);
        ImGui::SameLine();
        if (ImGui::Button("Natural")) game::Action("weather clear");
        ImGui::TextDisabled("Forced weather lasts until you pick Natural or close the trainer.");
    }

    Section("CREATURES");
    Check("Freeze Creatures Nearby", c.freezeMobs);
    ImGui::SameLine();
    ImGui::TextDisabled("(within ESP range)");
    ImGui::SetNextItemWidth(200);
    SliderF("Radius", c.killRadius, 8.f, 128.f, "%.0f blocks");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.10f, 0.10f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.15f, 0.15f, 1.f));
    char t[64];
    if (ImGui::Button("Kill Hostiles", ImVec2(160, 0))) { snprintf(t, sizeof(t), "killmobs %.0f hostile", c.killRadius.load()); game::Action(t); }
    ImGui::SameLine();
    if (ImGui::Button("Kill All Creatures", ImVec2(160, 0))) { snprintf(t, sizeof(t), "killmobs %.0f all", c.killRadius.load()); game::Action(t); }
    ImGui::PopStyleColor(2);
    ImGui::TextDisabled("Kills count as yours, so drops are awarded normally.");
    ImGui::EndDisabled();

    Section("MINING");
    Check("Instant Break - the block you look at breaks on the swing", c.instantBreak);
    ImGui::SetNextItemWidth(-130); SliderF("Break Reach", c.breakRange, 2.f, 32.f, "%.1f blocks");
    ImGui::TextDisabled("Normal drops and tool wear. Hold the button to keep breaking. Swings\n"
                        "that hit a creature never dig; unbreakable blocks stay put.");
    if (s.havePlayer && c.instantBreak.load()) ImGui::TextDisabled("blocks broken %lld", s.broken);
}

std::string Lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }

// Rarity colours by quality id (unknown ids stay neutral).
ImVec4 QualityColor(const std::string& q) {
    std::string l = Lower(q);
    if (l.find("legend") != std::string::npos) return ImVec4(1.00f, 0.62f, 0.20f, 1.f);
    if (l.find("epic") != std::string::npos)   return ImVec4(0.75f, 0.45f, 1.00f, 1.f);
    if (l.find("rare") != std::string::npos)   return ImVec4(0.35f, 0.62f, 1.00f, 1.f);
    if (l.find("uncommon") != std::string::npos) return ImVec4(0.40f, 0.90f, 0.45f, 1.f);
    if (l.find("junk") != std::string::npos || l.find("poor") != std::string::npos) return ImVec4(0.55f, 0.55f, 0.55f, 1.f);
    return ImVec4(0.86f, 0.87f, 0.89f, 1.f);
}

std::string g_itemGroup = "All", g_itemSub = "All", g_itemQuality = "All";

void TabItems() {
    static std::vector<game::ItemInfo> items;
    static size_t lastCount = 0;
    if (items.empty() || lastCount == 0) { items = game::Items(); lastCount = items.size(); }
    if (items.empty()) { Section("SPAWN ITEMS"); ImGui::TextDisabled("Item list arrives when the trainer attaches."); return; }

    // groups (item type) and their counts
    static std::map<std::string, int> groups;
    static std::vector<std::string> qualities;
    if (groups.empty()) {
        std::set<std::string> qs;
        for (auto& it : items) { groups[it.kind]++; if (!it.quality.empty()) qs.insert(it.quality); }
        qualities.assign(qs.begin(), qs.end());
    }

    Section("SPAWN ITEMS");
    // ---- left: groups
    ImGui::BeginChild("##groups", ImVec2(150, 330), ImGuiChildFlags_Borders);
    char lbl[96];
    snprintf(lbl, sizeof(lbl), "All  (%zu)", items.size());
    if (ImGui::Selectable(lbl, g_itemGroup == "All")) { g_itemGroup = "All"; g_itemSub = "All"; }
    for (auto& [g, n] : groups) {
        snprintf(lbl, sizeof(lbl), "%s  (%d)", g.c_str(), n);
        if (ImGui::Selectable(lbl, g_itemGroup == g)) { g_itemGroup = g; g_itemSub = "All"; }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- right: filters + table
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "search by name or id (e.g. iron sword, ore, potion)", g_itemFilter, sizeof(g_itemFilter));

    // sub-categories present in the current group
    std::set<std::string> subs;
    for (auto& it : items)
        if (g_itemGroup == "All" || it.kind == g_itemGroup) {
            std::string sc = !it.subCategory.empty() ? it.subCategory : it.category;
            if (!sc.empty()) subs.insert(sc);
        }
    ImGui::SetNextItemWidth(190);
    if (ImGui::BeginCombo("##sub", g_itemSub == "All" ? "All categories" : g_itemSub.c_str())) {
        if (ImGui::Selectable("All categories", g_itemSub == "All")) g_itemSub = "All";
        for (auto& sc : subs) if (ImGui::Selectable(sc.c_str(), g_itemSub == sc)) g_itemSub = sc;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##quality", g_itemQuality == "All" ? "All rarities" : g_itemQuality.c_str())) {
        if (ImGui::Selectable("All rarities", g_itemQuality == "All")) g_itemQuality = "All";
        for (auto& q : qualities) {
            ImGui::PushStyleColor(ImGuiCol_Text, QualityColor(q));
            if (ImGui::Selectable(q.c_str(), g_itemQuality == q)) g_itemQuality = q;
            ImGui::PopStyleColor();
        }
        ImGui::EndCombo();
    }

    std::string f = Lower(g_itemFilter);
    std::vector<int> shown;
    for (int i = 0; i < (int)items.size(); ++i) {
        auto& it = items[i];
        if (g_itemGroup != "All" && it.kind != g_itemGroup) continue;
        if (g_itemSub != "All" && (!it.subCategory.empty() ? it.subCategory : it.category) != g_itemSub) continue;
        if (g_itemQuality != "All" && it.quality != g_itemQuality) continue;
        if (!f.empty() && Lower(it.name).find(f) == std::string::npos && Lower(it.id).find(f) == std::string::npos) continue;
        shown.push_back(i);
    }

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##itemtable", 4, tf, ImVec2(-1, 262))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", 0, 3.f);
        ImGui::TableSetupColumn("Category", 0, 2.f);
        ImGui::TableSetupColumn("Rarity", 0, 1.2f);
        ImGui::TableSetupColumn("Stack", 0, 0.6f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)shown.size());
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                int i = shown[r];
                auto& it = items[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Text, QualityColor(it.quality));
                if (ImGui::Selectable(it.name.c_str(), g_itemSel == i, ImGuiSelectableFlags_SpanAllColumns)) g_itemSel = i;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    if (ImGui::IsMouseDoubleClicked(0)) game::Action("give " + it.id + " " + std::to_string(g_itemQty));
                    ImGui::SetTooltip("%s", it.id.c_str());
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", (!it.subCategory.empty() ? it.subCategory : it.category).c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(QualityColor(it.quality), "%s", it.quality.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%d", it.maxStack);
            }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%zu shown of %zu items", shown.size(), items.size());
    ImGui::EndGroup();

    // ---- give
    bool valid = g_itemSel >= 0 && g_itemSel < (int)items.size();
    ImGui::Spacing();
    if (valid) {
        auto& it = items[g_itemSel];
        ImGui::TextColored(QualityColor(it.quality), "%s", it.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s, stacks to %d)", it.id.c_str(), it.maxStack);
    } else {
        ImGui::TextDisabled("Select an item. Double-click gives it right away.");
    }
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("##qty", &g_itemQty, 1, 10);
    g_itemQty = (std::max)(1, (std::min)(g_itemQty, 9999));
    ImGui::SameLine(); if (ImGui::SmallButton("1")) g_itemQty = 1;
    ImGui::SameLine(); if (ImGui::SmallButton("10")) g_itemQty = 10;
    ImGui::SameLine(); if (ImGui::SmallButton("Stack") && valid) g_itemQty = items[g_itemSel].maxStack;
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Give", ImVec2(90, 0))) game::Action("give " + items[g_itemSel].id + " " + std::to_string(g_itemQty));
    ImGui::EndDisabled();

    auto& c = game::Cfg();
    auto snap = game::SnapshotPtr();
    Section("DROPS");
    ImGui::BeginDisabled(game::SharedLocked());
    Check("Item Vacuum - pull dropped items to you", c.vacuum);
    ImGui::EndDisabled();
    ImGui::SameLine(330); ImGui::SetNextItemWidth(-1);
    SliderF("##vacr", c.vacuumRadius, 2.f, 64.f, "%.0f blocks");
    Check("Instant Pickup - no pickup delay", c.fastPickup);
    ImGui::SameLine(330); ImGui::TextDisabled("items you throw keep theirs");
    Check("Duplicate new drops", c.autoDupe);
    ImGui::SameLine(330); ImGui::SetNextItemWidth(-1);
    SliderF("##dupe", c.dupeMult, 2.f, 16.f, "x%.0f");
    if (ImGui::Button("Duplicate drops nearby now")) {
        char t[64];
        snprintf(t, sizeof(t), "dupedrops %.0f %.0f", c.vacuumRadius.load(), c.dupeMult.load());
        game::Action(t);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(within the vacuum radius, xN)");
    ImGui::TextDisabled("Copies are real drops spawned the game's own way; they are never copied\n"
                        "again. Vacuumed items are picked up normally, so a full bag is respected.");
    if (snap->havePlayer) ImGui::TextDisabled("pulled %lld   copies made %lld", snap->pulled, snap->duped);
}

void TabVisuals() {
    auto& c = game::Cfg();
    Section("ESP");
    Check("2D Box ESP (master)", c.esp);
    Check("Snaplines", c.espLines);
    ImGui::SameLine(150); Check("Name / Distance", c.espInfo);
    ImGui::SameLine(300); Check("Health Bars", c.espHealth);
    ImGui::SetNextItemWidth(-90);
    SliderF("Range", c.espMaxDist, 16.f, 256.f, "%.0f blocks");

    Section("TARGETS");
    Check("Hostile", c.espHostile);
    ImGui::SameLine(150); Check("Neutral (NPCs & animals)", c.espNeutral);
    Check("Friendly", c.espFriendly);
    ImGui::SameLine(150); Check("Players", c.espPlayers);
    Check("Dropped items", c.espItems);

    Section("PLAYER STATS OVERLAY");
    Check("Show my stats on screen", c.statsHud);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset Position")) { g_hudOffset = ImVec2(16.f, 16.f); SaveHudOffset(); }
    ImGui::TextDisabled("While this menu is open, drag the overlay anywhere on the game view.");
    ImGui::TextDisabled("Health, stamina, mana, oxygen, position, speed, heading, time.");

    Section("CAMERA");
    Check("Read the camera from the game client", c.useClientCam);
    const cam::View& v = g_view;
    if (g_viewOk) {
        auto snp = game::SnapshotPtr();
        float boom = Boom(v, *snp);
        ImGui::TextColored(ImVec4(0.30f, 0.95f, 0.45f, 1.f), "client camera: locked");
        ImGui::TextDisabled("FOV %.1f (detected), aspect %.3f, %s person", v.fovY * 180.f / 3.14159265f, v.aspect,
                            boom > 0.75f ? "third" : "first");
    } else if (c.useClientCam.load()) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.f), "client camera: %s", cam::Status().c_str());
        ImGui::TextDisabled("Walk and look around for a few seconds to help it confirm.");
    }
    ImGui::TextDisabled("Field of view, third person and camera position are all detected from\n"
                        "the game; there is nothing to set by hand.");
    ImGui::TextDisabled("entities: %s", cent::Status().c_str());

    Section("PERFORMANCE");
    Check("Draw inside the game (in-game renderer)", c.inGameEsp);
    if (c.inGameEsp.load()) {
        bool on = ingame::Active();
        ImGui::TextColored(on ? ImVec4(0.30f, 0.95f, 0.45f, 1.f) : ImVec4(0.95f, 0.65f, 0.25f, 1.f),
                           "in-game renderer: %s", ingame::Status().c_str());
    }
    Check("Smooth movement between server ticks", c.espSmooth);
    Check("Use the server view until the camera is found", c.espServerFallback);
    ImGui::TextDisabled("Overlay %.0f fps  |  %s  |  server tick %.0f ms", g_stats.fps,
                        g_composition ? "DirectComposition" : "legacy swap chain", g_interp.interval * 1000.0);

    Section("LEGEND");
    Swatch(IM_COL32(255, 77, 77, 255));  ImGui::SameLine(); ImGui::TextDisabled("hostile");
    ImGui::SameLine(); Swatch(IM_COL32(255, 200, 60, 255)); ImGui::SameLine(); ImGui::TextDisabled("neutral");
    ImGui::SameLine(); Swatch(IM_COL32(70, 230, 110, 255)); ImGui::SameLine(); ImGui::TextDisabled("friendly");
    ImGui::SameLine(); Swatch(IM_COL32(235, 130, 255, 255)); ImGui::SameLine(); ImGui::TextDisabled("player");
    ImGui::SameLine(); Swatch(IM_COL32(120, 190, 255, 255)); ImGui::SameLine(); ImGui::TextDisabled("item");
}

void TabSettings() {
    Section("CONFIG");
    if (ImGui::Button("Save Settings")) g_cfgMsg = game::SaveConfig();
    ImGui::SameLine();
    if (ImGui::Button("Load Settings")) g_cfgMsg = game::LoadConfig();
    if (!g_cfgMsg.empty()) ImGui::TextWrapped("%s", g_cfgMsg.c_str());
    ImGui::TextDisabled("Settings auto-load on attach. No Clip, Freeze Creatures and\nFreeze Time always start off.");

    Section("HOTKEYS");
    ImGui::TextDisabled("Insert      open / close this menu");
    ImGui::TextDisabled("Ctrl+End    quit (restores everything)");

    Section("SHARED WORLDS");
    auto& c = game::Cfg();
    Check("Allow features that change the world for everyone", c.allowShared);
    ImGui::TextWrapped("While another player is in your world, time, weather, freezing and killing "
                       "creatures and auto attack are held back, because they change the world for "
                       "them too. Everything that only affects you (god mode, flight, speed, items, "
                       "teleport, ESP) keeps working. This switch is not saved and starts off each "
                       "session.");

    Section("ABOUT");
    ImGui::TextWrapped("Works in single-player worlds, including ones you have opened to other "
                       "players. Servers you join are never touched: the trainer only attaches to "
                       "a local single-player server started by your own client, so on a remote "
                       "server it stays detached and nothing can be switched on.");
}

void DrawFooter(const game::Snapshot& s) {
    ImGui::Separator();
    if (!game::Attached()) {
        if (game::ClientOnly())
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1),
                "Hytale is running but there is no local server: you are online or in a menu. "
                "The trainer works on worlds you open or host yourself.");
        else
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1), "Open a single-player world in Hytale.");
        return;
    }
    std::string r = game::LastResult();
    if (!r.empty()) ImGui::TextColored(ImVec4(0.65f, 0.75f, 0.65f, 1.f), "%s", r.c_str());
    else if (!s.havePlayer) ImGui::TextColored(ImVec4(0.45f, 0.46f, 0.49f, 1.f), "attached  |  loading world");
    else {
        const char* camTxt = g_viewOk ? "client camera" : "server view";
        if (s.shared)
            ImGui::TextColored(ImVec4(1.f, 0.72f, 0.30f, 1.f), "attached  |  %s  |  %s  |  %d players  |  %s",
                               s.world.c_str(), s.mode.c_str(), s.players, camTxt);
        else
            ImGui::TextColored(ImVec4(0.45f, 0.46f, 0.49f, 1.f), "attached  |  %s  |  %s  |  %s",
                               s.world.c_str(), s.mode.c_str(), camTxt);
    }
}

void DrawExitButton() {
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 60);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.10f, 0.10f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.15f, 0.15f, 1.f));
    if (ImGui::Button("EXIT", ImVec2(60, 0))) g_quit = true;
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore everything and close the trainer");
}

void DrawMenu(const game::Snapshot& s) {
    ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::Begin("##trainer", &g_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    DrawBanner();
    DrawTabBar();
    ImGui::Spacing();
    ImGui::BeginChild("##content", ImVec2(0, -34), ImGuiChildFlags_Borders);
    switch (g_tab) {
    case 0: TabPlayer(s); break;
    case 1: TabMovement(s); break;
    case 2: TabCombat(s); break;
    case 3: TabWorld(s); break;
    case 4: TabItems(); break;
    case 5: TabVisuals(); break;
    case 6: TabSettings(); break;
    }
    ImGui::EndChild();
    DrawFooter(s);
    DrawExitButton();
    ImGui::End();
}

// ---------------------------------------------------------------------------
//  ESP
//
//  First choice is the client's own world->clip matrix (camera.h): it is the
//  matrix the frame on screen was drawn with, so boxes sit still, follow the
//  mouse exactly and are already correct in third person and at any field of
//  view. If that is not available the camera is rebuilt from the server's
//  view of the player instead: eye position plus head pitch/yaw (radians;
//  forward = (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch), Y up) and
//  the field of view from the Game FOV slider. That fallback is a tick or two
//  behind the mouse and assumes first person.
// ---------------------------------------------------------------------------
// Projector for the overlay: the client matrix when it is locked, else the
// server view. Overlay pixels are monitor-relative, so the game's client area
// is offset by the monitor origin.
espdraw::Projector MakeProjector(const game::Snapshot& s) {
    espdraw::Projector pj;
    pj.cx = s.gameX - g_mon.left + s.gameW * 0.5f;
    pj.cy = s.gameY - g_mon.top + s.gameH * 0.5f;
    pj.halfW = s.gameW * 0.5f;
    pj.halfH = s.gameH * 0.5f;
    const float eye[3] = { g_interp.me.x, g_interp.me.y + s.eye, g_interp.me.z };
    if (g_viewOk) {
        pj.client = true;
        memcpy(pj.m, g_view.m, sizeof(pj.m));
        pj.layout = g_view.layout;
        pj.xSign = g_view.xSign;
        pj.ySign = g_view.ySign;
        // Camera-relative matrix without the client's own camera position:
        // use the interpolated eye, so walking glides instead of stepping and
        // stays on the same timeline as the interpolated entities.
        if (g_view.relative) memcpy(pj.origin, g_view.originFromClient ? g_view.origin : eye, sizeof(pj.origin));
        return pj;
    }
    pj.SetServerView(eye, s.pitch, s.yaw, game::Cfg().fov.load());
    return pj;
}

espdraw::Options EspOptions() {
    auto& c = game::Cfg();
    espdraw::Options o;
    o.hostile = c.espHostile; o.neutral = c.espNeutral; o.friendly = c.espFriendly;
    o.players = c.espPlayers; o.items = c.espItems;
    o.lines = c.espLines; o.info = c.espInfo; o.health = c.espHealth;
    o.maxDist = c.espMaxDist;
    return o;
}

// ---------------------------------------------------------------------------
//  Player stats overlay: a small panel over the game view. While the menu is
//  open it is an ordinary ImGui window you can drag anywhere on the game view;
//  while the menu is closed it is fixed and clicks pass through to the game.
//  Its offset from the game view's top-left is kept in stats_overlay.txt, so
//  it stays put when the game window moves and between sessions.
// ---------------------------------------------------------------------------
void DrawStatsHud(const game::Snapshot& s) {
    auto& c = game::Cfg();
    if (!c.statsHud || !s.havePlayer || s.gameW <= 0) return;
    HWND fg = GetForegroundWindow();
    if (IsIconic(GetAncestor(fg, GA_ROOT))) return;

    // speed over ground from the drawn position, smoothed
    static game::Vec3 lastPos{};
    static double lastT = 0;
    static float speed = 0;
    double now = game::NowSeconds();
    game::Vec3 p = g_interp.me;
    if (lastT > 0 && now > lastT) {
        float dx = p.x - lastPos.x, dz = p.z - lastPos.z;
        float inst = std::sqrt(dx * dx + dz * dz) / (float)(now - lastT);
        if (inst < 100.f) speed = speed * 0.9f + inst * 0.1f;
    }
    lastPos = p; lastT = now;

    const float W = 230, lh = 15;
    int rows = 4 + (s.manaMax > 0) + (s.oxyMax > 0 && s.oxy < s.oxyMax);
    const float H = 30 + rows * 18 + 4 * lh;
    float gx = (float)(s.gameX - g_mon.left), gy = (float)(s.gameY - g_mon.top);
    auto clampOffset = [&](ImVec2 o) {
        return ImVec2((std::max)(0.f, (std::min)(o.x, (float)s.gameW - W)), (std::max)(0.f, (std::min)(o.y, (float)s.gameH - H)));
    };

    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!g_open) fl |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    static bool wasOpen = false;
    // Placed from the saved offset, except while you are dragging it.
    if (!g_open || !wasOpen) {
        g_hudOffset = clampOffset(g_hudOffset);
        ImGui::SetNextWindowPos(ImVec2(gx + g_hudOffset.x, gy + g_hudOffset.y), ImGuiCond_Always);
    }
    wasOpen = g_open;
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##statsoverlay", nullptr, fl);
    ImVec2 wp = ImGui::GetWindowPos();
    bool hovered = g_open && ImGui::IsWindowHovered();
    if (g_open) {
        ImVec2 o = clampOffset(ImVec2(wp.x - gx, wp.y - gy));      // never off the game view
        if (o.x != wp.x - gx || o.y != wp.y - gy) { wp = ImVec2(gx + o.x, gy + o.y); ImGui::SetWindowPos(wp); }
        bool moved = o.x != g_hudOffset.x || o.y != g_hudOffset.y;
        g_hudOffset = o;
        static bool dirty = false;
        if (moved) dirty = true;
        if (dirty && !ImGui::IsMouseDown(0)) { SaveHudOffset(); dirty = false; }   // drag finished
    }
    float x = wp.x, y = wp.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = 13.f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + W, y + H), IM_COL32(16, 17, 19, 205), 6.f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + W, y + H), hovered ? IM_COL32(235, 70, 75, 255) : IM_COL32(200, 40, 45, 140), 6.f,
                0, hovered ? 2.f : 1.f);
    dl->AddText(font, fs, ImVec2(x + 10, y + 7), IM_COL32(235, 70, 75, 255), "PLAYER");
    char t[96];
    snprintf(t, sizeof(t), "%s  |  %s", s.mode.c_str(), s.world.c_str());
    ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0, t);
    dl->AddText(font, fs, ImVec2(x + W - 10 - tsz.x, y + 7), IM_COL32(150, 152, 158, 255), t);

    float by = y + 28;
    auto bar = [&](const char* name, float v, float mx, ImU32 col) {
        float f = mx > 0 ? (std::max)(0.f, (std::min)(1.f, v / mx)) : 0.f;
        ImVec2 a(x + 10, by), b(x + W - 10, by + 14);
        dl->AddRectFilled(a, b, IM_COL32(40, 42, 46, 255), 3.f);
        if (f > 0) dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * f, b.y), col, 3.f);
        char l[64];
        snprintf(l, sizeof(l), "%s  %.0f / %.0f", name, v, mx);
        dl->AddText(font, 12.f, ImVec2(a.x + 6, a.y + 1), IM_COL32(245, 245, 245, 255), l);
        by += 18;
    };
    bar("Health", s.hp, s.hpMax, IM_COL32(200, 52, 56, 255));
    bar("Stamina", s.stam, s.stamMax, IM_COL32(215, 175, 50, 255));
    if (s.manaMax > 0) bar("Mana", s.mana, s.manaMax, IM_COL32(75, 115, 230, 255));
    if (s.oxyMax > 0 && s.oxy < s.oxyMax) bar("Oxygen", s.oxy, s.oxyMax, IM_COL32(100, 205, 230, 255));

    // heading: yaw 0 looks along -Z (north)
    float fx = -std::sin(s.yaw), fz = -std::cos(s.yaw);
    float hdg = std::atan2(fx, -fz) * 180.f / 3.14159265f;
    if (hdg < 0) hdg += 360.f;
    const char* dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    float h = s.dayProgress * 24.f;
    const ImU32 txt = IM_COL32(205, 207, 212, 255);
    by += 2;
    snprintf(t, sizeof(t), "XYZ  %.1f  %.1f  %.1f", p.x, p.y, p.z);
    dl->AddText(font, fs, ImVec2(x + 10, by), txt, t); by += lh;
    snprintf(t, sizeof(t), "Speed  %.1f b/s     Facing  %s %.0f", speed, dirs[(int)((hdg + 22.5f) / 45.f) % 8], hdg);
    dl->AddText(font, fs, ImVec2(x + 10, by), txt, t); by += lh;
    snprintf(t, sizeof(t), "Time  %02d:%02d     Players  %d", (int)h, (int)((h - (int)h) * 60), s.players);
    dl->AddText(font, fs, ImVec2(x + 10, by), txt, t); by += lh;
    snprintf(t, sizeof(t), "Overlay  %.0f fps", g_stats.fps);
    dl->AddText(font, fs, ImVec2(x + 10, by), IM_COL32(140, 142, 148, 255), t);
    if (g_open) {
        const char* hint = "drag to move";
        ImVec2 hs = font->CalcTextSizeA(11.f, FLT_MAX, 0, hint);
        dl->AddText(font, 11.f, ImVec2(x + W - 10 - hs.x, by + 1), hovered ? IM_COL32(235, 70, 75, 255) : IM_COL32(110, 112, 118, 255), hint);
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// External overlay ESP: the fallback when the in-game renderer is not drawing.
void DrawEsp(const game::Snapshot& s) {
    auto& c = game::Cfg();
    if (!c.esp || !s.havePlayer || s.gameW <= 0) return;
    if (ingame::Active()) return;                 // the game is drawing it, in its own frame
    if (!g_viewOk && !c.espServerFallback) return; // no laggy boxes while the camera is being found
    HWND fg = GetForegroundWindow();
    if (IsIconic(GetAncestor(fg, GA_ROOT))) return;

    static std::vector<espdraw::Ent> ents;
    ents.clear();
    for (auto& e : s.ents) {
        game::Vec3 p = g_interp.Pos(e);
        ents.push_back({ (char)e.kind, { p.x, p.y, p.z }, e.height, e.hp, e.maxHp, e.name.c_str() });
    }
    float me[3] = { g_interp.me.x, g_interp.me.y, g_interp.me.z };
    cent::Layout cl;
    if (cent::GetLayout(cl)) {
        static std::vector<std::array<float, 3>> cpos;
        cpos.clear();
        for (uintptr_t o : cent::Objects()) {
            float v[3];
            if (cent::ReadPos(o, cl, v)) cpos.push_back({ v[0], v[1], v[2] });
        }
        bool meClient;
        espdraw::MatchClient(ents.data(), (int)ents.size(), (const float(*)[3])cpos.data(), (int)cpos.size(), me, meClient);
    }
    ImVec2 snap(s.gameX - g_mon.left + s.gameW * 0.5f, s.gameY - g_mon.top + (float)s.gameH);
    espdraw::Draw(ImGui::GetBackgroundDrawList(), ImGui::GetFont(), MakeProjector(s), EspOptions(), me,
                  ents.data(), (int)ents.size(),
                  (float)(g_mon.right - g_mon.left), (float)(g_mon.bottom - g_mon.top), snap);
}

// Hands this frame's ESP data to the in-game renderer. It interpolates and
// projects on its own, at the moment the game presents.
void PublishInGame(const game::Snapshot& s) {
    shm::Block* b = ingame::BeginWrite();
    if (!b) return;
    auto& c = game::Cfg();
    b->enabled = c.esp && s.havePlayer && s.gameW > 0;

    cam::Lock lk;
    if (c.useClientCam && cam::GetLock(lk)) {
        b->camAddr = lk.addr; b->camAddrAlt = lk.addrAlt; b->layout = lk.layout; b->negate = lk.negate; b->relative = lk.relative;
        b->xSign = lk.xSign; b->ySign = lk.ySign;
        b->originAddr = lk.origin; b->originDbl = lk.originDbl;
    } else {
        b->camAddr = 0;
    }
    b->pitch = s.pitch; b->yaw = s.yaw; b->eyeHeight = s.eye; b->fovDeg = c.fov;

    b->smooth = c.espSmooth;
    b->curTimeMs = s.sampleTime * 1000.0;           // steady_clock and QPC share a clock on Windows
    b->intervalMs = g_interp.interval * 1000.0;
    const game::Snapshot* prev = g_interp.prev.get();
    b->me[0] = s.pos.x; b->me[1] = s.pos.y; b->me[2] = s.pos.z;
    b->meHasPrev = prev != nullptr;
    if (prev) { b->mePrev[0] = prev->pos.x; b->mePrev[1] = prev->pos.y; b->mePrev[2] = prev->pos.z; }

    espdraw::Options o = EspOptions();
    b->hostile = o.hostile; b->neutral = o.neutral; b->friendly = o.friendly; b->players = o.players;
    b->items = o.items; b->lines = o.lines; b->info = o.info; b->health = o.health; b->maxDist = o.maxDist;
    b->serverFallback = c.espServerFallback;

    int n = 0;
    for (auto& e : s.ents) {
        if (n >= shm::kMaxEnts) break;
        shm::Ent& d = b->ents[n++];
        d.kind = (char)e.kind;
        d.id = e.id;
        d.pos[0] = e.pos.x; d.pos[1] = e.pos.y; d.pos[2] = e.pos.z;
        auto it = g_interp.prevPos.find(Interp::Key(e));
        d.hasPrev = it != g_interp.prevPos.end();
        if (d.hasPrev) { d.prev[0] = it->second.x; d.prev[1] = it->second.y; d.prev[2] = it->second.z; }
        d.height = e.height; d.hp = e.hp; d.maxHp = e.maxHp;
        strncpy_s(d.name, e.name.c_str(), _TRUNCATE);
    }
    b->count = n;

    cent::Layout cl;
    if (cent::GetLayout(cl)) {
        auto objs = cent::Objects();
        int k = 0;
        for (uintptr_t o : objs) { if (k >= shm::kMaxObjs) break; b->objs[k++] = o; }
        b->objCount = k;
        b->entPosOff = cl.posOff;
        b->entSlot = cent::kRenderSlot;
        b->entMt = cl.mt;
        b->entMt2 = cl.mt2;
        b->playerObj = cent::PlayerObject();
    } else {
        b->entMt = 0;
        b->objCount = 0;
        b->playerObj = 0;
    }
    ingame::EndWrite();
}

HWND GameWindow(const game::Snapshot& s) {
    if (s.gameW <= 0) return nullptr;
    POINT c{ s.gameX + s.gameW / 2, s.gameY + s.gameH / 2 };
    return GetAncestor(WindowFromPoint(c), GA_ROOT);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        log_::Init((std::filesystem::path(buf).parent_path() / L"trainer.log").c_str());
    }
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc >= 3 && wcscmp(argv[1], L"--entprobe") == 0) {
            log_::Info("==================== entprobe");
            int rc = game::EntProbe(argv[2]);
            LocalFree(argv);
            log_::Shutdown();
            return rc;
        }
        if (argc >= 3 && wcscmp(argv[1], L"--selftest") == 0) {
            log_::Info("==================== selftest");
            int rc = game::SelfTest(argv[2], argc >= 4 ? argv[3] : nullptr);
            LocalFree(argv);
            log_::Shutdown();
            return rc;
        }
        if (argc >= 5 && wcscmp(argv[1], L"--attachtest") == 0) {   // <pid> <agent.jar> <options>: attach only
            auto narrow = [](const wchar_t* w) { char b[2048]; WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof(b), nullptr, nullptr); return std::string(b); };
            std::string err;
            bool ok = attach::LoadAgent((DWORD)_wtoi(argv[2]), narrow(argv[3]), narrow(argv[4]), err);
            log_::Info("attachtest: %s %s", ok ? "OK" : "FAIL", err.c_str());
            LocalFree(argv);
            log_::Shutdown();
            return ok ? 0 : 1;
        }
        LocalFree(argv);
    }
    // --bench N: run the overlay N seconds with the menu closed, log frame
    // timing once a second, then exit normally (everything is restored).
    double benchSeconds = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc >= 3 && wcscmp(argv[1], L"--bench") == 0) benchSeconds = _wtof(argv[2]);
        LocalFree(argv);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    log_::Info("==================== Hytale trainer  build " __DATE__ " " __TIME__);
    if (!CreateOverlay()) { log_::Error("overlay creation failed"); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ApplyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    LoadHudOffset();
    if (benchSeconds > 0) g_open = false;
    else SetInteractive(true);
    game::StartWorker();
    const double benchEnd = benchSeconds > 0 ? game::NowSeconds() + benchSeconds : 0;
    double nextBenchLog = game::NowSeconds() + 1.0;

    bool prevIns = false, running = true;
    HWND lastGame = nullptr;
    MSG msg;
    while (running) {
        // Block until the compositor can take another frame, so everything
        // below is sampled as late as possible before it is shown.
        if (g_frameWait) WaitForSingleObjectEx(g_frameWait, 100, TRUE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        const double now = game::NowSeconds();
        g_stats.Tick(now);
        if (benchEnd > 0) {
            if (now >= nextBenchLog) {
                nextBenchLog = now + 1.0;
                log_::Info("[bench] %.1f fps  %.2f ms avg  %.1f ms worst  %s  camera=%s  tick=%.0f ms",
                           g_stats.fps, g_stats.ms, g_stats.worstMs,
                           g_composition ? "dcomp" : "legacy", g_viewOk ? "client" : cam::Status().c_str(),
                           g_interp.interval * 1000.0);
            }
            if (now >= benchEnd) break;
        }

        auto snap = game::SnapshotPtr();
        g_interp.Update(snap, now, game::Cfg().espSmooth.load());
        g_viewOk = game::Cfg().useClientCam.load() && cam::Read(g_view) && g_view.valid;
        const game::Snapshot& s = *snap;
        PublishInGame(s);
        if (!g_open) if (HWND gw = GameWindow(s); gw && gw != g_hwnd) lastGame = gw;

        bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (ins && !prevIns) {
            g_open = !g_open;
            if (!g_open && lastGame) SetForegroundWindow(lastGame);
        }
        prevIns = ins;
        if (g_quit || ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_END) & 0x8000))) break;

        FollowGameMonitor(s);
        SetInteractive(g_open);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (g_viewOk) game::Cfg().fov = g_view.fovY * 180.f / 3.14159265f;   // detected; used only by the fallback
        DrawEsp(s);
        DrawStatsHud(s);
        if (g_open) DrawMenu(s);
        ImGui::Render();
        const float clear[4] = { 0, 0, 0, 0 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
    log_::Info("overlay: %.1f fps at exit (%s)", g_stats.fps, g_composition ? "dcomp" : "legacy");

    game::StopWorker();       // agent restores everything we changed
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_frameWait) CloseHandle(g_frameWait);
    SafeRelease(g_dvisual); SafeRelease(g_dtarget); SafeRelease(g_dcomp);
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(g_hwnd);
    log_::Info("clean exit");
    log_::Shutdown();
    return 0;
}
