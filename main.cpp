/*
 * nvidia-smi-gui  —  C/C++ Win32 API 1:1 port
 * Build: g++ -std=c++17 -O3 -s -flto -static -Wall -Wextra -Werror -municode -o nvidia-smi-gui.exe main.cpp -lgdi32 -lmsimg32 -ldwmapi -ladvapi32 -mwindows
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <thread>
#include <algorithm>

#include "icons_data.h"

// ─── Theme ───────────────────────────────────────────────────────────────────
struct Theme {
    COLORREF bg, text, sub_text, title_text;
    COLORREF border, progress_bg, progress_chunk, progress_text;
};

static const Theme THEME_LIGHT = {
    RGB(0xf0,0xf5,0xf9), RGB(0x33,0x33,0x33), RGB(0x66,0x66,0x66), RGB(0x00,0x00,0x00),
    RGB(0xf0,0xf5,0xf9), RGB(0xc9,0xd6,0xdf), RGB(0x00,0x78,0xd4), RGB(0x55,0x55,0x55)
};
static const Theme THEME_DARK = {
    RGB(0x19,0x19,0x19), RGB(0xe0,0xe0,0xe0), RGB(0xa0,0xa0,0xa0), RGB(0xff,0xff,0xff),
    RGB(0x19,0x19,0x19), RGB(0x3c,0x3c,0x3c), RGB(0x00,0x78,0xd4), RGB(0xe0,0xe0,0xe0)
};

// ─── Custom messages ─────────────────────────────────────────────────────────
#define WM_SMI_UPDATE  (WM_USER + 1)

// ─── Globals ─────────────────────────────────────────────────────────────────
static Theme g_theme;
static bool  g_darkMode = false;
static HINSTANCE g_hInst;
static volatile bool g_running = true;
static HANDLE g_hProcess = NULL;
static float g_dpiScale = 1.0f;
static int D(int px) { return (int)(px * g_dpiScale); }

#define WS_FIXED (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

// ─── Icon bitmaps (created once at startup) ─────────────────────────────────
static HBITMAP g_bmpGear, g_bmpThermo, g_bmpFan, g_bmpWave, g_bmpRam, g_bmpGauge;
static HICON g_windowIcon;

static HBITMAP createPremultBitmap(const unsigned char* rgba, int w, int h) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbmp && bits) {
        unsigned char* dst = (unsigned char*)bits;
        for (int i = 0; i < w * h; ++i) {
            unsigned char r = rgba[i*4+0], g = rgba[i*4+1], b = rgba[i*4+2], a = rgba[i*4+3];
            dst[i*4+0] = (unsigned char)(b * a / 255);
            dst[i*4+1] = (unsigned char)(g * a / 255);
            dst[i*4+2] = (unsigned char)(r * a / 255);
            dst[i*4+3] = a;
        }
    }
    return hbmp;
}

static HICON createIconFromRGBA(const unsigned char* rgba, int w, int h) {
    HBITMAP hColor = createPremultBitmap(rgba, w, h);
    HBITMAP hMask = CreateBitmap(w, h, 1, 1, NULL);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask = hMask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(hColor);
    DeleteObject(hMask);
    return icon;
}

static void drawBmp(HDC hdc, HBITMAP bmp, int x, int y, int sz) {
    BITMAP bm; GetObject(bmp, sizeof(bm), &bm);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP old = (HBITMAP)SelectObject(memDC, bmp);
    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(hdc, x, y, sz, sz, memDC, 0, 0, bm.bmWidth, bm.bmHeight, bf);
    SelectObject(memDC, old);
    DeleteDC(memDC);
}

static void initIcons() {
    g_bmpGear   = createPremultBitmap(ICON_GEAR,        ICON_GEAR_SIZE,        ICON_GEAR_SIZE);
    g_bmpThermo = createPremultBitmap(ICON_THERMOMETER,  ICON_THERMOMETER_SIZE, ICON_THERMOMETER_SIZE);
    g_bmpFan    = createPremultBitmap(ICON_FAN,          ICON_FAN_SIZE,         ICON_FAN_SIZE);
    g_bmpWave   = createPremultBitmap(ICON_WAVE,         ICON_WAVE_SIZE,        ICON_WAVE_SIZE);
    g_bmpRam    = createPremultBitmap(ICON_RAM,          ICON_RAM_SIZE,         ICON_RAM_SIZE);
    g_bmpGauge  = createPremultBitmap(ICON_GAUGE,        ICON_GAUGE_SIZE,       ICON_GAUGE_SIZE);
    g_windowIcon = createIconFromRGBA(ICON_GRAPHIC_CARD, ICON_GRAPHIC_CARD_SIZE, ICON_GRAPHIC_CARD_SIZE);
}

static void cleanupIcons() {
    DeleteObject(g_bmpGear); DeleteObject(g_bmpThermo); DeleteObject(g_bmpFan);
    DeleteObject(g_bmpWave); DeleteObject(g_bmpRam); DeleteObject(g_bmpGauge);
    DestroyIcon(g_windowIcon);
}

// ─── Utility ────────────────────────────────────────────────────────────────
static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static std::wstring toW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

using SmiData = std::map<std::string, std::string>;

// ─── GPUInfoPanel ───────────────────────────────────────────────────────────
class GPUInfoPanel {
public:
    static int PANEL_HEIGHT() { return D(170); }
    static constexpr const wchar_t* CLASS_NAME = L"GPUInfoPanelClass";
    static bool s_registered;

    static void registerClass() {
        if (s_registered) return;
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = panelProc;
        wc.hInstance      = g_hInst;
        wc.lpszClassName  = CLASS_NAME;
        wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        s_registered = true;
    }

    GPUInfoPanel(HWND parent, int y, int w) {
        registerClass();
        m_hwnd = CreateWindowExW(0, CLASS_NAME, L"", WS_CHILD | WS_VISIBLE,
                                 0, y, w, PANEL_HEIGHT(), parent, NULL, g_hInst, this);

        m_fontTitle  = CreateFontW(-D(22), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                   0, 0, DEFAULT_QUALITY, 0, L"Segoe UI");
        m_fontNormal = CreateFontW(-D(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   0, 0, DEFAULT_QUALITY, 0, L"Segoe UI");
        m_fontSmall  = CreateFontW(-D(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   0, 0, DEFAULT_QUALITY, 0, L"Segoe UI");
        m_fontTiny   = CreateFontW(-D(10), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   0, 0, DEFAULT_QUALITY, 0, L"Segoe UI");
    }

    ~GPUInfoPanel() {
        DeleteObject(m_fontTitle); DeleteObject(m_fontNormal);
        DeleteObject(m_fontSmall); DeleteObject(m_fontTiny);
        if (m_hwnd) DestroyWindow(m_hwnd);
    }

    HWND hwnd() const { return m_hwnd; }
    void reposition(int y, int w) { MoveWindow(m_hwnd, 0, y, w, PANEL_HEIGHT(), TRUE); }

    void updateInfo(const SmiData& d) {
        auto get = [&](const char* k, const char* def = "N/A") -> std::string {
            auto it = d.find(k); return (it != d.end()) ? it->second : def;
        };

        m_gpuModel  = toW(get("name", "Unknown GPU"));
        m_gpuId     = L"#" + toW(get("index", "?"));
        m_pciBusId  = L"pci: " + toW(get("pci.bus_id", "N/A"));
        m_util      = toW(get("utilization.gpu", "0")) + L"%";
        m_clock     = toW(get("clocks.current.graphics", "0")) + L"MHz";
        m_memUsed   = toW(get("memory.used", "0")) + L"M";
        m_memTotal  = toW(get("memory.total", "0")) + L"M";

        std::string temp = get("temperature.gpu", "N/A");
        m_temp = (temp != "N/A") ? toW(temp) + L"\u2103" : L"N/A";

        std::string fan = get("fan.speed", "N/A");
        m_fan = (fan.find("N/A") != std::string::npos || fan.find("Not Supported") != std::string::npos)
                ? L"N/A" : toW(fan) + L"%";

        m_powerDraw  = toW(get("power.draw", "0")) + L"W";
        m_powerLimit = toW(get("enforced.power.limit", "0")) + L"W";

        double mu = 0, mt = 1;
        try { mu = std::stod(get("memory.used", "0")); } catch (...) {}
        try { mt = std::stod(get("memory.total", "1")); } catch (...) {}
        m_memPct = (mt > 0) ? (int)(mu * 100.0 / mt) : 0;

        double pd = 0, pl = 1;
        try { pd = std::stod(get("power.draw", "0")); } catch (...) {}
        try { pl = std::stod(get("enforced.power.limit", "1")); } catch (...) {}
        m_powerPct = (pl > 0) ? (int)(pd * 100.0 / pl) : 0;

        InvalidateRect(m_hwnd, NULL, FALSE);
    }

private:
    HWND m_hwnd = NULL;
    HFONT m_fontTitle, m_fontNormal, m_fontSmall, m_fontTiny;

    std::wstring m_gpuModel = L"Graphics Device", m_gpuId = L"#0", m_pciBusId = L"bus: 00:00.0";
    std::wstring m_temp = L"N/A", m_fan = L"N/A", m_util = L"N/A", m_clock = L"N/A";
    std::wstring m_memUsed = L"N/A", m_memTotal = L"N/A";
    std::wstring m_powerDraw = L"N/A", m_powerLimit = L"N/A";
    int m_memPct = 0, m_powerPct = 0;

    void drawProgressBar(HDC hdc, int x, int y, int w, int h, int pct) {
        HRGN clip = CreateRoundRectRgn(x, y, x + w, y + h, D(16), D(16));
        SelectClipRgn(hdc, clip);

        RECT rcBg = {x, y, x + w, y + h};
        HBRUSH bgBr = CreateSolidBrush(g_theme.progress_bg);
        FillRect(hdc, &rcBg, bgBr); DeleteObject(bgBr);

        if (pct > 0) {
            int cw = w * std::min(pct, 100) / 100;
            RECT rcChunk = {x, y, x + cw, y + h};
            HBRUSH chBr = CreateSolidBrush(g_theme.progress_chunk);
            FillRect(hdc, &rcChunk, chBr); DeleteObject(chBr);
        }
        SelectClipRgn(hdc, NULL); DeleteObject(clip);

        HPEN pen = CreatePen(PS_SOLID, 1, g_theme.border);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, x, y, x + w, y + h, D(16), D(16));
        SelectObject(hdc, oldPen); DeleteObject(pen);

        wchar_t buf[16]; wsprintfW(buf, L"%d%%", pct);
        RECT rcText = {x, y, x + w, y + h};
        SelectObject(hdc, m_fontNormal);
        SetTextColor(hdc, g_theme.progress_text);
        DrawTextW(hdc, buf, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void onPaint() {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hwnd, &ps);
        RECT rc; GetClientRect(m_hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        SelectObject(mem, bmp);

        // Background
        HBRUSH bgBr = CreateSolidBrush(g_theme.bg);
        FillRect(mem, &rc, bgBr); DeleteObject(bgBr);
        SetBkMode(mem, TRANSPARENT);

        int xPad = D(10);
        int iconSz = D(24);

        // ── Row 1: GPU model name ──
        SelectObject(mem, m_fontTitle);
        SetTextColor(mem, g_theme.title_text);
        RECT r1 = {xPad, D(5), W - xPad, D(33)};
        DrawTextW(mem, m_gpuModel.c_str(), -1, &r1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        // ── Row 2: GPU ID + PCI bus ──
        SelectObject(mem, m_fontSmall);
        SetTextColor(mem, g_theme.sub_text);
        RECT r2a = {xPad, D(35), xPad + D(30), D(49)};
        DrawTextW(mem, m_gpuId.c_str(), -1, &r2a, DT_LEFT | DT_SINGLELINE);
        RECT r2b = {xPad + D(32), D(35), W - xPad, D(49)};
        DrawTextW(mem, m_pciBusId.c_str(), -1, &r2b, DT_LEFT | DT_SINGLELINE);

        // ── Row 3: Stats with icons ──
        int statsY = D(55);
        int usableW = W - 2 * xPad;
        struct StatItem { HBITMAP icon; const std::wstring* val; };
        StatItem stats[] = {
            {g_bmpGear,   &m_util},
            {g_bmpThermo, &m_temp},
            {g_bmpFan,    &m_fan},
            {g_bmpWave,   &m_clock},
        };

        SelectObject(mem, m_fontNormal);
        SetTextColor(mem, g_theme.text);
        for (int i = 0; i < 4; ++i) {
            int sx = xPad + i * usableW / 4;
            drawBmp(mem, stats[i].icon, sx, statsY, iconSz);
            RECT rs = {sx + iconSz + D(4), statsY, sx + usableW / 4, statsY + iconSz};
            DrawTextW(mem, stats[i].val->c_str(), -1, &rs, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // ── Row 4: Memory bar ──
        int barRowY1 = D(88);
        int xIcon = xPad;
        int xVal  = xPad + iconSz + D(6);
        int wVal  = D(60);
        int xBar  = xVal + wVal + D(8);
        int wBar  = W - xBar - xPad;
        int barH  = D(20);
        int rowH  = D(38);

        drawBmp(mem, g_bmpRam, xIcon, barRowY1 + (rowH - iconSz) / 2, iconSz);

        SelectObject(mem, m_fontTiny);
        SetTextColor(mem, g_theme.text);
        RECT rmU = {xVal, barRowY1, xVal + wVal, barRowY1 + D(14)};
        DrawTextW(mem, m_memUsed.c_str(), -1, &rmU, DT_CENTER | DT_SINGLELINE);
        HPEN sepPen = CreatePen(PS_SOLID, 1, g_theme.sub_text);
        HPEN oldP = (HPEN)SelectObject(mem, sepPen);
        MoveToEx(mem, xVal, barRowY1 + D(15), NULL);
        LineTo(mem, xVal + wVal, barRowY1 + D(15));
        SelectObject(mem, oldP); DeleteObject(sepPen);
        RECT rmT = {xVal, barRowY1 + D(17), xVal + wVal, barRowY1 + D(31)};
        DrawTextW(mem, m_memTotal.c_str(), -1, &rmT, DT_CENTER | DT_SINGLELINE);
        drawProgressBar(mem, xBar, barRowY1 + (rowH - barH) / 2, wBar, barH, m_memPct);

        // ── Row 5: Power bar ──
        int barRowY2 = D(130);

        drawBmp(mem, g_bmpGauge, xIcon, barRowY2 + (rowH - iconSz) / 2, iconSz);

        SelectObject(mem, m_fontTiny);
        SetTextColor(mem, g_theme.text);
        RECT rpD = {xVal, barRowY2, xVal + wVal, barRowY2 + D(14)};
        DrawTextW(mem, m_powerDraw.c_str(), -1, &rpD, DT_CENTER | DT_SINGLELINE);
        sepPen = CreatePen(PS_SOLID, 1, g_theme.sub_text);
        oldP = (HPEN)SelectObject(mem, sepPen);
        MoveToEx(mem, xVal, barRowY2 + D(15), NULL);
        LineTo(mem, xVal + wVal, barRowY2 + D(15));
        SelectObject(mem, oldP); DeleteObject(sepPen);
        RECT rpL = {xVal, barRowY2 + D(17), xVal + wVal, barRowY2 + D(31)};
        DrawTextW(mem, m_powerLimit.c_str(), -1, &rpL, DT_CENTER | DT_SINGLELINE);
        drawProgressBar(mem, xBar, barRowY2 + (rowH - barH) / 2, wBar, barH, m_powerPct);

        // Bottom border line
        HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
        oldP = (HPEN)SelectObject(mem, borderPen);
        MoveToEx(mem, 0, H - 1, NULL);
        LineTo(mem, W, H - 1);
        SelectObject(mem, oldP); DeleteObject(borderPen);

        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(m_hwnd, &ps);
    }

    static LRESULT CALLBACK panelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        GPUInfoPanel* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<GPUInfoPanel*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<GPUInfoPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        switch (msg) {
        case WM_PAINT:    if (self) self->onPaint(); return 0;
        case WM_ERASEBKGND: return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
bool GPUInfoPanel::s_registered = false;

// ─── MainWindow ─────────────────────────────────────────────────────────────
class MainWindow {
public:
    static constexpr const wchar_t* CLASS_NAME = L"NvSmiGuiMainClass";

    static void registerClass() {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = wndProc;
        wc.hInstance      = g_hInst;
        wc.lpszClassName  = CLASS_NAME;
        wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground  = CreateSolidBrush(g_theme.bg);
        RegisterClassW(&wc);
    }

    MainWindow(const std::wstring& title) {
        registerClass();
        m_hwnd = CreateWindowExW(0, CLASS_NAME, title.c_str(),
                                 WS_FIXED, CW_USEDEFAULT, CW_USEDEFAULT,
                                 D(500), D(100), NULL, NULL, g_hInst, this);
        if (g_windowIcon) {
            SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_windowIcon);
            SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_windowIcon);
        }
        // Dark title bar (Windows 10 1809+ / Windows 11)
        if (g_darkMode) {
            BOOL useDark = TRUE;
            // DWMWA_USE_IMMERSIVE_DARK_MODE = 20
            DwmSetWindowAttribute(m_hwnd, 20, &useDark, sizeof(useDark));
        }
    }

    ~MainWindow() { for (auto* p : m_panels) delete p; }
    HWND hwnd() const { return m_hwnd; }
    void show() { ShowWindow(m_hwnd, SW_SHOW); UpdateWindow(m_hwnd); }
    int panelCount() const { return (int)m_panels.size(); }
    GPUInfoPanel* panel(int i) { return m_panels[i]; }

    GPUInfoPanel* addNewPanel() {
        RECT rc; GetClientRect(m_hwnd, &rc);
        int y = (int)m_panels.size() * GPUInfoPanel::PANEL_HEIGHT();
        auto* p = new GPUInfoPanel(m_hwnd, y, rc.right);
        m_panels.push_back(p);

        int totalH = (int)m_panels.size() * GPUInfoPanel::PANEL_HEIGHT();
        RECT adj = {0, 0, D(480), totalH};
        AdjustWindowRectEx(&adj, WS_FIXED, FALSE, 0);
        int newW = adj.right - adj.left, newH = adj.bottom - adj.top;
        if (m_panels.size() == 1) {
            HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(hMon, &mi);
            int cx = (mi.rcWork.left + mi.rcWork.right - newW) / 2;
            int cy = (mi.rcWork.top + mi.rcWork.bottom - newH) / 2;
            SetWindowPos(m_hwnd, NULL, cx, cy, newW, newH, SWP_NOZORDER);
        } else {
            SetWindowPos(m_hwnd, NULL, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER);
        }
        return p;
    }

private:
    HWND m_hwnd = NULL;
    std::vector<GPUInfoPanel*> m_panels;

    void repositionPanels() {
        RECT rc; GetClientRect(m_hwnd, &rc);
        for (int i = 0; i < (int)m_panels.size(); ++i)
            m_panels[i]->reposition(i * GPUInfoPanel::PANEL_HEIGHT(), rc.right);
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        MainWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        switch (msg) {
        case WM_SIZE: if (self) self->repositionPanels(); return 0;
        case WM_GETMINMAXINFO: {
            auto* m = reinterpret_cast<MINMAXINFO*>(lp);
            m->ptMinTrackSize.x = D(480); m->ptMinTrackSize.y = D(100); return 0;
        }
        case WM_SMI_UPDATE: {
            if (!self) break;
            int idx = (int)wp;
            SmiData* data = reinterpret_cast<SmiData*>(lp);
            if (idx >= self->panelCount()) self->addNewPanel();
            if (idx < self->panelCount()) self->panel(idx)->updateInfo(*data);
            delete data; return 0;
        }
        case WM_CLOSE: g_running = false; DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

// ─── SmiReader thread ───────────────────────────────────────────────────────
static void smiReaderThread(const std::vector<std::string>& fields, HWND hwnd, HANDLE hPipe) {
    char buffer[4096]; std::string lineBuf; DWORD bytesRead;
    while (g_running) {
        if (!ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) || bytesRead == 0) break;
        buffer[bytesRead] = '\0';
        lineBuf += buffer;
        size_t pos;
        while ((pos = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, pos);
            lineBuf = lineBuf.substr(pos + 1);
            line = trim(line);
            if (line.empty()) continue;
            std::vector<std::string> values; std::stringstream ss(line); std::string tok;
            while (std::getline(ss, tok, ',')) values.push_back(trim(tok));
            auto* data = new SmiData();
            for (size_t i = 0; i < fields.size() && i < values.size(); ++i)
                (*data)[fields[i]] = values[i];
            int idx = -1;
            try { idx = std::stoi((*data)["index"]); } catch (...) { delete data; continue; }
            PostMessage(hwnd, WM_SMI_UPDATE, (WPARAM)idx, (LPARAM)data);
        }
    }
}

// ─── Process creation ───────────────────────────────────────────────────────
static HANDLE startProcess(const std::wstring& cmdLine, HANDLE* hStdoutRead) {
    SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead, hWrite;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite; si.hStdError = hWrite; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = cmdLine;
    BOOL ok = CreateProcessW(NULL, &cmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) { CloseHandle(hRead); *hStdoutRead = NULL; return NULL; }
    CloseHandle(pi.hThread); *hStdoutRead = hRead; return pi.hProcess;
}

// ─── Command line parsing ───────────────────────────────────────────────────
static bool isSystemDarkMode() {
    HKEY hKey; DWORD val = 1, size = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hKey);
    }
    return val == 0; // 0 = dark, 1 = light
}

// theme: 0=auto, 1=force dark, 2=force light
struct AppArgs { std::string host, user, sshArgs; int port = 22; int theme = 0; };

static AppArgs parseArgs(int argc, wchar_t** argv) {
    AppArgs a;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto nextVal = [&]() -> std::string {
            if (i + 1 < argc) {
                std::wstring v = argv[++i];
                int n = WideCharToMultiByte(CP_UTF8, 0, v.c_str(), -1, NULL, 0, NULL, NULL);
                std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, v.c_str(), -1, &s[0], n, NULL, NULL);
                s.resize(n - 1); return s;
            } return "";
        };
        if (arg == L"-H" || arg == L"--host") a.host = nextVal();
        else if (arg == L"-p" || arg == L"--port") { auto v = nextVal(); a.port = v.empty() ? 22 : std::stoi(v); }
        else if (arg == L"-u" || arg == L"--user") a.user = nextVal();
        else if (arg == L"--ssh-args") a.sshArgs = nextVal();
        else if (arg == L"--dark") a.theme = 1;
        else if (arg == L"--light") a.theme = 2;
    }
    return a;
}

// ─── Entry point ────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;
    SetProcessDPIAware();
    HDC hScr = GetDC(NULL);
    g_dpiScale = GetDeviceCaps(hScr, LOGPIXELSY) / 96.0f;
    ReleaseDC(NULL, hScr);
    int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    AppArgs args = parseArgs(argc, argv); LocalFree(argv);
    g_darkMode = (args.theme == 1) ? true : (args.theme == 2) ? false : isSystemDarkMode();
    g_theme = g_darkMode ? THEME_DARK : THEME_LIGHT;

    initIcons();

    std::vector<std::string> fields = {
        "index","count","pci.bus_id","name","uuid","memory.used","memory.total",
        "temperature.gpu","power.draw","enforced.power.limit","clocks.current.graphics",
        "fan.speed","utilization.gpu"
    };
    std::string qf; for (size_t i = 0; i < fields.size(); ++i) { if (i) qf += ","; qf += fields[i]; }

    wchar_t hostBuf[256] = {}; DWORD hostSz = 256;
    GetComputerNameW(hostBuf, &hostSz);
    std::wstring hostname = hostBuf;

    std::wstring cmdLine;
    std::string sshHostname, sshUsername = args.user;
    if (!args.host.empty()) {
        if (args.host.find('@') != std::string::npos && sshUsername.empty()) {
            auto at = args.host.rfind('@');
            sshUsername = args.host.substr(0, at); sshHostname = args.host.substr(at + 1);
        } else sshHostname = args.host;
        hostname = toW(sshHostname);
        std::string cmd = "ssh -p " + std::to_string(args.port) + " -o BatchMode=yes -o ConnectTimeout=10";
        if (!args.sshArgs.empty()) cmd += " " + args.sshArgs;
        cmd += " " + (sshUsername.empty() ? sshHostname : sshUsername + "@" + sshHostname);
        cmd += " nvidia-smi --query-gpu=" + qf + " --format=csv,noheader,nounits -lms 300";
        cmdLine = toW(cmd);
    } else {
        cmdLine = toW("nvidia-smi --query-gpu=" + qf + " --format=csv,noheader,nounits -lms 300");
    }

    HANDLE hStdoutRead = NULL;
    g_hProcess = startProcess(cmdLine, &hStdoutRead);
    if (!g_hProcess) {
        MessageBoxW(NULL, L"Failed to start nvidia-smi.\nMake sure nvidia-smi is in PATH.",
                     L"Error", MB_OK | MB_ICONERROR);
        cleanupIcons(); return 1;
    }

    MainWindow mw(L"GPU Status on " + hostname);
    mw.show();
    std::thread reader(smiReaderThread, fields, mw.hwnd(), hStdoutRead);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    g_running = false;
    TerminateProcess(g_hProcess, 0); CloseHandle(g_hProcess); CloseHandle(hStdoutRead);
    if (reader.joinable()) reader.join();
    cleanupIcons();
    return 0;
}
