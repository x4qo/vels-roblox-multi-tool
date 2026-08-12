#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <wincodec.h>
#include <shellapi.h>
#include <commdlg.h>
#include <tchar.h>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <set>
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
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool g_swapChainIsFlipModel = false;

namespace idlectl {
    static double animTime = 0.0;

    static bool  windowActive = true;
    static bool  cursorInside = false;
    static int   forceFrames = 4;
    static double lastInteract = 0.0;

    inline void RequestFrames(int n) { if (n > forceFrames) forceFrames = n; }

    static void TrimWorkingSet() {
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    }

    static double NowSeconds() {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (double)c.QuadPart / (double)freq.QuadPart;
    }
}

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
    sd.Flags = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.BufferCount = 2;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = S_FALSE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
        if (res == DXGI_ERROR_UNSUPPORTED)
            res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
        if (res == S_OK) { g_swapChainIsFlipModel = (attempt == 0); break; }
    }
    if (res != S_OK) return false;

    IDXGIDevice1* dxgiDev = nullptr;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
        dxgiDev->SetMaximumFrameLatency(1);
        dxgiDev->Release();
    }

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

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

static ID3D11ShaderResourceView* GetOrCreatePlaceIconTexture(long long placeId, const std::vector<unsigned char>& iconPng) {
    if (iconPng.empty()) return nullptr;
    if (g_placeIconForPlaceId == placeId) return g_placeIconTexture;
    if (g_placeIconTexture) { g_placeIconTexture->Release(); g_placeIconTexture = nullptr; }
    g_placeIconTexture = CreateTextureFromImageBytes(iconPng);
    g_placeIconForPlaceId = placeId;
    return g_placeIconTexture;
}

static std::wstring g_exeDir;
static float g_desiredClientH = 0.0f;
static RECT g_btnMinRect = {};
static RECT g_btnMaxRect = {};
static RECT g_btnCloseRect = {};
const int TITLEBAR_H = 34;

static std::unordered_map<ImGuiID, float> g_widgetAnim;
static int g_pendingNavPage = -1;

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
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN,
                g_swapChainIsFlipModel ? 0 : DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
            CreateRenderTarget();
        }
        ApplyWindowShape(hWnd, (int)LOWORD(lParam), (int)HIWORD(lParam));
        idlectl::RequestFrames(2);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
    case WM_KILLFOCUS:
        g_widgetAnim.clear();
        if (msg == WM_ACTIVATE) idlectl::windowActive = (LOWORD(wParam) != WA_INACTIVE);
        else idlectl::windowActive = false;
        idlectl::RequestFrames(idlectl::windowActive ? 4 : 2);
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_NCCALCSIZE: {
        if (!wParam) break;
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
            bg = ImVec4(0.055f, 0.057f, 0.063f, 1.00f);
            sidebarBg = ImVec4(0.050f, 0.052f, 0.058f, 0.98f);
            panelBg = ImVec4(0.055f, 0.057f, 0.063f, 1.00f);
            panelBg2 = ImVec4(0.045f, 0.045f, 0.048f, 1.00f);
            glassFill = ImVec4(0.055f, 0.055f, 0.058f, 1.00f);
            glassHover = ImVec4(0.090f, 0.090f, 0.095f, 1.00f);
            glassActive = ImVec4(0.150f, 0.150f, 0.158f, 1.00f);
            accent = ImVec4(0.560f, 0.570f, 0.590f, 1.00f);
            accentHov = ImVec4(0.660f, 0.670f, 0.690f, 1.00f);
            accentAct = ImVec4(0.460f, 0.470f, 0.490f, 1.00f);
            accentText = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
            text = ImVec4(0.918f, 0.926f, 0.945f, 1.00f);
            subtext = ImVec4(0.545f, 0.565f, 0.615f, 1.00f);
            border = ImVec4(0.165f, 0.172f, 0.192f, 1.00f);
            softAccentBg = ImVec4(0.560f, 0.570f, 0.590f, 0.16f);
            titlebarBg = ImVec4(0.043f, 0.045f, 0.050f, 1.00f);
            cardLine = ImVec4(1, 1, 1, 0.09f);
        } else {
            bg = ImVec4(0.965f, 0.970f, 0.988f, 1.00f);
            sidebarBg = ImVec4(0.985f, 0.987f, 0.996f, 0.98f);
            panelBg = ImVec4(1.000f, 1.000f, 1.000f, 0.98f);
            panelBg2 = ImVec4(0.970f, 0.974f, 0.990f, 1.00f);
            glassFill = ImVec4(0.976f, 0.978f, 0.992f, 1.00f);
            glassHover = ImVec4(0.925f, 0.955f, 0.995f, 1.00f);
            glassActive = ImVec4(0.870f, 0.875f, 0.890f, 1.00f);
            accent = ImVec4(0.420f, 0.430f, 0.450f, 1.00f);
            accentHov = ImVec4(0.520f, 0.530f, 0.550f, 1.00f);
            accentAct = ImVec4(0.330f, 0.340f, 0.360f, 1.00f);
            accentText = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
            text = ImVec4(0.100f, 0.115f, 0.180f, 1.00f);
            subtext = ImVec4(0.410f, 0.450f, 0.580f, 1.00f);
            border = ImVec4(0.850f, 0.865f, 0.920f, 1.00f);
            softAccentBg = ImVec4(0.420f, 0.430f, 0.450f, 0.10f);
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
        s.PopupRounding = 10.0f;
        s.PopupBorderSize = 1.0f;
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
        c[ImGuiCol_PopupBg] = darkMode ? ImVec4(0.115f, 0.120f, 0.132f, 1.0f)
                                       : ImVec4(0.985f, 0.987f, 0.996f, 1.0f);
    }

    std::vector<ImVec2> stars;
    std::vector<float> starSize;
    std::vector<float> starAlpha;
    std::vector<float> starTwinkleSpeed;
    std::vector<float> starTwinklePhase;
    std::vector<float> starDrift;

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
        float t = (float)idlectl::animTime;
        for (size_t i = 0; i < stars.size(); ++i) {
            float yNorm = stars[i].y + t * starDrift[i];
            yNorm -= floorf(yNorm);

            float twinkle = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * starTwinkleSpeed[i] + starTwinklePhase[i]));
            float alpha = starAlpha[i] * twinkle;
            if (alpha < 0.03f) continue;

            ImVec2 p(origin.x + stars[i].x * size.x, origin.y + yNorm * size.y);
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, alpha));
            if (starSize[i] <= 0.9f) {
                dl->AddRectFilled(ImVec2(p.x - starSize[i], p.y - starSize[i]),
                                  ImVec2(p.x + starSize[i], p.y + starSize[i]), col);
            } else {
                dl->AddCircleFilled(p, starSize[i], col, 6);
            }
        }
    }
}

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
    constexpr unsigned COPY = 0xE09E;
    constexpr unsigned STOP_SQ = 1;

    inline std::string Str(unsigned cp) {
        std::string s;
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
        return s;
    }
}

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

static void DrawSoftRectGlow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float spread, float peakAlpha,
    ImVec4 color = ImVec4(1, 1, 1, 1)) {
    const int steps = 26;
    float cumulative = 0.0f;
    for (int i = steps; i >= 0; --i) {
        float t = (float)i / (float)steps;
        float u = 1.0f - t;
        float smooth = u * u * (3.0f - 2.0f * u);
        float targetAlpha = peakAlpha * smooth;
        float layerAlpha = (targetAlpha - cumulative) / std::max(1.0f - cumulative, 0.0001f);
        layerAlpha = std::clamp(layerAlpha, 0.0f, 1.0f);
        cumulative = layerAlpha + cumulative * (1.0f - layerAlpha);

        float pad = spread * t;
        dl->AddRectFilled(ImVec2(min.x - pad, min.y - pad), ImVec2(max.x + pad, max.y + pad),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, layerAlpha)), rounding + pad);
    }
}

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
    dl->AddLine(ImVec2(0, (float)TITLEBAR_H - 0.5f), ImVec2(winWidth, (float)TITLEBAR_H - 0.5f), borderCol, 1.0f);

    {
        float s = 18.0f;
        float bx = 12.0f, by = TITLEBAR_H * 0.5f - s * 0.5f;
        ImU32 accentU = ImGui::ColorConvertFloat4ToU32(ImVec4(0.97f, 0.98f, 1.00f, 1.00f));
        ImVec2 p[6] = {
            ImVec2(bx + s * 0.56f, by + s * 0.16f),
            ImVec2(bx + s * 0.34f, by + s * 0.56f),
            ImVec2(bx + s * 0.47f, by + s * 0.56f),
            ImVec2(bx + s * 0.42f, by + s * 0.86f),
            ImVec2(bx + s * 0.68f, by + s * 0.42f),
            ImVec2(bx + s * 0.55f, by + s * 0.42f),
        };
        dl->AddTriangleFilled(p[0], p[1], p[2], accentU);
        dl->AddTriangleFilled(p[0], p[2], p[3], accentU);
        dl->AddTriangleFilled(p[0], p[3], p[4], accentU);
        dl->AddTriangleFilled(p[0], p[4], p[5], accentU);
    }
    {
        const char* title = "Account Manager";
        ImVec2 ts = ImGui::CalcTextSize(title);
        dl->AddText(ImVec2((winWidth - ts.x) * 0.5f, (TITLEBAR_H - ts.y) * 0.5f), textCol, title);
    }

    float xMin = winWidth - BTN_W * 3;
    float xMax = winWidth - BTN_W * 2;
    float xClose = winWidth - BTN_W;

    auto hoverFill = [&](float x, ImU32 col) {
        dl->AddRectFilled(ImVec2(x, 0), ImVec2(x + BTN_W, (float)TITLEBAR_H), col);
        };

    ImGui::SetCursorScreenPos(ImVec2(xMin, 0));
    ImGui::InvisibleButton("##titlebar_min", ImVec2(BTN_W, (float)TITLEBAR_H));
    if (ImGui::IsItemHovered()) hoverFill(xMin, ImGui::ColorConvertFloat4ToU32(theme::glassHover));
    if (ImGui::IsItemClicked()) ShowWindow(hwnd, SW_MINIMIZE);
    {
        float cx = xMin + BTN_W * 0.5f, cy = TITLEBAR_H * 0.5f;
        dl->AddLine(ImVec2(cx - 5, cy), ImVec2(cx + 5, cy), textCol, 1.3f);
    }
    g_btnMinRect = { (LONG)xMin, 0, (LONG)(xMin + BTN_W), (LONG)TITLEBAR_H };

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

static float PopAnim(ImGuiID id, bool hovered, bool pressed) {
    float& anim = g_widgetAnim[id];
    float target = pressed ? -1.0f : (hovered ? 1.0f : 0.0f);
    if (target == 0.0f) {
        anim = 0.0f;
    } else {
        float rate = pressed ? 22.0f : 12.0f;
        anim += (target - anim) * std::min(1.0f, ImGui::GetIO().DeltaTime * rate);
    }
    return anim;
}

static bool DrawPopButton(const char* label, ImVec2 size, bool primary, unsigned iconCp = 0,
    const ImVec4* colorOverride = nullptr, const ImVec4* iconTint = nullptr) {
    ImGuiID id = ImGui::GetID(label);
    ImVec2 pos = ImGui::GetCursorScreenPos();

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

    float lift = pressAnim > 0.0f ? 0.0f : hoverAnim * 1.6f;
    float scale = 1.0f - pressAnim * 0.018f + hoverAnim * 0.02f;
    ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f - lift);
    ImVec2 half(size.x * 0.5f * scale, size.y * 0.5f * scale);
    ImVec2 rmin(center.x - half.x, center.y - half.y);
    ImVec2 rmax(center.x + half.x, center.y + half.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 accentColor = colorOverride ? *colorOverride : theme::accent;
    auto fade = [uiAlpha](ImVec4 c) { c.w *= uiAlpha; return c; };

    if (primary) {
        if (hoverAnim > 0.01f)
            DrawSoftRectGlow(dl, rmin, rmax, 9.0f, 8.0f * hoverAnim, 0.45f * hoverAnim, accentColor);
        ImVec4 col = hovered ? theme::accentHov : theme::accent;
        dl->AddRectFilled(rmin, rmax, ImGui::ColorConvertFloat4ToU32(fade(col)), 8.0f);
    } else {
        ImVec4 fillCol, borderCol;
        if (colorOverride) {
            fillCol = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.05f + 0.09f * hoverAnim);
            borderCol = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.42f + 0.34f * hoverAnim);
        } else {
            fillCol = hovered ? theme::glassHover : theme::glassFill;
            borderCol = hovered
                ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.55f)
                : theme::border;
        }
        dl->AddRectFilled(rmin, rmax, ImGui::ColorConvertFloat4ToU32(fade(fillCol)), 8.0f);
        dl->AddRect(rmin, rmax, ImGui::ColorConvertFloat4ToU32(fade(borderCol)), 8.0f, 0, 1.3f);
    }

    ImVec4 textColor = fade(colorOverride ? accentColor : (primary ? theme::accentText : theme::text));
    ImVec4 iconColor = iconTint ? fade(*iconTint) : textColor;
    ImVec2 textSize = ImGui::CalcTextSize(label);

    if (iconCp != 0) {
        bool isPlay = (iconCp == icon::PLAY);
        bool isStopSq = (iconCp == icon::STOP_SQ);
        ImVec2 glyphSize(0, 0);
        if (!isPlay && !isStopSq) {
            std::string glyph = icon::Str(iconCp);
            ImGui::PushFont(theme::fontIcon);
            glyphSize = ImGui::CalcTextSize(glyph.c_str());
            ImGui::PopFont();
        }

        float gap = 8.0f;
        float iconW = isPlay ? 16.0f : (isStopSq ? 13.0f : glyphSize.x);
        float totalW = iconW + gap + textSize.x;
        float startX = center.x - totalW * 0.5f;

        if (isPlay) {
            ImVec2 c(startX + 8.0f, center.y);
            ImVec2 tri[] = {
                ImVec2(c.x - 3.5f, c.y - 5.0f),
                ImVec2(c.x - 3.5f, c.y + 5.0f),
                ImVec2(c.x + 4.8f, c.y),
            };
            dl->AddConvexPolyFilled(tri, 3, ImGui::ColorConvertFloat4ToU32(iconColor));
        } else if (isStopSq) {
            ImVec2 c(startX + 6.0f, center.y);
            dl->AddRectFilled(ImVec2(c.x - 5.5f, c.y - 5.5f), ImVec2(c.x + 5.5f, c.y + 5.5f), ImGui::ColorConvertFloat4ToU32(iconColor), 2.0f);
        } else {
            std::string glyph = icon::Str(iconCp);
            ImGui::PushFont(theme::fontIcon);
            dl->AddText(ImVec2(startX, center.y - glyphSize.y * 0.5f), ImGui::ColorConvertFloat4ToU32(iconColor), glyph.c_str());
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

static bool SecondaryIconButtonColored(unsigned iconCp, const char* label, ImVec4 color, ImVec2 size = ImVec2(0, 34)) {
    return DrawPopButton(label, size, false, iconCp, &color);
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

    float fillAlpha = active ? (0.15f + 0.07f * hoverAnim) : (0.075f * hoverAnim);
    if (fillAlpha > 0.004f) {
        ImVec4 fill(theme::accent.x, theme::accent.y, theme::accent.z, fillAlpha);
        dl->AddRectFilled(pillMin, pillMax, ImGui::ColorConvertFloat4ToU32(fill), rounding);
    }

    if (anim > 0.001f && active) {
        float barH = (size.y - 22.0f) * anim;
        float cy = pos.y + size.y * 0.5f;
        ImVec2 barMin(pos.x + 2.0f, cy - barH * 0.5f);
        ImVec2 barMax(pos.x + 5.5f, cy + barH * 0.5f);
        dl->AddRectFilled(barMin, barMax, ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);
    }

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

    float lift = pressed ? 0.0f : anim * 3.0f;
    float scale = pressed ? 0.99f : (1.0f + anim * 0.012f);
    ImVec2 center(pos.x + width * 0.5f, pos.y + height * 0.5f - lift);
    ImVec2 half(width * 0.5f * scale, height * 0.5f * scale);
    ImVec2 min(center.x - half.x, center.y - half.y);
    ImVec2 max(center.x + half.x, center.y + half.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();

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
    float cardH = 210.0f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

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

static void Tip(const std::string& text) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::BeginTooltip();

    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find('\n', start);
            lines.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    const float padL = 16.0f, padR = 14.0f, padY = 10.0f, lineH = ImGui::GetFontSize() + 4.0f;
    float wMax = 0.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == 0) ImGui::PushFont(theme::fontBrand);
        wMax = std::max(wMax, ImGui::CalcTextSize(lines[i].c_str()).x);
        if (i == 0) ImGui::PopFont();
    }
    float w = wMax + padL + padR, h = lines.size() * lineH + padY * 2.0f - 4.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 mn = p, mx = ImVec2(p.x + w, p.y + h);
    dl->AddRectFilled(mn, mx, ImGui::ColorConvertFloat4ToU32(ImVec4(0.115f, 0.120f, 0.132f, 1.0f)), 10.0f);
    dl->AddRect(mn, mx, ImGui::ColorConvertFloat4ToU32(ImVec4(0.225f, 0.235f, 0.255f, 1.0f)), 10.0f, 0, 1.2f);
    dl->AddRectFilled(ImVec2(mn.x + 1.0f, mn.y + 9.0f), ImVec2(mn.x + 4.0f, mx.y - 9.0f),
        ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);

    for (size_t i = 0; i < lines.size(); ++i) {
        float ly = p.y + padY + i * lineH;
        if (i == 0) {
            ImGui::PushFont(theme::fontBrand);
            dl->AddText(ImVec2(p.x + padL, ly), ImGui::ColorConvertFloat4ToU32(theme::text), lines[i].c_str());
            ImGui::PopFont();
        } else {
            dl->AddText(ImVec2(p.x + padL, ly), ImGui::ColorConvertFloat4ToU32(theme::subtext), lines[i].c_str());
        }
    }
    ImGui::Dummy(ImVec2(w, h));

    ImGui::EndTooltip();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

static std::string CompactCount(long long n) {
    char buf[32];
    if (n >= 1000000000LL)   snprintf(buf, sizeof(buf), "%.1fB", n / 1000000000.0);
    else if (n >= 1000000LL) snprintf(buf, sizeof(buf), "%.1fM", n / 1000000.0);
    else if (n >= 1000LL)    snprintf(buf, sizeof(buf), "%.1fK", n / 1000.0);
    else                     snprintf(buf, sizeof(buf), "%lld", n);
    std::string s(buf);
    size_t dot = s.find(".0");
    if (dot != std::string::npos) s.erase(dot, 2);
    return s;
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

static void DrawCopyGlyph(ImDrawList* dl, ImVec2 topLeft, float size, ImU32 col) {
    float r = size * 0.62f;
    float off = size - r;
    dl->AddRect(ImVec2(topLeft.x, topLeft.y + off), ImVec2(topLeft.x + r, topLeft.y + off + r), col, 2.0f, 0, 1.3f);
    dl->AddRect(ImVec2(topLeft.x + off, topLeft.y), ImVec2(topLeft.x + off + r, topLeft.y + r), col, 2.0f, 0, 1.3f);
}

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

struct CookieRowInfo {
    unsigned iconCp;
    std::string title;
    std::string caption;
    std::string pillLabel;
    ImVec4 color;
    CookieGlyph glyph;
};

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
            float iconAlpha = scanningNow
                ? (0.55f + 0.45f * (0.5f + 0.5f * sinf((float)idlectl::animTime * 6.0f)))
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

        float chipAreaRight = refreshPos.x - 10.0f;
        if (scanning) {
            float pulse = 0.4f + 0.6f * (0.5f + 0.5f * sinf((float)idlectl::animTime * 5.0f));
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


static bool EnsureElevatedFor(const wchar_t* feature) {
    if (backend::IsElevated()) return true;
    std::wstring msg = std::wstring(feature) +
        L" needs administrator rights.\n\nRestart Vels Multi Tool as administrator now?";
    if (MessageBoxW(nullptr, msg.c_str(), L"Vels Multi Tool", MB_YESNO | MB_ICONWARNING) == IDYES) {
        if (backend::RelaunchAsAdmin()) {
            backend::Shutdown();
            ExitProcess(0);
        }
        MessageBoxW(nullptr, L"Could not relaunch as administrator.",
            L"Vels Multi Tool", MB_OK | MB_ICONERROR);
    }
    return false;
}

static void PageMultiInstance() {
    SectionTitle(icon::ROCKET, "Multi-Instance Launcher",
        "Watches for Roblox and closes the singleton lock handle so you can run more than one client at once.");

    bool watching = backend::watching;
    ImGui::Indent(12);
    if (watching) {
        if (PrimaryIconButton(icon::PLAY, "Stop Watching", ImVec2(190, 48))) backend::StopWatching();
    } else {
        if (PrimaryIconButton(icon::PLAY, "Start Watching", ImVec2(190, 48))) {
            if (EnsureElevatedFor(L"Closing Roblox singleton handles")) backend::StartWatching();
        }
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
        if (EnsureElevatedFor(L"MAC address spoofing")) {
            int idx = selected;
            std::thread([idx]() { backend::SpoofAdapter(idx); }).detach();
        }
    }
    ImGui::SameLine(0, 12);
    if (MacActionButton("##mac_restore", "Restore Default", MacButtonIcon::Restore, ImVec2(168, 40), false)) {
        if (EnsureElevatedFor(L"Restoring the default MAC address")) {
            int idx = selected;
            std::thread([idx]() { backend::RestoreAdapter(idx); }).detach();
        }
    }
    ImGui::Unindent(12);

    ImGui::Dummy(ImVec2(0, 12));
    DrawMacStatusCard(ImGui::GetContentRegionAvail().y);
}

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
        if (PrimaryIconButton(icon::PLAY, "Launch Roblox", ImVec2(190.0f, 42.0f))) {
            if (!canQuickLaunch) {
                backend::Log("[!] Select an account and set a Place ID first, then Launch Roblox.");
            } else {
                int idx = selected;
                std::thread([idx, pid]() { backend::LaunchAccountIntoPlace(idx, pid); }).detach();
            }
        }
        ImGui::PopID();

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
                            wantOpenAliasModal = true;
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
            }

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

    {
        float fullW = ImGui::GetContentRegionAvail().x;
        float gap = 16.0f;
        float colW = (fullW - gap) * 0.5f;
        float cardH = 380.0f;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

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
            }

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


static bool ToggleSwitch(const char* id, bool* v) {
    ImVec2 sz(46, 26);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, sz);
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    float& anim = g_widgetAnim[ImGui::GetID(id)];
    float target = *v ? 1.0f : 0.0f;
    anim += (target - anim) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 off = theme::bad, on = theme::good;
    ImVec4 track(off.x + (on.x - off.x) * anim, off.y + (on.y - off.y) * anim,
                 off.z + (on.z - off.z) * anim, 1.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ImGui::ColorConvertFloat4ToU32(track), sz.y * 0.5f);
    dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y),
        ImGui::ColorConvertFloat4ToU32(ImVec4(track.x, track.y, track.z, 0.55f)), sz.y * 0.5f, 0, 1.0f);
    float knobR = sz.y * 0.5f - 3.0f;
    float kx = pos.x + 3.0f + knobR + (sz.x - 6.0f - knobR * 2.0f) * anim;
    dl->AddCircleFilled(ImVec2(kx, pos.y + sz.y * 0.5f), knobR, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 1)));
    return clicked;
}

static int SegmentedControl(const char* id, const char* const* opts, int count, int idx, ImVec2 size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
        ImGui::ColorConvertFloat4ToU32(theme::panelBg2), 8.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
        ImGui::ColorConvertFloat4ToU32(theme::border), 8.0f, 0, 1.0f);
    float segW = size.x / (float)count;
    int result = idx;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        ImVec2 sp(pos.x + segW * i, pos.y);
        ImGui::SetCursorScreenPos(sp);
        ImGui::InvisibleButton(id, ImVec2(segW, size.y));
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) result = i;
        if (i == idx)
            dl->AddRectFilled(ImVec2(sp.x + 3, sp.y + 3), ImVec2(sp.x + segW - 3, sp.y + size.y - 3),
                ImGui::ColorConvertFloat4ToU32(theme::glassActive), 6.0f);
        ImVec4 col = (i == idx) ? theme::accentText : (hov ? theme::text : theme::subtext);
        ImVec2 ts = ImGui::CalcTextSize(opts[i]);
        dl->AddText(ImVec2(sp.x + (segW - ts.x) * 0.5f, sp.y + (size.y - ts.y) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(col), opts[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);
    return result;
}

static void DrawLabelValue(ImVec2 pos, const char* label, const std::string& value, ImVec4 valColor) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, ImGui::ColorConvertFloat4ToU32(theme::subtext), label);
    ImVec2 ls = ImGui::CalcTextSize(label);
    ImGui::PushFont(theme::fontBrand);
    dl->AddText(ImVec2(pos.x + ls.x + 8, pos.y - 1), ImGui::ColorConvertFloat4ToU32(valColor), value.c_str());
    ImGui::PopFont();
}

static std::wstring OpenCookieFileDialog(HWND owner) {
    wchar_t file[2048] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Cookie / text files\0*.txt;*.dat;*.csv;*.log\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 2048;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) return std::wstring(file);
    return L"";
}

static void ImportCookiesFromFile(const std::wstring& path) {
    std::thread([path]() {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { backend::Log("[!] Could not open cookie file."); return; }
        std::string data;
        char buf[8192]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
        int added = 0, tried = 0;
        size_t start = 0;
        while (start <= data.size()) {
            size_t nl = data.find('\n', start);
            std::string line = data.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            start = (nl == std::string::npos) ? data.size() + 1 : nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            line = line.substr(b);
            if (line.empty()) continue;
            const std::string pfx = ".ROBLOSECURITY=";
            if (line.rfind(pfx, 0) == 0) line = line.substr(pfx.size());
            if (line.size() < 40) continue;
            tried++;
            if (backend::AddAccountFromCookie(line)) added++;
        }
        backend::Log("[i] Cookie import: added " + std::to_string(added) + " of " + std::to_string(tried) + ".");
    }).detach();
}

static void PageAccountManager(HWND hwnd, bool& gotoMultiInstance) {
    (void)gotoMultiInstance;
    static char placeIdBuf[32] = "";
    static bool placeIdInit = false;
    static char psLinkBuf[512] = "";
    static char psNameBuf[64] = "";
    static char verHashBuf[64] = "";
    static std::set<int> selectedAccts;
    static std::atomic<bool> loginInProgress{ false };
    static bool wantPasteModal = false;
    static std::vector<char> pasteBuf(8192, 0);

    if (!placeIdInit) {
        long long pid = backend::savedPlaceId.load();
        if (pid > 0) { std::string s = std::to_string(pid); memcpy(placeIdBuf, s.data(), std::min(s.size(), sizeof(placeIdBuf) - 1)); }
        placeIdInit = true;
    }

    const float MARGIN = 30.0f;
    const float TOPMARGIN = 14.0f;
    const ImVec2 availTL = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float W = avail.x - MARGIN * 2.0f;
    const float bottomY = availTL.y + avail.y - MARGIN - 16.0f;
    const float padX = 20.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec4 cGreen = theme::good;
    const ImVec4 cRed   = theme::bad;
    const ImVec4 cGold  = theme::warn;
    const ImVec4 cBlue  = ImVec4(0.36f, 0.62f, 0.98f, 1.0f);
    const ImU32 cardBgU     = ImGui::ColorConvertFloat4ToU32(ImVec4(0.093f, 0.098f, 0.109f, 1.0f));
    const ImU32 cardBorderU = ImGui::ColorConvertFloat4ToU32(ImVec4(0.225f, 0.235f, 0.255f, 1.0f));
    const ImU32 subU        = ImGui::ColorConvertFloat4ToU32(theme::subtext);
    const ImU32 textU       = ImGui::ColorConvertFloat4ToU32(theme::text);
    const ImU32 borderU     = ImGui::ColorConvertFloat4ToU32(theme::border);
    const float cardRound = 12.0f;

    auto card = [&](float x, float y, float w, float h) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), cardBgU, cardRound);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), cardBorderU, cardRound, 0, 1.0f);
    };
    auto dashH = [&](float x0, float x1, float yy, ImU32 col, float d, float g, float th) {
        for (float t = x0; t < x1; t += d + g) dl->AddLine(ImVec2(t, yy), ImVec2(std::min(t + d, x1), yy), col, th);
    };
    auto dashV = [&](float y0, float y1, float xx, ImU32 col, float d, float g, float th) {
        for (float t = y0; t < y1; t += d + g) dl->AddLine(ImVec2(xx, t), ImVec2(xx, std::min(t + d, y1)), col, th);
    };

    int acctCount = 0;
    struct RowInfo { std::string name; long long userId; };
    std::vector<RowInfo> rows;
    {
        std::lock_guard<std::mutex> lock(backend::accountsMutex);
        acctCount = (int)backend::accounts.size();
        for (auto& a : backend::accounts) rows.push_back({ a.alias.empty() ? a.username : a.alias, a.userId });
    }
    int aliveN = backend::CountRobloxProcesses();
    float cpu = backend::GetCpuUsagePercent();

    const float baseX = availTL.x + MARGIN, baseY = availTL.y + TOPMARGIN;

    float headY = baseY + 6.0f;
    IconBox(ImVec2(baseX, headY), 48.0f, icon::USER, theme::softAccentBg, theme::accent, 12.0f);
    ImGui::PushFont(theme::fontTitle);
    dl->AddText(ImVec2(baseX + 62.0f, headY + 3.0f), textU, "Account Manager");
    ImGui::PopFont();
    {
        char stats[256];
        snprintf(stats, sizeof(stats),
            "%d accounts  \xE2\x80\xA2  0 connected  \xE2\x80\xA2  %.1f%% CPU  \xE2\x80\xA2  %d alive  \xE2\x80\xA2  %d selected",
            acctCount, cpu, aliveN, (int)selectedAccts.size());
        dl->AddText(ImVec2(baseX + 62.0f, headY + 29.0f), subU, stats);
    }
    {
        bool multiOn = backend::watching.load();
        struct HBtn { const char* label; unsigned icon; int id; const ImVec4* tint; };
        HBtn btns[] = {
            { multiOn ? "Multi Instance: ON" : "Multi Instance", icon::ROCKET, 0, multiOn ? &cGreen : nullptr },
            { "Kill All Instances", icon::TRASH2,  1, &cRed },
            { "Launch Selected",    icon::PLAY,    2, &cGreen },
        };
        const int n = 3;
        const float bh2 = 34.0f, gap = 8.0f, padBtn = 14.0f, iconGap = 7.0f;
        float widths[3], total = 0;
        for (int i = 0; i < n; ++i) {
            ImVec2 ts = ImGui::CalcTextSize(btns[i].label);
            float iconW;
            if (btns[i].icon == icon::PLAY) iconW = 16.0f;
            else { ImGui::PushFont(theme::fontIcon); iconW = ImGui::CalcTextSize(icon::Str(btns[i].icon).c_str()).x; ImGui::PopFont(); }
            widths[i] = ts.x + iconGap + iconW + padBtn * 2.0f;
            total += widths[i];
        }
        total += gap * (n - 1);
        float x = baseX + W - total;
        float by = headY + (48.0f - bh2) * 0.5f;
        for (int i = 0; i < n; ++i) {
            auto& b = btns[i];
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            ImGui::PushID(b.id);
            bool clicked = b.tint
                ? DrawPopButton(b.label, ImVec2(widths[i], bh2), false, b.icon, b.tint)
                : DrawPopButton(b.label, ImVec2(widths[i], bh2), false, b.icon);
            ImGui::PopID();
            if (clicked) {
                if (b.id == 0) {
                    if (backend::watching.load()) backend::StopWatching();
                    else if (EnsureElevatedFor(L"Closing Roblox singleton handles")) backend::StartWatching();
                }
                else if (b.id == 1) std::thread([]() { backend::KillAllRobloxInstances(); }).detach();
                else if (b.id == 2) {
                    backend::PrivateServer aps;
                    { std::lock_guard<std::mutex> lock(backend::activePrivateServerMutex); aps = backend::activePrivateServer; }
                    long long pid = aps.linkCode.empty() ? backend::savedPlaceId.load() : aps.placeId;
                    if (pid <= 0) backend::Log("[!] Set a Place ID first, then Launch Selected.");
                    else if (selectedAccts.empty()) backend::Log("[!] Select at least one account to launch.");
                    else {
                        std::vector<int> idxs(selectedAccts.begin(), selectedAccts.end());
                        std::string code = aps.linkCode;
                        std::thread([idxs, pid, code]() {
                            for (int i : idxs) { backend::LaunchAccountIntoPrivateServer(i, pid, code); Sleep(300); }
                        }).detach();
                    }
                }
            }
            x += widths[i] + gap;
        }
    }

    const float row1Y = baseY + 68.0f;
    const float cardGap = 16.0f;
    const float cardW = (W - cardGap) * 0.5f;
    const float card1H = 306.0f;
    const float launchH = card1H;

    {
        float cx = baseX, cy = row1Y, cw = cardW;
        card(cx, cy, cw, card1H);
        ImGui::PushFont(theme::fontBrand);
        dl->AddText(ImVec2(cx + padX, cy + 20), textU, "Add Accounts");
        ImGui::PopFont();
        dl->AddText(ImVec2(cx + padX, cy + 46), subU, "Add your accounts securely using one of the methods below.");

        struct AB { const char* label; unsigned icon; const ImVec4* iconTint; int id; };
        AB abs2[] = {
            { "Login from Browser", icon::GLOBE,     &cGreen, 0 },
            { "Paste Cookies",      icon::FILE_TEXT, &cGold,  1 },
            { "Load from File",     icon::FILE_TEXT, &cBlue,  2 },
        };
        float bh3 = 40.0f, gap = 12.0f, y = cy + 82.0f, x = cx + padX;
        for (auto& b : abs2) {
            ImGui::PushFont(theme::fontIcon);
            float iconW = ImGui::CalcTextSize(icon::Str(b.icon).c_str()).x;
            ImGui::PopFont();
            float w = ImGui::CalcTextSize(b.label).x + iconW + 8.0f + 32.0f;
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(100 + b.id);
            bool clicked = DrawPopButton(b.label, ImVec2(w, bh3), false, b.icon, nullptr, b.iconTint);
            ImGui::PopID();
            if (clicked) {
                if (b.id == 0 && !loginInProgress.load()) {
                    loginInProgress.store(true);
                    std::thread([]() {
                        login::ShowRobloxLoginWindow(g_exeDir, [](bool ok, std::string cookie) {
                            if (ok) backend::AddAccountFromCookie(cookie);
                            else backend::Log("[i] Login cancelled.");
                            loginInProgress.store(false);
                        });
                    }).detach();
                } else if (b.id == 1) {
                    wantPasteModal = true;
                } else if (b.id == 2) {
                    std::wstring path = OpenCookieFileDialog(hwnd);
                    if (!path.empty()) ImportCookiesFromFile(path);
                }
            }
            x += w + gap;
        }

        {
            backend::RobloxBuildState bs;
            std::vector<std::string> builds;
            {
                std::lock_guard<std::mutex> lock(backend::robloxBuildMutex);
                bs = backend::robloxBuild;
                builds = backend::downloadedBuilds;
            }
            static bool weaoKicked = false;
            if (!weaoKicked) { weaoKicked = true; std::thread([]() { backend::FetchWeaoVersions(); }).detach(); }

            static bool lastBusy = false;
            if (bs.busy != lastBusy) { lastBusy = bs.busy; idlectl::RequestFrames(4); }

            float sy = cy + 142.0f;
            dl->AddLine(ImVec2(cx + padX, sy), ImVec2(cx + cw - padX, sy), borderU, 1.0f);

            ImGui::PushFont(theme::fontIcon);
            std::string sg = icon::Str(icon::ROCKET);
            dl->AddText(ImVec2(cx + padX, sy + 15.0f), ImGui::ColorConvertFloat4ToU32(theme::accent), sg.c_str());
            float sgw = ImGui::CalcTextSize(sg.c_str()).x;
            ImGui::PopFont();
            dl->AddText(ImVec2(cx + padX + sgw + 8.0f, sy + 14.0f), subU, "Roblox Version");

            {
                std::string live = bs.weaoLoaded ? bs.liveVersion : "checking live...";
                ImVec2 ls = ImGui::CalcTextSize(live.c_str());
                float lx = cx + cw - padX - ls.x;
                ImGui::SetCursorScreenPos(ImVec2(lx - 6.0f, sy + 10.0f));
                ImGui::PushID("weaorefresh");
                ImGui::InvisibleButton("##weaoref", ImVec2(ls.x + 12.0f, 22.0f));
                bool rh = ImGui::IsItemHovered();
                if (rh && bs.weaoLoaded) {
                    std::string tip = "Live version\n" + bs.liveVersion;
                    if (!bs.liveDate.empty()) tip += "\n" + bs.liveDate;
                    if (!bs.futureVersion.empty()) tip += "\nNext: " + bs.futureVersion;
                    tip += "\nClick to re-check WEAO";
                    Tip(tip);
                }
                if (ImGui::IsItemClicked()) std::thread([]() { backend::FetchWeaoVersions(); }).detach();
                ImGui::PopID();
                dl->AddText(ImVec2(lx, sy + 14.0f),
                    ImGui::ColorConvertFloat4ToU32(rh ? theme::accent : theme::subtext), live.c_str());
            }

            float fh = 34.0f;
            {
                float half = (cw - padX * 2.0f - 8.0f) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(cx + padX, sy + 40.0f));
                ImVec4 latestTint = cGreen;
                ImGui::PushID("dl_latest");
                if (DrawPopButton("Download Latest", ImVec2(half, fh), false, 0, &latestTint) && !bs.busy)
                    std::thread([]() { backend::ForceLiveBuild(); }).detach();
                ImGui::PopID();
                if (ImGui::IsItemHovered() && !bs.liveVersion.empty())
                    Tip("Download Latest\n" + bs.liveVersion +
                        (bs.liveDate.empty() ? "" : "\n" + bs.liveDate));

                ImGui::SetCursorScreenPos(ImVec2(cx + padX + half + 8.0f, sy + 40.0f));
                ImVec4 prevTint = cGold;
                ImGui::PushID("dl_prev");
                if (DrawPopButton("Download Previous", ImVec2(half, fh), false, 0, &prevTint) && !bs.busy)
                    std::thread([]() { backend::DownloadPreviousBuild(); }).detach();
                ImGui::PopID();
                if (ImGui::IsItemHovered())
                    Tip(bs.pastVersion.empty()
                        ? std::string("Download Previous\nThe build Roblox shipped before this one")
                        : "Download Previous\n" + bs.pastVersion +
                          (bs.pastDate.empty() ? "" : "\n" + bs.pastDate));
            }

            float fy = sy + 40.0f + fh + 8.0f;
            float fullW = cw - padX * 2.0f;
            float dgW = 96.0f, inW3 = fullW - dgW - 8.0f;
            {
                ImVec2 imn(cx + padX, fy), imx(cx + padX + inW3, fy + fh);
                dl->AddRectFilled(imn, imx, ImGui::ColorConvertFloat4ToU32(ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 8.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, (fh - ImGui::GetFontSize()) * 0.5f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::SetCursorScreenPos(imn);
                ImGui::SetNextItemWidth(inW3);
                ImGui::InputTextWithHint("##verhash", "version-hash to downgrade to (rdd.weao.gg)", verHashBuf, sizeof(verHashBuf));
                bool vFocused = ImGui::IsItemActive();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(3);
                if (vFocused) DrawSoftRectGlow(dl, imn, imx, 8.0f, 6.0f, 0.14f, theme::accent);
                dl->AddRect(imn, imx, ImGui::ColorConvertFloat4ToU32(vFocused
                    ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.95f)
                    : ImVec4(0.225f, 0.235f, 0.255f, 1.0f)), 8.0f, 0, 1.3f);

                ImGui::SetCursorScreenPos(ImVec2(imx.x + 8.0f, fy));
                ImVec4 dgTint = cGold;
                ImGui::PushID("downgrade");
                bool dgClick = DrawPopButton("Downgrade", ImVec2(dgW, fh), false, 0, &dgTint);
                ImGui::PopID();
                if (dgClick && !bs.busy) {
                    std::string hash(verHashBuf);
                    std::thread([hash]() { backend::DownloadRobloxBuild(hash); }).detach();
                }
            }

            float py = fy + fh + 8.0f;
            if (bs.busy) {
                float pct = std::min(1.0f, std::max(0.0f, bs.progress));
                char pctBuf[16]; snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)(pct * 100.0f + 0.5f));
                ImVec2 ps2 = ImGui::CalcTextSize(pctBuf);
                dl->AddText(ImVec2(cx + cw - padX - ps2.x, py + 2.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::accent), pctBuf);
                dl->AddText(ImVec2(cx + padX, py + 2.0f), subU,
                    FitTextToWidth(bs.status, fullW - ps2.x - 10.0f).c_str());

                ImVec2 bmin(cx + padX, py + 24.0f), bmax(cx + cw - padX, py + 30.0f);
                dl->AddRectFilled(bmin, bmax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 3.0f);
                dl->AddRectFilled(bmin, ImVec2(bmin.x + (bmax.x - bmin.x) * pct, bmax.y),
                    ImGui::ColorConvertFloat4ToU32(theme::accent), 3.0f);

                idlectl::RequestFrames(2);
            } else if (builds.empty()) {
                dl->AddText(ImVec2(cx + padX, py + (fh - ImGui::GetFontSize()) * 0.5f), subU,
                    "No client downloaded yet");
            } else {
                bool on = !bs.activeVersion.empty();
                ImGui::SetCursorScreenPos(ImVec2(cx + padX, py + (fh - 26.0f) * 0.5f));
                bool wasOn = on;
                ToggleSwitch("##useclient", &on);
                if (on != wasOn) {
                    if (!on) backend::SetActiveBuild("");
                    else {
                        std::string pick = bs.preferredVersion.empty() ? builds.back() : bs.preferredVersion;
                        if (std::find(builds.begin(), builds.end(), pick) == builds.end()) pick = builds.back();
                        backend::SetActiveBuild(pick);
                    }
                }

                float lx = cx + padX + 46.0f + 12.0f;
                float ly = py + (fh - ImGui::GetFontSize()) * 0.5f;
                std::string shown = on ? bs.activeVersion
                    : (bs.preferredVersion.empty() ? builds.back() : bs.preferredVersion);

                float remW = ImGui::CalcTextSize("Remove").x;
                float remX = cx + cw - padX - remW;
                ImGui::SetCursorScreenPos(ImVec2(remX - 6.0f, py + 6.0f));
                ImGui::PushID("delbuild");
                ImGui::InvisibleButton("##delbuild", ImVec2(remW + 12.0f, fh - 12.0f));
                bool remHov = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    std::thread([shown]() { backend::DeleteBuild(shown); }).detach();
                }
                ImGui::PopID();
                if (remHov) Tip("Remove\nDeletes Builds\\" + shown + "\nnext to VelsMultiTool.exe");
                dl->AddText(ImVec2(remX, ly),
                    ImGui::ColorConvertFloat4ToU32(remHov ? theme::bad : theme::subtext), "Remove");

                std::string label = on ? "Launching with downloaded client"
                                       : "Launch with downloaded client";
                dl->AddText(ImVec2(lx, ly), ImGui::ColorConvertFloat4ToU32(on ? theme::text : theme::subtext),
                    FitTextToWidth(label + "  (" + shown + ")", remX - 12.0f - lx).c_str());
            }
        }
    }

    {
        float cx = baseX + cardW + cardGap, cy = row1Y, cw = cardW;
        card(cx, cy, cw, launchH);
        ImGui::PushFont(theme::fontBrand);
        dl->AddText(ImVec2(cx + padX, cy + 20), textU, "Launch Settings");
        ImGui::PopFont();

        float leftW = cw * 0.44f;
        dl->AddText(ImVec2(cx + padX, cy + 52), subU, "Place ID");

        {
            float ibx0 = cx + padX, iby0 = cy + 72.0f;
            float ibw = leftW - padX - 6.0f, ibh = 40.0f;
            ImVec2 imn(ibx0, iby0), imx(ibx0 + ibw, iby0 + ibh);
            dl->AddRectFilled(imn, imx, ImGui::ColorConvertFloat4ToU32(ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 9.0f);
            {
                std::string g = icon::Str(icon::BOX);
                ImGui::PushFont(theme::fontIcon);
                ImVec2 gs = ImGui::CalcTextSize(g.c_str());
                dl->AddText(ImVec2(ibx0 + 13.0f, iby0 + (ibh - gs.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(theme::subtext), g.c_str());
                ImGui::PopFont();
            }
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, (ibh - ImGui::GetFontSize()) * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            float textX = ibx0 + 40.0f;
            ImGui::SetCursorScreenPos(ImVec2(textX, iby0));
            ImGui::SetNextItemWidth(imx.x - textX - 10.0f);
            bool changed = ImGui::InputTextWithHint("##am_place", "Enter a Roblox Place ID", placeIdBuf, sizeof(placeIdBuf), ImGuiInputTextFlags_CharsDecimal);
            bool focused = ImGui::IsItemActive();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            if (changed) {
                long long pid = _atoi64(placeIdBuf);
                backend::SavePlaceId(pid);
                if (pid > 0) {
                    std::string cookie;
                    { std::lock_guard<std::mutex> lock(backend::accountsMutex);
                      if (!backend::accounts.empty()) cookie = backend::accounts[0].cookie; }
                    std::thread([pid, cookie]() { backend::FetchPlaceInfo(pid, cookie); }).detach();
                }
            }
            dl->AddLine(ImVec2(ibx0 + 36.0f, iby0 + 9.0f), ImVec2(ibx0 + 36.0f, iby0 + ibh - 9.0f),
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.21f, 0.23f, 1.0f)), 1.0f);
            if (focused) DrawSoftRectGlow(dl, imn, imx, 9.0f, 6.0f, 0.16f, theme::accent);
            ImVec4 bcol = focused ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.95f)
                                  : ImVec4(0.225f, 0.235f, 0.255f, 1.0f);
            dl->AddRect(imn, imx, ImGui::ColorConvertFloat4ToU32(bcol), 9.0f, 0, 1.4f);
        }
        {
            long long curId = _atoi64(placeIdBuf);
            float ddx = cx + padX, ddy = cy + 120.0f;
            float ddw = leftW - padX - 6.0f, ddh = 34.0f;
            ImVec2 dmn(ddx, ddy), dmx(ddx + ddw, ddy + ddh);

            std::vector<backend::SavedPlace> snapshot;
            { std::lock_guard<std::mutex> lock(backend::savedPlacesMutex); snapshot = backend::savedPlaces; }

            bool popupOpen = ImGui::IsPopupOpen("places_popup");

            ImGui::SetCursorScreenPos(dmn);
            ImGui::InvisibleButton("##places_dd", ImVec2(ddw, ddh));
            bool hdrHover = ImGui::IsItemHovered();
            bool hdrHeld  = ImGui::IsItemActive();
            if (ImGui::IsItemClicked() && !popupOpen) ImGui::OpenPopup("places_popup");
            float hAnim = PopAnim(ImGui::GetID("##places_dd"), hdrHover, hdrHeld);
            float hHov  = std::max(hAnim, 0.0f);

            ImVec4 hdrFill = popupOpen ? theme::glassHover
                                       : ImVec4(0.132f, 0.138f, 0.150f, 1.0f);
            dl->AddRectFilled(dmn, dmx, ImGui::ColorConvertFloat4ToU32(hdrFill), 8.0f);
            if (hHov > 0.01f)
                DrawSoftRectGlow(dl, dmn, dmx, 8.0f, 6.0f * hHov, 0.12f * hHov, theme::accent);
            ImVec4 hdrBorder = (popupOpen || hHov > 0.01f)
                ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.45f + 0.4f * hHov)
                : ImVec4(0.225f, 0.235f, 0.255f, 1.0f);
            dl->AddRect(dmn, dmx, ImGui::ColorConvertFloat4ToU32(hdrBorder), 8.0f, 0, 1.3f);

            std::string curLabel;
            for (auto& p : snapshot) if (p.id == curId) { curLabel = p.name; break; }
            bool isHint = curLabel.empty();
            if (isHint) curLabel = "Saved places";
            dl->AddText(ImVec2(ddx + 12.0f, ddy + (ddh - ImGui::GetFontSize()) * 0.5f),
                isHint ? subU : textU, curLabel.c_str());

            static float chevT = 0.0f;
            chevT += ((popupOpen ? 1.0f : 0.0f) - chevT) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
            {
                float cxp = dmx.x - 16.0f, cyp = ddy + ddh * 0.5f;
                float ang = 3.14159265f * chevT;
                float ca = cosf(ang), sa = sinf(ang);
                auto rot = [&](float rx, float ry) {
                    return ImVec2(cxp + (rx * ca - ry * sa), cyp + (rx * sa + ry * ca));
                };
                ImU32 chevU = ImGui::ColorConvertFloat4ToU32(
                    isHint ? theme::subtext : ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.9f));
                dl->AddTriangleFilled(rot(-4.0f, -2.2f), rot(4.0f, -2.2f), rot(0.0f, 3.0f), chevU);
            }

            static float ddAnim = 0.0f;
            ddAnim += ((popupOpen ? 1.0f : 0.0f) - ddAnim) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
            if (ddAnim < 0.001f) ddAnim = 0.0f;

            const float rowH = 36.0f;
            const int   rowN = (int)snapshot.size();
            const float footerH = 8.0f + 18.0f + 8.0f + 32.0f + 12.0f;
            const float fullH = 8.0f + rowN * rowH + 10.0f + 1.0f + footerH;
            float eased = ddAnim * ddAnim * (3.0f - 2.0f * ddAnim);

            float pw = ddw;
            for (const auto& p : snapshot) {
                float need = 12.0f + ImGui::CalcTextSize(p.name.empty() ? "(unnamed)" : p.name.c_str()).x
                           + 14.0f + ImGui::CalcTextSize(std::to_string(p.id).c_str()).x + 41.0f;
                pw = std::max(pw, need);
            }
            pw = std::min(pw, cw - padX * 2.0f);

            ImGui::SetNextWindowPos(ImVec2(ddx, ddy + ddh + 6.0f));
            ImGui::SetNextWindowSize(ImVec2(pw, fullH * eased));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            bool vis = ImGui::BeginPopup("places_popup",
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
            if (vis) {
                ImDrawList* pdl = ImGui::GetWindowDrawList();
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 pmin = wp, pmax = ImVec2(wp.x + pw, wp.y + fullH);

                pdl->AddRectFilled(pmin, pmax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.115f, 0.120f, 0.132f, 1.0f)), 10.0f);
                pdl->AddRect(pmin, pmax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.21f, 0.24f, 1.0f)), 10.0f, 0, 1.2f);

                for (int i = 0; i < rowN; ++i) {
                    const backend::SavedPlace& p = snapshot[i];
                    float ry = wp.y + 8.0f + i * rowH;
                    char rid[32]; snprintf(rid, sizeof(rid), "##ddrow%d", i);
                    char did[32]; snprintf(did, sizeof(did), "##dddel%d", i);

                    ImGui::SetCursorScreenPos(ImVec2(wp.x + 5.0f, ry));
                    ImGui::InvisibleButton(rid, ImVec2(pw - 10.0f - 30.0f, rowH - 4.0f));
                    bool selHov = ImGui::IsItemHovered();
                    bool selHeld = ImGui::IsItemActive();
                    bool selClick = ImGui::IsItemClicked();
                    float ra = PopAnim(ImGui::GetID(rid), selHov, selHeld);
                    float rHov = std::max(ra, 0.0f);

                    ImGui::SetCursorScreenPos(ImVec2(wp.x + pw - 5.0f - 28.0f, ry + 2.0f));
                    ImGui::InvisibleButton(did, ImVec2(28.0f, rowH - 8.0f));
                    bool delHov = ImGui::IsItemHovered();
                    bool delClick = ImGui::IsItemClicked();

                    bool selected = (p.id == curId);
                    ImVec2 rmin(wp.x + 5.0f, ry), rmax(wp.x + pw - 5.0f, ry + rowH - 4.0f);

                    if (rHov > 0.01f || selected || delHov) {
                        float fa = selected ? 0.16f : (0.05f + 0.11f * std::max(rHov, delHov ? 1.0f : 0.0f));
                        pdl->AddRectFilled(rmin, rmax,
                            ImGui::ColorConvertFloat4ToU32(ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, fa)), 7.0f);
                    }
                    if (selected)
                        pdl->AddRectFilled(ImVec2(rmin.x, rmin.y + 6.0f), ImVec2(rmin.x + 3.0f, rmax.y - 6.0f),
                            ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);

                    ImVec4 nameCol = (selected || rHov > 0.01f) ? theme::text
                        : ImVec4(theme::text.x, theme::text.y, theme::text.z, 0.82f);
                    std::string ids = std::to_string(p.id);
                    ImVec2 idsz = ImGui::CalcTextSize(ids.c_str());
                    float idRight = wp.x + pw - 5.0f - 30.0f;

                    std::string nm = p.name.empty() ? std::string("(unnamed)") : p.name;
                    float nameX = rmin.x + 12.0f;
                    float nameW = (idRight - idsz.x - 12.0f) - nameX;
                    pdl->AddText(ImVec2(nameX, ry + (rowH - 4.0f - ImGui::GetFontSize()) * 0.5f),
                        ImGui::ColorConvertFloat4ToU32(nameCol), FitTextToWidth(nm, nameW).c_str());
                    float idAlpha = (selHov || delHov) ? 0.0f : 0.55f;
                    if (idAlpha > 0.01f)
                        pdl->AddText(ImVec2(idRight - idsz.x - 6.0f, ry + (rowH - 4.0f - idsz.y) * 0.5f),
                            ImGui::ColorConvertFloat4ToU32(ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, idAlpha)), ids.c_str());

                    if (selHov || delHov) {
                        float dcx = wp.x + pw - 5.0f - 15.0f, dcy = ry + (rowH - 4.0f) * 0.5f;
                        ImVec4 xcol = delHov ? theme::bad : ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, 0.85f);
                        ImU32 xu = ImGui::ColorConvertFloat4ToU32(xcol);
                        float r = 4.0f;
                        pdl->AddLine(ImVec2(dcx - r, dcy - r), ImVec2(dcx + r, dcy + r), xu, 1.6f);
                        pdl->AddLine(ImVec2(dcx - r, dcy + r), ImVec2(dcx + r, dcy - r), xu, 1.6f);
                    }

                    if (delClick) { backend::RemoveSavedPlace(p.id); }
                    else if (selClick) {
                        std::string s = std::to_string(p.id);
                        size_t len = std::min(s.size(), sizeof(placeIdBuf) - 1);
                        memcpy(placeIdBuf, s.data(), len); placeIdBuf[len] = '\0';
                        backend::SavePlaceId(p.id);
                        std::string cookie;
                        { std::lock_guard<std::mutex> lock(backend::accountsMutex);
                          if (!backend::accounts.empty()) cookie = backend::accounts[0].cookie; }
                        long long pid = p.id;
                        std::thread([pid, cookie]() { backend::FetchPlaceInfo(pid, cookie); }).detach();
                        ImGui::CloseCurrentPopup();
                    }
                }

                float sy = wp.y + 8.0f + rowN * rowH + 8.0f;
                pdl->AddLine(ImVec2(wp.x + 10.0f, sy), ImVec2(wp.x + pw - 10.0f, sy),
                    ImGui::ColorConvertFloat4ToU32(theme::border), 1.0f);

                float fy = sy + 10.0f;
                pdl->AddText(ImVec2(wp.x + 12.0f, fy), subU, "Save current Place ID");
                fy += 20.0f;

                static char newName[64] = "";
                float inW = pw - 20.0f - 62.0f;
                ImVec2 inMin(wp.x + 10.0f, fy), inMax(wp.x + 10.0f + inW, fy + 32.0f);
                pdl->AddRectFilled(inMin, inMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 7.0f);

                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, (32.0f - ImGui::GetFontSize()) * 0.5f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::SetCursorScreenPos(inMin);
                ImGui::SetNextItemWidth(inW);
                ImGui::InputTextWithHint("##newplacename", "Name", newName, sizeof(newName));
                bool inFocused = ImGui::IsItemActive();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(3);
                pdl->AddRect(inMin, inMax, ImGui::ColorConvertFloat4ToU32(inFocused
                    ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.9f)
                    : ImVec4(0.20f, 0.21f, 0.24f, 1.0f)), 7.0f, 0, 1.2f);

                ImGui::SetCursorScreenPos(ImVec2(inMax.x + 8.0f, fy));
                ImVec4 saveTint = theme::good;
                if (DrawPopButton("Save", ImVec2(pw - 20.0f - inW - 8.0f, 32.0f), false, 0, &saveTint)) {
                    if (curId > 0) {
                        std::string nmS = newName[0] ? std::string(newName) : ("Place " + std::to_string(curId));
                        backend::AddSavedPlace(curId, nmS);
                        newName[0] = '\0';
                    }
                }

                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }

        {
            float secY = cy + 172.0f;
            dl->AddLine(ImVec2(cx + padX, secY), ImVec2(cx + cw - padX, secY), borderU, 1.0f);

            float psx = cx + padX, psy = secY + 14.0f;
            float psw = cw - padX * 2.0f;
            ImGui::PushFont(theme::fontIcon);
            std::string secGlyph = icon::Str(icon::SHIELD_CHECK);
            dl->AddText(ImVec2(psx, psy + 1.0f), ImGui::ColorConvertFloat4ToU32(theme::accent), secGlyph.c_str());
            float glyphW = ImGui::CalcTextSize(secGlyph.c_str()).x;
            ImGui::PopFont();
            dl->AddText(ImVec2(psx + glyphW + 8.0f, psy), subU, "Private Server");

            backend::PrivateServer active;
            { std::lock_guard<std::mutex> lock(backend::activePrivateServerMutex); active = backend::activePrivateServer; }
            bool resolving = backend::privateServerResolving.load();

            float useW = 62.0f, inW2 = psw - useW - 8.0f;
            float iy = psy + 22.0f, ih = 34.0f;
            ImVec2 imn(psx, iy), imx(psx + inW2, iy + ih);
            dl->AddRectFilled(imn, imx, ImGui::ColorConvertFloat4ToU32(ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 8.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, (ih - ImGui::GetFontSize()) * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::SetCursorScreenPos(imn);
            ImGui::SetNextItemWidth(inW2);
            ImGui::InputTextWithHint("##ps_link", "https://www.roblox.com/share?code=...&type=Server", psLinkBuf, sizeof(psLinkBuf));
            bool psFocused = ImGui::IsItemActive();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            if (psFocused) DrawSoftRectGlow(dl, imn, imx, 8.0f, 6.0f, 0.14f, theme::accent);
            dl->AddRect(imn, imx, ImGui::ColorConvertFloat4ToU32(psFocused
                ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.95f)
                : ImVec4(0.225f, 0.235f, 0.255f, 1.0f)), 8.0f, 0, 1.3f);

            ImGui::SetCursorScreenPos(ImVec2(imx.x + 8.0f, iy));
            ImVec4 useTint = theme::accent;
            ImGui::PushID("ps_use");
            bool useClicked = DrawPopButton(resolving ? "..." : "Use", ImVec2(useW, ih), false, 0, &useTint);
            ImGui::PopID();
            if (useClicked && !resolving) {
                std::string link(psLinkBuf);
                std::string cookie;
                { std::lock_guard<std::mutex> lock(backend::accountsMutex);
                  if (!backend::accounts.empty()) cookie = backend::accounts[0].cookie; }
                std::thread([link, cookie]() {
                    backend::PrivateServer ps;
                    if (!backend::ResolvePrivateServerLink(link, cookie, ps)) return;
                    backend::SetActivePrivateServer(ps);
                    backend::SavePlaceId(ps.placeId);
                    backend::FetchPlaceInfo(ps.placeId, cookie);
                }).detach();
            }

            {
                float ddx = psx, ddy = iy + ih + 10.0f, ddw = psw * 0.5f - 6.0f, ddh = 34.0f;
                ImVec2 dmn(ddx, ddy), dmx(ddx + ddw, ddy + ddh);

                std::vector<backend::PrivateServer> snap;
                { std::lock_guard<std::mutex> lock(backend::privateServersMutex); snap = backend::savedPrivateServers; }

                bool popupOpen = ImGui::IsPopupOpen("ps_popup");
                ImGui::SetCursorScreenPos(dmn);
                ImGui::InvisibleButton("##ps_dd", ImVec2(ddw, ddh));
                bool hdrHover = ImGui::IsItemHovered(), hdrHeld = ImGui::IsItemActive();
                if (ImGui::IsItemClicked() && !popupOpen) ImGui::OpenPopup("ps_popup");
                float hHov = std::max(PopAnim(ImGui::GetID("##ps_dd"), hdrHover, hdrHeld), 0.0f);

                dl->AddRectFilled(dmn, dmx, ImGui::ColorConvertFloat4ToU32(popupOpen ? theme::glassHover
                    : ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 8.0f);
                if (hHov > 0.01f) DrawSoftRectGlow(dl, dmn, dmx, 8.0f, 6.0f * hHov, 0.12f * hHov, theme::accent);
                dl->AddRect(dmn, dmx, ImGui::ColorConvertFloat4ToU32((popupOpen || hHov > 0.01f)
                    ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.45f + 0.4f * hHov)
                    : ImVec4(0.225f, 0.235f, 0.255f, 1.0f)), 8.0f, 0, 1.3f);

                std::string activeName;
                for (auto& p : snap) if (p.linkCode == active.linkCode && !active.linkCode.empty()) { activeName = p.name; break; }
                dl->AddText(ImVec2(ddx + 12.0f, ddy + (ddh - ImGui::GetFontSize()) * 0.5f), subU, "Saved private servers");
                {
                    float cxp = dmx.x - 16.0f, cyp = ddy + ddh * 0.5f;
                    float ang = 3.14159265f * (popupOpen ? 1.0f : 0.0f);
                    float ca = cosf(ang), sa = sinf(ang);
                    auto rot = [&](float rx, float ry) { return ImVec2(cxp + (rx * ca - ry * sa), cyp + (rx * sa + ry * ca)); };
                    dl->AddTriangleFilled(rot(-4.0f, -2.2f), rot(4.0f, -2.2f), rot(0.0f, 3.0f), subU);
                }

                {
                    bool on = !active.linkCode.empty();
                    float chx = psx + psw * 0.5f + 6.0f, chw = psw * 0.5f - 6.0f;
                    ImVec2 cmn(chx, ddy), cmx(chx + chw, ddy + ddh);
                    ImVec4 tone = on ? theme::good : theme::subtext;
                    dl->AddRectFilled(cmn, cmx, ImGui::ColorConvertFloat4ToU32(
                        ImVec4(tone.x, tone.y, tone.z, on ? 0.10f : 0.05f)), 8.0f);
                    dl->AddRect(cmn, cmx, ImGui::ColorConvertFloat4ToU32(
                        ImVec4(tone.x, tone.y, tone.z, on ? 0.55f : 0.30f)), 8.0f, 0, 1.2f);

                    float dotY = ddy + ddh * 0.5f;
                    dl->AddCircleFilled(ImVec2(chx + 14.0f, dotY), 4.0f, ImGui::ColorConvertFloat4ToU32(tone));
                    if (on) dl->AddCircleFilled(ImVec2(chx + 14.0f, dotY), 7.5f,
                        ImGui::ColorConvertFloat4ToU32(ImVec4(tone.x, tone.y, tone.z, 0.20f)));

                    float textRight = cmx.x - 12.0f;
                    if (on) {
                        float bxc = cmx.x - 16.0f;
                        ImGui::SetCursorScreenPos(ImVec2(bxc - 12.0f, dotY - 11.0f));
                        ImGui::PushID("ps_clear");
                        ImGui::InvisibleButton("##ps_clear", ImVec2(24.0f, 22.0f));
                        bool cHov = ImGui::IsItemHovered();
                        if (ImGui::IsItemClicked()) backend::ClearActivePrivateServer();
                        ImGui::PopID();
                        if (cHov) Tip("Clear private server\nLaunches go to the public game");
                        ImU32 xu = ImGui::ColorConvertFloat4ToU32(cHov ? theme::bad : theme::subtext);
                        float r = 4.5f;
                        dl->AddLine(ImVec2(bxc - r, dotY - r), ImVec2(bxc + r, dotY + r), xu, 1.7f);
                        dl->AddLine(ImVec2(bxc - r, dotY + r), ImVec2(bxc + r, dotY - r), xu, 1.7f);
                        textRight = bxc - 12.0f;
                    }

                    std::string chipText = resolving ? "Resolving link..."
                        : (on ? (activeName.empty() ? ("Private " + std::to_string(active.placeId)) : activeName)
                              : "Public server");
                    float chipTextX = chx + 26.0f;
                    dl->AddText(ImVec2(chipTextX, dotY - ImGui::GetFontSize() * 0.5f),
                        ImGui::ColorConvertFloat4ToU32(on ? theme::text : theme::subtext),
                        FitTextToWidth(chipText, std::max(20.0f, textRight - chipTextX)).c_str());
                }

                const float rowH = 36.0f;
                const int   rowN = (int)snap.size();
                const float footerH = 8.0f + 18.0f + 8.0f + 32.0f + 12.0f;
                const float fullH = 8.0f + std::max(rowN, 1) * rowH + 10.0f + 1.0f + footerH;

                static float psAnim = 0.0f;
                psAnim += ((popupOpen ? 1.0f : 0.0f) - psAnim) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
                if (psAnim < 0.001f) psAnim = 0.0f;
                float eased = psAnim * psAnim * (3.0f - 2.0f * psAnim);

                float ppw = ddw;
                for (const auto& p : snap) {
                    float need = 12.0f + ImGui::CalcTextSize(p.name.empty() ? "(unnamed)" : p.name.c_str()).x
                               + 14.0f + ImGui::CalcTextSize(std::to_string(p.placeId).c_str()).x + 51.0f;
                    ppw = std::max(ppw, need);
                }
                ppw = std::min(ppw, psw);

                ImGui::SetNextWindowPos(ImVec2(ddx, ddy + ddh + 6.0f));
                ImGui::SetNextWindowSize(ImVec2(ppw, fullH * eased));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
                if (ImGui::BeginPopup("ps_popup",
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove)) {
                    ImDrawList* pdl = ImGui::GetWindowDrawList();
                    ImVec2 wp = ImGui::GetWindowPos();
                    pdl->AddRectFilled(wp, ImVec2(wp.x + ppw, wp.y + fullH),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.115f, 0.120f, 0.132f, 1.0f)), 10.0f);
                    pdl->AddRect(wp, ImVec2(wp.x + ppw, wp.y + fullH),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.21f, 0.24f, 1.0f)), 10.0f, 0, 1.2f);

                    if (rowN == 0) {
                        pdl->AddText(ImVec2(wp.x + 14.0f, wp.y + 8.0f + (rowH - ImGui::GetFontSize()) * 0.5f),
                            subU, "No saved private servers");
                    }
                    for (int i = 0; i < rowN; ++i) {
                        const backend::PrivateServer& p = snap[i];
                        float ry = wp.y + 8.0f + i * rowH;
                        char rid[32]; snprintf(rid, sizeof(rid), "##psrow%d", i);
                        char did[32]; snprintf(did, sizeof(did), "##psdel%d", i);

                        ImGui::SetCursorScreenPos(ImVec2(wp.x + 5.0f, ry));
                        ImGui::InvisibleButton(rid, ImVec2(ppw - 10.0f - 30.0f, rowH - 4.0f));
                        bool selHov = ImGui::IsItemHovered(), selHeld = ImGui::IsItemActive();
                        bool selClick = ImGui::IsItemClicked();
                        float rHov = std::max(PopAnim(ImGui::GetID(rid), selHov, selHeld), 0.0f);

                        ImGui::SetCursorScreenPos(ImVec2(wp.x + ppw - 5.0f - 28.0f, ry + 2.0f));
                        ImGui::InvisibleButton(did, ImVec2(28.0f, rowH - 8.0f));
                        bool delHov = ImGui::IsItemHovered();
                        bool delClick = ImGui::IsItemClicked();

                        bool selected = (p.linkCode == active.linkCode);
                        ImVec2 rmin(wp.x + 5.0f, ry), rmax(wp.x + ppw - 5.0f, ry + rowH - 4.0f);
                        if (rHov > 0.01f || selected || delHov) {
                            float fa = selected ? 0.16f : (0.05f + 0.11f * std::max(rHov, delHov ? 1.0f : 0.0f));
                            pdl->AddRectFilled(rmin, rmax,
                                ImGui::ColorConvertFloat4ToU32(ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, fa)), 7.0f);
                        }
                        if (selected)
                            pdl->AddRectFilled(ImVec2(rmin.x, rmin.y + 6.0f), ImVec2(rmin.x + 3.0f, rmax.y - 6.0f),
                                ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);

                        std::string ids = std::to_string(p.placeId);
                        ImVec2 idsz = ImGui::CalcTextSize(ids.c_str());

                        std::string nm = p.name.empty() ? std::string("(unnamed)") : p.name;
                        float nameX = rmin.x + 12.0f;
                        float nameW = (wp.x + ppw - 41.0f - idsz.x - 10.0f) - nameX;
                        pdl->AddText(ImVec2(nameX, ry + (rowH - 4.0f - ImGui::GetFontSize()) * 0.5f), textU,
                            FitTextToWidth(nm, nameW).c_str());
                        float idAlpha = (selHov || delHov) ? 0.0f : 0.55f;
                        if (idAlpha > 0.01f)
                            pdl->AddText(ImVec2(wp.x + ppw - 41.0f - idsz.x, ry + (rowH - 4.0f - idsz.y) * 0.5f),
                                ImGui::ColorConvertFloat4ToU32(ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, idAlpha)), ids.c_str());

                        if (selHov || delHov) {
                            float dcx = wp.x + ppw - 20.0f, dcy = ry + (rowH - 4.0f) * 0.5f;
                            ImU32 xu = ImGui::ColorConvertFloat4ToU32(delHov ? theme::bad
                                : ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, 0.85f));
                            float r = 4.0f;
                            pdl->AddLine(ImVec2(dcx - r, dcy - r), ImVec2(dcx + r, dcy + r), xu, 1.6f);
                            pdl->AddLine(ImVec2(dcx - r, dcy + r), ImVec2(dcx + r, dcy - r), xu, 1.6f);
                        }

                        if (delClick) backend::RemoveSavedPrivateServer(p.linkCode);
                        else if (selClick) {
                            backend::PrivateServer pick = p;
                            std::string cookie;
                            { std::lock_guard<std::mutex> lock(backend::accountsMutex);
                              if (!backend::accounts.empty()) cookie = backend::accounts[0].cookie; }
                            std::string s = std::to_string(pick.placeId);
                            size_t len = std::min(s.size(), sizeof(placeIdBuf) - 1);
                            memcpy(placeIdBuf, s.data(), len); placeIdBuf[len] = '\0';
                            std::thread([pick, cookie]() {
                                backend::SetActivePrivateServer(pick);
                                backend::SavePlaceId(pick.placeId);
                                backend::FetchPlaceInfo(pick.placeId, cookie);
                            }).detach();
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    float sy = wp.y + 8.0f + std::max(rowN, 1) * rowH + 8.0f;
                    pdl->AddLine(ImVec2(wp.x + 10.0f, sy), ImVec2(wp.x + ppw - 10.0f, sy),
                        ImGui::ColorConvertFloat4ToU32(theme::border), 1.0f);

                    float fy = sy + 10.0f;
                    pdl->AddText(ImVec2(wp.x + 12.0f, fy), subU, "Save current private server");
                    fy += 20.0f;

                    float inW = ppw - 20.0f - 62.0f;
                    ImVec2 inMin(wp.x + 10.0f, fy), inMax(wp.x + 10.0f + inW, fy + 32.0f);
                    pdl->AddRectFilled(inMin, inMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 7.0f);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, (32.0f - ImGui::GetFontSize()) * 0.5f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                    ImGui::SetCursorScreenPos(inMin);
                    ImGui::SetNextItemWidth(inW);
                    ImGui::InputTextWithHint("##ps_newname", "Name", psNameBuf, sizeof(psNameBuf));
                    bool inFocused = ImGui::IsItemActive();
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(3);
                    pdl->AddRect(inMin, inMax, ImGui::ColorConvertFloat4ToU32(inFocused
                        ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.9f)
                        : ImVec4(0.20f, 0.21f, 0.24f, 1.0f)), 7.0f, 0, 1.2f);

                    ImGui::SetCursorScreenPos(ImVec2(inMax.x + 8.0f, fy));
                    ImVec4 saveTint = theme::good;
                    if (DrawPopButton("Save", ImVec2(ppw - 20.0f - inW - 8.0f, 32.0f), false, 0, &saveTint)) {
                        if (active.linkCode.empty()) {
                            backend::Log("[!] Paste a private server link and hit Use before saving it.");
                        } else {
                            backend::PrivateServer ps = active;
                            ps.name = psNameBuf[0] ? std::string(psNameBuf) : ("Private " + std::to_string(ps.placeId));
                            backend::AddSavedPrivateServer(ps);
                            psNameBuf[0] = '\0';
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
            }
        }

        std::string placeName; bool loaded = false;
        std::vector<unsigned char> iconPng; long long infoPlaceId = 0;
        long long curPlaceId = _atoi64(placeIdBuf);
        { std::lock_guard<std::mutex> lock(backend::placeInfoMutex);
          if (backend::placeInfo.loaded && backend::placeInfo.placeId == curPlaceId && curPlaceId > 0) {
              placeName = backend::placeInfo.name; loaded = true;
              iconPng = backend::placeInfo.iconPng; infoPlaceId = backend::placeInfo.placeId; } }
        if (!loaded && curPlaceId > 0) {
            static long long lastAutoFetch = 0;
            if (lastAutoFetch != curPlaceId) {
                lastAutoFetch = curPlaceId;
                std::string cookie;
                { std::lock_guard<std::mutex> lock(backend::accountsMutex);
                  if (!backend::accounts.empty()) cookie = backend::accounts[0].cookie; }
                std::thread([curPlaceId, cookie]() { backend::FetchPlaceInfo(curPlaceId, cookie); }).detach();
            }
        }
        float bxL = cx + leftW + 6.0f, bxR = cx + cw - padX;
        float byT = cy + 50.0f, byB = cy + 156.0f;
        long long playing = -1, visits = -1, favorites = -1;
        std::string creator;
        { std::lock_guard<std::mutex> lock(backend::placeInfoMutex);
          if (loaded) { playing = backend::placeInfo.playing; visits = backend::placeInfo.visits;
                        favorites = backend::placeInfo.favorites; creator = backend::placeInfo.creator; } }

        const float thumb = 72.0f;
        ImVec2 tp(bxL, byT + 8.0f), tpMax(tp.x + thumb, tp.y + thumb);
        float infoX = tpMax.x + 12.0f;
        if (loaded) {
            ID3D11ShaderResourceView* iconTex = GetOrCreatePlaceIconTexture(infoPlaceId, iconPng);
            if (iconTex) {
                dl->AddImageRounded(iconTex, tp, tpMax, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 14.0f);
            } else {
                dl->AddRectFilled(tp, tpMax, ImGui::ColorConvertFloat4ToU32(theme::panelBg2), 14.0f);
                IconBox(ImVec2(tp.x + (thumb - 32.0f) * 0.5f, tp.y + (thumb - 32.0f) * 0.5f), 32.0f,
                    icon::FILE_TEXT, theme::panelBg2, theme::subtext, 8.0f);
            }

            float infoW = bxR - infoX;
            ImGui::PushFont(theme::fontBrand);
            dl->AddText(ImVec2(infoX, tp.y), textU, FitTextToWidth(placeName, infoW).c_str());
            ImGui::PopFont();
            if (!creator.empty())
                dl->AddText(ImVec2(infoX, tp.y + 21.0f), subU,
                    FitTextToWidth("by " + creator, infoW).c_str());

            float statY = tp.y + 42.0f;
            if (playing >= 0) {
                dl->AddCircleFilled(ImVec2(infoX + 4.0f, statY + 7.0f), 3.4f,
                    ImGui::ColorConvertFloat4ToU32(theme::good));
                dl->AddCircleFilled(ImVec2(infoX + 4.0f, statY + 7.0f), 6.5f,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(theme::good.x, theme::good.y, theme::good.z, 0.20f)));
                std::string live = CompactCount(playing) + " playing";
                dl->AddText(ImVec2(infoX + 14.0f, statY), ImGui::ColorConvertFloat4ToU32(theme::good),
                    FitTextToWidth(live, infoW - 14.0f).c_str());
                statY += 19.0f;
            }
            if (visits >= 0)
                dl->AddText(ImVec2(infoX, statY), subU,
                    FitTextToWidth(CompactCount(visits) + " visits", infoW).c_str());
            if (favorites >= 0)
                dl->AddText(ImVec2(infoX, statY + 19.0f), subU,
                    FitTextToWidth(CompactCount(favorites) + " favourites", infoW).c_str());
        } else {
            dl->AddRectFilled(tp, tpMax, ImGui::ColorConvertFloat4ToU32(
                ImVec4(0.132f, 0.138f, 0.150f, 1.0f)), 14.0f);
            ImGui::PushFont(theme::fontIcon);
            std::string eg = icon::Str(icon::BOX);
            ImVec2 egs = ImGui::CalcTextSize(eg.c_str());
            dl->AddText(ImVec2(tp.x + (thumb - egs.x) * 0.5f, tp.y + (thumb - egs.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, 0.55f)),
                eg.c_str());
            ImGui::PopFont();
            dl->AddText(ImVec2(infoX, tp.y + 22.0f), subU,
                FitTextToWidth("No place set", bxR - infoX).c_str());
            dl->AddText(ImVec2(infoX, tp.y + 41.0f),
                ImGui::ColorConvertFloat4ToU32(ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, 0.7f)),
                FitTextToWidth("Enter a Place ID", bxR - infoX).c_str());
        }
    }

    const float row2Y = row1Y + std::max(card1H, launchH) + 16.0f;
    const float accountsH = rows.empty()
        ? 320.0f
        : std::max(200.0f, 106.0f + (float)rows.size() * 40.0f);
    g_desiredClientH = row2Y + accountsH + MARGIN + 16.0f;
    {
        float cx = baseX, cy = row2Y, cw = W;
        card(cx, cy, cw, accountsH);
        IconBox(ImVec2(cx + padX, cy + 16), 26.0f, icon::SHARE2, theme::softAccentBg, theme::accent, 7.0f);
        ImGui::PushFont(theme::fontBrand);
        dl->AddText(ImVec2(cx + padX + 36, cy + 19), textU, "Accounts");
        ImGui::PopFont();

        float bx = cx + cw - padX;
        const float delW = 84, menuW = 34, chkW = 90, webW = 140, gap = 8;
        bx -= delW; ImGui::SetCursorScreenPos(ImVec2(bx, cy + 14));
        if (SecondaryIconButtonColored(icon::TRASH2, "Delete", cRed, ImVec2(delW, 32))) {
            std::vector<int> idxs(selectedAccts.begin(), selectedAccts.end());
            std::sort(idxs.rbegin(), idxs.rend());
            for (int i : idxs) backend::RemoveAccount(i);
            selectedAccts.clear();
        }
        bx -= gap + menuW;
        {
            ImGui::SetCursorScreenPos(ImVec2(bx, cy + 14));
            ImGui::PushID("acctmenu");
            ImGui::InvisibleButton("##menu", ImVec2(menuW, 32));
            bool mh = ImGui::IsItemHovered();
            ImGui::PopID();
            ImVec2 mmin(bx, cy + 14), mmax(bx + menuW, cy + 46);
            dl->AddRectFilled(mmin, mmax, ImGui::ColorConvertFloat4ToU32(mh ? theme::glassHover : theme::glassFill), 8.0f);
            dl->AddRect(mmin, mmax, borderU, 8.0f, 0, 1.3f);
            float mcx = bx + menuW * 0.5f, mcy = cy + 30;
            dl->AddCircleFilled(ImVec2(mcx, mcy - 5), 1.6f, textU);
            dl->AddCircleFilled(ImVec2(mcx, mcy), 1.6f, textU);
            dl->AddCircleFilled(ImVec2(mcx, mcy + 5), 1.6f, textU);
        }
        bx -= gap + chkW; ImGui::SetCursorScreenPos(ImVec2(bx, cy + 14));
        if (SecondaryIconButton(icon::SHIELD_CHECK, "Check", ImVec2(chkW, 32)))
            std::thread([]() { backend::RefreshSystemStatus(-1); }).detach();
        bx -= gap + webW; ImGui::SetCursorScreenPos(ImVec2(bx, cy + 14));
        if (SecondaryIconButtonColored(icon::GLOBE, "Launch Browser", cGreen, ImVec2(webW, 32))) {
            if (selectedAccts.empty()) backend::Log("[!] Select an account first, then Launch Browser.");
            else {
                std::vector<int> idxs(selectedAccts.begin(), selectedAccts.end());
                std::thread([idxs]() { for (int i : idxs) backend::OpenAccountWeb(i); }).detach();
            }
        }
        char pc[32]; snprintf(pc, sizeof(pc), "%d processes", aliveN);
        float metaX = bx - 18.0f;
        ImVec2 ps = ImGui::CalcTextSize(pc); metaX -= ps.x;
        dl->AddText(ImVec2(metaX, cy + 20), subU, pc);
        const char* cc = "0 connected";
        ImVec2 cs = ImGui::CalcTextSize(cc); metaX -= cs.x + 16;
        dl->AddText(ImVec2(metaX, cy + 20), ImGui::ColorConvertFloat4ToU32(theme::good), cc);

        float tableTop = cy + 58.0f;
        dl->AddLine(ImVec2(cx + 12, tableTop - 6), ImVec2(cx + cw - 12, tableTop - 6), borderU, 1.0f);
        float colChk     = cx + padX + 2;
        float colAccount = cx + padX + 30;
        float colConn    = cx + cw * 0.40f;
        float colMem     = cx + cw * 0.52f;
        float colCpu     = cx + cw * 0.62f;
        float colPs      = cx + cw * 0.70f;
        float colStat    = cx + cw * 0.82f;
        float colAct     = cx + cw * 0.90f;
        const float hdrY = tableTop + 3.0f;
        const float cbcY = tableTop + 10.0f;
        {
            bool allSel = !rows.empty() && (int)selectedAccts.size() == (int)rows.size();
            ImGui::SetCursorScreenPos(ImVec2(colChk, cbcY - 8.0f));
            ImGui::PushID("selall");
            ImGui::InvisibleButton("##selall", ImVec2(16, 16));
            if (ImGui::IsItemClicked()) {
                if (allSel) selectedAccts.clear();
                else for (int i = 0; i < (int)rows.size(); ++i) selectedAccts.insert(i);
            }
            ImGui::PopID();
            ImVec2 cbc(colChk + 7, cbcY);
            dl->AddRect(ImVec2(cbc.x - 7, cbc.y - 7), ImVec2(cbc.x + 7, cbc.y + 7), ImGui::ColorConvertFloat4ToU32(allSel ? theme::accent : theme::border), 4.0f, 0, 1.5f);
            if (allSel) {
                dl->AddLine(ImVec2(cbc.x - 3, cbc.y), ImVec2(cbc.x - 1, cbc.y + 3), ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);
                dl->AddLine(ImVec2(cbc.x - 1, cbc.y + 3), ImVec2(cbc.x + 4, cbc.y - 3), ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);
            }
        }
        dl->AddText(ImVec2(colAccount, hdrY), subU, "Account");
        dl->AddText(ImVec2(colConn, hdrY), subU, "Connection");
        dl->AddText(ImVec2(colMem, hdrY), subU, "Memory");
        dl->AddText(ImVec2(colCpu, hdrY), subU, "CPU");
        dl->AddText(ImVec2(colPs, hdrY), subU, "Private Server");
        dl->AddText(ImVec2(colStat, hdrY), subU, "Status");
        dl->AddText(ImVec2(colAct, hdrY), subU, "Actions");
        dl->AddLine(ImVec2(cx + 12, tableTop + 26), ImVec2(cx + cw - 12, tableTop + 26), borderU, 1.0f);

        if (rows.empty()) {
            float regionTop = tableTop + 26.0f;
            float ecy = (regionTop + (cy + accountsH)) * 0.5f;
            float centerX = cx + cw * 0.5f;
            IconBox(ImVec2(centerX - 24, ecy - 74), 48.0f, icon::USER, theme::panelBg2, theme::subtext, 12.0f);
            ImGui::PushFont(theme::fontBrand);
            const char* t1 = "No accounts imported";
            ImVec2 s1 = ImGui::CalcTextSize(t1);
            dl->AddText(ImVec2(centerX - s1.x * 0.5f, ecy - 10), textU, t1);
            ImGui::PopFont();
            const char* t2 = "Use browser login, paste cookies, or load a cookie file to start.";
            ImVec2 s2 = ImGui::CalcTextSize(t2);
            dl->AddText(ImVec2(centerX - s2.x * 0.5f, ecy + 16), subU, t2);

            float bw1 = 190, bw2 = 158, g = 12, bhh = 40;
            float bxx = centerX - (bw1 + bw2 + g) * 0.5f, byy = ecy + 48;
            ImGui::SetCursorScreenPos(ImVec2(bxx, byy));
            if (DrawPopButton("Login from Browser", ImVec2(bw1, bhh), false, icon::GLOBE, nullptr, &cGreen) && !loginInProgress.load()) {
                loginInProgress.store(true);
                std::thread([]() {
                    login::ShowRobloxLoginWindow(g_exeDir, [](bool ok, std::string cookie) {
                        if (ok) backend::AddAccountFromCookie(cookie);
                        else backend::Log("[i] Login cancelled.");
                        loginInProgress.store(false);
                    });
                }).detach();
            }
            ImGui::SetCursorScreenPos(ImVec2(bxx + bw1 + g, byy));
            if (DrawPopButton("Load from File", ImVec2(bw2, bhh), false, icon::FILE_TEXT, nullptr, &cBlue)) {
                std::wstring path = OpenCookieFileDialog(hwnd);
                if (!path.empty()) ImportCookiesFromFile(path);
            }
        } else {
            std::string rowPsCode, rowPsLabel = "None";
            {
                backend::PrivateServer aps;
                { std::lock_guard<std::mutex> lock(backend::activePrivateServerMutex); aps = backend::activePrivateServer; }
                rowPsCode = aps.linkCode;
                if (!rowPsCode.empty()) {
                    std::string nm;
                    { std::lock_guard<std::mutex> lock(backend::privateServersMutex);
                      for (auto& p : backend::savedPrivateServers) if (p.linkCode == rowPsCode) { nm = p.name; break; } }
                    rowPsLabel = nm.empty() ? "Linked" : nm;
                }
            }
            float ry = tableTop + 34;
            for (int i = 0; i < (int)rows.size(); ++i) {
                float rowH = 40.0f;
                ImVec2 rmin(cx + 12, ry), rmax(cx + cw - 12, ry + rowH - 4);
                bool sel = selectedAccts.count(i) > 0;
                const float actBW = 28.0f, actBH = 24.0f;
                const float actX = colAct + (ImGui::CalcTextSize("Actions").x - actBW) * 0.5f;
                ImVec2 actMin(actX, ry + (rowH - 4.0f - actBH) * 0.5f);
                ImVec2 actMax(actMin.x + actBW, actMin.y + actBH);
                bool overActions = ImGui::IsMouseHoveringRect(actMin, actMax);

                ImGui::SetCursorScreenPos(rmin);
                ImGui::PushID(i);
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton("##row", ImVec2(rmax.x - rmin.x, rowH - 4));
                bool hov = ImGui::IsItemHovered() && !overActions;
                if (ImGui::IsItemClicked() && !overActions) { if (sel) selectedAccts.erase(i); else selectedAccts.insert(i); }
                ImGui::PopID();
                if (sel || hov)
                    dl->AddRectFilled(rmin, rmax, ImGui::ColorConvertFloat4ToU32(sel ? theme::glassActive : theme::glassHover), 7.0f);
                ImVec2 cbc(colChk + 7, ry + rowH * 0.5f - 2);
                dl->AddRect(ImVec2(cbc.x - 7, cbc.y - 7), ImVec2(cbc.x + 7, cbc.y + 7), ImGui::ColorConvertFloat4ToU32(sel ? theme::accent : theme::border), 4.0f, 0, 1.5f);
                if (sel) {
                    dl->AddLine(ImVec2(cbc.x - 3, cbc.y), ImVec2(cbc.x - 1, cbc.y + 3), ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);
                    dl->AddLine(ImVec2(cbc.x - 1, cbc.y + 3), ImVec2(cbc.x + 4, cbc.y - 3), ImGui::ColorConvertFloat4ToU32(theme::accent), 2.0f);
                }
                ID3D11ShaderResourceView* avTex = nullptr;
                {
                    std::lock_guard<std::mutex> alock(backend::accountsMutex);
                    if (i < (int)backend::accounts.size()) {
                        RobloxAccount& a = backend::accounts[i];
                        if (!a.avatarRequested) { a.avatarRequested = true; std::thread(backend::FetchAccountAvatar, i).detach(); }
                        avTex = GetOrCreateAvatarTexture(a);
                    }
                }
                float avSz = 26.0f;
                DrawAvatarCircle(ImVec2(colAccount, ry + (rowH - 4.0f - avSz) * 0.5f), avSz, avTex, rows[i].name);
                dl->AddText(ImVec2(colAccount + avSz + 10.0f, ry + 9), textU, rows[i].name.c_str());
                dl->AddText(ImVec2(colConn, ry + 9), subU, "Idle");
                dl->AddText(ImVec2(colMem, ry + 9), subU, "-");
                dl->AddText(ImVec2(colCpu, ry + 9), subU, "-");
                dl->AddText(ImVec2(colPs, ry + 9), rowPsCode.empty() ? subU : ImGui::ColorConvertFloat4ToU32(theme::good),
                    rowPsLabel.c_str());
                dl->AddText(ImVec2(colStat, ry + 9), ImGui::ColorConvertFloat4ToU32(theme::good), "Ready");

                {
                    const float abw = actBW, abh = actBH;
                    ImVec2 amn = actMin, amx = actMax;
                    ImGui::PushID(5000 + i);
                    ImGui::SetCursorScreenPos(amn);
                    ImGui::InvisibleButton("##acts", ImVec2(abw, abh));
                    bool aHov = ImGui::IsItemHovered();
                    bool popOpen = ImGui::IsPopupOpen("rowacts");
                    if (ImGui::IsItemClicked() && !popOpen) ImGui::OpenPopup("rowacts");
                    popOpen = ImGui::IsPopupOpen("rowacts");

                    dl->AddRectFilled(amn, amx, ImGui::ColorConvertFloat4ToU32(
                        (aHov || popOpen) ? theme::glassHover : theme::glassFill), 7.0f);
                    dl->AddRect(amn, amx, ImGui::ColorConvertFloat4ToU32(
                        (aHov || popOpen) ? ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.55f)
                                          : theme::border), 7.0f, 0, 1.1f);
                    {
                        float dcx = floorf(amn.x + abw * 0.5f) + 0.5f;
                        float dcy = floorf(amn.y + abh * 0.5f) + 0.5f;
                        ImU32 du = ImGui::ColorConvertFloat4ToU32((aHov || popOpen) ? theme::text : theme::subtext);
                        ImGui::PushFont(theme::fontIcon);
                        std::string cg = icon::Str(icon::COPY);
                        ImVec2 cgs = ImGui::CalcTextSize(cg.c_str());
                        dl->AddText(ImVec2(floorf(dcx - cgs.x * 0.5f), floorf(dcy - cgs.y * 0.5f)),
                            du, cg.c_str());
                        ImGui::PopFont();
                    }

                    if (popOpen) {
                        std::string uname, pass, cookie;
                        { std::lock_guard<std::mutex> alock(backend::accountsMutex);
                          if (i < (int)backend::accounts.size()) {
                              uname = backend::accounts[i].username;
                              pass = backend::accounts[i].password;
                              cookie = backend::accounts[i].cookie; } }

                        struct CopyItem { const char* label; std::string value; bool enabled; };
                        CopyItem items[] = {
                            { "Copy cookie",    ".ROBLOSECURITY=" + cookie, !cookie.empty() },
                            { "Copy user:pass", uname + ":" + pass,         !pass.empty()   },
                            { "Copy username",  uname,                      !uname.empty()  },
                            { "Copy password",  pass,                       !pass.empty()   },
                        };
                        const int itemN = 4;
                        const float miH = 30.0f, mPad = 6.0f;
                        float mw = 168.0f;
                        for (auto& it : items)
                            mw = std::max(mw, ImGui::CalcTextSize(it.label).x + 28.0f);
                        float mh = mPad * 2.0f + itemN * miH;

                        ImGui::SetNextWindowSize(ImVec2(mw, mh));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
                        if (ImGui::BeginPopup("rowacts", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove)) {
                            ImDrawList* mdl = ImGui::GetWindowDrawList();
                            ImVec2 wp = ImGui::GetWindowPos();
                            mdl->AddRectFilled(wp, ImVec2(wp.x + mw, wp.y + mh),
                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.115f, 0.120f, 0.132f, 1.0f)), 10.0f);
                            mdl->AddRect(wp, ImVec2(wp.x + mw, wp.y + mh),
                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.225f, 0.235f, 0.255f, 1.0f)), 10.0f, 0, 1.2f);

                            for (int k = 0; k < itemN; ++k) {
                                float iy = wp.y + mPad + k * miH;
                                char iid[24]; snprintf(iid, sizeof(iid), "##copy%d", k);
                                ImGui::SetCursorScreenPos(ImVec2(wp.x + 5.0f, iy));
                                ImGui::InvisibleButton(iid, ImVec2(mw - 10.0f, miH - 2.0f));
                                bool iHov = ImGui::IsItemHovered() && items[k].enabled;
                                if (ImGui::IsItemClicked() && items[k].enabled) {
                                    ImGui::SetClipboardText(items[k].value.c_str());
                                    backend::Log(std::string("[v] ") + items[k].label + " for " + uname + ".");
                                    ImGui::CloseCurrentPopup();
                                }
                                if (iHov)
                                    mdl->AddRectFilled(ImVec2(wp.x + 5.0f, iy), ImVec2(wp.x + mw - 5.0f, iy + miH - 2.0f),
                                        ImGui::ColorConvertFloat4ToU32(ImVec4(theme::accent.x, theme::accent.y, theme::accent.z, 0.16f)), 7.0f);
                                ImVec4 col = !items[k].enabled
                                    ? ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, 0.40f)
                                    : (iHov ? theme::text : theme::subtext);
                                mdl->AddText(ImVec2(wp.x + 14.0f, iy + (miH - 2.0f - ImGui::GetFontSize()) * 0.5f),
                                    ImGui::ColorConvertFloat4ToU32(col), items[k].label);
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::PopStyleVar();
                    }
                    ImGui::PopID();
                }
                ry += rowH;
            }
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(baseX, row2Y + accountsH + 8.0f));
    ImGui::Dummy(ImVec2(1, 1));

    if (wantPasteModal) { wantPasteModal = false; ImGui::OpenPopup("PasteCookiesModal"); pasteBuf.assign(8192, 0); }
    ImGui::SetNextWindowSize(ImVec2(460, 300));
    if (ImGui::BeginPopupModal("PasteCookiesModal", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(theme::subtext, "Paste one .ROBLOSECURITY cookie per line.");
        ImGui::Spacing();
        ImGui::InputTextMultiline("##paste", pasteBuf.data(), pasteBuf.size(), ImVec2(-1, 180));
        ImGui::Spacing();
        if (PrimaryButton("Import", ImVec2(110, 34))) {
            std::string data(pasteBuf.data());
            std::thread([data]() {
                size_t start = 0; int added = 0;
                while (start <= data.size()) {
                    size_t nl = data.find('\n', start);
                    std::string line = data.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                    start = (nl == std::string::npos) ? data.size() + 1 : nl + 1;
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    const std::string pfx = ".ROBLOSECURITY=";
                    if (line.rfind(pfx, 0) == 0) line = line.substr(pfx.size());
                    if (line.size() < 40) continue;
                    if (backend::AddAccountFromCookie(line)) added++;
                }
                backend::Log("[i] Pasted cookies: added " + std::to_string(added) + ".");
            }).detach();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (SecondaryButton("Cancel", ImVec2(110, 34))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI* SetCtxFn)(DPI_AWARENESS_CONTEXT);
        auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }


    wchar_t pathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, pathBuf, MAX_PATH);
    std::wstring full(pathBuf);
    std::wstring exeDir = full.substr(0, full.find_last_of(L"\\/"));
    g_exeDir = exeDir;
    backend::Init(exeDir);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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
        100, 100, 1100, 800, nullptr, nullptr, wc.hInstance, nullptr);

    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

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

    int activePage = 0;
    int lastActivePage = 0;
    float pageAnimT = 1.0f;
    (void)lastActivePage; (void)pageAnimT;
    bool running = true;
    bool animating = true;
    double lastFrameTime = 0.0;
    bool wasDormant = false;
    idlectl::windowActive = (GetForegroundWindow() == hwnd);
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            if ((msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) ||
                (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
                msg.message == WM_NCMOUSEMOVE || msg.message == WM_NCLBUTTONDOWN) {
                idlectl::lastInteract = idlectl::NowSeconds();
                idlectl::RequestFrames(2);
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        if (IsIconic(hwnd)) {
            if (!wasDormant) { wasDormant = true; idlectl::TrimWorkingSet(); }
            WaitMessage();
            continue;
        }
        if (!idlectl::windowActive) {
            if (!wasDormant) { wasDormant = true; idlectl::TrimWorkingSet(); }
        } else {
            wasDormant = false;
        }

        backend::uiForeground.store(idlectl::windowActive);

        const double now = idlectl::NowSeconds();
        const bool interactive = (now - idlectl::lastInteract) < 0.5;

        double minInterval;
        if (!idlectl::windowActive)
            minInterval = (idlectl::forceFrames > 0) ? (1.0 / 30.0) : 0.25;
        else if (interactive || animating || idlectl::forceFrames > 0)
            minInterval = 0.0;
        else
            minInterval = 1.0 / 20.0;

        double waitFor = minInterval - (now - lastFrameTime);
        if (waitFor > 0.001) {
            MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(waitFor * 1000.0 + 0.5),
                                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        if (idlectl::windowActive) {
            double dt = (lastFrameTime > 0.0) ? (now - lastFrameTime) : 0.0;
            idlectl::animTime += (dt < 0.25 ? dt : 0.25);
        }
        lastFrameTime = now;
        if (idlectl::forceFrames > 0) idlectl::forceFrames--;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 200, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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

        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::SetCursorScreenPos(ImVec2(0, (float)TITLEBAR_H));
            ImGui::BeginChild("Content", ImVec2(0, 0), false);
            if (activePage == 1) {
                if (SecondaryIconButton(icon::CHEVRON_RIGHT, "Back to Accounts", ImVec2(200, 38)))
                    activePage = 0;
                ImGui::Dummy(ImVec2(0, 10));
                PageMultiInstance();
            } else {
                bool gotoMulti = false;
                PageAccountManager(hwnd, gotoMulti);
                if (gotoMulti) activePage = 1;
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            if (activePage != lastActivePage) {
                g_widgetAnim.clear();
                lastActivePage = activePage;
                idlectl::RequestFrames(2);
            }

            ImDrawList* rootDl = ImGui::GetWindowDrawList();
            rootDl->AddRect(ImVec2(0.5f, 0.5f), ImVec2(io2.DisplaySize.x - 0.5f, io2.DisplaySize.y - 0.5f),
                ImGui::ColorConvertFloat4ToU32(theme::border), 12.0f, 0, 1.0f);

            {
                ImDrawList* rootDl = ImGui::GetForegroundDrawList();
                const char* credit = "credits to vzqnx";
                ImVec2 cs = ImGui::CalcTextSize(credit);
                float fy = io2.DisplaySize.y - 18.0f;
                float mid = io2.DisplaySize.x * 0.5f;
                float gap = cs.x * 0.5f + 14.0f;
                ImU32 lineU = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(theme::border.x, theme::border.y, theme::border.z, 0.9f));
                rootDl->AddLine(ImVec2(30.0f, fy), ImVec2(mid - gap, fy), lineU, 1.0f);
                rootDl->AddLine(ImVec2(mid + gap, fy), ImVec2(io2.DisplaySize.x - 30.0f, fy), lineU, 1.0f);
                rootDl->AddText(ImVec2(mid - cs.x * 0.5f, fy - cs.y * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(theme::subtext.x, theme::subtext.y, theme::subtext.z, 0.75f)),
                    credit);
            }
        }
        ImGui::End();

        {
            const bool cursorInside = ImGui::IsMousePosValid(&io2.MousePos)
                && io2.MousePos.x >= 0 && io2.MousePos.y >= 0
                && io2.MousePos.x < io2.DisplaySize.x && io2.MousePos.y < io2.DisplaySize.y;
            idlectl::cursorInside = cursorInside;

            bool widgetsSettling = false;
            if (cursorInside) for (auto& kv : g_widgetAnim) {
                if (kv.second > 0.004f && kv.second < 0.996f) { widgetsSettling = true; break; }
                if (kv.second < -0.004f) { widgetsSettling = true; break; }
            }
            animating = widgetsSettling
                || io2.WantTextInput
                || ImGui::IsAnyItemActive()
                || ImGui::IsMouseDown(ImGuiMouseButton_Left)
                || ImGui::IsMouseDown(ImGuiMouseButton_Right)
                || backend::browserCookieScanning.load();
        }

        ImGui::Render();

        if (activePage == 0 && g_desiredClientH > 0.0f && !IsZoomed(hwnd) && !IsIconic(hwnd)) {
            RECT cr{};
            GetClientRect(hwnd, &cr);
            int curClientH = cr.bottom - cr.top;
            int wantClientH = (int)(g_desiredClientH + 0.5f);

            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                int workH = mi.rcWork.bottom - mi.rcWork.top;
                if (wantClientH > workH - 60) wantClientH = workH - 60;
            }
            if (wantClientH < 420) wantClientH = 420;

            if (abs(wantClientH - curClientH) > 4) {
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                int chrome = (wr.bottom - wr.top) - curClientH;
                SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wantClientH + chrome,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        const float clearColorNew[4] = { theme::bg.x, theme::bg.y, theme::bg.z, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorNew);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        {
            HRESULT hrp = g_pSwapChain->Present(minInterval > 0.0 ? 0 : 1, 0);
            g_SwapChainOccluded = (hrp == DXGI_STATUS_OCCLUDED);
        }
        continue;
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
