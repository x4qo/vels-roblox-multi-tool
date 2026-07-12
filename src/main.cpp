// Vels Multi Tool - Dear ImGui front-end (Win32 + DirectX 11 backend).
// All Roblox/cookie/MAC logic lives in backend.cpp; this file is UI only.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <wincodec.h>
#include <shellapi.h>
#include <tchar.h>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <filesystem>
#include <random>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cctype>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "backend.h"
#include "login.h"
#include "resource.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// D3D11 boilerplate (standard Dear ImGui example pattern)
// ---------------------------------------------------------------------------
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}
static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// ---------------------------------------------------------------------------
// Image loading: decode downloaded PNG bytes (WIC) into a D3D11 texture.
// Decoding happens on the UI thread on demand (cheap for small thumbnails);
// only the network download is backgrounded, in backend.cpp.
// ---------------------------------------------------------------------------
static ID3D11ShaderResourceView* CreateTextureFromImageBytes(const std::vector<unsigned char>& bytes) {
    if (bytes.empty() || !g_pd3dDevice) return nullptr;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return nullptr;

    ID3D11ShaderResourceView* srv = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {

        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w > 0 && h > 0) {
            std::vector<BYTE> pixels((size_t)w * h * 4);
            if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data()))) {
                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = w;
                desc.Height = h;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA sub = {};
                sub.pSysMem = pixels.data();
                sub.SysMemPitch = w * 4;

                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex))) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    g_pd3dDevice->CreateShaderResourceView(tex, &srvDesc, &srv);
                    tex->Release();
                }
            }
        }
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return srv;
}

// Keyed by Roblox userId. A cached nullptr means "decode already attempted and failed" - avoids retrying every frame.
static std::unordered_map<long long, ID3D11ShaderResourceView*> g_avatarTextures;
static ID3D11ShaderResourceView* g_placeIconTexture = nullptr;
static long long g_placeIconForPlaceId = 0;

static ID3D11ShaderResourceView* GetOrCreateAvatarTexture(const RobloxAccount& acc) {
    auto it = g_avatarTextures.find(acc.userId);
    if (it != g_avatarTextures.end()) return it->second;
    if (!acc.avatarLoaded || acc.avatarPng.empty()) return nullptr;
    ID3D11ShaderResourceView* srv = CreateTextureFromImageBytes(acc.avatarPng);
    g_avatarTextures[acc.userId] = srv;
    return srv;
}

// Caller must already hold (and release) backend::placeInfoMutex before calling this -
// it touches no shared state itself, so it's safe to call with or without that lock held.
static ID3D11ShaderResourceView* GetOrCreatePlaceIconTexture(long long placeId, const std::vector<unsigned char>& iconPng) {
    if (iconPng.empty()) return nullptr;
    if (g_placeIconForPlaceId == placeId) return g_placeIconTexture;
    if (g_placeIconTexture) { g_placeIconTexture->Release(); g_placeIconTexture = nullptr; }
    g_placeIconTexture = CreateTextureFromImageBytes(iconPng);
    g_placeIconForPlaceId = placeId;
    return g_placeIconTexture;
}

// Client-space rects of the custom titlebar's min/max/close buttons, updated
// every frame by DrawTitleBar() so WM_NCHITTEST can carve them out of the
// otherwise-draggable caption area.
static std::wstring g_exeDir;
static RECT g_btnMinRect = {};
static RECT g_btnMaxRect = {};
static RECT g_btnCloseRect = {};
const int TITLEBAR_H = 34;

// Per-widget animation state: smoothly eases toward +1 (hovered) or -1
// (pressed), settling at 0 at rest, so buttons/nav items/chips "pop" in and
// out instead of snapping instantly between style states. Declared up here
// (rather than down with the rest of the UI helpers) so WndProc can clear it
// directly on focus changes - see WM_ACTIVATE below.
static std::unordered_map<ImGuiID, float> g_widgetAnim;
static int g_pendingNavPage = -1;

// Same radius as the decorative border main() draws just inside the window
// edge. The window's *actual* shape needs to match that radius exactly -
// otherwise the area between the drawn rounded outline and the window's
// real (square) physical corner shows through as a solid near-black sliver,
// which is what looked like "black corners" leaking out. Maximized windows
// stay a plain rectangle (rounding a window flush against the screen edge
// looks broken), everything else gets the rounded region.
const int WINDOW_CORNER_RADIUS = 12;

static void ApplyWindowShape(HWND hWnd, int width, int height) {
    if (width <= 0 || height <= 0) return;
    HRGN rgn = IsZoomed(hWnd)
        ? CreateRectRgn(0, 0, width, height)
        : CreateRoundRectRgn(0, 0, width, height, WINDOW_CORNER_RADIUS * 2, WINDOW_CORNER_RADIUS * 2);
    SetWindowRgn(hWnd, rgn, TRUE);
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 1000;
        mmi->ptMinTrackSize.y = 680;
        return 0;
    }
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        ApplyWindowShape(hWnd, (int)LOWORD(lParam), (int)HIWORD(lParam));
        return 0;
    case WM_ERASEBKGND:
        // Without this, Windows can paint its own default (white) background
        // for a frame before our D3D content is presented - e.g. right after
        // switching back to this window from another app - which is exactly
        // the white flash this is meant to prevent. Claim we handled erasing
        // and do nothing, since every pixel comes from Present() anyway.
        return 1;
    case WM_ACTIVATE:
    case WM_KILLFOCUS:
        // Switching to another app (Discord, Spotify, alt-tab, ...) doesn't
        // necessarily move the mouse anywhere - it can sit exactly where it
        // was over a sidebar item the whole time. Win32 only fires
        // WM_MOUSELEAVE when the cursor physically exits the window, so
        // without this, a button's hover animation target stays "hovered"
        // indefinitely while this window isn't even active, and is still
        // mid-animation (or stuck) by the time focus returns - read as a
        // stale highlight that "lingers". Wiping all animation state on
        // every focus transition guarantees nothing is left mid-flight.
        g_widgetAnim.clear();
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // disable ALT menu beep
        break;
    case WM_NCCALCSIZE: {
        if (!wParam) break;
        // Drop the native caption/border entirely; when maximized, clamp to
        // the monitor's work area so the window doesn't cover the taskbar.
        if (IsZoomed(hWnd)) {
            NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                params->rgrc[0] = mi.rcWork;
            }
        }
        return 0;
    }
    case WM_NCHITTEST: {
        // Without WS_CAPTION, DefWindowProc only ever reports resize-edge
        // hits (still works thanks to WS_THICKFRAME) or HTCLIENT. We turn
        // the top strip - minus our own button rects - into the drag region.
        LRESULT hit = DefWindowProcW(hWnd, msg, wParam, lParam);
        if (hit != HTCLIENT) return hit;

        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hWnd, &pt);
        if (pt.y >= 0 && pt.y < TITLEBAR_H) {
            if (PtInRect(&g_btnMinRect, pt) || PtInRect(&g_btnMaxRect, pt) || PtInRect(&g_btnCloseRect, pt))
                return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
namespace theme {
    bool darkMode = true;
    ImVec4 bg, sidebarBg, panelBg, panelBg2, glassFill, glassHover, glassActive;
    ImVec4 accent, accentHov, accentAct, accentText, text, subtext, good, warn, bad, border;
    ImVec4 softAccentBg, titlebarBg, cardLine;

    ImFont* fontBody = nullptr;
    ImFont* fontTitle = nullptr;
    ImFont* fontBrand = nullptr;
    ImFont* fontIcon = nullptr;
    ImFont* fontIconLg = nullptr;

    void SetMode(bool dark) {
        darkMode = dark;
        if (dark) {
            bg = ImVec4(0.050f, 0.052f, 0.070f, 1.00f);
            sidebarBg = ImVec4(0.075f, 0.078f, 0.105f, 0.98f);
            panelBg = ImVec4(0.095f, 0.100f, 0.135f, 0.98f);
            panelBg2 = ImVec4(0.120f, 0.126f, 0.168f, 1.00f);
            glassFill = ImVec4(0.135f, 0.140f, 0.185f, 1.00f);
            glassHover = ImVec4(0.165f, 0.170f, 0.225f, 1.00f);
            glassActive = ImVec4(0.150f, 0.260f, 0.420f, 1.00f);
            accent = ImVec4(0.290f, 0.620f, 0.945f, 1.00f);
            accentHov = ImVec4(0.400f, 0.710f, 0.995f, 1.00f);
            accentAct = ImVec4(0.220f, 0.520f, 0.855f, 1.00f);
            accentText = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
            text = ImVec4(0.940f, 0.945f, 0.970f, 1.00f);
            subtext = ImVec4(0.645f, 0.675f, 0.780f, 1.00f);
            border = ImVec4(0.210f, 0.220f, 0.290f, 1.00f);
            softAccentBg = ImVec4(0.290f, 0.620f, 0.945f, 0.18f);
            titlebarBg = ImVec4(0.050f, 0.052f, 0.070f, 0.98f);
            cardLine = ImVec4(1, 1, 1, 0.06f);
        } else {
            bg = ImVec4(0.965f, 0.970f, 0.988f, 1.00f);
            sidebarBg = ImVec4(0.985f, 0.987f, 0.996f, 0.98f);
            panelBg = ImVec4(1.000f, 1.000f, 1.000f, 0.98f);
            panelBg2 = ImVec4(0.970f, 0.974f, 0.990f, 1.00f);
            glassFill = ImVec4(0.976f, 0.978f, 0.992f, 1.00f);
            glassHover = ImVec4(0.925f, 0.955f, 0.995f, 1.00f);
            glassActive = ImVec4(0.870f, 0.925f, 0.990f, 1.00f);
            accent = ImVec4(0.235f, 0.585f, 0.925f, 1.00f);
            accentHov = ImVec4(0.330f, 0.660f, 0.965f, 1.00f);
            accentAct = ImVec4(0.180f, 0.490f, 0.830f, 1.00f);
            accentText = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
            text = ImVec4(0.100f, 0.115f, 0.180f, 1.00f);
            subtext = ImVec4(0.410f, 0.450f, 0.580f, 1.00f);
            border = ImVec4(0.850f, 0.865f, 0.920f, 1.00f);
            softAccentBg = ImVec4(0.235f, 0.585f, 0.925f, 0.10f);
            titlebarBg = ImVec4(0.985f, 0.987f, 0.996f, 0.98f);
            cardLine = ImVec4(1, 1, 1, 0.75f);
        }
        good = ImVec4(0.070f, 0.720f, 0.330f, 1.00f);
        warn = ImVec4(0.940f, 0.620f, 0.120f, 1.00f);
        bad = ImVec4(0.900f, 0.240f, 0.250f, 1.00f);
    }

    void Apply() {
        SetMode(darkMode);
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 0.0f;
        s.ChildRounding = 10.0f;
        s.FrameRounding = 6.0f;
        s.PopupRounding = 6.0f;
        s.ScrollbarRounding = 8.0f;
        s.GrabRounding = 8.0f;
        s.TabRounding = 6.0f;
        s.WindowBorderSize = 0.0f;
        s.ChildBorderSize = 1.0f;
        s.FrameBorderSize = 0.0f;
        s.ItemSpacing = ImVec2(8, 8);
        s.FramePadding = ImVec2(10, 7);
        s.WindowPadding = ImVec2(0, 0);
        s.ScrollbarSize = 10.0f;

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_ChildBg] = panelBg;
        c[ImGuiCol_Border] = border;
        c[ImGuiCol_Text] = text;
        c[ImGuiCol_TextDisabled] = subtext;

        c[ImGuiCol_Button] = glassFill;
        c[ImGuiCol_ButtonHovered] = glassHover;
        c[ImGuiCol_ButtonActive] = glassActive;

        c[ImGuiCol_FrameBg] = panelBg2;
        c[ImGuiCol_FrameBgHovered] = glassHover;
        c[ImGuiCol_FrameBgActive] = glassActive;

        c[ImGuiCol_Header] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
        c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.14f);
        c[ImGuiCol_HeaderActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);

        c[ImGuiCol_ScrollbarBg] = panelBg2;
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.22f, 0.24f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.32f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = accent;

        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = accentHov;

        c[ImGuiCol_Separator] = border;
    }

    // --- animated starfield background ---
    std::vector<ImVec2> stars;       // normalized [0,1] base positions
    std::vector<float> starSize;
    std::vector<float> starAlpha;    // peak alpha
    std::vector<float> starTwinkleSpeed;
    std::vector<float> starTwinklePhase;
    std::vector<float> starDrift;    // normalized units per second, vertical

    void GenerateStars(int count) {
        stars.clear(); starSize.clear(); starAlpha.clear();
        starTwinkleSpeed.clear(); starTwinklePhase.clear(); starDrift.clear();
        std::mt19937 rng(1337);
        std::uniform_real_distribution<float> posDist(0.0f, 1.0f);
        std::uniform_real_distribution<float> sizeDist(0.45f, 1.65f);
        std::uniform_real_distribution<float> alphaDist(0.12f, 0.72f);
        std::uniform_real_distribution<float> speedDist(0.4f, 1.6f);
        std::uniform_real_distribution<float> phaseDist(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> driftDist(0.0025f, 0.012f);
        for (int i = 0; i < count; ++i) {
            stars.push_back(ImVec2(posDist(rng), posDist(rng)));
            starSize.push_back(sizeDist(rng));
            starAlpha.push_back(alphaDist(rng));
            starTwinkleSpeed.push_back(speedDist(rng));
            starTwinklePhase.push_back(phaseDist(rng));
            starDrift.push_back(driftDist(rng));
        }
    }

    void DrawStarfield(ImVec2 origin, ImVec2 size) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float t = (float)ImGui::GetTime();
        for (size_t i = 0; i < stars.size(); ++i) {
            float yNorm = stars[i].y + t * starDrift[i];
            yNorm -= floorf(yNorm); // wrap into [0,1) - slow downward drift, loops seamlessly

            float twinkle = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * starTwinkleSpeed[i] + starTwinklePhase[i]));
            float alpha = starAlpha[i] * twinkle;

            ImVec2 p(origin.x + stars[i].x * size.x, origin.y + yNorm * size.y);
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, alpha));
            dl->AddCircleFilled(p, starSize[i], col);
        }
    }
}

// ---------------------------------------------------------------------------
// Icons (Lucide icon font, MIT licensed - fonts/Lucide.ttf)
// ---------------------------------------------------------------------------
namespace icon {
    constexpr unsigned HOME = 0xE0F4;
    constexpr unsigned ROCKET = 0xE286;
    constexpr unsigned COOKIE = 0xE26B;
    constexpr unsigned SHARE2 = 0xE156;
    constexpr unsigned USER = 0xE19F;
    constexpr unsigned FILE_TEXT = 0xE0CC;
    constexpr unsigned TRASH2 = 0xE18E;
    constexpr unsigned BOX = 0xE061;
    constexpr unsigned CIRCLE = 0xE076;
    constexpr unsigned PLAY = 0xE13C;
    constexpr unsigned PLUS = 0xE13D;
    constexpr unsigned GLOBE = 0xE0E8;
    constexpr unsigned SHIELD = 0xE158;
    constexpr unsigned CHEVRON_RIGHT = 0xE06F;
    constexpr unsigned CROWN = 0xE1D6;
    constexpr unsigned ETHERNET_PORT = 0xE620;
    constexpr unsigned ACTIVITY = 0xE038;
    constexpr unsigned BAR_CHART = 0xE06A;
    constexpr unsigned ZAP = 0xE1B4;
    constexpr unsigned CPU = 0xE0A9;
    constexpr unsigned MEMORY_STICK = 0xE445;
    constexpr unsigned GAUGE = 0xE1BF;
    constexpr unsigned REFRESH_CW = 0xE145;
    constexpr unsigned SHIELD_CHECK = 0xE1FF;
    constexpr unsigned CLOCK = 0xE24C;
    constexpr unsigned SETTINGS = 0xE152;
    constexpr unsigned CIRCLE_HELP = 0xE0BD;

    // Encodes a codepoint from the font's private-use area as UTF-8 (always 3 bytes for 0xE000-0xFFFF).
    inline std::string Str(unsigned cp) {
        std::string s;
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
        return s;
    }
}

// Small rounded box with a centered icon glyph - used for nav items, section headers, etc.
static void IconBox(ImVec2 pos, float size, unsigned codepoint, ImVec4 bg, ImVec4 fg, float rounding = 9.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), ImGui::ColorConvertFloat4ToU32(bg), rounding);
    dl->AddRect(pos, ImVec2(pos.x + size, pos.y + size), ImGui::ColorConvertFloat4ToU32(theme::border), rounding, 0, 1.0f);

    std::string glyph = icon::Str(codepoint);
    ImGui::PushFont(size >= 48.0f ? theme::fontIconLg : theme::fontIcon);
    ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
    ImVec2 glyphPos(pos.x + (size - glyphSize.x) * 0.5f, pos.y + (size - glyphSize.y) * 0.5f);
    dl->AddText(glyphPos, ImGui::ColorConvertFloat4ToU32(fg), glyph.c_str());
    ImGui::PopFont();
}

// Soft colored halo built from stacked filled rects, with the per-layer
// alpha solved exactly (not approximated) so the *composited* opacity right
// at the shape's edge always equals `peakAlpha`, fading by smoothstep to 0
// at `spread` pixels outward. Naive stacking (equal alpha per layer, or alpha
// scaled by a fixed constant) either washes out under many overlapping thin
// layers or rings visibly under few thick ones; solving each layer's alpha
// from the *target* cumulative curve via the standard "over" compositing
// formula (newAlpha = layerAlpha + old*(1-layerAlpha)) avoids both.
static void DrawSoftRectGlow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float spread, float peakAlpha,
    ImVec4 color = ImVec4(1, 1, 1, 1)) {
    const int steps = 26;
    float cumulative = 0.0f;
    for (int i = steps; i >= 0; --i) {
        float t = (float)i / (float)steps;        // 1 (outer edge of halo) -> 0 (at the shape's own edge)
        float u = 1.0f - t;
        float smooth = u * u * (3.0f - 2.0f * u);  // smoothstep target curve
        float targetAlpha = peakAlpha * smooth;
        float layerAlpha = (targetAlpha - cumulative) / std::max(1.0f - cumulative, 0.0001f);
        layerAlpha = std::clamp(layerAlpha, 0.0f, 1.0f);
        cumulative = layerAlpha + cumulative * (1.0f - layerAlpha);

        float pad = spread * t;
        dl->AddRectFilled(ImVec2(min.x - pad, min.y - pad), ImVec2(max.x + pad, max.y + pad),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, layerAlpha)), rounding + pad);
    }
}

// Brand lettermark: stylized white "V" mark on the black sidebar.
static void DrawLogoMark(ImVec2 pos, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(pos.x + size * 0.47f, pos.y + size * 0.54f), size * 0.72f,
        ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.065f)), 32);
    dl->AddCircleFilled(ImVec2(pos.x + size * 0.47f, pos.y + size * 0.54f), size * 0.42f,
        ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.075f)), 32);
    ImU32 white = ImGui::ColorConvertFloat4ToU32(theme::accent);
    float x = pos.x;
    float y = pos.y;
    ImVec2 left[] = {
        ImVec2(x + size * 0.00f, y + size * 0.08f),
        ImVec2(x + size * 0.28f, y + size * 0.08f),
        ImVec2(x + size * 0.58f, y + size * 0.92f),
        ImVec2(x + size * 0.32f, y + size * 0.92f),
    };
    ImVec2 right[] = {
        ImVec2(x + size * 0.45f, y + size * 0.92f),
        ImVec2(x + size * 0.73f, y + size * 0.08f),
        ImVec2(x + size * 1.00f, y + size * 0.08f),
        ImVec2(x + size * 0.70f, y + size * 0.92f),
    };
    dl->AddConvexPolyFilled(left, 4, white);
    dl->AddConvexPolyFilled(right, 4, white);
}

// Inline icon glyph at the current cursor position (no background box).
static void InlineIcon(unsigned codepoint, ImVec4 col) {
    std::string glyph = icon::Str(codepoint);
    ImGui::PushFont(theme::fontIcon);
    ImGui::TextColored(col, "%s", glyph.c_str());
    ImGui::PopFont();
}

static void DrawTitleBar(HWND hwnd, float winWidth) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float BTN_W = 46.0f;
    ImU32 bgCol = ImGui::ColorConvertFloat4ToU32(theme::titlebarBg);
    ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(theme::border);
    ImU32 textCol = ImGui::ColorConvertFloat4ToU32(theme::text);

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(winWidth, (float)TITLEBAR_H), bgCol);
    (void)borderCol;

    float xMin = winWidth - BTN_W * 3;
    float xMax = winWidth - BTN_W * 2;
    float xClose = winWidth - BTN_W;

    auto hoverFill = [&](float x, ImU32 col) {
        dl->AddRectFilled(ImVec2(x, 0), ImVec2(x + BTN_W, (float)TITLEBAR_H), col);
        };

    // Minimize
    ImGui::SetCursorScreenPos(ImVec2(xMin, 0));
    ImGui::InvisibleButton("##titlebar_min", ImVec2(BTN_W, (float)TITLEBAR_H));
    if (ImGui::IsItemHovered()) hoverFill(xMin, ImGui::ColorConvertFloat4ToU32(theme::glassHover));
    if (ImGui::IsItemClicked()) ShowWindow(hwnd, SW_MINIMIZE);
    {
        float cx = xMin + BTN_W * 0.5f, cy = TITLEBAR_H * 0.5f;
        dl->AddLine(ImVec2(cx - 5, cy), ImVec2(cx + 5, cy), textCol, 1.3f);
    }
    g_btnMinRect = { (LONG)xMin, 0, (LONG)(xMin + BTN_W), (LONG)TITLEBAR_H };

    // Maximize / restore
    ImGui::SetCursorScreenPos(ImVec2(xMax, 0));
    ImGui::InvisibleButton("##titlebar_max", ImVec2(BTN_W, (float)TITLEBAR_H));
    if (ImGui::IsItemHovered()) hoverFill(xMax, ImGui::ColorConvertFloat4ToU32(theme::glassHover));
    if (ImGui::IsItemClicked()) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    {
        float cx = xMax + BTN_W * 0.5f, cy = TITLEBAR_H * 0.5f;
        if (!IsZoomed(hwnd)) {
            dl->AddRect(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), textCol, 0.0f, 0, 1.2f);
        } else {
            dl->AddRect(ImVec2(cx - 5, cy - 3), ImVec2(cx + 3, cy + 5), textCol, 0.0f, 0, 1.2f);
            dl->AddRectFilled(ImVec2(cx - 3, cy - 5), ImVec2(cx + 5, cy + 3), bgCol);
            dl->AddRect(ImVec2(cx - 3, cy - 5), ImVec2(cx + 5, cy + 3), textCol, 0.0f, 0, 1.2f);
        }
    }
    g_btnMaxRect = { (LONG)xMax, 0, (LONG)(xMax + BTN_W), (LONG)TITLEBAR_H };

    // Close
    ImGui::SetCursorScreenPos(ImVec2(xClose, 0));
    ImGui::InvisibleButton("##titlebar_close", ImVec2(BTN_W, (float)TITLEBAR_H));
    bool hovClose = ImGui::IsItemHovered();
    if (hovClose) hoverFill(xClose, ImGui::ColorConvertFloat4ToU32(ImVec4(0.92f, 0.18f, 0.22f, 0.92f)));
    if (ImGui::IsItemClicked()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    {
        float cx = xClose + BTN_W * 0.5f, cy = TITLEBAR_H * 0.5f;
        ImU32 col = hovClose ? IM_COL32(255, 255, 255, 255) : textCol;
        dl->AddLine(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), col, 1.3f);
        dl->AddLine(ImVec2(cx - 5, cy + 5), ImVec2(cx + 5, cy - 5), col, 1.3f);
    }
    g_btnCloseRect = { (LONG)xClose, 0, (LONG)(xClose + BTN_W), (LONG)TITLEBAR_H };

    ImGui::SetCursorScreenPos(ImVec2(0, (float)TITLEBAR_H));
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------
static float PopAnim(ImGuiID id, bool hovered, bool pressed) {
    float& anim = g_widgetAnim[id];
    float target = pressed ? -1.0f : (hovered ? 1.0f : 0.0f);
    if (target == 0.0f) {
        // Returning to rest is a hard snap, not an eased decay - even a fast
        // decay is still *a* fade, which reads as "lingering" when moving
        // down a list of nav items at normal speed. Nothing to see here
        // means nothing, the instant the mouse leaves.
        anim = 0.0f;
    } else {
        float rate = pressed ? 22.0f : 12.0f; // entering hover/press still eases in smoothly (the "pop")
        anim += (target - anim) * std::min(1.0f, ImGui::GetIO().DeltaTime * rate);
    }
    return anim;
}

// anim in [-1,1]: negative = pressed (shrink), positive = hovered (grow).
// iconCp == 0 means no icon (just centered text).
static bool DrawPopButton(const char* label, ImVec2 size, bool primary, unsigned iconCp = 0) {
    ImGuiID id = ImGui::GetID(label);
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // BeginDisabled() only lowers style.Alpha; since this button paints itself
    // through the draw list with hard-coded alpha, a disabled button would
    // otherwise look fully active yet do nothing on click. Detect the reduced
    // alpha and (a) grey the button out and (b) suppress the hover glow/pop so
    // the disabled state is honest and obvious.
    float uiAlpha = ImGui::GetStyle().Alpha;
    bool disabled = uiAlpha < 0.999f;

    ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();
    bool pressed = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    float anim = disabled ? 0.0f : PopAnim(id, hovered, pressed);
    if (disabled) { hovered = false; pressed = false; }
    float hoverAnim = std::max(anim, 0.0f);
    float pressAnim = std::max(-anim, 0.0f);

    float lift = pressAnim > 0.0f ? 0.0f : hoverAnim * 2.2f;
    float scale = 1.0f - pressAnim * 0.018f + hoverAnim * 0.03f;
    ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f - lift);
    ImVec2 half(size.x * 0.5f * scale, size.y * 0.5f * scale);
    ImVec2 rmin(center.x - half.x, center.y - half.y);
    ImVec2 rmax(center.x + half.x, center.y + half.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float hoverLift = hovered ? 1.0f : 0.0f;

    if (hoverAnim > 0.01f) {
        DrawSoftRectGlow(dl, rmin, rmax, 9.0f, 15.0f * hoverAnim, (primary ? 0.45f : 0.30f) * hoverAnim, theme::accent);
    }

    auto fade = [uiAlpha](ImVec4 c) { c.w *= uiAlpha; return c; };

    if (primary) {
        ImVec4 col = hoverLift > 0.001f ? theme::accentHov : theme::accent;
        dl->AddRectFilled(rmin, rmax, ImGui::ColorConvertFloat4ToU32(fade(col)), 9.0f);
    } else {
        ImVec4 fill = hoverLift > 0.001f ? theme::glassHover : theme::panelBg;
        dl->AddRectFilled(rmin, rmax, ImGui::ColorConvertFloat4ToU32(fade(fill)), 9.0f);
        ImVec4 borderCol = hoverAnim > 0.01f
            ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.45f + 0.45f * hoverAnim)
            : theme::border;
        dl->AddRect(rmin, rmax, ImGui::ColorConvertFloat4ToU32(fade(borderCol)), 9.0f, 0, 2.4f);
    }

    ImVec4 textColor = fade(primary ? theme::accentText : theme::text);
    ImVec2 textSize = ImGui::CalcTextSize(label);

    if (iconCp != 0) {
        std::string glyph = icon::Str(iconCp);
        ImGui::PushFont(theme::fontIcon);
        ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
        ImGui::PopFont();

        float gap = 8.0f;
        float iconW = (primary && iconCp == icon::PLAY) ? 18.0f : glyphSize.x;
        float totalW = iconW + gap + textSize.x;
        float startX = center.x - totalW * 0.5f;

        if (primary && iconCp == icon::PLAY) {
            ImVec2 c(startX + 9.0f, center.y);
            dl->AddCircleFilled(c, 9.0f, ImGui::ColorConvertFloat4ToU32(fade(theme::accentText)), 24);
            ImVec2 tri[] = {
                ImVec2(c.x - 2.5f, c.y - 4.5f),
                ImVec2(c.x - 2.5f, c.y + 4.5f),
                ImVec2(c.x + 4.8f, c.y),
            };
            dl->AddConvexPolyFilled(tri, 3, ImGui::ColorConvertFloat4ToU32(fade(theme::accent)));
        } else {
            ImGui::PushFont(theme::fontIcon);
            dl->AddText(ImVec2(startX, center.y - glyphSize.y * 0.5f), ImGui::ColorConvertFloat4ToU32(textColor), glyph.c_str());
            ImGui::PopFont();
        }
        dl->AddText(ImVec2(startX + iconW + gap, center.y - textSize.y * 0.5f), ImGui::ColorConvertFloat4ToU32(textColor), label);
    } else {
        ImVec2 textPos(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);
        dl->AddText(textPos, ImGui::ColorConvertFloat4ToU32(textColor), label);
    }

    return clicked;
}

static bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 34)) {
    return DrawPopButton(label, size, true);
}

static bool SecondaryButton(const char* label, ImVec2 size = ImVec2(0, 34)) {
    return DrawPopButton(label, size, false);
}

static bool PrimaryIconButton(unsigned iconCp, const char* label, ImVec2 size = ImVec2(0, 34)) {
    return DrawPopButton(label, size, true, iconCp);
}

static bool SecondaryIconButton(unsigned iconCp, const char* label, ImVec2 size = ImVec2(0, 34)) {
    return DrawPopButton(label, size, false, iconCp);
}

static bool NavItem(const char* label, unsigned iconCp, bool active) {
    ImGuiID id = ImGui::GetID(label);
    ImVec2 size(ImGui::GetContentRegionAvail().x, 58);
    ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float hoverAnim = std::max(PopAnim(id, hovered, false), 0.0f);
    float anim = active ? 1.0f : hoverAnim;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pillMin(pos.x + 8.0f, pos.y + 6.0f);
    ImVec2 pillMax(pos.x + size.x - 8.0f, pos.y + size.y - 6.0f);
    const float rounding = 10.0f;

    // One clean background layer instead of the old glow+fill+border+circle
    // stack (which doubled up on edges and animated on different curves, so it
    // read as choppy). Active is a steady soft-accent tint that brightens a
    // touch on hover; an inactive item just fades in a faint accent wash.
    float fillAlpha = active ? (0.15f + 0.07f * hoverAnim) : (0.075f * hoverAnim);
    if (fillAlpha > 0.004f) {
        ImVec4 fill(theme::accent.x, theme::accent.y, theme::accent.z, fillAlpha);
        dl->AddRectFilled(pillMin, pillMax, ImGui::ColorConvertFloat4ToU32(fill), rounding);
    }

    // Crisp vertically-centred accent bar marks the selected item. It grows in
    // from zero as the active state settles so switching tabs glides.
    if (anim > 0.001f && active) {
        float barH = (size.y - 22.0f) * anim;
        float cy = pos.y + size.y * 0.5f;
        ImVec2 barMin(pos.x + 2.0f, cy - barH * 0.5f);
        ImVec2 barMax(pos.x + 5.5f, cy + barH * 0.5f);
        dl->AddRectFilled(barMin, barMax, ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);
    }

    // Icon + label share one colour that eases subtext -> text on hover, or sits
    // on the accent when active. No separate icon disc anymore.
    float colorT = active ? 1.0f : hoverAnim;
    ImVec4 rest = active ? theme::accent : theme::subtext;
    ImVec4 hot = active ? theme::accent : theme::text;
    ImVec4 fg(rest.x + (hot.x - rest.x) * colorT,
              rest.y + (hot.y - rest.y) * colorT,
              rest.z + (hot.z - rest.z) * colorT, 1.0f);
    ImU32 fgU32 = ImGui::ColorConvertFloat4ToU32(fg);

    std::string glyph = icon::Str(iconCp);
    ImGui::PushFont(theme::fontIconLg);
    ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
    dl->AddText(ImVec2(pos.x + 28.0f, pos.y + (size.y - glyphSize.y) * 0.5f), fgU32, glyph.c_str());
    ImGui::PopFont();

    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 textPos(pos.x + 74.0f, pos.y + (size.y - textSize.y) * 0.5f);
    dl->AddText(textPos, fgU32, label);

    return clicked;
}

static void SectionTitle(unsigned iconCp, const char* title, const char* desc) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    IconBox(pos, 70.0f, iconCp, theme::softAccentBg, theme::accent, 13.0f);

    ImGui::SetCursorScreenPos(ImVec2(pos.x + 90, pos.y + 10));
    ImGui::PushFont(theme::fontTitle);
    ImGui::TextColored(theme::text, "%s", title);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 90, pos.y + 42));
    // Clamp the wrap so the description never runs underneath the top-right
    // action buttons (e.g. "Open Web" / "Launch Roblox" on the Accounts tab).
    float descX = ImGui::GetCursorScreenPos().x;
    float winRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
    float wrapX = descX + 480;
    float safeRight = winRight - 400;
    if (wrapX > safeRight) wrapX = safeRight;
    if (wrapX < descX + 160) wrapX = descX + 160;
    ImGui::PushTextWrapPos(wrapX);
    ImGui::TextColored(theme::subtext, "%s", desc);
    ImGui::PopTextWrapPos();

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + 82));
    ImGui::Dummy(ImVec2(0, 8));
}

static bool ClearLogButton(ImVec2 size) {
    ImGuiID id = ImGui::GetID("##clear_log_button");
    ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##clear_log_button", size);
    bool hovered = ImGui::IsItemHovered();
    bool pressed = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    float anim = PopAnim(id, hovered, pressed);

    float hover = anim > 0.0f ? anim : 0.0f;
    float press = anim < 0.0f ? -anim : 0.0f;
    ImVec4 fill(0.020f + hover * 0.018f - press * 0.006f,
                0.020f + hover * 0.018f - press * 0.006f,
                0.024f + hover * 0.018f - press * 0.006f,
                1.0f);

    float lift = press > 0.0f ? 0.0f : hover * 1.6f;
    ImVec2 liftedPos(pos.x, pos.y - lift);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 max(liftedPos.x + size.x, liftedPos.y + size.y);

    if (hover > 0.01f) {
        DrawSoftRectGlow(dl, liftedPos, max, 8.0f, 12.0f * hover, 0.30f * hover, theme::accent);
    }
    dl->AddRectFilled(liftedPos, max, ImGui::ColorConvertFloat4ToU32(fill), 8.0f);
    dl->AddRect(liftedPos, max, ImGui::ColorConvertFloat4ToU32(
        hover > 0.01f ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.35f + 0.35f * hover) : theme::border),
        8.0f, 0, 1.0f);
    pos = liftedPos;

    std::string glyph = icon::Str(icon::TRASH2);
    ImGui::PushFont(theme::fontIcon);
    ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
    dl->AddText(ImVec2(pos.x + 18.0f, pos.y + (size.y - glyphSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(theme::text), glyph.c_str());
    ImGui::PopFont();

    const char* label = "Clear Log";
    ImVec2 labelSize = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + 44.0f, pos.y + (size.y - labelSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(theme::text), label);

    return clicked;
}

static void DrawLogCard(float height) {
    ImVec2 logPos = ImGui::GetCursorScreenPos();
    ImVec2 logSize(ImGui::GetContentRegionAvail().x, height);
    DrawSoftRectGlow(ImGui::GetWindowDrawList(), logPos, ImVec2(logPos.x + logSize.x, logPos.y + logSize.y),
        10.0f, 14.0f, 0.105f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("LogCard", ImVec2(0, height), true, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cardPos = ImGui::GetWindowPos();
        ImVec2 cardSize = ImGui::GetWindowSize();
        const float padX = 28.0f;
        const float headerTop = 28.0f;
        const float clearBtnW = 128.0f;
        const float clearBtnH = 38.0f;
        const float headerCenterY = cardPos.y + headerTop + clearBtnH * 0.5f;

        std::string fileGlyph = icon::Str(icon::FILE_TEXT);
        ImGui::PushFont(theme::fontIcon);
        ImVec2 fileSize = ImGui::CalcTextSize(fileGlyph.c_str());
        dl->AddText(ImVec2(cardPos.x + padX, headerCenterY - fileSize.y * 0.5f),
            ImGui::ColorConvertFloat4ToU32(theme::text), fileGlyph.c_str());
        ImGui::PopFont();

        ImGui::PushFont(theme::fontBrand);
        ImVec2 titleSize = ImGui::CalcTextSize("Activity Log");
        dl->AddText(ImVec2(cardPos.x + padX + 34.0f, headerCenterY - titleSize.y * 0.5f),
            ImGui::ColorConvertFloat4ToU32(theme::text), "Activity Log");
        ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(cardPos.x + cardSize.x - padX - clearBtnW, cardPos.y + headerTop));
        if (ClearLogButton(ImVec2(clearBtnW, clearBtnH))) {
            backend::ClearLog();
        }

        const float dividerY = cardPos.y + 86.0f;
        dl->AddLine(ImVec2(cardPos.x + padX, dividerY), ImVec2(cardPos.x + cardSize.x - padX, dividerY),
            ImGui::ColorConvertFloat4ToU32(theme::border), 1.0f);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::SetCursorScreenPos(ImVec2(cardPos.x + padX, dividerY + 18.0f));
        ImGui::BeginChild("LogInner", ImVec2(cardSize.x - padX * 2.0f, std::max(0.0f, cardSize.y - 120.0f)), false);
        {
            std::lock_guard<std::mutex> lock(backend::logMutex);
            bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
            ImDrawList* logDl = ImGui::GetWindowDrawList();
            float logBaseX = ImGui::GetCursorScreenPos().x;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
            for (auto& entry : backend::logLines) {
                ImVec2 rowPos = ImGui::GetCursorScreenPos();
                rowPos.x = logBaseX;
                ImGui::SetCursorScreenPos(rowPos);
                float rowH = ImGui::GetTextLineHeight();
                float dotX = rowPos.x + 7.0f;
                float dotY = rowPos.y + rowH * 0.5f;
                ImU32 lineCol = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.10f));
                ImU32 dotCol = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.16f));
                logDl->AddLine(ImVec2(dotX, rowPos.y - 4.0f), ImVec2(dotX, rowPos.y + rowH + 4.0f), lineCol, 1.0f);
                logDl->AddCircleFilled(ImVec2(dotX, dotY), 2.0f, dotCol, 12);
                ImGui::SetCursorScreenPos(ImVec2(rowPos.x + 22.0f, rowPos.y));
                ImGui::TextColored(theme::subtext, "[%s]", entry.time.c_str());
                ImGui::SameLine(0, 14);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(theme::text, "%s", entry.text.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::PopStyleVar();
            if (atBottom) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

struct StatusItem {
    unsigned iconCp;
    const char* label;
    std::string value;
    ImVec4 valueColor;
    ImVec4 iconColor = theme::subtext;
};

// Bordered card split evenly into columns, each showing an icon + label + value.
static void StatusCard(std::initializer_list<StatusItem> items) {
    float h = 62.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), ImGui::ColorConvertFloat4ToU32(theme::border), 10.0f, 0, 1.0f);

    size_t n = items.size();
    float segW = w / (float)n;
    size_t i = 0;
    for (auto& item : items) {
        float x = pos.x + i * segW;
        if (i > 0) {
            dl->AddLine(ImVec2(x, pos.y + 14), ImVec2(x, pos.y + h - 14), ImGui::ColorConvertFloat4ToU32(theme::border), 1.0f);
        }

        std::string glyph = icon::Str(item.iconCp);
        ImGui::PushFont(theme::fontIcon);
        ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
        dl->AddText(ImVec2(x + 20, pos.y + (h - glyphSize.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(item.iconColor), glyph.c_str());
        ImGui::PopFont();

        float textX = x + 20 + glyphSize.x + 10;
        ImVec2 labelSize = ImGui::CalcTextSize(item.label);
        ImVec2 valueSize = ImGui::CalcTextSize(item.value.c_str());
        float gap = 6.0f;
        float blockH = labelSize.y;
        float blockY = pos.y + (h - blockH) * 0.5f;

        dl->AddText(ImVec2(textX, blockY), ImGui::ColorConvertFloat4ToU32(theme::subtext), item.label);
        ImGui::PushFont(theme::fontBrand);
        dl->AddText(ImVec2(textX + labelSize.x + gap, blockY - 1), ImGui::ColorConvertFloat4ToU32(item.valueColor), item.value.c_str());
        ImGui::PopFont();

        ++i;
    }

    ImGui::Dummy(ImVec2(0, h));
}

// ---------------------------------------------------------------------------
// Multi-Instance dashboard: System Overview / Quick Actions / Performance Monitor
// ---------------------------------------------------------------------------
static void DrawStatRow(ImVec2 pos, float width, float rowH, unsigned iconCp, const char* label, const std::string& value, ImVec4 valueColor) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    std::string glyph = icon::Str(iconCp);
    ImGui::PushFont(theme::fontIcon);
    ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
    dl->AddText(ImVec2(pos.x, pos.y + (rowH - glyphSize.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(theme::subtext), glyph.c_str());
    ImGui::PopFont();

    ImVec2 labelSize = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + glyphSize.x + 10.0f, pos.y + (rowH - labelSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(theme::subtext), label);

    ImGui::PushFont(theme::fontBrand);
    ImVec2 valueSize = ImGui::CalcTextSize(value.c_str());
    dl->AddText(ImVec2(pos.x + width - valueSize.x, pos.y + (rowH - valueSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(valueColor), value.c_str());
    ImGui::PopFont();
}

static ImVec4 UsageColor(float pct) {
    if (pct >= 85.0f) return theme::bad;
    if (pct >= 60.0f) return theme::warn;
    return theme::good;
}

static void DrawMiniBar(ImVec2 pos, float width, float frac, ImVec4 fillColor) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 5.0f;
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.08f)), h * 0.5f);
    float w = width * std::clamp(frac, 0.0f, 1.0f);
    if (w > 1.0f) dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::ColorConvertFloat4ToU32(fillColor), h * 0.5f);
}

struct QuickAction {
    unsigned iconCp;
    const char* title;
    const char* subtitle;
    ImVec4 color;
};

static bool QuickActionRow(ImVec2 pos, float width, float height, const QuickAction& action) {
    ImGuiID id = ImGui::GetID(action.title);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(action.title, ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float anim = std::max(PopAnim(id, hovered, false), 0.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 rmax(pos.x + width, pos.y + height);
    if (anim > 0.01f) {
        DrawSoftRectGlow(dl, pos, rmax, 8.0f, 12.0f * anim, 0.28f * anim, action.color);
        dl->AddRectFilled(pos, rmax,
            ImGui::ColorConvertFloat4ToU32(ImVec4(action.color.x, action.color.y, action.color.z, 0.08f * anim)), 8.0f);
        dl->AddRect(pos, rmax,
            ImGui::ColorConvertFloat4ToU32(ImVec4(action.color.x, action.color.y, action.color.z, 0.45f * anim)), 8.0f, 0, 1.2f);
    }

    float iconSize = 38.0f;
    ImVec2 iconPos(pos.x + 4.0f, pos.y + (height - iconSize) * 0.5f);
    IconBox(iconPos, iconSize, action.iconCp, ImVec4(action.color.x, action.color.y, action.color.z, 0.14f + 0.06f * anim), action.color, 8.0f);

    float textX = iconPos.x + iconSize + 14.0f;
    ImGui::PushFont(theme::fontBrand);
    ImVec2 titleSize = ImGui::CalcTextSize(action.title);
    dl->AddText(ImVec2(textX, pos.y + height * 0.5f - titleSize.y - 1.0f),
        ImGui::ColorConvertFloat4ToU32(action.color), action.title);
    ImGui::PopFont();
    dl->AddText(ImVec2(textX, pos.y + height * 0.5f + 1.0f),
        ImGui::ColorConvertFloat4ToU32(theme::subtext), action.subtitle);

    std::string chev = icon::Str(icon::CHEVRON_RIGHT);
    ImGui::PushFont(theme::fontIcon);
    ImVec2 chevSize = ImGui::CalcTextSize(chev.c_str());
    ImVec4 chevColor(theme::subtext.x, theme::subtext.y, theme::subtext.z, theme::subtext.w * (0.6f + 0.4f * anim));
    dl->AddText(ImVec2(pos.x + width - chevSize.x - 2.0f, pos.y + (height - chevSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(chevColor), chev.c_str());
    ImGui::PopFont();

    return clicked;
}

static bool QuickActionTile(ImVec2 pos, float width, float height, const QuickAction& action) {
    ImGuiID id = ImGui::GetID(action.title);

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(action.title, ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    bool pressed = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();

    float anim = std::max(PopAnim(id, hovered, false), 0.0f);

    float lift = pressed ? 0.0f : anim * 3.0f;          // tile rises slightly on hover
    float scale = pressed ? 0.99f : (1.0f + anim * 0.012f);
    ImVec2 center(pos.x + width * 0.5f, pos.y + height * 0.5f - lift);
    ImVec2 half(width * 0.5f * scale, height * 0.5f * scale);
    ImVec2 min(center.x - half.x, center.y - half.y);
    ImVec2 max(center.x + half.x, center.y + half.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Soft colored glow that blooms in under the tile as it's hovered.
    if (anim > 0.01f) {
        DrawSoftRectGlow(dl, min, max, 10.0f, 16.0f * anim, 0.45f * anim, action.color);
    }

    ImVec4 fill = hovered
        ? ImVec4(action.color.x, action.color.y, action.color.z, 0.10f + 0.05f * anim)
        : theme::panelBg2;
    dl->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(fill), 10.0f);

    ImVec4 borderCol = hovered
        ? ImVec4(action.color.x, action.color.y, action.color.z, 0.55f + 0.35f * anim)
        : theme::border;
    dl->AddRect(min, max, ImGui::ColorConvertFloat4ToU32(borderCol), 10.0f, 0, hovered ? 1.4f : 1.0f);

    // Thin accent bar along the top edge, fading in with hover - ties the
    // tile's color back to its icon without needing a full colored fill.
    if (anim > 0.01f) {
        dl->AddRectFilled(ImVec2(min.x + 10.0f, min.y), ImVec2(max.x - 10.0f, min.y + 2.2f),
            ImGui::ColorConvertFloat4ToU32(ImVec4(action.color.x, action.color.y, action.color.z, 0.9f * anim)), 2.0f);
    }

    IconBox(ImVec2(min.x + 13.0f, min.y + (height - 36.0f) * 0.5f), 36.0f, action.iconCp,
        ImVec4(action.color.x, action.color.y, action.color.z, 0.18f + 0.06f * anim), action.color, 9.0f);

    ImGui::PushFont(theme::fontBrand);
    ImVec2 titleSize = ImGui::CalcTextSize(action.title);
    dl->AddText(ImVec2(min.x + 61.0f, min.y + height * 0.5f - titleSize.y - 1.0f),
        ImGui::ColorConvertFloat4ToU32(theme::text), action.title);
    ImGui::PopFont();
    dl->AddText(ImVec2(min.x + 61.0f, min.y + height * 0.5f + 2.0f),
        ImGui::ColorConvertFloat4ToU32(theme::subtext), action.subtitle);

    // Chevron affordance, same language as QuickActionRow, so hover clearly
    // reads as "this opens something" even on a compact tile.
    std::string chev = icon::Str(icon::CHEVRON_RIGHT);
    ImGui::PushFont(theme::fontIcon);
    ImVec2 chevSize = ImGui::CalcTextSize(chev.c_str());
    ImVec4 chevColor(action.color.x, action.color.y, action.color.z, 0.25f + 0.65f * anim);
    dl->AddText(ImVec2(max.x - chevSize.x - 10.0f + 3.0f * anim, min.y + (height - chevSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(chevColor), chev.c_str());
    ImGui::PopFont();

    return clicked;
}

static void DrawCardHeader(ImVec2 pos, unsigned iconCp, const char* title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string glyph = icon::Str(iconCp);
    ImGui::PushFont(theme::fontIcon);
    ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
    dl->AddText(pos, ImGui::ColorConvertFloat4ToU32(theme::text), glyph.c_str());
    ImGui::PopFont();
    ImGui::PushFont(theme::fontBrand);
    dl->AddText(ImVec2(pos.x + glyphSize.x + 10.0f, pos.y - 1.0f), ImGui::ColorConvertFloat4ToU32(theme::text), title);
    ImGui::PopFont();
}

static void DrawMultiInstanceQuickNav() {
    const ImVec4 purple(0.36f, 0.66f, 0.96f, 1.0f);
    const ImVec4 blue(0.40f, 0.64f, 0.98f, 1.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 134.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::ColorConvertFloat4ToU32(theme::panelBg), 12.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), ImGui::ColorConvertFloat4ToU32(theme::border), 12.0f, 0, 1.0f);

    const float padX = 22.0f;
    DrawCardHeader(ImVec2(pos.x + padX, pos.y + 20.0f), icon::ZAP, "Overview");
    dl->AddText(ImVec2(pos.x + padX, pos.y + 40.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), "Jump straight to the rest of the toolkit");

    const float dividerY = pos.y + 62.0f;
    dl->AddLine(ImVec2(pos.x + padX, dividerY), ImVec2(pos.x + w - padX, dividerY),
        ImGui::ColorConvertFloat4ToU32(theme::border), 1.0f);

    float tileY = dividerY + 14.0f;
    float tileH = 46.0f;
    float gap = 12.0f;
    float tileW = (w - padX * 2.0f - gap * 2.0f) / 3.0f;
    if (QuickActionTile(ImVec2(pos.x + padX, tileY), tileW, tileH, { icon::COOKIE, "Cookie Cleaner", "Clear Roblox traces", purple })) g_pendingNavPage = 1;
    if (QuickActionTile(ImVec2(pos.x + padX + tileW + gap, tileY), tileW, tileH, { icon::SHARE2, "MAC Spoofer", "Network identity", blue })) g_pendingNavPage = 2;
    if (QuickActionTile(ImVec2(pos.x + padX + (tileW + gap) * 2.0f, tileY), tileW, tileH, { icon::USER, "Accounts", "Launch places", theme::good })) g_pendingNavPage = 3;
    ImGui::Dummy(ImVec2(w, h));
}

static void DrawMultiInstanceOverview() {
    const ImVec4 colorRed(0.94f, 0.34f, 0.34f, 1.0f);
    const ImVec4 colorPurple(0.36f, 0.66f, 0.96f, 1.0f);
    const ImVec4 colorGreen = theme::good;

    float fullW = ImGui::GetContentRegionAvail().x;
    float gap = 16.0f;
    float colW = (fullW - gap) * 0.5f;
    // Quick Actions' third row ("Open Roblox") used to land flush with the
    // card's bottom border with zero padding - bumped a bit so it isn't cut off.
    float cardH = 210.0f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // --- System Overview ---
    ImVec2 leftMin = origin;
    ImVec2 leftMax(origin.x + colW, origin.y + cardH);
    dl->AddRectFilled(leftMin, leftMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg), 10.0f);
    dl->AddRect(leftMin, leftMax, ImGui::ColorConvertFloat4ToU32(theme::border), 10.0f, 0, 1.0f);
    {
        float padX = 22.0f;
        ImVec2 p(leftMin.x + padX, leftMin.y + 20.0f);
        float rowW = colW - padX * 2.0f;
        DrawCardHeader(p, icon::ACTIVITY, "System Overview");

        int totalInstances = backend::instanceCount.load();
        int robloxProcs = backend::CountRobloxProcesses();
        DrawStatRow(ImVec2(p.x, p.y + 36.0f), rowW, 20.0f, icon::BOX, "Total Instances", std::to_string(totalInstances), theme::text);
        DrawStatRow(ImVec2(p.x, p.y + 66.0f), rowW, 20.0f, icon::FILE_TEXT, "Roblox Processes", std::to_string(robloxProcs), theme::text);

        float cpu = backend::GetCpuUsagePercent();
        ImVec4 cpuColor = UsageColor(cpu);
        DrawStatRow(ImVec2(p.x, p.y + 96.0f), rowW, 18.0f, icon::CPU, "CPU Usage", std::to_string((long)std::lround(cpu)) + "%", cpuColor);
        DrawMiniBar(ImVec2(p.x, p.y + 118.0f), rowW, cpu / 100.0f, cpuColor);

        float mem = backend::GetMemoryUsagePercent();
        ImVec4 memColor = UsageColor(mem);
        DrawStatRow(ImVec2(p.x, p.y + 136.0f), rowW, 18.0f, icon::MEMORY_STICK, "Memory Usage", std::to_string((long)std::lround(mem)) + "%", memColor);
        DrawMiniBar(ImVec2(p.x, p.y + 158.0f), rowW, mem / 100.0f, memColor);
    }

    // --- Quick Actions ---
    ImVec2 rightMin(origin.x + colW + gap, origin.y);
    ImVec2 rightMax(rightMin.x + colW, origin.y + cardH);
    dl->AddRectFilled(rightMin, rightMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg), 10.0f);
    dl->AddRect(rightMin, rightMax, ImGui::ColorConvertFloat4ToU32(theme::border), 10.0f, 0, 1.0f);
    {
        float padX = 22.0f;
        ImVec2 p(rightMin.x + padX, rightMin.y + 20.0f);
        float rowW = colW - padX * 2.0f;
        DrawCardHeader(p, icon::ZAP, "Quick Actions");

        float rowH = 40.0f;
        float y = p.y + 34.0f;
        if (QuickActionRow(ImVec2(p.x, y), rowW, rowH,
            { icon::TRASH2, "Kill All Instances", "Close all running Roblox instances", colorRed })) {
            std::thread(backend::KillAllRobloxInstances).detach();
        }
        y += rowH + 8.0f;
        if (QuickActionRow(ImVec2(p.x, y), rowW, rowH,
            { icon::REFRESH_CW, "Refresh Instances", "Re-check running Roblox processes", colorPurple })) {
            std::thread([]() {
                int n = backend::CountRobloxProcesses(true);
                backend::Log("[i] Refreshed - " + std::to_string(n) + " Roblox process(es) detected.");
            }).detach();
        }
        y += rowH + 8.0f;
        if (QuickActionRow(ImVec2(p.x, y), rowW, rowH,
            { icon::PLAY, "Open Roblox", "Launch Roblox Player", colorGreen })) {
            std::thread(backend::LaunchNewInstance).detach();
        }
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(fullW, cardH));
}

enum class MacButtonIcon {
    Refresh,
    Shuffle,
    Restore
};

static std::string FitTextToWidth(const std::string& text, float maxWidth) {
    if (maxWidth <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
    std::string fitted = text;
    while (!fitted.empty()) {
        fitted.pop_back();
        std::string candidate = fitted + "...";
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth) return candidate;
    }
    return "...";
}

static void DrawMacButtonIcon(ImDrawList* dl, ImVec2 c, MacButtonIcon iconKind, ImU32 col) {
    const float pi = 3.14159265f;
    const float r = 6.2f;

    if (iconKind == MacButtonIcon::Refresh) {
        dl->PathArcTo(c, r, -0.35f * pi, 1.18f * pi, 18);
        dl->PathStroke(col, 0, 1.6f);
        dl->AddLine(ImVec2(c.x - 5.4f, c.y - 1.6f), ImVec2(c.x - 8.3f, c.y - 2.7f), col, 1.6f);
        dl->AddLine(ImVec2(c.x - 5.4f, c.y - 1.6f), ImVec2(c.x - 6.0f, c.y - 4.8f), col, 1.6f);
        return;
    }

    if (iconKind == MacButtonIcon::Restore) {
        dl->PathArcTo(c, r, 0.10f * pi, 1.65f * pi, 18);
        dl->PathStroke(col, 0, 1.6f);
        dl->AddLine(ImVec2(c.x - 5.2f, c.y - 4.6f), ImVec2(c.x - 8.3f, c.y - 5.0f), col, 1.6f);
        dl->AddLine(ImVec2(c.x - 5.2f, c.y - 4.6f), ImVec2(c.x - 5.9f, c.y - 1.5f), col, 1.6f);
        return;
    }

    ImVec2 a0(c.x - 8.0f, c.y - 4.8f);
    ImVec2 a1(c.x - 1.4f, c.y - 4.8f);
    ImVec2 a2(c.x + 4.2f, c.y + 4.8f);
    ImVec2 a3(c.x + 8.2f, c.y + 4.8f);
    ImVec2 b0(c.x - 8.0f, c.y + 4.8f);
    ImVec2 b1(c.x - 1.4f, c.y + 4.8f);
    ImVec2 b2(c.x + 4.2f, c.y - 4.8f);
    ImVec2 b3(c.x + 8.2f, c.y - 4.8f);
    dl->AddBezierCubic(a0, a1, a2, a3, col, 1.6f);
    dl->AddBezierCubic(b0, b1, b2, b3, col, 1.6f);
    dl->AddLine(ImVec2(a3.x - 3.1f, a3.y - 3.0f), a3, col, 1.6f);
    dl->AddLine(ImVec2(a3.x - 3.1f, a3.y + 3.0f), a3, col, 1.6f);
    dl->AddLine(ImVec2(b3.x - 3.1f, b3.y - 3.0f), b3, col, 1.6f);
    dl->AddLine(ImVec2(b3.x - 3.1f, b3.y + 3.0f), b3, col, 1.6f);
}

static bool MacActionButton(const char* id, const char* label, MacButtonIcon iconKind, ImVec2 size, bool primary) {
    ImGuiID imguiId = ImGui::GetID(id);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool pressed = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    float anim = PopAnim(imguiId, hovered, pressed);
    float hover = std::max(anim, 0.0f);
    float press = std::max(-anim, 0.0f);

    float lift = press > 0.0f ? 0.0f : hover * 2.2f;
    float scale = 1.0f - press * 0.018f + hover * 0.03f;
    ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f - lift);
    ImVec2 half(size.x * 0.5f * scale, size.y * 0.5f * scale);
    ImVec2 rmin(center.x - half.x, center.y - half.y);
    ImVec2 rmax(center.x + half.x, center.y + half.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (hover > 0.01f) {
        DrawSoftRectGlow(dl, rmin, rmax, 7.0f, 14.0f * hover, (primary ? 0.45f : 0.30f) * hover, theme::accent);
    }

    ImVec4 fill = primary ? (hover > 0.001f ? theme::accentHov : theme::accent) : (hover > 0.001f ? theme::glassHover : theme::panelBg);
    if (press > 0.001f) fill = primary ? theme::accentAct : theme::glassActive;
    ImVec4 textCol = primary ? theme::accentText : theme::text;

    dl->AddRectFilled(rmin, rmax, ImGui::ColorConvertFloat4ToU32(fill), 7.0f);
    ImVec4 borderCol = primary
        ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.55f + 0.30f * hover)
        : (hover > 0.01f ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.45f + 0.40f * hover) : theme::border);
    dl->AddRect(rmin, rmax, ImGui::ColorConvertFloat4ToU32(borderCol), 7.0f, 0, 1.0f);

    ImVec2 labelSize = ImGui::CalcTextSize(label);
    float iconW = 18.0f;
    float gap = 10.0f;
    float totalW = iconW + gap + labelSize.x;
    float startX = center.x - totalW * 0.5f;
    ImU32 col = ImGui::ColorConvertFloat4ToU32(textCol);
    DrawMacButtonIcon(dl, ImVec2(startX + iconW * 0.5f, center.y), iconKind, col);
    dl->AddText(ImVec2(startX + iconW + gap, center.y - labelSize.y * 0.5f),
        ImGui::ColorConvertFloat4ToU32(textCol), label);

    return clicked;
}

// Shows only the adapter currently in use (not the full adapter list) with a
// monochrome ethernet-port icon, matching the app's black/white/gray theme.
static void DrawAdapterCard(int& selected) {
    const float cardH = 64.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 max(pos.x + w, pos.y + cardH);

    dl->AddRectFilled(pos, max, ImGui::ColorConvertFloat4ToU32(theme::panelBg), 8.0f);
    dl->AddRect(pos, max, ImGui::ColorConvertFloat4ToU32(theme::border), 8.0f, 0, 1.0f);

    ImGui::Dummy(ImVec2(w, cardH));

    std::lock_guard<std::mutex> lock(backend::adaptersMutex);

    int activeIdx = -1;
    for (int i = 0; i < (int)backend::adapters.size(); ++i) {
        if (backend::adapters[i].isActive) { activeIdx = i; break; }
    }
    selected = activeIdx;

    if (activeIdx < 0) {
        const char* msg = backend::adapters.empty()
            ? "No adapters loaded yet - click Refresh."
            : "No active adapter detected - click Refresh.";
        dl->AddText(ImVec2(pos.x + 20.0f, pos.y + (cardH - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(theme::subtext), msg);
        return;
    }

    const auto& a = backend::adapters[activeIdx];

    float iconSize = 36.0f;
    ImVec2 iconPos(pos.x + 14.0f, pos.y + (cardH - iconSize) * 0.5f);
    IconBox(iconPos, iconSize, icon::ETHERNET_PORT, theme::softAccentBg, theme::accent, 9.0f);

    float textX = iconPos.x + iconSize + 14.0f;

    // Lay out right-to-left so the pill always hugs the card's right edge
    // instead of trailing off into empty space on wide cards.
    const char* pillText = "In use";
    ImVec2 pillTextSize = ImGui::CalcTextSize(pillText);
    float pillW = pillTextSize.x + 25.0f;
    ImVec2 pillMin(pos.x + w - 16.0f - pillW, pos.y + (cardH - 22.0f) * 0.5f);
    ImVec2 pillMax(pillMin.x + pillW, pillMin.y + 22.0f);

    std::string mac = a.currentMac.empty() ? "-- (could not read)" : a.currentMac;
    ImVec2 macSize = ImGui::CalcTextSize(mac.c_str());
    float macX = pillMin.x - 20.0f - macSize.x;

    std::string conn = a.connectionName.empty() ? "" : " (" + a.connectionName + ")";
    std::string left = FitTextToWidth(a.description + conn, macX - textX - 18.0f);
    ImVec2 leftSize = ImGui::CalcTextSize(left.c_str());

    dl->AddText(ImVec2(textX, pos.y + (cardH - leftSize.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(theme::text), left.c_str());
    dl->AddText(ImVec2(macX, pos.y + (cardH - macSize.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(theme::text), mac.c_str());

    dl->AddRectFilled(pillMin, pillMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg2), 6.0f);
    dl->AddRect(pillMin, pillMax, ImGui::ColorConvertFloat4ToU32(theme::border), 6.0f, 0, 1.0f);
    dl->AddCircleFilled(ImVec2(pillMin.x + 10.0f, pillMin.y + 11.0f), 2.2f, ImGui::ColorConvertFloat4ToU32(theme::text), 10);
    dl->AddText(ImVec2(pillMin.x + 18.0f, pillMin.y + (22.0f - pillTextSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(theme::text), pillText);
}

// Small hand-drawn "copy" glyph (two overlapping square outlines) so we don't
// depend on guessing an unverified codepoint in the subsetted icon font.
static void DrawCopyGlyph(ImDrawList* dl, ImVec2 topLeft, float size, ImU32 col) {
    float r = size * 0.62f;
    float off = size - r;
    dl->AddRect(ImVec2(topLeft.x, topLeft.y + off), ImVec2(topLeft.x + r, topLeft.y + off + r), col, 2.0f, 0, 1.3f);
    dl->AddRect(ImVec2(topLeft.x + off, topLeft.y), ImVec2(topLeft.x + off + r, topLeft.y + r), col, 2.0f, 0, 1.3f);
}

// Glassy status overview for the MAC Spoofer page: current MAC / connection /
// active state as three colored stat cards, plus a usage tip. Mirrors
// DrawLogCard's translucent-card chrome (soft glow + black glass fill)
// instead of a flat panel.
static void DrawMacStatusCard(float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    height = std::min(height, 286.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("MacStatusCard", ImVec2(0, height), true, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cardPos = ImGui::GetWindowPos();
        ImVec2 cardSize = ImGui::GetWindowSize();
        const float padX = 28.0f;

        DrawCardHeader(ImVec2(cardPos.x + padX, cardPos.y + 24.0f), icon::BAR_CHART, "Overview");
        ImGui::PushFont(theme::fontBody);
        ImGui::SetCursorScreenPos(ImVec2(cardPos.x + padX, cardPos.y + 48.0f));
        ImGui::TextColored(theme::subtext, "Real-time details about your network adapter.");
        ImGui::PopFont();

        std::string mac = "-- (no active adapter)";
        std::string conn = "--";
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(backend::adaptersMutex);
            for (auto& a : backend::adapters) {
                if (a.isActive) {
                    mac = a.currentMac.empty() ? "-- (could not read)" : a.currentMac;
                    conn = a.connectionName.empty() ? a.description : a.connectionName;
                    active = true;
                    break;
                }
            }
        }

        const ImVec4 colorPurple(0.36f, 0.66f, 0.96f, 1.0f);
        const ImVec4 colorBlue(0.40f, 0.64f, 0.98f, 1.0f);
        const ImVec4& colorGreen = theme::good;

        float rowTop = cardPos.y + 82.0f;
        float gap = 16.0f;
        float cellH = 90.0f;
        float cellW = (cardSize.x - padX * 2.0f - gap * 2.0f) / 3.0f;

        auto drawStatCell = [&](int i, unsigned iconCp, const ImVec4& color, const char* label,
                                 const std::string& value, bool showCopy, const char* subText) {
            float x = cardPos.x + padX + i * (cellW + gap);
            ImVec2 cMin(x, rowTop);
            ImVec2 cMax(x + cellW, rowTop + cellH);
            dl->AddRectFilled(cMin, cMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg2), 10.0f);
            dl->AddRect(cMin, cMax, ImGui::ColorConvertFloat4ToU32(theme::border), 10.0f, 0, 1.0f);

            float badgeSize = 38.0f;
            ImVec2 badgePos(x + 14.0f, rowTop + 14.0f);
            IconBox(badgePos, badgeSize, iconCp, ImVec4(color.x, color.y, color.z, 0.16f), color, badgeSize * 0.5f);

            float textX = badgePos.x + badgeSize + 14.0f;
            ImGui::PushFont(theme::fontBody);
            ImVec2 labelSize = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(textX, rowTop + 14.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), label);
            ImGui::PopFont();

            ImGui::PushFont(theme::fontBrand);
            float valueMaxW = cMax.x - textX - (showCopy ? 30.0f : 12.0f);
            std::string fitValue = FitTextToWidth(value, valueMaxW);
            float valueY = rowTop + 14.0f + labelSize.y + 6.0f;
            ImVec2 valueSize = ImGui::CalcTextSize(fitValue.c_str());
            dl->AddText(ImVec2(textX, valueY), ImGui::ColorConvertFloat4ToU32(theme::text), fitValue.c_str());
            ImGui::PopFont();

            if (showCopy) {
                ImVec2 copyTL(textX + valueSize.x + 10.0f, valueY + valueSize.y * 0.5f - 7.0f);
                ImGui::SetCursorScreenPos(copyTL);
                ImGui::InvisibleButton("##copy_mac", ImVec2(16, 16));
                bool hoveredCopy = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) ImGui::SetClipboardText(value.c_str());
                DrawCopyGlyph(dl, copyTL, 14.0f,
                    ImGui::ColorConvertFloat4ToU32(hoveredCopy ? theme::text : theme::subtext));
            }

            float subY = rowTop + cellH - 24.0f;
            dl->AddCircleFilled(ImVec2(x + 20.0f, subY + 8.0f), 3.0f, ImGui::ColorConvertFloat4ToU32(color), 12);
            ImGui::PushFont(theme::fontBody);
            dl->AddText(ImVec2(x + 30.0f, subY), ImGui::ColorConvertFloat4ToU32(theme::subtext), subText);
            ImGui::PopFont();
            };

        drawStatCell(0, icon::ETHERNET_PORT, colorPurple, "Current MAC Address", mac, true,
            active ? "Adapter detected" : "No adapter detected");
        drawStatCell(1, icon::GLOBE, colorBlue, "Connection", conn, false,
            active ? "Adapter is connected" : "Not connected");
        drawStatCell(2, icon::SHIELD_CHECK, colorGreen, "Status", active ? "Active" : "Inactive", false,
            active ? "Adapter is active" : "Adapter is idle");

        // --- Tip callout ---
        float tipY = rowTop + cellH + 16.0f;
        float tipH = 44.0f;
        ImVec2 tipMin(cardPos.x + padX, tipY);
        ImVec2 tipMax(cardPos.x + cardSize.x - padX, tipY + tipH);
        dl->AddRectFilled(tipMin, tipMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg), 8.0f);
        dl->AddRect(tipMin, tipMax, ImGui::ColorConvertFloat4ToU32(theme::border), 8.0f, 0, 1.0f);

        const ImVec4 colorAccent(0.45f, 0.68f, 0.98f, 1.0f);
        float dotR = 8.0f;
        ImVec2 dotCenter(tipMin.x + 24.0f, tipMin.y + tipH * 0.5f);
        dl->AddCircle(dotCenter, dotR, ImGui::ColorConvertFloat4ToU32(colorAccent), 16, 1.4f);
        ImGui::PushFont(theme::fontBrand);
        ImVec2 iSize = ImGui::CalcTextSize("i");
        dl->AddText(ImVec2(dotCenter.x - iSize.x * 0.5f, dotCenter.y - iSize.y * 0.5f),
            ImGui::ColorConvertFloat4ToU32(colorAccent), "i");
        ImGui::PopFont();

        const char* tip = "Click \"Spoof MAC\" to generate a new random MAC address and apply it to your network adapter.";
        float textX = dotCenter.x + dotR + 16.0f;
        float wrapWidth = tipMax.x - 16.0f - textX;
        ImVec2 tipSize = ImGui::CalcTextSize(tip, nullptr, false, wrapWidth);
        ImGui::SetCursorScreenPos(ImVec2(textX, tipMin.y + (tipH - tipSize.y) * 0.5f));
        ImGui::PushTextWrapPos(textX + wrapWidth);
        ImGui::TextColored(theme::subtext, "%s", tip);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// Status pill shared by every row in the Browser Cookies card: a softly
// tinted capsule (background + border + a small hand-drawn glyph, all keyed
// off the same status color) instead of a flat gray box with just a dot.
enum class CookieGlyph { Good, Warn, Neutral };

static void DrawCookieGlyph(ImDrawList* dl, ImVec2 center, float r, CookieGlyph kind, ImU32 col) {
    switch (kind) {
        case CookieGlyph::Good: {
            ImVec2 a(center.x - r * 0.60f, center.y + r * 0.02f);
            ImVec2 b(center.x - r * 0.12f, center.y + r * 0.52f);
            ImVec2 c(center.x + r * 0.62f, center.y - r * 0.42f);
            dl->AddLine(a, b, col, 1.6f);
            dl->AddLine(b, c, col, 1.6f);
            break;
        }
        case CookieGlyph::Warn: {
            dl->AddLine(ImVec2(center.x, center.y - r * 0.62f), ImVec2(center.x, center.y + r * 0.10f), col, 1.7f);
            dl->AddCircleFilled(ImVec2(center.x, center.y + r * 0.55f), 1.4f, col, 8);
            break;
        }
        default:
            dl->AddCircleFilled(center, r * 0.30f, col, 12);
            break;
    }
}

static void DrawCookieStatusPill(ImVec2 rowPos, float rightEdgeX, float rowH, const char* label, ImVec4 color, CookieGlyph glyph) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(theme::fontBrand);
    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    float pillW = textSize.x + 36.0f;
    float pillH = 25.0f;
    ImVec2 pillMin(rightEdgeX - pillW, rowPos.y + (rowH - pillH) * 0.5f);
    ImVec2 pillMax(pillMin.x + pillW, pillMin.y + pillH);

    dl->AddRectFilled(pillMin, pillMax, ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.16f)), pillH * 0.5f);
    dl->AddRect(pillMin, pillMax, ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.50f)), pillH * 0.5f, 0, 1.1f);
    DrawCookieGlyph(dl, ImVec2(pillMin.x + 15.0f, pillMin.y + pillH * 0.5f), 7.0f, glyph, ImGui::ColorConvertFloat4ToU32(color));
    ImGui::PushFont(theme::fontBrand);
    dl->AddText(ImVec2(pillMin.x + 27.0f, pillMin.y + (pillH - textSize.y) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(color), label);
    ImGui::PopFont();
}

// One entry in the Browser Cookies list: a roblox.com-cookie source (the
// local cookie file, or one browser's profile) and its current status.
struct CookieRowInfo {
    unsigned iconCp;
    std::string title;
    std::string caption;
    std::string pillLabel;
    ImVec4 color;
    CookieGlyph glyph;
};

// Each source gets its own elevated mini-card (fill + border + a status-
// colored accent bar down the left edge) instead of a bare row pinned
// between hairlines - reads as a list of distinct items, not one gray slab.
static void DrawCookieRow(ImVec2 pos, float width, float rowH, const CookieRowInfo& info) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cardMax(pos.x + width, pos.y + rowH);

    dl->AddRectFilled(pos, cardMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg2), 10.0f);
    dl->AddRect(pos, cardMax, ImGui::ColorConvertFloat4ToU32(theme::cardLine), 10.0f, 0, 1.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + 3.0f, cardMax.y),
        ImGui::ColorConvertFloat4ToU32(info.color), 10.0f, ImDrawFlags_RoundCornersLeft);

    float iconSize = 34.0f;
    ImVec2 iconPos(pos.x + 15.0f, pos.y + (rowH - iconSize) * 0.5f);
    IconBox(iconPos, iconSize, info.iconCp, ImVec4(info.color.x, info.color.y, info.color.z, 0.16f), info.color, 9.0f);

    float textX = iconPos.x + iconSize + 14.0f;
    ImGui::PushFont(theme::fontBrand);
    ImVec2 titleSize = ImGui::CalcTextSize(info.title.c_str());
    dl->AddText(ImVec2(textX, pos.y + rowH * 0.5f - titleSize.y - 1.0f),
        ImGui::ColorConvertFloat4ToU32(theme::text), info.title.c_str());
    ImGui::PopFont();
    dl->AddText(ImVec2(textX, pos.y + rowH * 0.5f + 2.0f),
        ImGui::ColorConvertFloat4ToU32(theme::subtext), info.caption.c_str());

    DrawCookieStatusPill(pos, cardMax.x - 14.0f, rowH, info.pillLabel.c_str(), info.color, info.glyph);
}

static CookieRowInfo BuildBrowserCookieRowInfo(const char* name, const char* engine, const BrowserCookieStatus* status, bool scanning) {
    CookieRowInfo info;
    info.iconCp = icon::GLOBE;
    info.title = name;
    info.caption = engine;
    if (!status) {
        info.pillLabel = scanning ? "Scanning..." : "Pending";
        info.color = theme::subtext;
        info.glyph = CookieGlyph::Neutral;
    } else if (!status->installed) {
        info.pillLabel = "Not Installed";
        info.color = theme::subtext;
        info.glyph = CookieGlyph::Neutral;
    } else if (status->found) {
        info.pillLabel = std::to_string(status->count) + (status->count == 1 ? " Cookie Found" : " Cookies Found");
        info.color = theme::warn;
        info.glyph = CookieGlyph::Warn;
    } else if (status->scanFailed) {
        // The profile's Cookies DB couldn't be located, copied, or queried -
        // in practice that's almost always because the browser is open right
        // now with a live roblox.com cookie still in the WAL file we can't
        // read. Fail closed (assume dirty) rather than show an ambiguous
        // "Unverified" - check the Logs tab for the specific reason.
        info.pillLabel = "Cookies Found";
        info.color = theme::warn;
        info.glyph = CookieGlyph::Warn;
    } else {
        info.pillLabel = "No Cookies";
        info.color = theme::good;
        info.glyph = CookieGlyph::Good;
    }
    return info;
}

static CookieRowInfo BuildRobloxFileCookieRowInfo() {
    bool hasData = backend::RobloxCookieFileHasData();
    CookieRowInfo info;
    info.iconCp = icon::FILE_TEXT;
    info.title = "Roblox Local Cookie File";
    info.caption = "Stored under your Local AppData\\Roblox folder";
    info.pillLabel = hasData ? "Cookie Data Found" : "Clean / Locked";
    info.color = hasData ? theme::warn : theme::good;
    info.glyph = hasData ? CookieGlyph::Warn : CookieGlyph::Good;
    return info;
}

// Lists every browser the cookie cleaner supports and whether it currently
// holds a roblox.com cookie. Backed by backend::browserCookieStatus, which
// PageCookieCleaner() refreshes (read-only) each time its tab is opened.
static void DrawBrowserCookieCard(float maxHeight) {
    struct BrowserDef { const char* name; const char* engine; };
    static const BrowserDef kSupportedBrowsers[] = {
        { "Google Chrome", "Chromium engine" },
        { "Microsoft Edge", "Chromium engine" },
        { "Opera", "Chromium engine" },
        { "Opera GX", "Chromium engine" },
        { "Firefox", "Gecko engine" },
    };
    const size_t count = sizeof(kSupportedBrowsers) / sizeof(kSupportedBrowsers[0]);

    const float padX = 24.0f;
    const float headerH = 56.0f;
    const float rowH = 56.0f;
    const float rowGap = 10.0f;
    const float bottomPad = 16.0f;
    float contentHeight = headerH + (float)(count + 1) * rowH + (float)count * rowGap + bottomPad;
    float height = std::min(maxHeight, contentHeight);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("BrowserCookieCard", ImVec2(0, height), true);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cardPos = ImGui::GetWindowPos();
        ImVec2 cardSize = ImGui::GetWindowSize();
        const float headerY = cardPos.y + 18.0f;

        DrawCardHeader(ImVec2(cardPos.x + padX, headerY), icon::GLOBE, "Browser Cookies");

        // Manual rescan - the automatic scan only ever runs once, when the tab
        // is first opened, so a cookie written afterward (e.g. signing in
        // while this tab is already open) is never picked up without this.
        const float refreshBtnSize = 28.0f;
        float controlsRight = cardPos.x + cardSize.x - padX;
        ImVec2 refreshPos(controlsRight - refreshBtnSize, headerY - 3.0f);
        {
            ImGuiID refreshId = ImGui::GetID("##refresh_cookies");
            ImGui::SetCursorScreenPos(refreshPos);
            ImGui::InvisibleButton("##refresh_cookies", ImVec2(refreshBtnSize, refreshBtnSize));
            bool refreshHovered = ImGui::IsItemHovered();
            bool refreshClicked = ImGui::IsItemClicked();
            float refreshAnim = std::max(PopAnim(refreshId, refreshHovered, false), 0.0f);
            bool scanningNow = backend::browserCookieScanning.load();

            ImVec4 btnFill = refreshHovered && !scanningNow
                ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.16f + 0.06f * refreshAnim)
                : theme::panelBg2;
            dl->AddRectFilled(refreshPos, ImVec2(refreshPos.x + refreshBtnSize, refreshPos.y + refreshBtnSize),
                ImGui::ColorConvertFloat4ToU32(btnFill), 8.0f);
            dl->AddRect(refreshPos, ImVec2(refreshPos.x + refreshBtnSize, refreshPos.y + refreshBtnSize),
                ImGui::ColorConvertFloat4ToU32(refreshHovered ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.5f) : theme::border),
                8.0f, 0, 1.0f);

            std::string refreshGlyph = icon::Str(icon::REFRESH_CW);
            ImGui::PushFont(theme::fontIcon);
            ImVec2 glyphSize = ImGui::CalcTextSize(refreshGlyph.c_str());
            ImVec2 glyphCenter(refreshPos.x + refreshBtnSize * 0.5f, refreshPos.y + refreshBtnSize * 0.5f);
            // Pulse the icon's alpha while a scan is in flight, so the button
            // itself shows work happening, not just the "Scanning..." text
            // off to the side.
            float iconAlpha = scanningNow
                ? (0.55f + 0.45f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.0f)))
                : 1.0f;
            ImVec4 iconColor(theme::subtext.x, theme::subtext.y, theme::subtext.z, iconAlpha);
            dl->AddText(ImVec2(glyphCenter.x - glyphSize.x * 0.5f, glyphCenter.y - glyphSize.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(iconColor), refreshGlyph.c_str());
            ImGui::PopFont();

            if (refreshClicked && !scanningNow) {
                std::thread(backend::ScanBrowserCookies).detach();
            }
        }

        std::vector<BrowserCookieStatus> snapshot;
        {
            std::lock_guard<std::mutex> lock(backend::browserCookieMutex);
            snapshot = backend::browserCookieStatus;
        }

        bool scanning = backend::browserCookieScanning.load();
        int totalFound = backend::RobloxCookieFileHasData() ? 1 : 0;
        bool anyUnverified = false;
        for (auto& s : snapshot) {
            if (s.installed && s.found) totalFound += s.count;
            if (s.installed && s.scanFailed) anyUnverified = true;
        }

        // Right-aligned status chip: a pulsing dot while scanning, otherwise
        // a quick "all clean" / "N found" readout so the header itself
        // answers the question before you read a single row. Anchored off
        // the refresh button's left edge so the two never overlap.
        float chipAreaRight = refreshPos.x - 10.0f;
        if (scanning) {
            float pulse = 0.4f + 0.6f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.0f));
            ImGui::PushFont(theme::fontBrand);
            const char* scanText = "Scanning...";
            ImVec2 scanSize = ImGui::CalcTextSize(scanText);
            float dotR = 4.0f;
            float chipX = chipAreaRight - scanSize.x - dotR * 2.0f - 8.0f;
            dl->AddCircleFilled(ImVec2(chipX, headerY + scanSize.y * 0.5f), dotR,
                ImGui::ColorConvertFloat4ToU32(ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, pulse)), 12);
            dl->AddText(ImVec2(chipX + dotR * 2.0f + 8.0f, headerY - 1.0f),
                ImGui::ColorConvertFloat4ToU32(theme::subtext), scanText);
            ImGui::PopFont();
        } else {
            std::string summary = totalFound > 0
                ? (std::to_string(totalFound) + (totalFound == 1 ? " cookie found" : " cookies found"))
                : (anyUnverified ? "Cookies found" : "All clean");
            ImVec4 sumColor = (totalFound > 0 || anyUnverified) ? theme::warn : theme::good;
            CookieGlyph sumGlyph = (totalFound > 0 || anyUnverified) ? CookieGlyph::Warn : CookieGlyph::Good;
            ImGui::PushFont(theme::fontBrand);
            ImVec2 sumSize = ImGui::CalcTextSize(summary.c_str());
            ImGui::PopFont();
            float chipPadX = 12.0f, chipH = 26.0f;
            float chipTop = headerY + 9.0f - chipH * 0.5f;
            float chipRight = chipAreaRight;
            ImVec2 chipMin(chipRight - sumSize.x - chipPadX * 2.0f - 14.0f, chipTop);
            dl->AddRectFilled(chipMin, ImVec2(chipRight, chipMin.y + chipH),
                ImGui::ColorConvertFloat4ToU32(ImVec4(sumColor.x, sumColor.y, sumColor.z, 0.16f)), chipH * 0.5f);
            dl->AddRect(chipMin, ImVec2(chipRight, chipMin.y + chipH),
                ImGui::ColorConvertFloat4ToU32(ImVec4(sumColor.x, sumColor.y, sumColor.z, 0.45f)), chipH * 0.5f, 0, 1.0f);
            DrawCookieGlyph(dl, ImVec2(chipMin.x + chipPadX + 4.0f, chipMin.y + chipH * 0.5f), 7.0f, sumGlyph,
                ImGui::ColorConvertFloat4ToU32(sumColor));
            ImGui::PushFont(theme::fontBrand);
            dl->AddText(ImVec2(chipMin.x + chipPadX + 16.0f, chipMin.y + (chipH - sumSize.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(sumColor), summary.c_str());
            ImGui::PopFont();
        }

        float rowW = cardSize.x - padX * 2.0f;
        float rowY = cardPos.y + headerH;

        DrawCookieRow(ImVec2(cardPos.x + padX, rowY), rowW, rowH, BuildRobloxFileCookieRowInfo());
        rowY += rowH + rowGap;

        for (size_t i = 0; i < count; ++i) {
            const BrowserCookieStatus* status = nullptr;
            for (auto& s : snapshot) {
                if (s.name == kSupportedBrowsers[i].name) { status = &s; break; }
            }
            DrawCookieRow(ImVec2(cardPos.x + padX, rowY), rowW, rowH,
                BuildBrowserCookieRowInfo(kSupportedBrowsers[i].name, kSupportedBrowsers[i].engine, status, scanning));
            rowY += rowH + rowGap;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
static void PageMultiInstance() {
    SectionTitle(icon::ROCKET, "Multi-Instance Launcher",
        "Watches for Roblox and closes the singleton lock handle so you can run more than one client at once.");

    bool watching = backend::watching;
    ImGui::Indent(12);
    if (watching) {
        if (PrimaryIconButton(icon::PLAY, "Stop Watching", ImVec2(190, 48))) backend::StopWatching();
    } else {
        if (PrimaryIconButton(icon::PLAY, "Start Watching", ImVec2(190, 48))) backend::StartWatching();
    }
    ImGui::SameLine();
    if (SecondaryIconButton(icon::PLUS, "Launch New Instance", ImVec2(230, 48))) {
        std::thread(backend::LaunchNewInstance).detach();
    }
    ImGui::Unindent(12);

    const ImVec4 colorBlue(0.40f, 0.64f, 0.98f, 1.0f);
    ImVec4 statusColor = watching ? theme::good : theme::bad;
    ImGui::Dummy(ImVec2(0, 16));
    StatusCard({
        { icon::CIRCLE, "Status:", watching ? "Watching" : "Idle", statusColor, statusColor },
        { icon::BOX, "Instances detected:", std::to_string(backend::instanceCount.load()), theme::text, colorBlue },
        { icon::CLOCK, "Uptime:", backend::GetUptimeString(), theme::text, theme::warn },
        });

    ImGui::Dummy(ImVec2(0, 18));
    DrawMultiInstanceOverview();
    ImGui::Dummy(ImVec2(0, 14));
    DrawMultiInstanceQuickNav();
}

static void PageCookieCleaner() {
    SectionTitle(icon::COOKIE, "Cookie Cleaner",
        "Deletes the Roblox cookie file and removes roblox.com cookies from Chrome, Edge, Opera, Opera GX and Firefox.");

    ImGui::Indent(12);
    if (PrimaryIconButton(icon::TRASH2, "Clear Roblox Folder Cookies", ImVec2(250, 38))) {
        std::thread(backend::ClearRobloxCookieFile).detach();
    }
    ImGui::SameLine(0, 12);
    if (SecondaryIconButton(icon::COOKIE, "Clear Browser Cookies", ImVec2(220, 38))) {
        std::thread(backend::ClearBrowserCookies).detach();
    }
    ImGui::Unindent(12);

    ImGui::Dummy(ImVec2(0, 20));
    DrawBrowserCookieCard(ImGui::GetContentRegionAvail().y);
}

static void PageMacSpoofer() {
    static int selected = -1;
    static bool firstShow = true;
    if (firstShow) {
        std::thread(backend::RefreshAdapters).detach();
        firstShow = false;
    }

    SectionTitle(icon::SHARE2, "MAC Address Spoofer",
        "Randomizes the MAC address of your active network adapter. This briefly disables and re-enables it.");

    DrawAdapterCard(selected);

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::Indent(12);
    if (MacActionButton("##mac_refresh", "Refresh", MacButtonIcon::Refresh, ImVec2(126, 40), false)) {
        std::thread(backend::RefreshAdapters).detach();
    }
    ImGui::SameLine(0, 12);
    if (MacActionButton("##mac_spoof", "Spoof MAC", MacButtonIcon::Shuffle, ImVec2(150, 40), true)) {
        int idx = selected;
        std::thread([idx]() { backend::SpoofAdapter(idx); }).detach();
    }
    ImGui::SameLine(0, 12);
    if (MacActionButton("##mac_restore", "Restore Default", MacButtonIcon::Restore, ImVec2(168, 40), false)) {
        int idx = selected;
        std::thread([idx]() { backend::RestoreAdapter(idx); }).detach();
    }
    ImGui::Unindent(12);

    ImGui::Dummy(ImVec2(0, 12));
    DrawMacStatusCard(ImGui::GetContentRegionAvail().y);
}

// ---------------------------------------------------------------------------
// Accounts page helpers: avatar circles + linked-account chips
// ---------------------------------------------------------------------------
static void DrawAvatarCircle(ImVec2 pos, float size, ID3D11ShaderResourceView* tex, const std::string& username) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
    if (tex) {
        dl->AddImageRounded(tex, pos, ImVec2(pos.x + size, pos.y + size), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, size * 0.5f);
    } else {
        dl->AddCircleFilled(center, size * 0.5f, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.08f)), 32);
        std::string initial = username.empty() ? "?" : username.substr(0, 1);
        if (!initial.empty()) initial[0] = (char)toupper((unsigned char)initial[0]);
        ImGui::PushFont(theme::fontBrand);
        ImVec2 ts = ImGui::CalcTextSize(initial.c_str());
        dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), ImGui::ColorConvertFloat4ToU32(theme::subtext), initial.c_str());
        ImGui::PopFont();
    }
    dl->AddCircle(center, size * 0.5f, ImGui::ColorConvertFloat4ToU32(theme::border), 32, 1.2f);
}

static bool DrawAccountChip(int index, const RobloxAccount& acc, bool isActive, ImVec2 size) {
    ImGui::PushID(index);
    ImGuiID animId = ImGui::GetID("chip");
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("chip", size);
    bool hovered = ImGui::IsItemHovered();
    bool pressed = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    float anim = std::max(PopAnim(animId, hovered, pressed), 0.0f);

    float lift = (pressed || !hovered) ? 0.0f : anim * 2.0f;
    ImVec2 liftedPos(pos.x, pos.y - lift);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 max(liftedPos.x + size.x, liftedPos.y + size.y);

    // Active account keeps a permanent soft glow; hovering any chip blooms
    // one in too, so selection and hover speak the same visual language.
    // The chip sits in a horizontally-scrolling strip sized exactly to its
    // height, so the glow needs an expanded clip rect or it gets hard-cut
    // top and bottom (the same bug the Community card had).
    float glowAlpha = (isActive ? 0.14f : 0.0f) + 0.30f * anim;
    if (glowAlpha > 0.01f) {
        float glowSpread = 10.0f + 6.0f * anim;
        ImVec2 clipPad(glowSpread + 6.0f, glowSpread + 6.0f);
        dl->PushClipRect(ImVec2(liftedPos.x - clipPad.x, liftedPos.y - clipPad.y),
            ImVec2(max.x + clipPad.x, max.y + clipPad.y), false);
        DrawSoftRectGlow(dl, liftedPos, max, 12.0f, glowSpread, glowAlpha, theme::accent);
        dl->PopClipRect();
    }

    ImVec4 fillCol = anim > 0.001f ? theme::glassHover : theme::panelBg;
    ImVec4 borderCol = isActive
        ? theme::accent
        : (anim > 0.01f ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.45f * anim) : theme::border);
    dl->AddRectFilled(liftedPos, max, ImGui::ColorConvertFloat4ToU32(fillCol), 12.0f);
    dl->AddRect(liftedPos, max, ImGui::ColorConvertFloat4ToU32(borderCol), 12.0f, 0, isActive ? 1.6f : 1.0f);
    pos = liftedPos;

    float avatarSize = 40.0f;
    ImVec2 avatarPos(pos.x + 14.0f, pos.y + 14.0f);
    DrawAvatarCircle(avatarPos, avatarSize, GetOrCreateAvatarTexture(acc), acc.username);

    float textX = avatarPos.x + avatarSize + 12.0f;
    float textMaxW = (pos.x + size.x - 12.0f) - textX;
    ImGui::PushFont(theme::fontBrand);
    std::string name = FitTextToWidth(acc.alias.empty() ? acc.username : acc.alias, textMaxW);
    dl->AddText(ImVec2(textX, pos.y + 18.0f), ImGui::ColorConvertFloat4ToU32(theme::text), name.c_str());
    ImGui::PopFont();
    std::string idStr = "ID: " + std::to_string(acc.userId);
    dl->AddText(ImVec2(textX, pos.y + 40.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), idStr.c_str());

    if (isActive) {
        const char* badge = "Active";
        ImVec2 badgeTextSize = ImGui::CalcTextSize(badge);
        float bh = 18.0f;
        float bw = badgeTextSize.x + 20.0f;
        ImVec2 bMin(pos.x + 14.0f, max.y - bh - 12.0f);
        ImVec2 bMax(bMin.x + bw, bMin.y + bh);
        dl->AddRectFilled(bMin, bMax, ImGui::ColorConvertFloat4ToU32(ImVec4(theme::good.x, theme::good.y, theme::good.z, 0.16f)), bh * 0.5f);
        dl->AddCircleFilled(ImVec2(bMin.x + 10.0f, bMin.y + bh * 0.5f), 3.0f, ImGui::ColorConvertFloat4ToU32(theme::good), 10);
        dl->AddText(ImVec2(bMin.x + 18.0f, bMin.y + (bh - badgeTextSize.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(theme::good), badge);
    }

    ImGui::PopID();
    return clicked;
}

static bool DrawAddAccountChip(ImVec2 size, bool busy) {
    ImGui::PushID("add_account_chip");
    ImGuiID id = ImGui::GetID("addchip");
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("addchip", size);
    bool hovered = ImGui::IsItemHovered() && !busy;
    bool clicked = ImGui::IsItemClicked() && !busy;
    float anim = std::max(PopAnim(id, hovered, false), 0.0f);

    float lift = anim * 2.0f;
    ImVec2 liftedPos(pos.x, pos.y - lift);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 max(liftedPos.x + size.x, liftedPos.y + size.y);

    if (anim > 0.01f) {
        float glowSpread = 10.0f + 6.0f * anim;
        ImVec2 clipPad(glowSpread + 6.0f, glowSpread + 6.0f);
        dl->PushClipRect(ImVec2(liftedPos.x - clipPad.x, liftedPos.y - clipPad.y),
            ImVec2(max.x + clipPad.x, max.y + clipPad.y), false);
        DrawSoftRectGlow(dl, liftedPos, max, 12.0f, glowSpread, 0.30f * anim, theme::accent);
        dl->PopClipRect();
    }

    ImVec4 fillCol = anim > 0.001f ? theme::glassHover : theme::panelBg;
    ImVec4 borderCol = anim > 0.01f ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.45f * anim) : theme::border;
    dl->AddRectFilled(liftedPos, max, ImGui::ColorConvertFloat4ToU32(fillCol), 12.0f);
    dl->AddRect(liftedPos, max, ImGui::ColorConvertFloat4ToU32(borderCol), 12.0f, 0, 1.0f);
    pos = liftedPos;

    ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f - 8.0f);
    ImU32 plusCol = ImGui::ColorConvertFloat4ToU32(theme::text);
    dl->AddLine(ImVec2(center.x - 8.0f, center.y), ImVec2(center.x + 8.0f, center.y), plusCol, 1.6f);
    dl->AddLine(ImVec2(center.x, center.y - 8.0f), ImVec2(center.x, center.y + 8.0f), plusCol, 1.6f);

    const char* label = busy ? "Signing in..." : "Add Account";
    ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + (size.x - ls.x) * 0.5f, pos.y + size.y - 26.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), label);

    ImGui::PopID();
    return clicked;
}

static void PageAccounts(HWND hwnd) {
    static int selected = -1;
    static char placeIdBuf[32] = "";
    static bool placeIdBufInit = false;
    static std::atomic<bool> loginInProgress{ false };
    static int contextEditIndex = -1;
    static char aliasEditBuf[64] = "";
    static char passwordEditBuf[512] = "";
    static bool wantOpenAliasModal = false;
    static bool wantOpenPasswordModal = false;

    SectionTitle(icon::USER, "Account Launcher", "Manage and launch your Roblox accounts.");
    {
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        ImVec2 oldCursor = ImGui::GetCursorScreenPos();
        long long pid = backend::savedPlaceId.load();
        bool canQuickLaunch = pid > 0;
        {
            std::lock_guard<std::mutex> lock(backend::accountsMutex);
            canQuickLaunch = canQuickLaunch && selected >= 0 && selected < (int)backend::accounts.size();
        }
        ImGui::SetCursorScreenPos(ImVec2(winPos.x + winSize.x - 220.0f, winPos.y + 20.0f));
        ImGui::PushID("quick_launch_top");
        // Kept clickable even when prerequisites are missing - a disabled ImGui
        // item can't be hovered or pressed at all, which reads as "the button is
        // broken". Instead we validate on click and tell the user what's needed.
        if (PrimaryIconButton(icon::PLAY, "Launch Roblox", ImVec2(190.0f, 42.0f))) {
            if (!canQuickLaunch) {
                backend::Log("[!] Select an account and set a Place ID first, then Launch Roblox.");
            } else {
                int idx = selected;
                std::thread([idx, pid]() { backend::LaunchAccountIntoPlace(idx, pid); }).detach();
            }
        }
        ImGui::PopID();

        // Open Web: launch a separate browser instance signed into the selected
        // account. Only needs an account with a cookie - no Place ID required.
        bool canOpenWeb = false;
        {
            std::lock_guard<std::mutex> lock(backend::accountsMutex);
            canOpenWeb = selected >= 0 && selected < (int)backend::accounts.size()
                         && !backend::accounts[selected].cookie.empty();
        }
        ImGui::SetCursorScreenPos(ImVec2(winPos.x + winSize.x - 380.0f, winPos.y + 20.0f));
        ImGui::PushID("open_web_top");
        if (SecondaryIconButton(icon::GLOBE, "Open Web", ImVec2(150.0f, 42.0f))) {
            if (!canOpenWeb) {
                backend::Log("[!] Add and select an account first, then Open Web.");
            } else {
                int idx = selected;
                std::thread([idx]() { backend::OpenAccountWeb(idx); }).detach();
            }
        }
        ImGui::PopID();

        ImGui::SetCursorScreenPos(oldCursor);
    }

    if (!placeIdBufInit) {
        long long pid = backend::savedPlaceId.load();
        if (pid > 0) {
            std::string s = std::to_string(pid);
            size_t len = std::min(s.size(), sizeof(placeIdBuf) - 1);
            memcpy(placeIdBuf, s.data(), len);
            placeIdBuf[len] = '\0';
        }
        placeIdBufInit = true;
    }

    bool hasSelectedAccount = false;
    std::string selectedUsername, selectedAlias;
    long long selectedUserId = 0;
    long long selFriends = -1, selFollowers = -1, selFollowing = -1;
    std::string selJoinDate, selAccountAge;
    bool selStatsLoaded = false;

    // --- Linked Accounts strip ---
    {
        const float chipW = 190.0f, chipH = 92.0f, gap = 10.0f;
        const float cardH = chipH + 64.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panelBg);
        ImGui::PushStyleColor(ImGuiCol_Border, theme::border);
        ImGui::BeginChild("LinkedAccountsCard", ImVec2(0, cardH), true);
        {
            ImVec2 cardPos = ImGui::GetWindowPos();
            DrawCardHeader(ImVec2(cardPos.x + 24.0f, cardPos.y + 22.0f), icon::SHARE2, "Linked Accounts");
            ImGui::SetCursorScreenPos(ImVec2(cardPos.x + 24.0f, cardPos.y + 56.0f));

            ImGui::BeginChild("LinkedAccountsStrip", ImVec2(ImGui::GetContentRegionAvail().x - 24.0f, chipH), false, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> lock(backend::accountsMutex);
                if (selected >= (int)backend::accounts.size()) selected = (int)backend::accounts.size() - 1;
                if (selected < -1) selected = -1;
                if (selected == -1 && !backend::accounts.empty()) selected = 0;

                for (int i = 0; i < (int)backend::accounts.size(); ++i) {
                    auto& a = backend::accounts[i];
                    if (!a.statsRequested) { a.statsRequested = true; std::thread(backend::FetchAccountStats, i).detach(); }
                    if (!a.avatarRequested) { a.avatarRequested = true; std::thread(backend::FetchAccountAvatar, i).detach(); }

                    if (i > 0) ImGui::SameLine(0, gap);
                    if (DrawAccountChip(i, a, selected == i, ImVec2(chipW, chipH))) selected = i;
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected = i;
                    std::string ctxPopupId = "account_context##" + std::to_string(i);
                    if (ImGui::BeginPopupContextItem(ctxPopupId.c_str())) {
                        selected = i;
                        const bool hasPassword = !a.password.empty();
                        if (ImGui::MenuItem("Copy Username")) { ImGui::SetClipboardText(a.username.c_str()); backend::Log("[v] Copied username for " + a.username); }
                        if (ImGui::MenuItem("Copy Password", nullptr, false, hasPassword)) { ImGui::SetClipboardText(a.password.c_str()); backend::Log("[v] Copied password for " + a.username); }
                        std::string combo = a.username + ":" + a.password;
                        if (ImGui::MenuItem("Copy User:Pass", nullptr, false, hasPassword)) { ImGui::SetClipboardText(combo.c_str()); backend::Log("[v] Copied user:pass for " + a.username); }
                        std::string cookieStr = ".ROBLOSECURITY=" + a.cookie;
                        if (ImGui::MenuItem("Copy Cookies", nullptr, false, !a.cookie.empty())) { ImGui::SetClipboardText(cookieStr.c_str()); backend::Log("[v] Copied cookies for " + a.username); }
                        if (ImGui::MenuItem("Open in Browser", nullptr, false, !a.cookie.empty())) {
                            int idx = i;
                            std::thread([idx]() { backend::OpenAccountWeb(idx); }).detach();
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Set Alias...")) {
                            contextEditIndex = i;
                            size_t len = std::min(a.alias.size(), sizeof(aliasEditBuf) - 1);
                            memcpy(aliasEditBuf, a.alias.data(), len);
                            aliasEditBuf[len] = '\0';
                            wantOpenAliasModal = true; // opened below, outside this nested popup's own ID scope
                        }
                        if (ImGui::MenuItem("Set Password...")) {
                            contextEditIndex = i;
                            size_t len = std::min(a.password.size(), sizeof(passwordEditBuf) - 1);
                            memcpy(passwordEditBuf, a.password.data(), len);
                            passwordEditBuf[len] = '\0';
                            wantOpenPasswordModal = true;
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Remove Account")) {
                            int idx = i;
                            if (selected == idx) selected = -1;
                            std::thread([idx]() { backend::RemoveAccount(idx); }).detach();
                        }
                        ImGui::EndPopup();
                    }
                }

                if (!backend::accounts.empty()) ImGui::SameLine(0, gap);
                bool busy = loginInProgress.load();
                if (DrawAddAccountChip(ImVec2(chipW, chipH), busy) && !busy) {
                    loginInProgress.store(true);
                    std::thread([]() {
                        login::ShowRobloxLoginWindow(g_exeDir,
                            [](bool success, std::string cookie) {
                                if (success) backend::AddAccountFromCookie(cookie);
                                else backend::Log("[i] Login cancelled.");
                                loginInProgress.store(false);
                            });
                        }).detach();
                }

                hasSelectedAccount = selected >= 0 && selected < (int)backend::accounts.size();
                if (hasSelectedAccount) {
                    auto& a = backend::accounts[selected];
                    selectedUsername = a.username;
                    selectedAlias = a.alias;
                    selectedUserId = a.userId;
                    selFriends = a.friendsCount; selFollowers = a.followersCount; selFollowing = a.followingCount;
                    selJoinDate = a.joinDate; selAccountAge = a.accountAge;
                    selStatsLoaded = a.statsLoaded;
                }
            } // accountsMutex released here - safe to open/draw modals below

            // Opening these here (not inside the context-menu popup above) keeps the ID
            // resolution consistent with the BeginPopupModal calls right below.
            if (wantOpenAliasModal) { wantOpenAliasModal = false; ImGui::OpenPopup("SetAliasModal"); }
            if (wantOpenPasswordModal) { wantOpenPasswordModal = false; ImGui::OpenPopup("SetPasswordModal"); }

            ImGui::SetNextWindowSize(ImVec2(340, 0));
            if (ImGui::BeginPopupModal("SetAliasModal", nullptr, ImGuiWindowFlags_NoResize)) {
                ImGui::TextColored(theme::subtext, "Display name shown instead of the Roblox username.");
                ImGui::Spacing();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##alias_edit", aliasEditBuf, sizeof(aliasEditBuf));
                ImGui::Spacing();
                if (PrimaryButton("Save", ImVec2(100, 32))) {
                    int idx = contextEditIndex;
                    std::string alias = aliasEditBuf;
                    std::thread([idx, alias]() { backend::SetAccountAlias(idx, alias); }).detach();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (SecondaryButton("Cancel", ImVec2(100, 32))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            ImGui::SetNextWindowSize(ImVec2(340, 0));
            if (ImGui::BeginPopupModal("SetPasswordModal", nullptr, ImGuiWindowFlags_NoResize)) {
                ImGui::TextColored(theme::subtext, "Stored locally for your reference only.");
                ImGui::Spacing();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##password_edit", passwordEditBuf, sizeof(passwordEditBuf), ImGuiInputTextFlags_Password);
                ImGui::Spacing();
                if (PrimaryButton("Save", ImVec2(100, 32))) {
                    int idx = contextEditIndex;
                    std::string password = passwordEditBuf;
                    std::thread([idx, password]() { backend::SetAccountPassword(idx, password); }).detach();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (SecondaryButton("Cancel", ImVec2(100, 32))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    ImGui::Dummy(ImVec2(0, 16));

    // --- Account Overview + Place Configuration ---
    {
        float fullW = ImGui::GetContentRegionAvail().x;
        float gap = 16.0f;
        float colW = (fullW - gap) * 0.5f;
        float cardH = 380.0f;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Account Overview (left)
        ImVec2 leftMin = origin, leftMax(origin.x + colW, origin.y + cardH);
        dl->AddRectFilled(leftMin, leftMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg), 10.0f);
        dl->AddRect(leftMin, leftMax, ImGui::ColorConvertFloat4ToU32(theme::border), 10.0f, 0, 1.0f);
        {
            float padX = 22.0f;
            ImVec2 p(leftMin.x + padX, leftMin.y + 20.0f);
            float rowW = colW - padX * 2.0f;
            DrawCardHeader(p, icon::USER, "Account Overview");

            if (!hasSelectedAccount) {
                dl->AddText(ImVec2(p.x, p.y + 44.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), "Select an account above to see its overview.");
            } else {
                std::string displayName = selectedAlias.empty() ? selectedUsername : selectedAlias;
                float avatarSize = 72.0f;
                ImVec2 avatarPos(p.x, p.y + 40.0f);
                ID3D11ShaderResourceView* tex = nullptr;
                {
                    std::lock_guard<std::mutex> lock(backend::accountsMutex);
                    if (selected >= 0 && selected < (int)backend::accounts.size())
                        tex = GetOrCreateAvatarTexture(backend::accounts[selected]);
                }
                DrawAvatarCircle(avatarPos, avatarSize, tex, displayName);

                float textX = avatarPos.x + avatarSize + 16.0f;
                ImGui::PushFont(theme::fontBrand);
                dl->AddText(ImVec2(textX, avatarPos.y + 4.0f), ImGui::ColorConvertFloat4ToU32(theme::text), displayName.c_str());
                ImGui::PopFont();

                const char* badge = "Active";
                ImVec2 bts = ImGui::CalcTextSize(badge);
                float bh = 18.0f, bw = bts.x + 18.0f;
                ImVec2 bMin(textX, avatarPos.y + 30.0f);
                dl->AddRectFilled(bMin, ImVec2(bMin.x + bw, bMin.y + bh), ImGui::ColorConvertFloat4ToU32(ImVec4(theme::good.x, theme::good.y, theme::good.z, 0.16f)), bh * 0.5f);
                dl->AddText(ImVec2(bMin.x + 9.0f, bMin.y + (bh - bts.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(theme::good), badge);

                std::string idLine = "ID: " + std::to_string(selectedUserId);
                if (!selectedAlias.empty()) idLine += "   ·   @" + selectedUsername;
                dl->AddText(ImVec2(textX, avatarPos.y + 56.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), idLine.c_str());

                float infoY = p.y + 132.0f;
                std::string ageLine = "Account Age: " + (selStatsLoaded && !selAccountAge.empty() ? selAccountAge : std::string("loading..."));
                std::string joinLine = "Join Date: " + (selStatsLoaded && !selJoinDate.empty() ? selJoinDate : std::string("loading..."));
                dl->AddText(ImVec2(p.x, infoY), ImGui::ColorConvertFloat4ToU32(theme::subtext), ageLine.c_str());
                dl->AddText(ImVec2(p.x, infoY + 20.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), joinLine.c_str());

                auto fmtStat = [](long long v) -> std::string { return v < 0 ? std::string("-") : std::to_string(v); };
                ImGui::SetCursorScreenPos(ImVec2(p.x, infoY + 48.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
                ImGui::BeginChild("overview_stats", ImVec2(rowW, 62.0f), false, ImGuiWindowFlags_NoScrollbar);
                StatusCard({
                    { icon::USER, "Friends", fmtStat(selFriends), theme::text },
                    { icon::SHARE2, "Followers", fmtStat(selFollowers), theme::text },
                    { icon::ACTIVITY, "Following", fmtStat(selFollowing), theme::text },
                });
                ImGui::EndChild();
                ImGui::PopStyleColor();

                ImGui::SetCursorScreenPos(ImVec2(p.x, leftMax.y - 62.0f));
                if (SecondaryButton("View on Roblox", ImVec2(rowW, 42))) {
                    std::wstring url = L"https://www.roblox.com/users/" + std::to_wstring(selectedUserId) + L"/profile";
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }

        // Place Configuration (right)
        ImVec2 rightMin(origin.x + colW + gap, origin.y), rightMax(rightMin.x + colW, origin.y + cardH);
        dl->AddRectFilled(rightMin, rightMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg), 10.0f);
        dl->AddRect(rightMin, rightMax, ImGui::ColorConvertFloat4ToU32(theme::border), 10.0f, 0, 1.0f);
        {
            float padX = 22.0f;
            ImVec2 p(rightMin.x + padX, rightMin.y + 20.0f);
            float rowW = colW - padX * 2.0f;
            DrawCardHeader(p, icon::GLOBE, "Place Configuration");

            dl->AddText(ImVec2(p.x, p.y + 40.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), "Place ID");
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 60.0f));
            ImGui::SetNextItemWidth(rowW - 96.0f);
            ImGui::InputText("##placeid", placeIdBuf, sizeof(placeIdBuf), ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            if (PrimaryButton("Save", ImVec2(80, 32))) {
                long long pid = 0;
                try { pid = std::stoll(placeIdBuf); } catch (...) {}
                if (pid > 0) std::thread([pid]() { backend::SavePlaceId(pid); }).detach();
            }

            long long currentPlaceId = backend::savedPlaceId.load();
            float infoY = p.y + 112.0f;

            bool placeLoaded = false;
            PlaceInfo info;
            {
                std::lock_guard<std::mutex> lock(backend::placeInfoMutex);
                if (currentPlaceId > 0 && backend::placeInfo.placeId != currentPlaceId && !backend::placeInfo.requested) {
                    backend::placeInfo.requested = true;
                    std::string anyCookie;
                    {
                        std::lock_guard<std::mutex> alock(backend::accountsMutex);
                        if (selected >= 0 && selected < (int)backend::accounts.size()) anyCookie = backend::accounts[selected].cookie;
                        else if (!backend::accounts.empty()) anyCookie = backend::accounts[0].cookie;
                    }
                    std::thread(backend::FetchPlaceInfo, currentPlaceId, anyCookie).detach();
                }
                placeLoaded = backend::placeInfo.placeId == currentPlaceId && backend::placeInfo.loaded;
                if (placeLoaded) info = backend::placeInfo;
            } // lock released before any texture work below

            auto fmtCount = [](long long v) -> std::string {
                if (v < 0) return "-";
                char buf[32];
                if (v >= 1000000) { snprintf(buf, sizeof(buf), "%.1fM+", v / 1000000.0); return buf; }
                if (v >= 1000) { snprintf(buf, sizeof(buf), "%.1fK+", v / 1000.0); return buf; }
                return std::to_string(v);
            };

            if (currentPlaceId <= 0) {
                dl->AddText(ImVec2(p.x, infoY), ImGui::ColorConvertFloat4ToU32(theme::subtext), "No place configured yet.");
            } else if (!placeLoaded) {
                dl->AddText(ImVec2(p.x, infoY), ImGui::ColorConvertFloat4ToU32(theme::subtext), "Loading place info...");
            } else {
                float iconSize = 56.0f;
                ImVec2 iconPos(p.x, infoY);
                ID3D11ShaderResourceView* iconTex = GetOrCreatePlaceIconTexture(info.placeId, info.iconPng);
                if (iconTex) {
                    dl->AddImageRounded(iconTex, iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 10.0f);
                } else {
                    dl->AddRectFilled(iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize), ImGui::ColorConvertFloat4ToU32(theme::panelBg2), 10.0f);
                }
                dl->AddRect(iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize), ImGui::ColorConvertFloat4ToU32(theme::border), 10.0f, 0, 1.0f);

                float textX = iconPos.x + iconSize + 14.0f;
                ImGui::PushFont(theme::fontBrand);
                std::string placeName = FitTextToWidth(info.name, (p.x + rowW) - textX);
                dl->AddText(ImVec2(textX, iconPos.y + 2.0f), ImGui::ColorConvertFloat4ToU32(theme::text), placeName.c_str());
                ImGui::PopFont();
                if (!info.creator.empty()) {
                    std::string byLine = "by " + info.creator;
                    dl->AddText(ImVec2(textX, iconPos.y + 24.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), byLine.c_str());
                }

                std::string visitsLine = "Visits: " + fmtCount(info.visits);
                std::string favLine = "Favorites: " + fmtCount(info.favorites);
                dl->AddText(ImVec2(textX, iconPos.y + iconSize - 18.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), visitsLine.c_str());
                dl->AddText(ImVec2(textX + 110.0f, iconPos.y + iconSize - 18.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), favLine.c_str());

                float metricTop = infoY + 72.0f;
                float metricGap = 8.0f;
                float metricW = (rowW - metricGap) * 0.5f;
                float metricH = 54.0f;
                auto metric = [&](int col, int row, const char* label, const std::string& value, ImVec4 valueColor) {
                    ImVec2 mMin(p.x + col * (metricW + metricGap), metricTop + row * (metricH + metricGap));
                    ImVec2 mMax(mMin.x + metricW, mMin.y + metricH);
                    dl->AddRectFilled(mMin, mMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg2), 8.0f);
                    dl->AddRect(mMin, mMax, ImGui::ColorConvertFloat4ToU32(theme::border), 8.0f, 0, 1.0f);
                    dl->AddText(ImVec2(mMin.x + 14.0f, mMin.y + 10.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), label);
                    ImGui::PushFont(theme::fontBrand);
                    std::string fit = FitTextToWidth(value, metricW - 28.0f);
                    dl->AddText(ImVec2(mMin.x + 14.0f, mMin.y + 30.0f), ImGui::ColorConvertFloat4ToU32(valueColor), fit.c_str());
                    ImGui::PopFont();
                };
                metric(0, 0, "Place ID", std::to_string(info.placeId), theme::text);
                metric(1, 0, "Visits", fmtCount(info.visits), theme::accent);
            }

            float actionGap = 14.0f;
            float actionW = (rowW - actionGap) * 0.5f;
            float actionY = rightMax.y - 62.0f;
            ImGui::SetCursorScreenPos(ImVec2(p.x, actionY));
            bool canLaunch = hasSelectedAccount && currentPlaceId > 0;
            // Always hoverable/clickable (see note on the top buttons); validate
            // on click rather than disabling, which would kill hover entirely.
            if (PrimaryIconButton(icon::PLAY, "Launch Roblox", ImVec2(actionW, 42))) {
                if (!canLaunch) {
                    backend::Log("[!] Select an account and set a Place ID first, then Launch Roblox.");
                } else {
                    int idx = selected;
                    long long pid = currentPlaceId;
                    std::thread([idx, pid]() { backend::LaunchAccountIntoPlace(idx, pid); }).detach();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x + actionW + actionGap, actionY));
            ImGui::PushID("place_view_on_roblox");
            if (PrimaryButton("View on Roblox", ImVec2(actionW, 42))) {
                if (currentPlaceId <= 0) {
                    backend::Log("[!] Set a Place ID first to view it on Roblox.");
                } else {
                    std::wstring url = L"https://www.roblox.com/games/" + std::to_wstring(currentPlaceId);
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            ImGui::PopID();
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(fullW, cardH));
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI* SetCtxFn)(DPI_AWARENESS_CONTEXT);
        auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    if (!backend::IsElevated()) {
        if (backend::RelaunchAsAdmin()) return 0;
        MessageBoxW(nullptr, L"Vels Multi Tool needs administrator rights to close singleton handles and change MAC addresses.",
            L"Vels Multi Tool", MB_OK | MB_ICONWARNING);
    }

    wchar_t pathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, pathBuf, MAX_PATH);
    std::wstring full(pathBuf);
    std::wstring exeDir = full.substr(0, full.find_last_of(L"\\/"));
    g_exeDir = exeDir;
    backend::Init(exeDir);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // required before any WebView2 calls

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"VelsMultiToolClass";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Vels Multi Tool",
        WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        100, 100, 1100, 720, nullptr, nullptr, wc.hInstance, nullptr);

    // Force DWM to never draw non-client (caption/border) decoration,
    // independent of window style - this is what actually killed the
    // residual native bar that WS_CAPTION removal alone didn't fully clear.
    //
    // (A DWMWA_WINDOW_CORNER_PREFERENCE call was tried here to fix the close
    // button's corner clipping against Windows 11's auto-rounded corners,
    // but it caused DWM to reinstate a native caption bar above our custom
    // one, so it's gone. The window's actual shape is now explicitly set via
    // SetWindowRgn/ApplyWindowShape below - a classic Win32 mechanism
    // independent of the DWM attribute that caused that regression.)
    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    // DWM plays its own fade/highlight transition animation on a window
    // whenever it activates/deactivates, independent of anything our render
    // loop does. That's a plausible source of the white flash on switching
    // to another app and back. Unlike DWMWA_WINDOW_CORNER_PREFERENCE (which
    // regressed the title bar earlier), this attribute is long-established
    // (pre-Windows 11) and only concerns animation, not NC rendering, so it
    // shouldn't interact with the policy set above.
    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT initialClientRect;
    GetClientRect(hwnd, &initialClientRect);
    ApplyWindowShape(hwnd, initialClientRect.right, initialClientRect.bottom);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    std::wstring fontPath = exeDir + L"\\fonts\\Inter-Variable.ttf";
    std::string fontPathNarrow(fontPath.begin(), fontPath.end());
    if (std::filesystem::exists(fontPath)) {
        ImFontConfig interCfg;
        interCfg.OversampleH = 3;
        interCfg.OversampleV = 2;
        interCfg.PixelSnapH = false;
        theme::fontBody = io.Fonts->AddFontFromFileTTF(fontPathNarrow.c_str(), 14.0f, &interCfg);
        theme::fontBrand = io.Fonts->AddFontFromFileTTF(fontPathNarrow.c_str(), 16.0f, &interCfg);
        theme::fontTitle = io.Fonts->AddFontFromFileTTF(fontPathNarrow.c_str(), 19.0f, &interCfg);
    } else {
        theme::fontBody = io.Fonts->AddFontDefault();
        theme::fontBrand = theme::fontBody;
        theme::fontTitle = theme::fontBody;
    }
    io.FontDefault = theme::fontBody;

    std::wstring iconFontPath = exeDir + L"\\fonts\\Lucide.ttf";
    std::string iconFontPathNarrow(iconFontPath.begin(), iconFontPath.end());
    if (std::filesystem::exists(iconFontPath)) {
        static const ImWchar iconRanges[] = { 0xE000, 0xE6FF, 0 };
        ImFontConfig cfg;
        cfg.GlyphRanges = iconRanges;
        theme::fontIcon = io.Fonts->AddFontFromFileTTF(iconFontPathNarrow.c_str(), 16.0f, &cfg);
        theme::fontIconLg = io.Fonts->AddFontFromFileTTF(iconFontPathNarrow.c_str(), 22.0f, &cfg);
    } else {
        theme::fontIcon = theme::fontBody;
        theme::fontIconLg = theme::fontBody;
    }

    theme::Apply();
    theme::GenerateStars(220);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    int activePage = 3;
    int lastActivePage = 3;
    float pageAnimT = 1.0f; // 0 = just switched, eases to 1 at rest
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        // Present() used to run here, at the *top* of the loop, before this
        // frame's UI was even built - so every shown frame was actually the
        // previous iteration's content, and the very first frame (or any
        // frame right after a buffer-invalidating event, e.g. another app
        // briefly taking focus or a screenshot tool grabbing the window)
        // could present a back buffer nothing had been rendered into yet,
        // which reads as a white flash. The real Present() now happens once
        // the frame is actually rendered, at the bottom of the loop; this is
        // just a cheap poll to skip building a frame nobody can see while
        // the window is known to be fully occluded.
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(50);
            continue;
        }
        g_SwapChainOccluded = false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io2 = ImGui::GetIO();
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io2.DisplaySize, ImGui::ColorConvertFloat4ToU32(theme::bg));
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io2.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("VelsMultiToolRoot", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();

        DrawTitleBar(hwnd, io2.DisplaySize.x);

        // --- Sidebar ---
        const float SIDEBAR_W = 218.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::sidebarBg);
        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        ImGui::BeginChild("Sidebar", ImVec2(SIDEBAR_W, 0), false);
        {
            ImGui::Dummy(ImVec2(0, 4));
            ImVec2 brandPos = ImGui::GetCursorScreenPos();
            IconBox(ImVec2(brandPos.x + 22.0f, brandPos.y + 4.0f), 52.0f, icon::ROCKET, theme::softAccentBg, theme::accent, 12.0f);
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            ImGui::PushFont(theme::fontBrand);
            sdl->AddText(ImVec2(brandPos.x + 92.0f, brandPos.y + 14.0f), ImGui::ColorConvertFloat4ToU32(theme::text), "Vels Multi Tool");
            ImGui::PopFont();
            sdl->AddText(ImVec2(brandPos.x + 92.0f, brandPos.y + 36.0f), ImGui::ColorConvertFloat4ToU32(theme::subtext), "v1.0.0");
            ImGui::Dummy(ImVec2(0, 54));

            ImGui::Indent(10);
            ImGui::PushFont(theme::fontBrand);
            if (NavItem("Multi-Instance", icon::ROCKET, activePage == 0)) activePage = 0;
            ImGui::Dummy(ImVec2(0, 8));
            if (NavItem("Cookie Cleaner", icon::COOKIE, activePage == 1)) activePage = 1;
            ImGui::Dummy(ImVec2(0, 8));
            if (NavItem("MAC Spoofer", icon::SHARE2, activePage == 2)) activePage = 2;
            ImGui::Dummy(ImVec2(0, 8));
            if (NavItem("Accounts", icon::USER, activePage == 3)) activePage = 3;
            ImGui::PopFont();
            ImGui::Unindent(10);

            // --- Footer "about" card, pinned to the bottom of the sidebar ---
            float footerH = 68.0f;
            float avail = ImGui::GetContentRegionAvail().y;
            if (avail > footerH) ImGui::Dummy(ImVec2(0, avail - footerH - 16));
            ImGui::Indent(10);
            ImVec2 fp = ImGui::GetCursorScreenPos();
            ImVec2 fSize(SIDEBAR_W - 20, footerH);
            ImDrawList* fdl = ImGui::GetWindowDrawList();

            ImGui::InvisibleButton("##community_card", fSize);
            bool communityHover = ImGui::IsItemHovered();
            ImGuiID communityId = ImGui::GetID("##community_card");
            float communityAnim = std::max(PopAnim(communityId, communityHover, false), 0.0f);

            // Slow, faint ambient pulse so the card reads as "alive" without
            // dominating the sidebar, with a clearer (but still tasteful)
            // boost while hovered. The "Sidebar" child window clips draws to
            // its own rect by default, which used to hard-cut the halo into
            // an ugly square ring right at the card's edge - expand the clip
            // rect for just this draw so the bloom can fall off naturally
            // into the margin around the card instead.
            float pulse = 0.5f + 0.5f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 1.4f));
            float glowAlpha = (0.07f + 0.03f * pulse) + 0.16f * communityAnim;
            float glowSpread = 9.0f + 5.0f * communityAnim;

            ImVec2 clipPad(glowSpread + 6.0f, glowSpread + 6.0f);
            fdl->PushClipRect(ImVec2(fp.x - clipPad.x, fp.y - clipPad.y),
                ImVec2(fp.x + fSize.x + clipPad.x, fp.y + fSize.y + clipPad.y), false);
            DrawSoftRectGlow(fdl, fp, ImVec2(fp.x + fSize.x, fp.y + fSize.y), 12.0f, glowSpread, glowAlpha, theme::accent);
            fdl->PopClipRect();

            // Single flat tinted fill (not a partial-height wash) so there's
            // no hard seam between a "purple half" and a "gray half" - the
            // whole card is one uniform color that just deepens on hover.
            fdl->AddRectFilled(fp, ImVec2(fp.x + fSize.x, fp.y + fSize.y),
                ImGui::ColorConvertFloat4ToU32(theme::panelBg), 12.0f);
            fdl->AddRectFilled(fp, ImVec2(fp.x + fSize.x, fp.y + fSize.y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.06f + 0.06f * communityAnim)),
                12.0f);
            fdl->AddRect(fp, ImVec2(fp.x + fSize.x, fp.y + fSize.y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.22f + 0.35f * communityAnim)),
                12.0f, 0, 1.2f);

            // A small, tight glow right behind the icon badge - a focal
            // point instead of one big halo washing out the whole card.
            ImVec2 iconPos(fp.x + 12, fp.y + (fSize.y - 42.0f) * 0.5f);
            DrawSoftRectGlow(fdl, iconPos, ImVec2(iconPos.x + 42.0f, iconPos.y + 42.0f), 13.0f, 5.0f + 3.0f * communityAnim,
                0.16f + 0.20f * communityAnim, theme::accent);
            IconBox(iconPos, 42.0f, icon::SHIELD,
                ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 1.0f), theme::accentText, 13.0f);

            ImGui::PushFont(theme::fontBrand);
            fdl->AddText(ImVec2(fp.x + 66, fp.y + 14), ImGui::ColorConvertFloat4ToU32(theme::text), "Vels Multi Tool");
            ImGui::PopFont();
            fdl->AddText(ImVec2(fp.x + 66, fp.y + 36), ImGui::ColorConvertFloat4ToU32(theme::subtext), "Free Edition");

            std::string chevron = icon::Str(icon::CHEVRON_RIGHT);
            ImGui::PushFont(theme::fontIcon);
            ImVec2 chevSize = ImGui::CalcTextSize(chevron.c_str());
            ImVec4 chevColor(theme::accent.x, theme::accent.y, theme::accent.z, 0.55f + 0.45f * communityAnim);
            fdl->AddText(ImVec2(fp.x + fSize.x - 12 - chevSize.x + 3.0f * communityAnim, fp.y + (fSize.y - chevSize.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(chevColor), chevron.c_str());
            ImGui::PopFont();

            ImGui::Unindent(10);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Tab-switch animation: new page eases up into place from a slight
        // offset instead of snapping straight in.
        if (g_pendingNavPage >= 0) {
            activePage = g_pendingNavPage;
            g_pendingNavPage = -1;
        }
        if (activePage != lastActivePage) {
            lastActivePage = activePage;
            pageAnimT = 0.0f;
            if (activePage == 1 && !backend::browserCookieScanning.load()) {
                std::thread(backend::ScanBrowserCookies).detach();
            }
        }
        pageAnimT += (1.0f - pageAnimT) * std::min(1.0f, io2.DeltaTime * 10.0f);
        float pageSlide = (1.0f - pageAnimT) * 18.0f;

        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(SIDEBAR_W, (float)TITLEBAR_H));

        // --- Content (transparent - only specific cards inside should have a visible fill) ---
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("Content", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
        {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Indent(24);
            float innerW = ImGui::GetContentRegionAvail().x - 24;
            float innerH = ImGui::GetContentRegionAvail().y - 8;
            // Small horizontal padding so button hover glow/scale has room to
            // breathe before it hits this child's clip rect (otherwise the
            // glow gets hard-clipped on the left, looking choppy).
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 0));
            ImGui::BeginChild("ContentInner", ImVec2(innerW, innerH), false, ImGuiWindowFlags_NoScrollbar);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pageSlide);
            switch (activePage) {
            case 0: PageMultiInstance(); break;
            case 1: PageCookieCleaner(); break;
            case 2: PageMacSpoofer(); break;
            case 3: PageAccounts(hwnd); break;
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::Unindent(24);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        {
            ImDrawList* rootDl = ImGui::GetWindowDrawList();
            ImU32 border = ImGui::ColorConvertFloat4ToU32(theme::border);
            rootDl->AddLine(ImVec2(SIDEBAR_W - 1.0f, 0.0f), ImVec2(SIDEBAR_W - 1.0f, io2.DisplaySize.y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.045f)), 3.0f);
            rootDl->AddLine(ImVec2(SIDEBAR_W + 2.0f, 0.0f), ImVec2(SIDEBAR_W + 2.0f, io2.DisplaySize.y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.025f)), 5.0f);
            rootDl->AddLine(ImVec2(SIDEBAR_W, 0.0f), ImVec2(SIDEBAR_W, io2.DisplaySize.y), border, 1.0f);
            rootDl->AddRect(ImVec2(0.5f, 0.5f), ImVec2(io2.DisplaySize.x - 0.5f, io2.DisplaySize.y - 0.5f),
                border, 12.0f, 0, 1.0f);
        }

        ImGui::End();
        ImGui::Render();

        const float clearColor[4] = { theme::bg.x, theme::bg.y, theme::bg.z, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    backend::Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
