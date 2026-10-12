// =====================================================================
//  AudioSight
//  Copyright (c) 2026 Peneaux Benjamin
//
//  A overlay that shows, on screen, from which side the
//  sounds of a game are coming (left / right / center).
//
//  - Captures the system audio output with WASAPI loopback
//  - Draws everything with GDI+ in click-through "layered" windows
//
//  Hotkeys (global, they work while the game has focus):
//    F7  : open / close the settings panel
//    F8  : move mode ON/OFF (drag the elements with the mouse)
//    F9  : volume bars more sensitive      F10 : less sensitive
//    F11 : shot detection more sensitive   F12 : less sensitive
//
//  Build (MinGW-w64):
//    g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++
//        -o AudioSight.exe AudioSight.cpp
//        -lgdiplus -lgdi32 -luser32 -lole32 -lshell32 -luuid -lwinhttp
//  Build (Visual Studio, "x64 Native Tools Command Prompt"):
//    cl /O2 /EHsc /MT /DUNICODE /D_UNICODE AudioSight.cpp
//       /link /SUBSYSTEM:WINDOWS gdiplus.lib gdi32.lib user32.lib
//       ole32.lib shell32.lib winhttp.lib
//
//  License: MIT (see LICENSE)
// =====================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <shellapi.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")   // Visual Studio: link WinHTTP automatically
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

using namespace Gdiplus;

// ============================ DEFAULT SETTINGS ============================
static const float  kDefaultBarsDb       = -70.0f;  // volume bars: level shown as "empty"
static const float  kBarsMaxDb           = -15.0f;  // volume bars: level shown as "full"
static const float  kDefaultShotDb       = -30.0f;  // minimum level of a "shot"
static const float  kDefaultSuddenness   = 12.0f;   // dB a sound must rise above the background
static const float  kDefaultCenterZone   = 3.0f;    // L/R difference (dB) below which a shot is "center"
static const float  kDefaultDuration     = 1.0f;    // indicator display time (s)
static const double kShotCooldown        = 0.15;    // min seconds between two detections
static const float  kFlashGlow           = 34.0f;   // size of the shot flash glow around the bar (logical px)
static const double kShotPeakWindow      = 0.08;    // s after a detection used to measure the shot's peak
static const float  kFlashFullDb         = -15.0f;  // shot level (dB) that gives a full-intensity flash
static const float  kDefaultFlashMin     = 0.20f;   // flash intensity of the quietest detectable shot (0..1)
static const int    kSegments            = 20;      // segments per side in the volume bar
static const float  kRestOpacity         = 0.30f;   // bar opacity when nothing happens
static const float  kEdgeDepth           = 130.f;   // depth of the screen-edge gradient (px at 1080p)
static const float  kEdgeLength          = 440.f;   // length of the gradient along the edge (px at 1080p)
static const float  kRectThick           = 36.f;    // plain rectangles (gradient off): thickness
static const float  kRectLen             = 220.f;   // plain rectangles: length
static const float  kRectPad             = 6.f;     // plain top rectangle: distance from the screen top
static const double kGraphSeconds        = 5.0;     // time span shown by the sound graph (s)
static const double kGraphGap            = 0.08;    // s without audio data after which the graph falls back to zero

static const int kVkPanel = VK_F7, kVkMove = VK_F8, kVkBarsUp = VK_F9,
                 kVkBarsDown = VK_F10, kVkShotsUp = VK_F11, kVkShotsDown = VK_F12;
// ==========================================================================

// ------------------------------------------------------------------ helpers
static double Now() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
static float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ----------------------------------------------------------------- settings
// Adjustable at runtime (settings panel / hotkeys), saved in the .ini file.
static std::atomic<float> gBarsDb(kDefaultBarsDb), gShotDb(kDefaultShotDb),
                          gSuddenness(kDefaultSuddenness), gCenterZone(kDefaultCenterZone);
static float gDuration = kDefaultDuration;
static bool gDebug = false;
static bool gFlashOn = true;                 // shot flash enabled (default: on)
static bool gEdgeOn = true;                  // screen-edge gradient (default: on, off = old rectangles)
static std::atomic<bool> gQuickOn(false);    // quick phrases window (default: off, needs the Discord bot)
static std::atomic<bool> gChatOn(false);     // Discord chat window (default: off, needs a bot, see [discord])
static bool gGraphOn = true;                 // sound graph enabled (default: on)
static float gFlashMin = kDefaultFlashMin;   // flash intensity of the quietest shot
static std::wstring gIniPath;
static float gChatW = 420.f, gChatH = 240.f;   // Discord chat window size (logical px, ini: chat_width / chat_height)
static float gQuickW = 300.f;                  // quick phrases window width (logical px, ini: quick_width)
static float gQuickRest = 0.50f;               // quick phrases window opacity at rest (ini: quick_rest_opacity)
static float gChatRest = 0.55f;                // chat window opacity when nothing new (ini: chat_rest_opacity)

static float IniFloat(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(sec, key, L"", buf, 64, gIniPath.c_str());
    return buf[0] ? (float)_wtof(buf) : def;
}
static int IniInt(const wchar_t* sec, const wchar_t* key, int def, bool* found = nullptr) {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(sec, key, L"", buf, 64, gIniPath.c_str());
    if (found) *found = buf[0] != 0;
    return buf[0] ? _wtoi(buf) : def;
}
static void IniWriteFloat(const wchar_t* sec, const wchar_t* key, float v) {
    wchar_t buf[64];
    _snwprintf(buf, 64, L"%.1f", v);
    WritePrivateProfileStringW(sec, key, buf, gIniPath.c_str());
}
static void IniWriteInt(const wchar_t* sec, const wchar_t* key, int v) {
    wchar_t buf[64];
    _snwprintf(buf, 64, L"%d", v);
    WritePrivateProfileStringW(sec, key, buf, gIniPath.c_str());
}
static void IniWriteFloat2(const wchar_t* sec, const wchar_t* key, float v) {
    wchar_t buf[64];
    _snwprintf(buf, 64, L"%.2f", v);
    WritePrivateProfileStringW(sec, key, buf, gIniPath.c_str());
}
static void SaveSettings() {
    IniWriteInt(L"settings", L"flash_enabled", gFlashOn ? 1 : 0);
    IniWriteInt(L"settings", L"graph_enabled", gGraphOn ? 1 : 0);
    IniWriteInt(L"settings", L"edge_gradient_enabled", gEdgeOn ? 1 : 0);
    IniWriteInt(L"settings", L"chat_enabled", gChatOn ? 1 : 0);
    IniWriteInt(L"settings", L"quick_enabled", gQuickOn ? 1 : 0);
    IniWriteInt(L"settings", L"quick_width", (int)gQuickW);
    IniWriteFloat2(L"settings", L"quick_rest_opacity", gQuickRest);
    IniWriteInt(L"settings", L"chat_width", (int)gChatW);
    IniWriteInt(L"settings", L"chat_height", (int)gChatH);
    IniWriteFloat2(L"settings", L"chat_rest_opacity", gChatRest);
    IniWriteFloat2(L"settings", L"flash_min_intensity", gFlashMin);
    IniWriteFloat(L"settings", L"bars_threshold_db", gBarsDb);
    IniWriteFloat(L"settings", L"shots_threshold_db", gShotDb);
    IniWriteFloat(L"settings", L"suddenness_db", gSuddenness);
    IniWriteFloat(L"settings", L"center_zone_db", gCenterZone);
    IniWriteFloat(L"settings", L"indicator_duration_s", gDuration);
}

// ------------------------------------------------------------ colors (theme)
// EVERY color of the interface is defined in this single list:
//   X(field, "ini key", default 0xAARRGGBB, "description")
#define COLOR_LIST(X) \
	X(leftColor,          L"left_color",            0xFFA019E1, "Left side accent: left volume bars, letter L, left end of the marker, tray icon") \
    X(rightColor,         L"right_color",           0xFF28E119, "Right side accent: right volume bars, letter R, right end of the marker, tray icon") \
    X(barBackground,      L"bar_background",        0xAA0C0E14, "Background of the bar container") \
    X(barBorder,          L"bar_border",            0x28FFFFFF, "Outline of the bar container") \
    X(barLeftOff,         L"bar_left_unlit",        0x23A019E1, "Left volume segments that are not lit") \
    X(barRightOff,        L"bar_right_unlit",       0x2328E119, "Right volume segments that are not lit") \
    X(barTrack,           L"direction_track",       0x32FFFFFF, "Line the direction marker slides on") \
    X(barTrackCenter,     L"direction_track_center",0x5AFFFFFF, "Center tick of the direction line") \
    X(markerBorder,       L"marker_border",         0xDCFFFFFF, "Outline of the direction marker") \
    X(barMessage,         L"bar_message_text",      0xE6FFFFFF, "Short messages shown in the bar (sensitivity changes...)") \
    X(gear,               L"gear_icon",             0xFFC8CEDA, "Settings gear icon") \
    X(gearHover,          L"gear_icon_hover",       0xFFFFFFFF, "Gear icon when hovered or when the panel is open") \
    X(gearGlow,           L"gear_hover_glow",       0x28FFFFFF, "Soft circle behind the gear when hovered") \
    X(gearSeparator,      L"gear_separator",        0x28FFFFFF, "Thin line between the bars and the gear") \
    X(moveMode,           L"move_mode_accent",      0xFFFFDC00, "Dashed outline and text shown in move mode") \
    X(indicator,          L"indicator_fill",        0xEB28E119, "Plain screen-edge rectangles (only used when the edge gradient is switched off)") \
    X(indicatorBorder,    L"indicator_border",      0x78FFFFFF, "Outline of the screen-edge indicators") \
    X(indicatorLabel,     L"indicator_move_label",  0xFFFFFFFF, "Label (L / R / TOP) shown on the indicators in move mode") \
    X(shotFlash,          L"shot_flash",            0xB428E119, "Glow flashed around the bar container when a shot is detected (AA = intensity, 00 disables it)") \
    X(toggleOn,           L"toggle_on",             0xFF34C759, "Switch color when ON (shot flash switch)") \
    X(toggleOff,          L"toggle_off",            0xFF4A4E64, "Switch color when OFF") \
    X(toggleThumb,        L"toggle_thumb",          0xFFFFFFFF, "Switch handle") \
    X(panelBackground,    L"panel_background",      0xF00E1018, "Settings panel background") \
    X(panelBorder,        L"panel_border",          0x3CFFFFFF, "Settings panel outline") \
    X(panelTitle,         L"panel_title",           0xFFFFFFFF, "Settings panel title") \
    X(panelText,          L"panel_text",            0xF0FFFFFF, "Slider labels and live readouts") \
    X(panelHint,          L"panel_hint",            0xFF969EAF, "Small help texts under the sliders and legend") \
    X(panelDivider,       L"panel_divider",         0x28FFFFFF, "Line under the panel title") \
    X(closeIcon,          L"close_icon",            0xFFDCE1EB, "Close (x) icon of the panel") \
    X(closeHover,         L"close_hover",           0xFF502A34, "Close button background when hovered") \
    X(sliderTrack,        L"slider_track",          0xFF34384C, "Empty part of the sliders") \
    X(sliderThumb,        L"slider_thumb",          0xFFFFFFFF, "Slider handle") \
    X(accentBars,         L"slider_accent_bars",    0xFFA019E1, "Filled part and value of the volume bars slider") \
    X(accentShots,        L"slider_accent_shots",   0xFF28E119, "Filled part and value of the shot detection sliders") \
    X(accentOther,        L"slider_accent_other",   0xFFAA96FF, "Filled part and value of the center zone and duration sliders") \
    X(meterBackground,    L"meter_background",      0xFF282C3E, "Background of the live level meter") \
    X(meterFill,          L"meter_fill",            0xFFA019E1, "Level meter fill (below the shot threshold)") \
    X(meterFillHot,       L"meter_fill_hot",        0xFF28E119, "Level meter fill (above the shot threshold)") \
    X(tickBars,           L"tick_bars_threshold",   0xFFA019E1, "Meter mark: volume bars threshold") \
    X(tickShots,          L"tick_shots_threshold",  0xFF28E119, "Meter mark: shot detection threshold") \
    X(tickLastShot,       L"tick_last_shot",        0xFFFFFFFF, "Meter mark: level of the last detected shot") \
    X(button,             L"button",                0xFF282C3E, "Panel buttons") \
    X(buttonHover,        L"button_hover",          0xFF404662, "Panel buttons when hovered") \
    X(buttonBorder,       L"button_border",         0x46FFFFFF, "Outline of the panel buttons") \
    X(buttonText,         L"button_text",           0xFFFFFFFF, "Text of the panel buttons") \
    X(buttonActive,       L"button_move_active",    0xFF5A4E14, "Move button while move mode is on") \
    X(buttonQuit,         L"button_quit",           0xFF54222A, "Quit button") \
    X(buttonQuitHover,    L"button_quit_hover",     0xFF822A32, "Quit button when hovered") \
    X(trayIconBackground, L"tray_icon_background",  0xFF0C0E14, "Background of the notification area icon") \
    X(graphBackground,    L"graph_background",      0xAA0C0E14, "Background of the sound graph container") \
    X(graphBorder,        L"graph_border",          0x28FFFFFF, "Outline of the sound graph container") \
    X(graphLine,          L"graph_line",            0xFF28E119, "Curve of the sound graph (sound signature of the last 5 seconds)") \
    X(graphFill,          L"graph_fill",            0x4628E119, "Area under the curve of the sound graph (AA 00 = line only)") \
    X(graphBaseline,      L"graph_baseline",        0x6428E119, "Zero line at the bottom of the sound graph") \
    X(graphGrid,          L"graph_grid",            0x14FFFFFF, "Vertical lines of the sound graph (one per second)") \
    X(graphShotLine,      L"graph_shot_threshold",  0x50FFDC00, "Dashed line showing the shot detection threshold on the sound graph (AA 00 hides it)") \
    X(edgeGradient,       L"edge_gradient",         0xCC28E119, "Gradient shown on the left / right / top screen edge when a shot is detected (AA = opacity of the loudest shots)") \
    X(chatBackground,     L"chat_background",       0xB00C0E14, "Background of the Discord chat window (AA = how see-through it is)") \
    X(chatBorder,         L"chat_border",           0x28FFFFFF, "Outline of the Discord chat window") \
    X(chatText,           L"chat_text",             0xFF28E119, "Text of the Discord chat messages") \
    X(chatAuthor,         L"chat_author",           0xFFBE5CF5, "Names of the people who write in the Discord chat") \
    X(chatHint,           L"chat_hint",             0xFF969EAF, "Help and status texts of the Discord chat window") \
    X(chatError,          L"chat_error",            0xFFFF7B7B, "Connection error messages of the Discord chat window") \
    X(quickBackground,    L"quick_background",      0xB00C0E14, "Background of the quick phrases window (AA = how see-through it is)") \
    X(quickBorder,        L"quick_border",          0x28FFFFFF, "Outline of the quick phrases window") \
    X(quickText,          L"quick_text",            0xFF28E119, "The phrases of the quick phrases window") \
    X(quickKey,           L"quick_key",             0xFFBE5CF5, "The key shown in front of each phrase") \
    X(quickHint,          L"quick_hint",            0xFF969EAF, "Title and help text of the quick phrases window") \
    X(quickError,         L"quick_error",           0xFFFF7B7B, "Error shown when a phrase could not be sent") \
    X(quickSent,          L"quick_sent",            0x6428E119, "Highlight of the phrase that has just been sent")

struct Theme {
#define X(field, key, def, desc) uint32_t field = def;
    COLOR_LIST(X)
#undef X
};
static Theme gTheme;

struct ColorEntry {
    const wchar_t* key;
    uint32_t Theme::*member;
    uint32_t def;
    const char* desc;
};
static const ColorEntry kColorEntries[] = {
#define X(field, key, def, desc) { key, &Theme::field, def, desc },
    COLOR_LIST(X)
#undef X
};
static const int kColorCount = (int)(sizeof(kColorEntries) / sizeof(kColorEntries[0]));

// 0xAARRGGBB -> GDI+ color
static Color Col(uint32_t c) { return Color((BYTE)(c >> 24), (BYTE)(c >> 16), (BYTE)(c >> 8), (BYTE)c); }
static Color ColA(uint32_t c, int alpha) { return Color((BYTE)alpha, (BYTE)(c >> 16), (BYTE)(c >> 8), (BYTE)c); }
static Color Brighten(uint32_t c, float k) {
    return Color((BYTE)(c >> 24), (BYTE)min(255.f, ((c >> 16) & 255) * k),
                 (BYTE)min(255.f, ((c >> 8) & 255) * k), (BYTE)min(255.f, (c & 255) * k));
}

// "#RRGGBB" (keeps the default opacity) or "#AARRGGBB"
static bool ParseColor(const wchar_t* s, uint32_t def, uint32_t* out) {
    while (*s == L' ' || *s == L'\t') s++;
    if (*s == L'#') s++;
    size_t n = 0;
    while (iswxdigit((wint_t)s[n])) n++;
    if (n != 6 && n != 8) return false;
    wchar_t buf[9];
    for (size_t i = 0; i < n; i++) buf[i] = s[i];
    buf[n] = 0;
    uint32_t v = (uint32_t)wcstoul(buf, nullptr, 16);
    *out = (n == 6) ? ((def & 0xFF000000u) | v) : v;
    return true;
}
static void FormatColor(uint32_t v, uint32_t def, wchar_t* out, size_t n) {
    if ((v >> 24) == 0xFF && (def >> 24) == 0xFF) _snwprintf(out, n, L"#%06X", (unsigned)(v & 0xFFFFFF));
    else _snwprintf(out, n, L"#%08X", (unsigned)v);
}

// Makes sure the documented [colors] section exists in the .ini file, and adds
// the keys that are missing (for example colors introduced by a newer version).
static void EnsureColorSection() {
    bool missing[256] = {};
    int nMissing = 0;
    for (int i = 0; i < kColorCount && i < 256; i++) {
        wchar_t buf[64] = L"";
        GetPrivateProfileStringW(L"colors", kColorEntries[i].key, L"", buf, 64, gIniPath.c_str());
        if (!buf[0]) { missing[i] = true; nMissing++; }
    }
    if (nMissing == 0) return;
    if (nMissing < kColorCount) {   // the section exists: only add the missing keys
        for (int i = 0; i < kColorCount && i < 256; i++) {
            if (!missing[i]) continue;
            wchar_t v[16];
            FormatColor(kColorEntries[i].def, kColorEntries[i].def, v, 16);
            WritePrivateProfileStringW(L"colors", kColorEntries[i].key, v, gIniPath.c_str());
        }
        return;
    }
    FILE* f = _wfopen(gIniPath.c_str(), L"ab");
    if (!f) return;
    fputs("\r\n[colors]\r\n"
          "; Interface colors. Format: #RRGGBB or #AARRGGBB (AA = opacity: FF opaque, 00 invisible).\r\n"
          "; If you omit AA, the default opacity of that color is kept.\r\n"
          "; Save this file and the colors update live (the program can stay running).\r\n"
          "; Delete this whole section to restore all default colors.\r\n\r\n", f);
    for (int i = 0; i < kColorCount; i++) {
        wchar_t v[16];
        FormatColor(kColorEntries[i].def, kColorEntries[i].def, v, 16);
        fprintf(f, "; %s\r\n%ls=%ls\r\n\r\n", kColorEntries[i].desc, kColorEntries[i].key, v);
    }
    fclose(f);
}

static void LoadTheme() {
    Theme t;   // starts with the default colors
    for (int i = 0; i < kColorCount; i++) {
        const ColorEntry& e = kColorEntries[i];
        wchar_t buf[64] = L"";
        GetPrivateProfileStringW(L"colors", e.key, L"", buf, 64, gIniPath.c_str());
        uint32_t v;
        if (buf[0] && ParseColor(buf, e.def, &v)) t.*(e.member) = v;   // invalid values are ignored
    }
    gTheme = t;
}

// Detects that the .ini file was saved (to apply color changes live).
static FILETIME gIniTime = {};
static bool IniChanged() {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(gIniPath.c_str(), GetFileExInfoStandard, &d)) return false;
    if (CompareFileTime(&d.ftLastWriteTime, &gIniTime) == 0) return false;
    gIniTime = d.ftLastWriteTime;
    return true;
}

// ------------------------------------------------------- shared audio state
// Written by the audio thread, read by the UI thread.
struct AudioState {
    std::atomic<float>  dbL{-100.f}, dbR{-100.f};   // level of the last analysis block
    std::atomic<double> lastDataTime{0.0};          // when audio data last arrived
    std::atomic<double> shotTime[3];                // 0 = left, 1 = right, 2 = top (center)
    std::atomic<int>    shotRev{0};                 // incremented whenever shotL / shotR change
    std::atomic<float>  shotL{-100.f}, shotR{-100.f};
    std::atomic<int>    shotCount{0};
    std::atomic<float>  shotLevel[3];               // loudness (dB) of the last shot per side (edge gradient intensity)
};
static AudioState gAudio;
static std::atomic<bool> gRun(true);

// History of the loudness (one value per analysis block, ~21 ms) used by the
// sound graph. Ring buffer written by the audio thread, read by the UI thread.
static const unsigned kHistCap = 1024;               // > 5 s of blocks, with plenty of margin
static std::atomic<double>   gHistT[kHistCap];       // time of the block
static std::atomic<float>    gHistDb[kHistCap];      // level (dB) of the block
static std::atomic<unsigned> gHistHead(0);           // number of blocks written so far

// Shot detector: a shot is a sound that is loud enough AND rises abruptly
// above the slowly-tracked background level.
struct Detector {
    double background = -80.0;
    double lastShot = 0.0;
    double peakEnd = 0.0;    // end of the peak measurement window of the last shot
    int    lastSide = 2;     // side of the last shot
    float  peakLevel = -100.f;
};

static void FinishBlock(Detector& d, double sumL, double sumR, int n) {
    double eL = sumL / n, eR = sumR / n;
    float l = eL > 1e-12 ? (float)(10.0 * log10(eL)) : -100.f;
    float r = eR > 1e-12 ? (float)(10.0 * log10(eR)) : -100.f;
    gAudio.dbL = l;
    gAudio.dbR = r;

    const float suddenness = gSuddenness, centerZone = gCenterZone, shotThreshold = gShotDb;
    float level = max(l, r);
    double t = Now();

    {   // record the level for the sound graph
        const unsigned h = gHistHead.load(std::memory_order_relaxed);
        gHistT[h % kHistCap] = t;
        gHistDb[h % kHistCap] = level;
        gHistHead.store(h + 1, std::memory_order_release);
    }

    if (level > shotThreshold && level > d.background + suddenness && t - d.lastShot > kShotCooldown) {
        float diff = r - l;
        int side = diff < -centerZone ? 0 : (diff > centerZone ? 1 : 2);
        gAudio.shotTime[side] = t;
        gAudio.shotLevel[side] = level;
        d.lastSide = side;
        gAudio.shotL = l;
        gAudio.shotR = r;
        gAudio.shotCount++;
        gAudio.shotRev++;
        d.lastShot = t;
        d.peakLevel = level;
        d.peakEnd = t + kShotPeakWindow;
    } else if (t < d.peakEnd && level > d.peakLevel) {
        // still in the attack of the shot: keep the loudest block for the direction
        d.peakLevel = level;
        gAudio.shotLevel[d.lastSide] = level;
        gAudio.shotL = l;
        gAudio.shotR = r;
        gAudio.shotRev++;
    }
    // the background rises slowly and falls quickly
    double coef = level > d.background ? 0.03 : 0.10;
    d.background += (level - d.background) * coef;
}

template <class T> struct Com {   // minimal COM smart pointer
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010,
                                   {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static float ReadSample(const BYTE* p, int bits, bool isFloat) {
    if (isFloat) { float f; memcpy(&f, p, 4); return f; }
    if (bits == 16) { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
    if (bits == 24) { int32_t v = (int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)); return v / 2147483648.0f; }
    int32_t v; memcpy(&v, p, 4); return v / 2147483648.0f;
}

// Which side does a speaker position belong to?
// Returns a bit mask: 1 = left, 2 = right, 3 = both (center), 0 = ignored (LFE).
static int ChannelSide(DWORD speakerBit) {
    switch (speakerBit) {
        case 0x1: case 0x10: case 0x40: case 0x200: case 0x1000: case 0x8000: return 1;   // FL BL FLC SL TFL TBL
        case 0x2: case 0x20: case 0x80: case 0x400: case 0x4000: case 0x20000: return 2;  // FR BR FRC SR TFR TBR
        case 0x4: case 0x100: case 0x800: case 0x2000: case 0x10000: return 3;            // FC BC TC TFC TBC
        default: return 0;                                                                // LFE, others
    }
}

// One capture session on the default output device.
// Returns on error, when the default device changes, or when stopping.
static void CaptureSession() {
    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) return;
    Com<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return;

    std::wstring currentId;
    { LPWSTR id = nullptr; if (SUCCEEDED(device->GetId(&id)) && id) { currentId = id; CoTaskMemFree(id); } }

    Com<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) return;
    WAVEFORMATEX* wf = nullptr;
    if (FAILED(client->GetMixFormat(&wf)) || !wf) return;

    const int channels = wf->nChannels, bits = wf->wBitsPerSample, frameBytes = wf->nBlockAlign;
    const int blockFrames = max(256, (int)(wf->nSamplesPerSec / 47));  // ~21 ms analysis blocks
    bool isFloat = false;
    DWORD mask = 0;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)wf;
        mask = x->dwChannelMask;
        isFloat = memcmp(&x->SubFormat, &kSubtypeFloat, sizeof(GUID)) == 0;
    } else {
        isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    }

    // Assign every channel to the left / right side
    std::vector<int> side(channels, 0);
    int c = 0, maskBits = 0;
    for (int b = 0; b < 32; b++) if (mask & (1u << b)) maskBits++;
    if (mask && maskBits == channels) {
        for (int b = 0; b < 32 && c < channels; b++)
            if (mask & (1u << b)) side[c++] = ChannelSide(1u << b);
    } else if (channels == 1) {
        side[0] = 3;
    } else if (channels == 2) {
        side[0] = 1; side[1] = 2;
    } else if (channels == 4) {
        side[0] = 1; side[1] = 2; side[2] = 1; side[3] = 2;
    } else {  // standard Windows order: FL FR FC LFE BL BR SL SR
        static const int std8[8] = {1, 2, 3, 0, 1, 2, 1, 2};
        for (int i = 0; i < channels && i < 8; i++) side[i] = std8[i];
    }

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    2000000 /* 200 ms */, 0, wf, nullptr);
    CoTaskMemFree(wf);
    if (FAILED(hr)) return;
    Com<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture))) return;
    if (FAILED(client->Start())) return;

    Detector det;
    double sumL = 0, sumR = 0;
    int n = 0;
    double lastDeviceCheck = Now();
    const int sampleBytes = bits / 8;

    while (gRun) {
        Sleep(4);
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) { client->Stop(); return; }
            if (packet == 0) break;
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) { client->Stop(); return; }
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 f = 0; f < frames; f++) {
                if (!silent) {
                    const BYTE* frame = data + (size_t)f * frameBytes;
                    for (int k = 0; k < channels; k++) {
                        int sd = side[k];
                        if (!sd) continue;
                        float s = ReadSample(frame + k * sampleBytes, bits, isFloat);
                        double e = (double)s * s;
                        if (sd & 1) sumL += e;
                        if (sd & 2) sumR += e;
                    }
                }
                if (++n >= blockFrames) {
                    FinishBlock(det, sumL, sumR, n);
                    sumL = sumR = 0; n = 0;
                }
            }
            capture->ReleaseBuffer(frames);
            gAudio.lastDataTime = Now();
        }
        double t = Now();
        if (t - gAudio.lastDataTime > 0.1) det.background = -80.0;   // silence: reset the background

        if (t - lastDeviceCheck > 1.0) {   // did the default output device change?
            lastDeviceCheck = t;
            Com<IMMDevice> d2;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &d2))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(d2->GetId(&id)) && id) {
                    bool changed = currentId != id;
                    CoTaskMemFree(id);
                    if (changed) { client->Stop(); return; }
                }
            }
        }
    }
    client->Stop();
}

static DWORD WINAPI AudioThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (gRun) {
        CaptureSession();
        if (gRun) Sleep(800);   // retry (device changed / unavailable)
    }
    CoUninitialize();
    return 0;
}

// ------------------------------------------------------------ Discord chat
// Optional. A Discord BOT reads the last messages of ONE text channel through the
// official REST API (plain HTTPS polling with WinHTTP) and the overlay shows them.
// Nothing is injected into the game. The bot token and the channel id are read from
// the [discord] section of AudioSight.ini (explained there, applied live).
struct ChatMsg { unsigned long long id = 0; std::wstring author, text; };
static CRITICAL_SECTION gChatCs;
static std::vector<ChatMsg> gChatMsgs;           // oldest first
static std::wstring gChatStatus;                 // empty = everything is fine
static bool gChatStatusErr = false;              // true: error (red), false: help text
static std::atomic<int> gChatRev(0);             // incremented whenever messages / status change
static std::atomic<double> gChatNewTime(-999.0); // when a new message last arrived
static const int kChatFetch = 12;                // messages requested at each poll
static const int kChatPollMs = 2500;             // time between two polls

// Quick phrases (see QuickSend): phrases waiting to be posted by the chat thread
static std::vector<std::wstring> gOutbox;        // protected by gChatCs
static HANDLE gChatWake = nullptr;               // wakes the chat thread when a phrase is queued
static std::wstring gQuickErr;                   // last "not sent" reason (protected by gChatCs)
static std::atomic<double> gQuickErrTime(-999.0);
static std::atomic<int> gQuickRev(0);            // incremented whenever the error changes

// ---- minimal JSON reader (only what the Discord answers need)
struct JV {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } k = Null;
    double n = 0;
    bool b = false;
    std::wstring s;
    std::vector<JV> a;                    // array items
    std::vector<std::wstring> keys;       // object keys ...
    std::vector<JV> vals;                 // ... and values
    const JV* get(const wchar_t* key) const {
        if (k != Obj) return nullptr;
        for (size_t j = 0; j < keys.size(); j++) if (keys[j] == key) return &vals[j];
        return nullptr;
    }
};
static void JSkip(const std::wstring& t, size_t& i) {
    while (i < t.size() && (t[i] == L' ' || t[i] == L'\n' || t[i] == L'\r' || t[i] == L'\t')) i++;
}
static bool JString(const std::wstring& t, size_t& i, std::wstring& out) {
    if (i >= t.size() || t[i] != L'"') return false;
    i++;
    while (i < t.size()) {
        wchar_t c = t[i++];
        if (c == L'"') return true;
        if (c != L'\\') { out.push_back(c); continue; }
        if (i >= t.size()) return false;
        wchar_t e = t[i++];
        switch (e) {
            case L'n': out.push_back(L'\n'); break;
            case L't': out.push_back(L'\t'); break;
            case L'r': out.push_back(L'\r'); break;
            case L'b': out.push_back(L'\b'); break;
            case L'f': out.push_back(L'\f'); break;
            case L'u': {   // \uXXXX is UTF-16: surrogate pairs are kept as they are
                if (i + 4 > t.size()) return false;
                unsigned v = 0;
                for (int q = 0; q < 4; q++) {
                    wchar_t h = t[i + q];
                    v <<= 4;
                    if (h >= L'0' && h <= L'9') v |= (unsigned)(h - L'0');
                    else if (h >= L'a' && h <= L'f') v |= (unsigned)(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') v |= (unsigned)(h - L'A' + 10);
                    else return false;
                }
                i += 4;
                out.push_back((wchar_t)v);
                break;
            }
            default: out.push_back(e); break;   // \" \\ \/
        }
    }
    return false;
}
static bool JValue(const std::wstring& t, size_t& i, JV& v, int depth) {
    if (depth > 24) return false;
    JSkip(t, i);
    if (i >= t.size()) return false;
    const wchar_t c = t[i];
    if (c == L'{') {
        v.k = JV::Obj;
        i++;
        JSkip(t, i);
        if (i < t.size() && t[i] == L'}') { i++; return true; }
        for (;;) {
            JSkip(t, i);
            std::wstring key;
            if (!JString(t, i, key)) return false;
            JSkip(t, i);
            if (i >= t.size() || t[i] != L':') return false;
            i++;
            JV val;
            if (!JValue(t, i, val, depth + 1)) return false;
            v.keys.push_back(key);
            v.vals.push_back(std::move(val));
            JSkip(t, i);
            if (i >= t.size()) return false;
            if (t[i] == L',') { i++; continue; }
            if (t[i] == L'}') { i++; return true; }
            return false;
        }
    }
    if (c == L'[') {
        v.k = JV::Arr;
        i++;
        JSkip(t, i);
        if (i < t.size() && t[i] == L']') { i++; return true; }
        for (;;) {
            JV item;
            if (!JValue(t, i, item, depth + 1)) return false;
            v.a.push_back(std::move(item));
            JSkip(t, i);
            if (i >= t.size()) return false;
            if (t[i] == L',') { i++; continue; }
            if (t[i] == L']') { i++; return true; }
            return false;
        }
    }
    if (c == L'"') { v.k = JV::Str; return JString(t, i, v.s); }
    if (t.compare(i, 4, L"true") == 0) { v.k = JV::Bool; v.b = true; i += 4; return true; }
    if (t.compare(i, 5, L"false") == 0) { v.k = JV::Bool; v.b = false; i += 5; return true; }
    if (t.compare(i, 4, L"null") == 0) { v.k = JV::Null; i += 4; return true; }
    wchar_t* end = nullptr;
    v.n = wcstod(t.c_str() + i, &end);
    if (end == t.c_str() + i) return false;
    v.k = JV::Num;
    i = (size_t)(end - t.c_str());
    return true;
}

static void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::wstring::npos) { s.replace(p, from.size(), to); p += to.size(); }
}
static std::wstring ChatDisplayName(const JV& u) {
    const JV* g = u.get(L"global_name");
    if (g && g->k == JV::Str && !g->s.empty()) return g->s;
    const JV* n = u.get(L"username");
    if (n && n->k == JV::Str && !n->s.empty()) return n->s;
    return L"?";
}
// <@123> -> @name, <:emoji:123> -> :emoji:, <#123> -> #channel, <@&123> -> @role ...
static std::wstring ChatCleanText(std::wstring s, const JV* mentions) {
    if (mentions && mentions->k == JV::Arr) {
        for (const JV& u : mentions->a) {
            const JV* id = u.get(L"id");
            if (!id || id->k != JV::Str || id->s.empty()) continue;
            const std::wstring name = L"@" + ChatDisplayName(u);
            ReplaceAll(s, L"<@!" + id->s + L">", name);
            ReplaceAll(s, L"<@" + id->s + L">", name);
        }
    }
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'<') {
            const size_t e = s.find(L'>', i);
            if (e != std::wstring::npos && e - i <= 80 && e > i + 1) {
                const std::wstring in = s.substr(i + 1, e - i - 1);
                const bool emoji = in[0] == L':' || (in.size() > 1 && in[0] == L'a' && in[1] == L':');
                if (emoji) {
                    const size_t c1 = in.find(L':');
                    const size_t c2 = in.find(L':', c1 + 1);
                    if (c2 != std::wstring::npos) { out += L":" + in.substr(c1 + 1, c2 - c1 - 1) + L":"; i = e + 1; continue; }
                } else if (in[0] == L'@') {
                    out += (in.size() > 1 && in[1] == L'&') ? L"@role" : L"@user"; i = e + 1; continue;
                } else if (in[0] == L'#') {
                    out += L"#channel"; i = e + 1; continue;
                } else if (in.compare(0, 2, L"t:") == 0) {
                    out += L"[time]"; i = e + 1; continue;
                }
            }
        }
        out.push_back(s[i++]);
    }
    for (wchar_t& c : out) if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
    if (out.size() > 500) { out.resize(499); out.push_back((wchar_t)0x2026); }
    return out;
}
// One message of the Discord answer -> ChatMsg (false = nothing to show)
static bool ChatParseMsg(const JV& m, ChatMsg& out) {
    const JV* id = m.get(L"id");
    if (!id || id->k != JV::Str) return false;
    out.id = wcstoull(id->s.c_str(), nullptr, 10);
    const JV* au = m.get(L"author");
    out.author = au ? ChatDisplayName(*au) : L"?";
    if (out.author.size() > 24) { out.author.resize(23); out.author.push_back((wchar_t)0x2026); }
    const JV* mentions = m.get(L"mentions");
    std::wstring text;
    const JV* content = m.get(L"content");
    if (content && content->k == JV::Str) text = ChatCleanText(content->s, mentions);
    const JV* embeds = m.get(L"embeds");   // many bots (transcription...) write in embeds
    if (embeds && embeds->k == JV::Arr) {
        for (const JV& e : embeds->a) {
            const JV* d = e.get(L"description");
            const JV* ti = e.get(L"title");
            std::wstring part;
            if (d && d->k == JV::Str && !d->s.empty()) part = ChatCleanText(d->s, mentions);
            else if (ti && ti->k == JV::Str) part = ChatCleanText(ti->s, mentions);
            if (!part.empty()) text += (text.empty() ? L"" : L" ") + part;
        }
    }
    const JV* att = m.get(L"attachments");
    if (att && att->k == JV::Arr && !att->a.empty()) text += std::wstring(text.empty() ? L"" : L" ") + L"[attachment]";
    return !text.empty() && (out.text = text, true);
}
// ---- end of the JSON / message helpers

static void ChatSet(const wchar_t* status, bool isErr, bool clearMsgs) {
    EnterCriticalSection(&gChatCs);
    bool changed = gChatStatus != status || gChatStatusErr != isErr;
    if (clearMsgs && !gChatMsgs.empty()) { gChatMsgs.clear(); changed = true; }
    gChatStatus = status;
    gChatStatusErr = isErr;
    LeaveCriticalSection(&gChatCs);
    if (changed) gChatRev++;
}
static std::wstring ChatTrim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'"' || s.back() == L'\r')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'"')) b++;
    return s.substr(b);
}
static std::wstring ChatIniStr(const wchar_t* key) {
    wchar_t buf[512] = L"";
    GetPrivateProfileStringW(L"discord", key, L"", buf, 512, gIniPath.c_str());
    return ChatTrim(buf);
}
static void ChatSleep(int ms) {   // interruptible: a queued quick phrase wakes the thread up
    for (int t = 0; t < ms && gRun; t += 100) {
        if (gChatWake) { if (WaitForSingleObject(gChatWake, 100) == WAIT_OBJECT_0) break; }
        else Sleep(100);
    }
}

static void QuickFail(const wchar_t* msg) {
    EnterCriticalSection(&gChatCs);
    gQuickErr = msg;
    LeaveCriticalSection(&gChatCs);
    gQuickErrTime = Now();
    gQuickRev++;
}
static void QuickOk() {
    EnterCriticalSection(&gChatCs);
    const bool had = !gQuickErr.empty();
    gQuickErr.clear();
    LeaveCriticalSection(&gChatCs);
    if (had) { gQuickErrTime = -999.0; gQuickRev++; }
}
// nothing can be sent: forget the waiting phrases and tell why
static void QuickDropPending(const wchar_t* msg) {
    EnterCriticalSection(&gChatCs);
    const bool had = !gOutbox.empty();
    gOutbox.clear();
    LeaveCriticalSection(&gChatCs);
    if (had) QuickFail(msg);
}

// {"content": "...", "allowed_mentions": {"parse": []}} as UTF-8 (no @everyone / @user pings)
static std::string ChatJsonBody(const std::wstring& text) {
    std::wstring esc;
    for (wchar_t c : text) {
        switch (c) {
            case L'"':  esc += L"\\\""; break;
            case L'\\': esc += L"\\\\"; break;
            case L'\n': esc += L"\\n"; break;
            case L'\r': esc += L"\\r"; break;
            case L'\t': esc += L"\\t"; break;
            default:
                if (c < 0x20) { wchar_t b[8]; _snwprintf(b, 8, L"\\u%04x", (unsigned)c); esc += b; }
                else esc.push_back(c);
        }
    }
    const std::wstring w = L"{\"content\":\"" + esc + L"\",\"allowed_mentions\":{\"parse\":[]}}";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// One HTTPS request to Discord (GET, or POST when a JSON payload is given).
static bool ChatHttp(HINTERNET con, const wchar_t* verb, const std::wstring& path, const std::wstring& token,
                     const std::string* payload, DWORD* status, std::string* body) {
    HINTERNET req = WinHttpOpenRequest(con, verb, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) return false;
    std::wstring hdr = L"Authorization: Bot " + token + L"\r\n";
    if (payload) hdr += L"Content-Type: application/json\r\n";
    const DWORD plen = payload ? (DWORD)payload->size() : 0;
    bool ok = WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1L,
                                 payload ? (LPVOID)payload->data() : WINHTTP_NO_REQUEST_DATA, plen, plen, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD sz = sizeof(DWORD);
        *status = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, status, &sz, WINHTTP_NO_HEADER_INDEX);
        body->clear();
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            const size_t old = body->size();
            if (old + avail > (1u << 20)) break;
            body->resize(old + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &(*body)[old], avail, &got)) { ok = false; break; }
            body->resize(old + got);
        }
    }
    WinHttpCloseHandle(req);
    return ok;
}

// The chat thread has two jobs: post the quick phrases (when they are on) and read the
// last messages of the channel (when the chat window is on).
static DWORD WINAPI ChatThread(LPVOID) {
    HINTERNET ses = nullptr, con = nullptr;
    std::wstring lastKey;
    unsigned long long newest = 0;
    bool first = true;
    while (gRun) {
        const bool wantChat = gChatOn, wantQuick = gQuickOn;
        if (!wantChat && !wantQuick) { ChatSleep(300); continue; }

        std::wstring token = ChatIniStr(L"bot_token");
        const std::wstring chan = ChatIniStr(L"channel_id");
        if (token.size() > 4 && _wcsnicmp(token.c_str(), L"Bot ", 4) == 0) token = ChatTrim(token.substr(4));
        if (token.empty() || chan.empty()) {
            if (wantChat) ChatSet(L"Discord chat is not set up yet. Open AudioSight.ini, fill bot_token and channel_id "
                                  L"in the [discord] section and save the file: it connects by itself.", false, true);
            QuickDropPending(L"Not sent: Discord is not set up");
            lastKey.clear();
            ChatSleep(1000);
            continue;
        }
        bool valid = true;
        for (wchar_t c : token) if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                                     c == L'.' || c == L'-' || c == L'_')) valid = false;
        for (wchar_t c : chan) if (c < L'0' || c > L'9') valid = false;
        if (!valid) {
            if (wantChat) ChatSet(L"Discord: bot_token or channel_id looks wrong. Check the [discord] section of AudioSight.ini.", true, true);
            QuickDropPending(L"Not sent: check [discord] in the .ini");
            lastKey.clear();
            ChatSleep(1000);
            continue;
        }
        const std::wstring key = token + L"|" + chan;
        if (key != lastKey) {   // new settings: start from scratch
            lastKey = key;
            newest = 0;
            first = true;
            ChatSet(L"", false, true);
        }

        if (!ses) {
            ses = WinHttpOpen(L"DiscordBot (https://discord.com, 1.0) AudioSight", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (ses) {
                WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);
                con = WinHttpConnect(ses, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            }
            if (!ses || !con) {
                if (con) WinHttpCloseHandle(con);
                if (ses) WinHttpCloseHandle(ses);
                ses = con = nullptr;
                if (wantChat) ChatSet(L"Discord: cannot open a connection, retrying...", true, false);
                QuickDropPending(L"Not sent: no connection");
                ChatSleep(5000);
                continue;
            }
        }

        int wait = kChatPollMs;

        // 1. quick phrases waiting to be posted
        bool lost = false;
        for (;;) {
            std::wstring text;
            EnterCriticalSection(&gChatCs);
            const bool has = !gOutbox.empty();
            if (has) { text = gOutbox.front(); gOutbox.erase(gOutbox.begin()); }
            LeaveCriticalSection(&gChatCs);
            if (!has) break;
            const std::string payload = ChatJsonBody(text);
            DWORD st = 0;
            std::string rb;
            if (!ChatHttp(con, L"POST", L"/api/v10/channels/" + chan + L"/messages", token, &payload, &st, &rb)) {
                lost = true;
                QuickFail(L"Not sent: no connection");
                break;
            }
            if (st >= 200 && st < 300) { QuickOk(); wait = 300; }   // read the channel again right away
            else if (st == 429) QuickFail(L"Not sent: Discord is busy, retry");
            else if (st == 401) QuickFail(L"Not sent: invalid bot token (401)");
            else if (st == 403) QuickFail(L"Not sent: the bot may not write here (403)");
            else if (st == 404) QuickFail(L"Not sent: channel not found (404)");
            else { wchar_t m[64]; _snwprintf(m, 64, L"Not sent: Discord error %lu", (unsigned long)st); QuickFail(m); }
        }
        if (lost) {
            WinHttpCloseHandle(con);
            WinHttpCloseHandle(ses);
            ses = con = nullptr;
            ChatSleep(2000);
            continue;
        }
        if (!wantChat) { ChatSleep(1000); continue; }   // quick phrases only: nothing to read

        // 2. read the last messages
        wchar_t path[160];
        _snwprintf(path, 160, L"/api/v10/channels/%ls/messages?limit=%d", chan.c_str(), kChatFetch);
        DWORD status = 0;
        std::string body;
        if (!ChatHttp(con, L"GET", path, token, nullptr, &status, &body)) {
            WinHttpCloseHandle(con);
            WinHttpCloseHandle(ses);
            ses = con = nullptr;
            ChatSet(L"Discord: no connection, retrying...", true, false);
            ChatSleep(4000);
            continue;
        }

        std::wstring wbody;
        if (!body.empty()) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
            if (n > 0) { wbody.resize((size_t)n); MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), &wbody[0], n); }
        }
        if (status == 200) {
            JV root;
            size_t pos = 0;
            if (!JValue(wbody, pos, root, 0) || root.k != JV::Arr) {
                ChatSet(L"Discord: unexpected answer, retrying...", true, false);
            } else {
                std::vector<ChatMsg> list;
                for (const JV& m : root.a) { ChatMsg cm; if (ChatParseMsg(m, cm)) list.push_back(cm); }
                std::sort(list.begin(), list.end(), [](const ChatMsg& x, const ChatMsg& y) { return x.id < y.id; });
                bool added = false;
                unsigned long long maxId = newest;
                for (const ChatMsg& cm : list) if (cm.id > newest) { added = true; if (cm.id > maxId) maxId = cm.id; }

                EnterCriticalSection(&gChatCs);
                bool changed = list.size() != gChatMsgs.size();
                for (size_t q = 0; !changed && q < list.size(); q++)
                    changed = list[q].id != gChatMsgs[q].id || list[q].text != gChatMsgs[q].text || list[q].author != gChatMsgs[q].author;
                if (changed) gChatMsgs = list;
                if (!gChatStatus.empty()) { gChatStatus.clear(); gChatStatusErr = false; changed = true; }
                LeaveCriticalSection(&gChatCs);
                if (changed) gChatRev++;
                if (!first && added) gChatNewTime = Now();
                newest = maxId;
                first = false;
            }
        } else if (status == 429) {   // too many requests: Discord says how long to wait
            JV root;
            size_t pos = 0;
            double retry = 2.0;
            if (JValue(wbody, pos, root, 0)) { const JV* r = root.get(L"retry_after"); if (r && r->k == JV::Num) retry = r->n; }
            wait = (int)(Clampf((float)retry, 0.5f, 30.f) * 1000.f) + 200;
        } else if (status == 401) {
            ChatSet(L"Discord: the bot token is not valid (401). Check bot_token in AudioSight.ini.", true, true);
            wait = 5000;
        } else if (status == 403) {
            ChatSet(L"Discord: the bot is not allowed to read this channel (403). Give it \"View Channel\" and "
                    L"\"Read Message History\" there.", true, true);
            wait = 5000;
        } else if (status == 404) {
            ChatSet(L"Discord: channel not found (404). Check channel_id and that the bot is on that server.", true, true);
            wait = 5000;
        } else {
            wchar_t msg[96];
            _snwprintf(msg, 96, L"Discord: error %lu, retrying...", (unsigned long)status);
            ChatSet(msg, true, false);
            wait = 5000;
        }
        ChatSleep(wait);
    }
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return 0;
}

// ---- reading AudioSight.ini as text (UTF-8, UTF-8 with BOM, UTF-16 or ANSI), so that accents
// typed in the quick phrases work whatever the editor used to save the file.
static std::wstring IniReadAllText() {
    std::wstring out;
    HANDLE h = CreateFileW(gIniPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::string raw;
    const DWORD sz = GetFileSize(h, nullptr);
    if (sz != INVALID_FILE_SIZE && sz < (4u << 20)) {
        raw.resize(sz);
        DWORD got = 0;
        if (!ReadFile(h, sz ? &raw[0] : nullptr, sz, &got, nullptr)) got = 0;
        raw.resize(got);
    }
    CloseHandle(h);
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {   // UTF-16 LE
        out.assign((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
        return out;
    }
    size_t off = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) off = 3;
    const char* p = raw.data() + off;
    const int len = (int)(raw.size() - off);
    if (len <= 0) return out;
    UINT cp = CP_UTF8;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(CP_ACP, 0, p, len, nullptr, 0); }   // not UTF-8: ANSI
    if (n <= 0) return out;
    out.resize((size_t)n);
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, p, len, &out[0], n);
    return out;
}
static std::wstring IniTrimW(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == (wchar_t)0xFEFF)) b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r')) e--;
    return s.substr(b, e - b);
}
// value of "key=" in [section] (names are case-insensitive); false if the key does not exist
static bool IniTextGet(const std::wstring& txt, const wchar_t* sec, const wchar_t* key, std::wstring* out) {
    bool inSec = false;
    size_t i = 0;
    while (i <= txt.size()) {
        size_t e = txt.find(L'\n', i);
        if (e == std::wstring::npos) e = txt.size();
        const std::wstring line = IniTrimW(txt.substr(i, e - i));
        i = e + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            const size_t c = line.find(L']');
            inSec = c != std::wstring::npos && _wcsicmp(line.substr(1, c - 1).c_str(), sec) == 0;
            continue;
        }
        if (!inSec) continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (_wcsicmp(IniTrimW(line.substr(0, eq)).c_str(), key) != 0) continue;
        std::wstring v = IniTrimW(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
        *out = v;
        return true;
    }
    return false;
}

// ---- quick phrases: configuration ([quick] section of the .ini)
struct QuickSlot {
    std::wstring text;    // the phrase (empty = slot not used)
    std::wstring label;   // what is shown in front of it ("5", "F5", "Ctrl+1"...)
    UINT vk = 0, mods = 0;
    bool badKey = false;
};
static QuickSlot gQuick[10];
static std::wstring gPlayerName;                 // written before each phrase ("Name: phrase")
static int gQuickFlashIdx = -1;                  // phrase that was just sent (highlighted)
static double gQuickFlashTime = -999.0, gQuickShowTime = -999.0, gQuickLastSend = -999.0;
static const wchar_t* const kQuickDefText[10] = {
    L"I cover A point", L"I cover B point", L"I'm dead", L"I follow you", L"Follow me",
    L"Ok", L"Cover me", L"I need help", L"I need a first aid kit now", L"Let's go!"};

// "NUMPAD5", "F5", "X", "5", "CTRL+1", "ALT+F2", "SHIFT+NUMPAD0", "0x65"...
static bool QuickParseKey(const std::wstring& spec, UINT* vk, UINT* mods, std::wstring* label) {
    std::wstring s;
    for (wchar_t c : spec) if (c != L' ' && c != L'\t') s.push_back((wchar_t)towupper(c));
    UINT m = 0;
    std::wstring pre;
    for (;;) {
        const size_t plus = s.find(L'+');
        if (plus == std::wstring::npos || plus == 0 || plus + 1 >= s.size()) break;
        const std::wstring part = s.substr(0, plus);
        if (part == L"CTRL" || part == L"CONTROL") { m |= MOD_CONTROL; pre += L"Ctrl+"; }
        else if (part == L"ALT") { m |= MOD_ALT; pre += L"Alt+"; }
        else if (part == L"SHIFT") { m |= MOD_SHIFT; pre += L"Shift+"; }
        else return false;
        s = s.substr(plus + 1);
    }
    if (s.empty()) return false;
    UINT v = 0;
    std::wstring name;
    std::wstring rest;   // what follows a "NUM" / "NUMPAD" prefix
    if (s.compare(0, 6, L"NUMPAD") == 0) rest = s.substr(6);
    else if (s.compare(0, 3, L"NUM") == 0) rest = s.substr(3);
    if (s.size() == 1 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'0' && s[0] <= L'9'))) {
        v = (UINT)s[0]; name = s;
    } else if (rest.size() == 1 && rest[0] >= L'0' && rest[0] <= L'9') {
        v = VK_NUMPAD0 + (UINT)(rest[0] - L'0'); name = rest;
    } else if (rest == L"*" || rest == L"MULTIPLY") { v = VK_MULTIPLY; name = L"Num*";
    } else if (rest == L"/" || rest == L"DIVIDE")   { v = VK_DIVIDE;   name = L"Num/";
    } else if (rest == L"+" || rest == L"ADD")      { v = VK_ADD;      name = L"Num+";
    } else if (rest == L"-" || rest == L"SUBTRACT") { v = VK_SUBTRACT; name = L"Num-";
    } else if (rest == L"." || rest == L"DECIMAL")  { v = VK_DECIMAL;  name = L"Num.";
    } else if (s[0] == L'F' && s.size() <= 3 && s.size() >= 2 && iswdigit(s[1]) && (s.size() == 2 || iswdigit(s[2]))) {
        const int n = _wtoi(s.c_str() + 1);
        if (n < 1 || n > 24) return false;
        v = VK_F1 + (UINT)(n - 1); name = s;
    } else if (s.compare(0, 2, L"0X") == 0 && s.size() > 2) {
        const unsigned long x = wcstoul(s.c_str() + 2, nullptr, 16);
        if (x < 1 || x > 254) return false;
        v = (UINT)x; name = s;
    } else {
        static const struct { const wchar_t* n; UINT vk; const wchar_t* label; } tab[] = {
            {L"INSERT", VK_INSERT, L"Ins"}, {L"INS", VK_INSERT, L"Ins"}, {L"DELETE", VK_DELETE, L"Del"}, {L"DEL", VK_DELETE, L"Del"},
            {L"HOME", VK_HOME, L"Home"}, {L"END", VK_END, L"End"}, {L"PAGEUP", VK_PRIOR, L"PgUp"}, {L"PGUP", VK_PRIOR, L"PgUp"},
            {L"PAGEDOWN", VK_NEXT, L"PgDn"}, {L"PGDN", VK_NEXT, L"PgDn"}, {L"UP", VK_UP, L"Up"}, {L"DOWN", VK_DOWN, L"Down"},
            {L"LEFT", VK_LEFT, L"Left"}, {L"RIGHT", VK_RIGHT, L"Right"}, {L"SPACE", VK_SPACE, L"Space"}, {L"TAB", VK_TAB, L"Tab"},
            {L"BACKSPACE", VK_BACK, L"Back"}, {L"PAUSE", VK_PAUSE, L"Pause"}};
        bool found = false;
        for (const auto& t : tab) if (s == t.n) { v = t.vk; name = t.label; found = true; break; }
        if (!found) return false;
    }
    *vk = v; *mods = m; *label = pre + name;
    return true;
}

static void LoadQuick() {
    const std::wstring txt = IniReadAllText();
    std::wstring v;
    gPlayerName = IniTextGet(txt, L"quick", L"player_name", &v) ? v : L"";
    if (gPlayerName.size() > 32) gPlayerName.resize(32);
    for (int i = 0; i < 10; i++) {
        wchar_t kn[16], tn[16];
        _snwprintf(kn, 16, L"key%d", i);
        _snwprintf(tn, 16, L"text%d", i);
        QuickSlot q;
        std::wstring ks, ts;
        if (!IniTextGet(txt, L"quick", tn, &ts)) ts = kQuickDefText[i];
        if (ts.size() > 300) ts.resize(300);
        q.text = ts;
        if (!IniTextGet(txt, L"quick", kn, &ks) || ks.empty()) { wchar_t d[16]; _snwprintf(d, 16, L"NUMPAD%d", i); ks = d; }
        q.badKey = !QuickParseKey(ks, &q.vk, &q.mods, &q.label);
        if (q.badKey) q.label = L"?";
        gQuick[i] = q;
    }
}

// Adds the documented [quick] section to AudioSight.ini on first run.
static void EnsureQuickSection() {
    std::wstring v;
    const std::wstring txt = IniReadAllText();
    if (IniTextGet(txt, L"quick", L"text0", &v) || IniTextGet(txt, L"quick", L"key0", &v)) return;
    FILE* f = _wfopen(gIniPath.c_str(), L"ab");
    if (!f) return;
    fputs("\r\n[quick]\r\n"
          "; Quick phrases (optional: switch them on in the settings panel). Press the key of a phrase\r\n"
          "; and it is posted in your Discord channel by the bot. This needs the [discord] setup above,\r\n"
          "; and the bot must be allowed to \"Send Messages\" in that channel.\r\n"
          ";\r\n"
          "; keyN  = the key of phrase N (N = 0..9). Examples: NUMPAD5, F5, X, 5, CTRL+1, ALT+F2.\r\n"
          ";          Names: NUMPAD0-9, A-Z, 0-9, F1-F24, UP, DOWN, LEFT, RIGHT, INSERT, DELETE, HOME,\r\n"
          ";          END, PAGEUP, PAGEDOWN, NUM*, NUM/, NUM+, NUM-, NUM., SPACE, TAB, or a code like 0x65.\r\n"
          ";          While the quick phrases are on, that key is taken by AudioSight (the game does not\r\n"
          ";          receive it). The numpad digits need NumLock to be on.\r\n"
          "; textN = the phrase. Leave it empty to remove the phrase from the list.\r\n"
          ";          Accents work if the file is saved as UTF-8 (the default of Notepad).\r\n"
          "; player_name = written before each phrase (\"Name: phrase\"). Leave empty for nothing.\r\n"
          "player_name=\r\n"
          "key0=NUMPAD0\r\ntext0=I cover A point\r\n"
          "key1=NUMPAD1\r\ntext1=I cover B point\r\n"
          "key2=NUMPAD2\r\ntext2=I'm dead\r\n"
          "key3=NUMPAD3\r\ntext3=I follow you\r\n"
          "key4=NUMPAD4\r\ntext4=Follow me\r\n"
          "key5=NUMPAD5\r\ntext5=Ok\r\n"
          "key6=NUMPAD6\r\ntext6=Cover me\r\n"
          "key7=NUMPAD7\r\ntext7=I need help\r\n"
          "key8=NUMPAD8\r\ntext8=I need a first aid kit now\r\n"
          "key9=NUMPAD9\r\ntext9=Let's go!\r\n", f);
    fclose(f);
}

// Adds the documented [discord] section to AudioSight.ini on first run.
static void EnsureDiscordSection() {
    wchar_t buf[8] = L"";
    GetPrivateProfileStringW(L"discord", L"bot_token", L"?", buf, 8, gIniPath.c_str());
    if (wcscmp(buf, L"?") != 0) return;   // already there
    FILE* f = _wfopen(gIniPath.c_str(), L"ab");
    if (!f) return;
    fputs("\r\n[discord]\r\n"
          "; Discord chat overlay (optional: switch it on in the settings panel). A Discord BOT reads\r\n"
          "; the messages of ONE text channel and the overlay shows them. Setup:\r\n"
          "; 1. https://discord.com/developers/applications -> New Application -> Bot -> Reset Token,\r\n"
          ";    then paste the token below. Keep it private: whoever has it controls the bot.\r\n"
          "; 2. On that same Bot page, turn ON \"Message Content Intent\".\r\n"
          "; 3. Invite the bot to your server: OAuth2 -> URL Generator -> scope \"bot\", permissions\r\n"
          ";    \"View Channels\" + \"Read Message History\", then open the generated link.\r\n"
          "; 4. In Discord: Settings -> Advanced -> Developer Mode ON, then right-click the text channel\r\n"
          ";    (the text chat of a voice channel works too) -> Copy Channel ID, and paste it below.\r\n"
          "; Save this file: the chat connects by itself, no restart needed.\r\n"
          "bot_token=\r\n"
          "channel_id=\r\n", f);
    fclose(f);
}

// ------------------------------------------------------------ GDI+ drawing
static FontFamily* gFont = nullptr;

static void RoundRectPath(GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = min(r * 2, min(w, h));
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRR(Graphics* g, Color c, float x, float y, float w, float h, float r) {
    GraphicsPath p; RoundRectPath(p, x, y, w, h, r);
    SolidBrush b(c); g->FillPath(&b, &p);
}
static void StrokeRR(Graphics* g, const Pen& pen, float x, float y, float w, float h, float r) {
    GraphicsPath p; RoundRectPath(p, x, y, w, h, r);
    g->DrawPath(&pen, &p);
}
static void Text(Graphics* g, const wchar_t* s, float px, bool bold, Color c,
                 float x, float y, float w, float h, StringAlignment align) {
    Font f(gFont, px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(align);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g->DrawString(s, -1, &f, RectF(x, y, w, h), &sf, &b);
}

// ------------------------------------------------------------ overlay windows
enum Type { T_FLASH = 0, T_BAR, T_LEFT, T_RIGHT, T_TOP, T_GRAPH, T_CHAT, T_QUICK, T_PANEL, NB_OV };

struct Overlay {
    Type type = T_BAR;
    const wchar_t* name = L"";
    const wchar_t* label = L"";
    int x = 0, y = 0, w = 0, h = 0;      // screen position and size (pixels)
    int defX = 0, defY = 0;              // default position
    float scale = 1.f;                   // drawing scale (high-resolution screens)
    HWND hwnd = nullptr;
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    Bitmap* bmp = nullptr;
    Graphics* g = nullptr;
    bool moveMode = false, dragging = false, pressed = false, tracking = false, savedPos = false;
    POINT dragOffset = {0, 0};
    unsigned long long sig = ~0ULL;      // signature of what is currently drawn
    int lastAlpha = -1;
};

static Overlay gOv[NB_OV];
static int gGlowPx = 0;            // size of the flash glow in screen pixels
static bool gMove = false;         // move mode
static bool gPanelOpen = false;
static bool gGearHover = false;    // cursor is over the gear (bar becomes clickable)
static int  gActiveSlider = -1;
static int  gHoverButton = -1;

// Logical (unscaled) layout of the bar container
static const float kBarW = 560.f, kBarH = 86.f;   // whole container
static const float kContentW = 520.f;             // volume bars + marker area
static const float kGearX = 524.f;                // start of the gear column

// Logical (unscaled) size of the sound graph container
static const float kGraphW = 560.f, kGraphH = 86.f;

// State of the bar
static struct {
    float smoothL = 0, smoothR = 0, balance = 0, fade = kRestOpacity, levelDb = -90.f;
    int litL = 0, litR = 0;
    std::wstring message;
    double messageEnd = 0, lastShotTime = -999;
    float flashIntensity = 1.f;   // 0..1, depends on how loud (close) the last shot was
    unsigned serial = 0;
} gBar;

static void SetMessage(const std::wstring& m) {
    gBar.message = m;
    gBar.messageEnd = Now() + 1.5;
    gBar.serial++;
}

// Gear icon (opens the settings panel)
static void DrawGearIcon(Graphics* g, float cx, float cy, bool bright) {
    GraphicsState st = g->Save();
    if (bright) { SolidBrush hl(Col(gTheme.gearGlow)); g->FillEllipse(&hl, cx - 15.f, cy - 15.f, 30.f, 30.f); }
    const Color c = Col(bright ? gTheme.gearHover : gTheme.gear);
    g->TranslateTransform(cx, cy);
    for (int k = 0; k < 8; k++) {
        g->RotateTransform(45.f);
        FillRR(g, c, -2.0f, -10.0f, 4.0f, 5.5f, 1.1f);
    }
    { SolidBrush b(c); g->FillEllipse(&b, -7.f, -7.f, 14.f, 14.f); }
    g->SetCompositingMode(CompositingModeSourceCopy);   // punch the hole with the background color
    { SolidBrush hole(Col(gTheme.barBackground)); g->FillEllipse(&hole, -3.f, -3.f, 6.f, 6.f); }
    g->SetCompositingMode(CompositingModeSourceOver);
    g->Restore(st);
}

static void DrawBar(Overlay* o, double now) {
    Graphics* g = o->g;
    g->ResetTransform();
    g->ScaleTransform(o->scale, o->scale);
    const float W = kContentW, H = kBarH;

    // container
    FillRR(g, Col(gTheme.barBackground), 0.5f, 0.5f, kBarW - 1, H - 1, 14);
    { Pen p(Col(gTheme.barBorder), 1.f); StrokeRR(g, p, 0.5f, 0.5f, kBarW - 1, H - 1, 14); }

    const float center = W / 2, margin = 18, gap = 12, spacing = 3;
    const float sideWidth = center - margin - gap / 2;
    const float segW = (sideWidth - spacing * (kSegments - 1)) / kSegments;
    const float barY = 16, barH = 28;

    auto side = [&](int lit, uint32_t color, uint32_t off, bool right) {
        for (int i = 0; i < kSegments; i++) {
            float off_x = i * (segW + spacing);
            float x = right ? center + gap / 2 + off_x : center - gap / 2 - off_x - segW;
            // outer segments are brighter
            Color c = i < lit ? Brighten(color, 1.f + 0.4f * i / kSegments) : Col(off);
            FillRR(g, c, x, barY, segW, barH, 2.5f);
        }
    };
    side(gBar.litL, gTheme.leftColor, gTheme.barLeftOff, false);
    side(gBar.litR, gTheme.rightColor, gTheme.barRightOff, true);

    Text(g, L"L", 13, true, Col(gTheme.leftColor), margin, barY + barH + 4, 20, 16, StringAlignmentNear);
    Text(g, L"R", 13, true, Col(gTheme.rightColor), W - margin - 20, barY + barH + 4, 20, 16, StringAlignmentFar);

    // direction track + marker (only moves when a shot is detected)
    const float trackY = barY + barH + 13;
    { Pen p(Col(gTheme.barTrack), 2.f); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
      g->DrawLine(&p, margin + 26, trackY, W - margin - 26, trackY); }
    { Pen p(Col(gTheme.barTrackCenter), 2.f); g->DrawLine(&p, center, trackY - 5, center, trackY + 5); }

    const float half = (W - 2 * (margin + 26)) / 2;
    {   // ticks at -45 and +45 degrees (the track covers -90 .. +90)
        Pen p(Col(gTheme.barTrackCenter), 1.5f);
        g->DrawLine(&p, center - half * 0.5f, trackY - 3, center - half * 0.5f, trackY + 3);
        g->DrawLine(&p, center + half * 0.5f, trackY - 3, center + half * 0.5f, trackY + 3);
    }
    const float mx = center + gBar.balance * half;
    const float t = (gBar.balance + 1.f) / 2.f;
    auto mix = [&](int shift) {   // blend left color -> right color
        float a = (float)((gTheme.leftColor >> shift) & 255), b = (float)((gTheme.rightColor >> shift) & 255);
        return (BYTE)(a * (1 - t) + b * t);
    };
    Color markerColor(255, mix(16), mix(8), mix(0));
    float age = (float)(now - gBar.lastShotTime);
    float radius = 6.f + 4.f * max(0.f, 1.f - age / 0.6f);   // the marker swells on every shot
    { SolidBrush sb(markerColor); g->FillEllipse(&sb, mx - radius, trackY - radius, 2 * radius, 2 * radius);
      Pen p(Col(gTheme.markerBorder), 1.5f); g->DrawEllipse(&p, mx - radius, trackY - radius, 2 * radius, 2 * radius); }

    // gear column
    { Pen p(Col(gTheme.gearSeparator), 1.f); g->DrawLine(&p, kGearX, 14.f, kGearX, H - 14.f); }
    DrawGearIcon(g, kGearX + (kBarW - kGearX) / 2, H / 2, gGearHover || gPanelOpen);

    if (o->moveMode) {
        Pen p(Col(gTheme.moveMode), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, kBarW - 4, H - 4, 10);
        Text(g, L"MOVE MODE - drag with the mouse - press F8 to confirm", 11, true,
             Col(gTheme.moveMode), 0, 1, W, 14, StringAlignmentCenter);
    } else if (now < gBar.messageEnd) {
        Text(g, gBar.message.c_str(), 11, false, Col(gTheme.barMessage), 0, 1, W, 14, StringAlignmentCenter);
    }
}

// Soft glow around the bar container, shown when a shot is detected. Many thin
// translucent layers stack up into a smooth falloff: the opacity of the color
// (AA) is reached at the container edge and fades to 0 over kFlashGlow pixels.
// The container itself is excluded, so only the outside is tinted.
static void DrawFlash(Overlay* o) {
    Graphics* g = o->g;
    g->ResetTransform();
    g->ScaleTransform(o->scale, o->scale);
    const uint32_t c = gTheme.shotFlash;
    const float peak = Clampf((float)(c >> 24) / 255.f, 0.f, 0.995f);
    if (peak <= 0.f) return;   // alpha 00 = flash disabled
    // number of stacked layers: one pixel each, but never so many that a single layer
    // would need an opacity below 1/255 (very faint flashes use fewer, wider-spaced layers)
    const int maxLayers = (int)kFlashGlow;
    const float fewest = logf(1.f - peak) / logf(1.f - 1.f / 255.f);
    const int layers = max(1, min(maxLayers, (int)(fewest + 0.5f)));
    const float a = 1.f - powf(1.f - peak, 1.f / layers);
    const BYTE alpha = (BYTE)max(1, (int)(a * 255.f + 0.5f));

    GraphicsPath hole;
    RoundRectPath(hole, kFlashGlow + 1, kFlashGlow + 1, kBarW - 2, kBarH - 2, 13);
    Region clip(RectF(0.f, 0.f, kBarW + 2 * kFlashGlow, kBarH + 2 * kFlashGlow));
    clip.Exclude(&hole);
    g->SetClip(&clip);
    SolidBrush brush(Color(alpha, (BYTE)(c >> 16), (BYTE)(c >> 8), (BYTE)c));
    for (int j = 0; j < layers; j++) {
        const float k = kFlashGlow * (layers - j) / layers;   // outermost layer first
        GraphicsPath path;
        RoundRectPath(path, kFlashGlow - k, kFlashGlow - k, kBarW + 2.f * k, kBarH + 2.f * k, 14.f + k);
        g->FillPath(&brush, &path);
    }
    g->ResetClip();
}

// Soft gradient hugging a screen edge (left / right / top). It is drawn once at
// full strength: Tick() only changes the window opacity (loudness of the shot x
// fade-out), like the glow around the bar container. The color's AA is the
// opacity at the very edge for the loudest shots.
static void DrawEdgeGradient(Overlay* o) {
    const int W = o->w, H = o->h;
    const uint32_t c = gTheme.edgeGradient;
    const float peak = (float)(c >> 24);
    const uint32_t R = (c >> 16) & 255, G = (c >> 8) & 255, B = c & 255;
    o->g->Flush(FlushIntentionSync);   // make sure GDI+ is done before writing the pixels directly
    uint32_t* px = (uint32_t*)o->bits;   // premultiplied ARGB, top-down, stride = W * 4
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float u, v;   // u: 0 at the screen edge .. 1 far from it, v: -1 .. 1 along the edge
            if (o->type == T_TOP) { u = (y + 0.5f) / H; v = (x + 0.5f) / W * 2.f - 1.f; }
            else { u = o->type == T_LEFT ? (x + 0.5f) / W : 1.f - (x + 0.5f) / W; v = (y + 0.5f) / H * 2.f - 1.f; }
            const float fu = (1.f - u) * (1.f - u);                          // smooth falloff away from the edge
            const float t = Clampf((1.f - fabsf(v)) / 0.75f, 0.f, 1.f);      // plateau, then soft ends
            const float fv = t * t * (3.f - 2.f * t);
            const uint32_t a = (uint32_t)(peak * fu * fv + 0.5f);
            px[(size_t)y * W + x] = (a << 24) | ((R * a / 255) << 16) | ((G * a / 255) << 8) | (B * a / 255);
        }
    }
    if (o->moveMode) {
        Graphics* g = o->g;
        Pen p(Col(gTheme.moveMode), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, (float)W - 4, (float)H - 4, 8);
        Text(g, o->label, 13, true, Col(gTheme.indicatorLabel), 0, 0, (float)W, (float)H, StringAlignmentCenter);
    }
}

static void DrawIndicator(Overlay* o) {
    Graphics* g = o->g;
    g->ResetTransform();
    if (gEdgeOn) { DrawEdgeGradient(o); return; }

    // plain rectangle (edge gradient switched off), anchored to its screen edge
    const float W = (float)o->w, H = (float)o->h;
    const float rt = kRectThick * o->scale, rl = kRectLen * o->scale;
    float rx, ry, rw, rh;
    if (o->type == T_TOP)        { rx = (W - rl) / 2; ry = kRectPad * o->scale; rw = rl; rh = rt; }
    else if (o->type == T_RIGHT) { rx = W - rt; ry = (H - rl) / 2; rw = rt; rh = rl; }
    else                         { rx = 0; ry = (H - rl) / 2; rw = rt; rh = rl; }
    if (o->moveMode) {
        FillRR(g, ColA(gTheme.indicator, 150), rx + 1, ry + 1, rw - 2, rh - 2, 8);
        Pen p(Col(gTheme.moveMode), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, rx + 2, ry + 2, rw - 4, rh - 4, 8);
        Text(g, o->label, 13, true, Col(gTheme.indicatorLabel), rx, ry, rw, rh, StringAlignmentCenter);
    } else {
        FillRR(g, Col(gTheme.indicator), rx + 1, ry + 1, rw - 2, rh - 2, 8);
        Pen p(Col(gTheme.indicatorBorder), 1.5f);
        StrokeRR(g, p, rx + 1, ry + 1, rw - 2, rh - 2, 8);
    }
}

// Sound graph: loudness of the last kGraphSeconds seconds, scrolling to the left.
// The vertical scale is the same as the volume bars (bars threshold = bottom,
// kBarsMaxDb = top), so the F9 / F10 sensitivity also applies to the graph.
static void DrawGraph(Overlay* o, double now) {
    Graphics* g = o->g;
    g->ResetTransform();
    g->ScaleTransform(o->scale, o->scale);
    const float W = kGraphW, H = kGraphH;

    FillRR(g, Col(gTheme.graphBackground), 0.5f, 0.5f, W - 1, H - 1, 14);
    { Pen p(Col(gTheme.graphBorder), 1.f); StrokeRR(g, p, 0.5f, 0.5f, W - 1, H - 1, 14); }

    // plotting area
    const float x0 = 12.f, x1 = W - 12.f, yTop = 12.f, yBase = H - 12.f;
    const float pw = x1 - x0, ph = yBase - yTop;
    const float floorDb = gBarsDb;
    auto frac = [&](float db) { return Clampf((db - floorDb) / (kBarsMaxDb - floorDb), 0.f, 1.f); };

    // one thin vertical line per second
    {
        Pen p(Col(gTheme.graphGrid), 1.f);
        const int secs = (int)(kGraphSeconds + 0.5);
        for (int s = 1; s < secs; s++) {
            const float x = x0 + pw * (float)s / (float)secs;
            g->DrawLine(&p, x, yTop, x, yBase);
        }
    }

    // shot detection threshold
    {
        const float ft = frac(gShotDb);
        if (ft > 0.01f && ft < 0.99f) {
            Pen p(Col(gTheme.graphShotLine), 1.f);
            p.SetDashStyle(DashStyleDash);
            const float y = yBase - ft * ph;
            g->DrawLine(&p, x0, y, x1, y);
        }
    }

    // zero line
    { Pen p(Col(gTheme.graphBaseline), 1.f); g->DrawLine(&p, x0, yBase, x1, yBase); }

    // curve: one point per analysis block; a pause in the audio data counts as silence
    std::vector<PointF> pts;
    pts.reserve(400);
    const double t0 = now - kGraphSeconds;
    auto add = [&](double t, float f) {
        const float x = x0 + (float)((t - t0) / kGraphSeconds) * pw;
        pts.push_back(PointF(Clampf(x, x0, x1), yBase - f * ph));
    };
    add(t0, 0.f);
    double prevT = t0;
    bool have = false;
    const unsigned head = gHistHead.load(std::memory_order_acquire);
    const unsigned count = head < kHistCap ? head : kHistCap;
    for (unsigned i = head - count; i != head; i++) {
        const double t = gHistT[i % kHistCap];
        const float db = gHistDb[i % kHistCap];
        if (t <= prevT) continue;   // too old (or overwritten while reading)
        if (t - prevT > kGraphGap) {
            if (have) add(prevT + 0.02, 0.f);
            add(t - 0.02, 0.f);
        }
        add(t, frac(db));
        prevT = t;
        have = true;
    }
    if (now - prevT > kGraphGap) {   // no recent data: back to zero until now
        if (have) add(prevT + 0.02, 0.f);
        add(now, 0.f);
    }
    if (pts.size() >= 2) {
        std::vector<PointF> area(pts);
        area.push_back(PointF(pts.back().X, yBase));
        area.push_back(PointF(pts.front().X, yBase));
        { SolidBrush b(Col(gTheme.graphFill)); g->FillPolygon(&b, area.data(), (INT)area.size()); }
        Pen p(Col(gTheme.graphLine), 1.6f);
        p.SetLineJoin(LineJoinRound);
        g->DrawLines(&p, pts.data(), (INT)pts.size());
    }

    if (o->moveMode) {
        Pen p(Col(gTheme.moveMode), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, W - 4, H - 4, 10);
        Text(g, L"SOUND GRAPH - drag with the mouse - press F8 to confirm", 11, true,
             Col(gTheme.moveMode), 0, 1, W, 14, StringAlignmentCenter);
    }
}

// Discord chat window: the newest message at the bottom, older ones fade out towards
// the top. Names are drawn in the "chat_author" color, text in "chat_text". The window
// is drawn only when the messages change; Tick() only changes its opacity.
static void DrawChat(Overlay* o) {
    Graphics* g = o->g;
    g->ResetTransform();
    g->ScaleTransform(o->scale, o->scale);
    const float W = gChatW, H = gChatH;
    FillRR(g, Col(gTheme.chatBackground), 0.5f, 0.5f, W - 1, H - 1, 12);
    { Pen p(Col(gTheme.chatBorder), 1.f); StrokeRR(g, p, 0.5f, 0.5f, W - 1, H - 1, 12); }

    std::vector<ChatMsg> msgs;
    std::wstring status;
    bool statusErr = false;
    EnterCriticalSection(&gChatCs);
    msgs = gChatMsgs;
    status = gChatStatus;
    statusErr = gChatStatusErr;
    LeaveCriticalSection(&gChatCs);
    if (o->moveMode && msgs.empty() && status.empty()) {   // example, to see the size while moving
        ChatMsg a; a.id = 1; a.author = L"Player"; a.text = L"Example message: this is where the Discord chat shows up.";
        ChatMsg b; b.id = 2; b.author = L"Teammate"; b.text = L"Enemy spotted near the bridge!";
        msgs.push_back(a); msgs.push_back(b);
    }

    const float pad = 10.f, lineH = 18.f, gap = 6.f, textW = W - 2 * pad;
    Font fr(gFont, 13.f, FontStyleRegular, UnitPixel), fb(gFont, 13.f, FontStyleBold, UnitPixel);
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
    auto tw = [&](const std::wstring& t, const Font& f) -> float {
        RectF r;
        g->MeasureString(t.c_str(), (INT)t.size(), &f, PointF(0.f, 0.f), &sf, &r);
        return r.Width;
    };
    float spaceW = tw(L" ", fr);
    if (spaceW < 2.f) spaceW = 4.f;

    float top = pad;
    if (o->moveMode) {
        Pen p(Col(gTheme.moveMode), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, W - 4, H - 4, 10);
        Text(g, L"DISCORD CHAT - drag with the mouse - press F8 to confirm", 11, true,
             Col(gTheme.moveMode), 0, 1, W, 14, StringAlignmentCenter);
        top += 14.f;
    }
    if (!status.empty()) {   // help text or error, at the top
        StringFormat df;
        RectF bb;
        g->MeasureString(status.c_str(), -1, &fr, RectF(pad, 0.f, textW, 1000.f), &df, &bb);
        SolidBrush b(Col(statusErr ? gTheme.chatError : gTheme.chatHint));
        g->DrawString(status.c_str(), -1, &fr, RectF(pad, top, textW, bb.Height + 2.f), &df, &b);
        top += bb.Height + 8.f;
    } else if (msgs.empty()) {
        SolidBrush b(Col(gTheme.chatHint));
        g->DrawString(L"Discord chat: waiting for messages...", -1, &fr, PointF(pad, H - pad - lineH), &sf, &b);
    }

    struct Run { std::wstring t; float x; bool bold; };
    typedef std::vector<Run> Line;
    // word wrap: "Name text text text" with the first line starting after the name
    auto layout = [&](const ChatMsg& m) {
        std::vector<Line> lines;
        Line cur;
        std::wstring au = m.author;
        float aw = tw(au, fb);
        while (au.size() > 1 && aw > textW * 0.6f) { au.pop_back(); aw = tw(au, fb); }
        cur.push_back({au, 0.f, true});
        float x = aw + spaceW * 1.5f;
        const std::wstring& body = m.text;
        size_t p = 0;
        while (p < body.size()) {
            while (p < body.size() && body[p] == L' ') p++;
            size_t e = p;
            while (e < body.size() && body[e] != L' ') e++;
            if (e == p) break;
            std::wstring w = body.substr(p, e - p);
            p = e;
            for (;;) {
                const float ww = tw(w, fr);
                if (x + ww <= textW) { cur.push_back({w, x, false}); x += ww + spaceW; break; }
                if (x > 0.f) { lines.push_back(cur); cur.clear(); x = 0.f; continue; }
                size_t k = w.size();   // a word wider than the window: cut it
                while (k > 1 && tw(w.substr(0, k), fr) > textW) k--;
                cur.push_back({w.substr(0, k), 0.f, false});
                lines.push_back(cur);
                cur.clear();
                w = w.substr(k);
                if (w.empty()) break;
            }
        }
        lines.push_back(cur);
        return lines;
    };

    float y = H - pad;   // bottom of the message being placed
    bool any = false;
    for (int mi = (int)msgs.size() - 1; mi >= 0; mi--) {
        const std::vector<Line> lines = layout(msgs[(size_t)mi]);
        int firstLine = 0;
        float h = (float)lines.size() * lineH;
        if (y - h < top - 0.5f) {
            if (any) break;   // an older message that does not fit entirely is not drawn
            firstLine = (int)lines.size() - (int)((y - top) / lineH);   // newest one too tall: keep its end
            if (firstLine < 0) firstLine = 0;
            if (firstLine >= (int)lines.size()) break;
            h = (float)((int)lines.size() - firstLine) * lineH;
        }
        const float y0 = y - h;
        for (int li = firstLine; li < (int)lines.size(); li++) {
            const float ly = y0 + (float)(li - firstLine) * lineH;
            const float fade = 0.40f + 0.60f * Clampf((ly - top) / max(1.f, H - pad - top - lineH), 0.f, 1.f);
            for (const Run& r : lines[(size_t)li]) {
                const uint32_t c = r.bold ? gTheme.chatAuthor : gTheme.chatText;
                SolidBrush b(ColA(c, (int)((float)(c >> 24) * fade + 0.5f)));
                g->DrawString(r.t.c_str(), (INT)r.t.size(), r.bold ? &fb : &fr, PointF(pad + r.x, ly), &sf, &b);
            }
        }
        y = y0 - gap;
        any = true;
    }
}

// Quick phrases window: "key  phrase" lines (key in violet, phrase in green). Long phrases
// are cut with "...". The window is drawn only when something changes; Tick() handles opacity.
static const int   kQuickRows = 10;
static const float kQuickRowH = 21.f, kQuickHead = 28.f;
static const float kQuickH = kQuickHead + kQuickRows * kQuickRowH + 12.f;

static void DrawQuick(Overlay* o, double now) {
    Graphics* g = o->g;
    g->ResetTransform();
    g->ScaleTransform(o->scale, o->scale);
    const float W = gQuickW, H = kQuickH, pad = 12.f;
    FillRR(g, Col(gTheme.quickBackground), 0.5f, 0.5f, W - 1, H - 1, 12);
    { Pen p(Col(gTheme.quickBorder), 1.f); StrokeRR(g, p, 0.5f, 0.5f, W - 1, H - 1, 12); }

    Font fr(gFont, 14.f, FontStyleRegular, UnitPixel), fb(gFont, 14.f, FontStyleBold, UnitPixel),
         fh(gFont, 11.f, FontStyleBold, UnitPixel);
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
    auto tw = [&](const std::wstring& t, const Font& f) -> float {
        RectF r;
        g->MeasureString(t.c_str(), (INT)t.size(), &f, PointF(0.f, 0.f), &sf, &r);
        return r.Width;
    };
    auto fit = [&](std::wstring t, const Font& f, float maxW) -> std::wstring {
        if (tw(t, f) <= maxW) return t;
        while (t.size() > 1 && tw(t + L"\x2026", f) > maxW) t.pop_back();
        return t + L"\x2026";
    };

    // title line: the title, or the reason why the last phrase was not sent
    std::wstring err;
    EnterCriticalSection(&gChatCs);
    err = gQuickErr;
    LeaveCriticalSection(&gChatCs);
    const bool errActive = !err.empty() && now - gQuickErrTime < 6.0;
    if (o->moveMode) {
        Text(g, L"QUICK PHRASES - drag with the mouse - press F8 to confirm", 11, true,
             Col(gTheme.moveMode), 0, 4, W, 16, StringAlignmentCenter);
        Pen p(Col(gTheme.moveMode), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, W - 4, H - 4, 10);
    } else {
        const std::wstring head = errActive ? fit(err, fh, W - 2 * pad) : std::wstring(L"QUICK PHRASES");
        SolidBrush b(Col(errActive ? gTheme.quickError : gTheme.quickHint));
        g->DrawString(head.c_str(), (INT)head.size(), &fh, PointF(pad, 8.f), &sf, &b);
    }

    float labelW = 0.f;
    for (int i = 0; i < kQuickRows; i++) if (!gQuick[i].text.empty()) labelW = max(labelW, tw(gQuick[i].label, fb));
    const float tx = pad + labelW + 12.f;
    const bool flashOn = gQuickFlashIdx >= 0 && now - gQuickFlashTime < 0.7;
    float y = kQuickHead;
    int shown = 0;
    for (int i = 0; i < kQuickRows; i++) {
        const QuickSlot& q = gQuick[i];
        if (q.text.empty()) continue;
        if (flashOn && gQuickFlashIdx == i) FillRR(g, Col(gTheme.quickSent), 6.f, y - 1.f, W - 12.f, kQuickRowH, 6.f);
        { SolidBrush b(Col(q.badKey ? gTheme.quickError : gTheme.quickKey));
          g->DrawString(q.label.c_str(), (INT)q.label.size(), &fb, PointF(pad, y + 1.f), &sf, &b); }
        { const std::wstring t = fit(q.text, fr, W - tx - pad);
          SolidBrush b(Col(gTheme.quickText));
          g->DrawString(t.c_str(), (INT)t.size(), &fr, PointF(tx, y + 1.f), &sf, &b); }
        y += kQuickRowH;
        shown++;
    }
    if (!shown) {
        const std::wstring t = fit(L"No phrase set: see [quick] in AudioSight.ini", fr, W - 2 * pad);
        SolidBrush b(Col(gTheme.quickHint));
        g->DrawString(t.c_str(), (INT)t.size(), &fr, PointF(pad, kQuickHead + 1.f), &sf, &b);
    }
}

// ------------------------------------------------------------ settings panel
struct SliderDef {
    const wchar_t* label;
    const wchar_t* hint;
    float lo, hi;       // value at the left end (t = 0) and at the right end (t = 1)
    int decimals;
    const wchar_t* unit;
    int accent;         // 0 = bars, 1 = shots, 2 = other (see the color list)
};
static const SliderDef kSliders[5] = {
    {L"Volume bars sensitivity", L"Right: quieter sounds (footsteps...) show up", -35.f, -90.f, 0, L" dB", 0},
    {L"Shot detection sensitivity", L"Right: quieter or more distant shots are detected", -5.f, -60.f, 0, L" dB", 1},
    {L"Sound suddenness (shots)", L"Right: also accepts less abrupt sounds", 24.f, 4.f, 0, L" dB", 1},
    {L"\"Center\" zone (top indicator)", L"Right: more shots are classified as \"top\"", 1.f, 10.f, 0, L" dB", 2},
    {L"Indicator duration", L"How long edge indicators and the flash stay visible", 0.5f, 3.0f, 1, L" s", 2},
};
static uint32_t AccentColor(int kind) {
    return kind == 0 ? gTheme.accentBars : (kind == 1 ? gTheme.accentShots : gTheme.accentOther);
}

// panel buttons: 0 move, 1 reset positions, 2 edit colors, 3 reset colors, 4 reset settings, 5 quit
static const int kButtonCount = 6, kCloseButton = 6, kToggleButton = 7, kGraphToggleButton = 8, kEdgeToggleButton = 9, kChatToggleButton = 10, kQuickToggleButton = 11;
static const float kToggleY = 376.f, kToggleH = 30.f;   // shot flash switch row
static const float kToggle2Y = 410.f;                   // sound graph switch row
static const float kToggle5Y = 512.f;                   // quick phrases switch row
static const float kToggle4Y = 478.f;                   // Discord chat switch row
static const float kToggle3Y = 444.f;                   // screen-edge gradient switch row
static const float kPanelW = 380.f, kPanelH = 750.f, kTrackX0 = 18.f, kTrackX1 = 362.f;
static const float kSliderY0 = 52.f, kSliderStep = 64.f;
static const float kBtnX[6] = {18.f, 195.f, 18.f, 195.f, 18.f, 195.f};
static const float kBtnY[6] = {622.f, 622.f, 662.f, 662.f, 702.f, 702.f};
static const float kBtnW = 167.f, kBtnH = 32.f;

static float GetSlider(int i) {
    switch (i) {
        case 0: return gBarsDb;
        case 1: return gShotDb;
        case 2: return gSuddenness;
        case 3: return gCenterZone;
        default: return gDuration;
    }
}
static void SetSlider(int i, float v) {
    const SliderDef& d = kSliders[i];
    v = Clampf(v, min(d.lo, d.hi), max(d.lo, d.hi));
    v = d.decimals ? floorf(v * 10.f + 0.5f) / 10.f : floorf(v + 0.5f);
    switch (i) {
        case 0: gBarsDb = v; break;
        case 1: gShotDb = v; break;
        case 2: gSuddenness = v; break;
        case 3: gCenterZone = v; break;
        default: gDuration = v; break;
    }
}
static float SliderT(int i) {
    const SliderDef& d = kSliders[i];
    return Clampf((GetSlider(i) - d.lo) / (d.hi - d.lo), 0.f, 1.f);
}
static float LastShotDb() {
    return gAudio.shotCount > 0 ? max((float)gAudio.shotL, (float)gAudio.shotR) : -999.f;
}

static void DrawPanel(Overlay* o) {
    Graphics* g = o->g;
    g->ResetTransform();
    g->ScaleTransform(o->scale, o->scale);
    const float W = kPanelW, H = kPanelH;

    FillRR(g, Col(gTheme.panelBackground), 0.5f, 0.5f, W - 1, H - 1, 14);
    { Pen p(Col(gTheme.panelBorder), 1.f); StrokeRR(g, p, 0.5f, 0.5f, W - 1, H - 1, 14); }
    Text(g, L"Overlay settings", 15, true, Col(gTheme.panelTitle), 18, 8, 280, 30, StringAlignmentNear);

    // close button
    if (gHoverButton == kCloseButton) FillRR(g, Col(gTheme.closeHover), 336, 9, 26, 26, 7);
    { Pen p(Col(gTheme.closeIcon), 2.f); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
      g->DrawLine(&p, 344.f, 17.f, 354.f, 27.f); g->DrawLine(&p, 354.f, 17.f, 344.f, 27.f); }
    { Pen p(Col(gTheme.panelDivider), 1.f); g->DrawLine(&p, 18.f, 44.f, 362.f, 44.f); }

    // sliders
    for (int i = 0; i < 5; i++) {
        const SliderDef& d = kSliders[i];
        const float y0 = kSliderY0 + i * kSliderStep, ty = y0 + 32.f;
        const Color accent = Col(AccentColor(d.accent));
        Text(g, d.label, 13, false, Col(gTheme.panelText), kTrackX0, y0, 260, 20, StringAlignmentNear);
        wchar_t val[48];
        _snwprintf(val, 48, d.decimals ? L"%.1f%ls" : L"%.0f%ls", GetSlider(i), d.unit);
        Text(g, val, 13, true, accent, 200, y0, 162, 20, StringAlignmentFar);

        const float tx = kTrackX0 + SliderT(i) * (kTrackX1 - kTrackX0);
        FillRR(g, Col(gTheme.sliderTrack), kTrackX0, ty - 3, kTrackX1 - kTrackX0, 6, 3);
        if (tx - kTrackX0 > 1.f) FillRR(g, accent, kTrackX0, ty - 3, tx - kTrackX0, 6, 3);
        const float r = (i == gActiveSlider) ? 10.f : 8.5f;
        { SolidBrush wb(Col(gTheme.sliderThumb)); g->FillEllipse(&wb, tx - r, ty - r, 2 * r, 2 * r);
          Pen p(accent, 2.f); g->DrawEllipse(&p, tx - r, ty - r, 2 * r, 2 * r); }
        Text(g, d.hint, 11, false, Col(gTheme.panelHint), kTrackX0, y0 + 42, kTrackX1 - kTrackX0, 16, StringAlignmentNear);
    }

    // switches (shot flash, sound graph)
    auto drawToggle = [&](float y, int id, const wchar_t* label, bool on) {
        if (gHoverButton == id)
            FillRR(g, ColA(gTheme.buttonHover, 120), kTrackX0 - 6, y, kTrackX1 - kTrackX0 + 12, kToggleH, 8);
        Text(g, label, 13, false, Col(gTheme.panelText), kTrackX0, y, 290, kToggleH, StringAlignmentNear);
        const float sw = 40, sh = 20, sx = kTrackX1 - sw, sy = y + (kToggleH - sh) / 2;
        FillRR(g, Col(on ? gTheme.toggleOn : gTheme.toggleOff), sx, sy, sw, sh, 10);
        SolidBrush thumb(Col(gTheme.toggleThumb));
        const float cx = on ? sx + sw - 10 : sx + 10;
        g->FillEllipse(&thumb, cx - 8, sy + sh / 2 - 8, 16.f, 16.f);
    };
    drawToggle(kToggleY, kToggleButton, L"Flash on shots (brighter when closer)", gFlashOn);
    drawToggle(kToggle2Y, kGraphToggleButton, L"Sound graph (last 5 seconds)", gGraphOn);
    drawToggle(kToggle3Y, kEdgeToggleButton, L"Screen-edge gradient on shots", gEdgeOn);
    drawToggle(kToggle4Y, kChatToggleButton, L"Discord chat (setup: [discord] in .ini)", gChatOn);
    drawToggle(kToggle5Y, kQuickToggleButton, L"Quick phrases (keys: [quick] in .ini)", gQuickOn);

    // live level meter
    const float level = gBar.levelDb;
    wchar_t buf[64];
    if (level <= -89.f) _snwprintf(buf, 64, L"Current level: silence");
    else _snwprintf(buf, 64, L"Current level: %.0f dB", level);
    Text(g, buf, 12, false, Col(gTheme.panelText), kTrackX0, 554, 180, 18, StringAlignmentNear);
    float lastShot = LastShotDb();
    if (lastShot < -900.f) _snwprintf(buf, 64, L"Last shot: none");
    else _snwprintf(buf, 64, L"Last shot: %.0f dB", lastShot);
    Text(g, buf, 12, false, Col(gTheme.panelText), 190, 554, 172, 18, StringAlignmentFar);

    const float meterW = kTrackX1 - kTrackX0, meterY = 578.f;
    auto xOf = [&](float db) { return kTrackX0 + Clampf((db + 90.f) / 90.f, 0.f, 1.f) * meterW; };
    FillRR(g, Col(gTheme.meterBackground), kTrackX0, meterY, meterW, 8, 4);
    float fillW = xOf(level) - kTrackX0;
    if (fillW > 1.f) FillRR(g, Col(level >= gShotDb ? gTheme.meterFillHot : gTheme.meterFill), kTrackX0, meterY, fillW, 8, 4);
    auto tick = [&](float db, uint32_t c) { Pen p(Col(c), 2.f); float x = xOf(db); g->DrawLine(&p, x, meterY - 4, x, meterY + 12); };
    tick(gBarsDb, gTheme.tickBars);
    tick(gShotDb, gTheme.tickShots);
    if (lastShot > -900.f) tick(lastShot, gTheme.tickLastShot);
    const float lx[3] = {18.f, 120.f, 225.f};
    const wchar_t* lt[3] = {L"bars threshold", L"shots threshold", L"last shot"};
    const uint32_t lc[3] = {gTheme.tickBars, gTheme.tickShots, gTheme.tickLastShot};
    for (int i = 0; i < 3; i++) {
        FillRR(g, Col(lc[i]), lx[i], 599.f, 8, 8, 2);
        Text(g, lt[i], 11, false, Col(gTheme.panelHint), lx[i] + 12, 594, 100, 18, StringAlignmentNear);
    }

    // buttons
    const wchar_t* labels[6] = {gMove ? L"Confirm placement" : L"Move elements", L"Reset positions",
                                L"Edit colors...", L"Reset colors", L"Reset settings", L"Quit"};
    for (int i = 0; i < kButtonCount; i++) {
        const bool hov = gHoverButton == i;
        Color fill = Col(hov ? gTheme.buttonHover : gTheme.button);
        Color border = Col(gTheme.buttonBorder);
        if (i == 0 && gMove) { fill = Col(gTheme.buttonActive); border = Col(gTheme.moveMode); }
        if (i == 5) fill = Col(hov ? gTheme.buttonQuitHover : gTheme.buttonQuit);
        FillRR(g, fill, kBtnX[i], kBtnY[i], kBtnW, kBtnH, 8);
        Pen p(border, 1.f);
        StrokeRR(g, p, kBtnX[i], kBtnY[i], kBtnW, kBtnH, 8);
        Text(g, labels[i], 12, false, Col(gTheme.buttonText), kBtnX[i], kBtnY[i], kBtnW, kBtnH, StringAlignmentCenter);
    }
}

// ------------------------------------------------------- layered window output
static void Present(Overlay* o, int alpha) {
    POINT pt = {o->x, o->y};
    SIZE sz = {o->w, o->h};
    POINT src = {0, 0};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(o->hwnd, nullptr, &pt, &sz, o->mem, &src, 0, &bf, ULW_ALPHA);
}

// Redraws only if the content changed (signature), re-presents if only the opacity changed.
static void Refresh(Overlay* o, unsigned long long sig, int alpha, double now) {
    alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    if (sig != o->sig) {
        o->sig = sig;
        o->g->Clear(Color(0, 0, 0, 0));
        switch (o->type) {
            case T_FLASH: DrawFlash(o); break;
            case T_BAR:   DrawBar(o, now); break;
            case T_GRAPH: DrawGraph(o, now); break;
            case T_CHAT:  DrawChat(o); break;
            case T_QUICK: DrawQuick(o, now); break;
            case T_PANEL: DrawPanel(o); break;
            default:      DrawIndicator(o); break;
        }
        o->lastAlpha = alpha;
        Present(o, alpha);
    } else if (alpha != o->lastAlpha) {
        o->lastAlpha = alpha;
        Present(o, alpha);
    }
}

static unsigned long long PanelSignature() {
    unsigned long long h = 1469598103934665603ULL;   // FNV-1a style mix
    auto mix = [&](long long v) { h = (h ^ (unsigned long long)v) * 1099511628211ULL; };
    for (int i = 0; i < 5; i++) mix((long long)floorf(GetSlider(i) * 10.f + 0.5f));
    mix(gActiveSlider);
    mix(gHoverButton);
    mix(gMove ? 1 : 0);
    mix(gFlashOn ? 1 : 0);
    mix(gGraphOn ? 1 : 0);
    mix(gEdgeOn ? 1 : 0);
    mix(gChatOn ? 1 : 0);
    mix(gQuickOn ? 1 : 0);
    mix((long long)floorf(gBar.levelDb * 2.f));
    mix((long long)floorf(LastShotDb()));
    return h;
}

// Click-through on/off (a window that is not click-through receives the mouse)
static void SetClickThrough(Overlay* o, bool through) {
    LONG_PTR st = GetWindowLongPtrW(o->hwnd, GWL_EXSTYLE);
    if (through) st |= WS_EX_TRANSPARENT; else st &= ~(LONG_PTR)WS_EX_TRANSPARENT;
    SetWindowLongPtrW(o->hwnd, GWL_EXSTYLE, st);
    SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void SetMoveMode(Overlay* o, bool on) {
    if (o->type == T_PANEL || o->type == T_FLASH) return;   // the flash follows the bar
    o->moveMode = on;
    SetClickThrough(o, !on);
    o->sig = ~0ULL;
}

// ------------------------------------------------------------ actions / tray
static HWND gMsgWnd = nullptr;
static NOTIFYICONDATAW gNid = {};
static const UINT WM_TRAY = WM_APP + 1;
enum { ID_PANEL = 100, ID_MOVE, ID_BARS_UP, ID_BARS_DOWN, ID_SHOTS_UP, ID_SHOTS_DOWN, ID_FLASH, ID_GRAPH, ID_EDGE, ID_CHAT, ID_QUICK, ID_QUIT };

static void SavePositions() {
    for (auto& o : gOv) {
        if (o.type == T_FLASH || (o.type == T_PANEL && !o.savedPos)) continue;
        wchar_t kx[32], ky[32];
        _snwprintf(kx, 32, L"%ls_x", o.name);
        _snwprintf(ky, 32, L"%ls_y", o.name);
        IniWriteInt(L"positions", kx, o.x);
        IniWriteInt(L"positions", ky, o.y);
    }
}

static void ToggleMove() {
    gMove = !gMove;
    gGearHover = false;
    for (auto& o : gOv) SetMoveMode(&o, gMove);
    if (!gMove) SavePositions();
}

static void TogglePanel() {
    Overlay* p = &gOv[T_PANEL];
    gPanelOpen = !gPanelOpen;
    gOv[T_BAR].sig = ~0ULL;   // the gear highlight changes
    if (gPanelOpen) {
        if (!p->savedPos) {   // default: just under the bar container, right-aligned
            const Overlay* b = &gOv[T_BAR];
            const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
            int x = b->x + b->w - p->w;
            int y = b->y + b->h + 8;
            x = max(10, min(x, sw - p->w - 10));
            if (y + p->h > sh - 10) y = max(10, sh - p->h - 10);
            p->x = x; p->y = y;
        }
        SetWindowPos(p->hwnd, HWND_TOPMOST, p->x, p->y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        p->sig = ~0ULL;
        Refresh(p, PanelSignature(), 255, Now());
        ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(p->hwnd, SW_HIDE);
        gActiveSlider = -1;
        gHoverButton = -1;
        SaveSettings();
    }
}

static void AdjustBars(float delta) {
    SetSlider(0, gBarsDb + delta);
    SaveSettings();
    wchar_t buf[96];
    _snwprintf(buf, 96, L"Bars threshold: %.0f dB", (float)gBarsDb);
    SetMessage(buf);
}
static void AdjustShots(float delta) {
    SetSlider(1, gShotDb + delta);
    SaveSettings();
    wchar_t buf[96];
    _snwprintf(buf, 96, L"Shots threshold: %.0f dB", (float)gShotDb);
    SetMessage(buf);
}

static void ToggleFlash() {
    gFlashOn = !gFlashOn;
    SaveSettings();
    if (gFlashOn) {   // test flash, so the effect is visible right away
        gBar.lastShotTime = Now();
        gBar.flashIntensity = 1.f;
    }
    SetMessage(gFlashOn ? L"Shot flash: ON" : L"Shot flash: OFF");
}

static void ToggleGraph() {
    gGraphOn = !gGraphOn;   // the window is shown / hidden by Tick()
    SaveSettings();
    gOv[T_GRAPH].sig = ~0ULL;
    SetMessage(gGraphOn ? L"Sound graph: ON" : L"Sound graph: OFF");
}

// Quick phrases: the keys are global hotkeys (RegisterHotKey, like F7..F12), registered only
// while the quick phrases are on, so the numpad is left alone otherwise.
static void RegisterQuickKeys(bool announce) {
    if (!gMsgWnd) return;
    for (int i = 0; i < 10; i++) UnregisterHotKey(gMsgWnd, 20 + i);
    if (!gQuickOn) return;
    std::wstring failed;
    for (int i = 0; i < 10; i++) {
        const QuickSlot& q = gQuick[i];
        if (q.badKey || q.text.empty() || !q.vk) continue;
        if (!RegisterHotKey(gMsgWnd, 20 + i, q.mods | MOD_NOREPEAT, q.vk)) failed += (failed.empty() ? L"" : L", ") + q.label;
    }
    if (announce && !failed.empty()) SetMessage(L"Quick phrase key not available: " + failed);
}

static void QuickSend(int i) {
    if (!gQuickOn || i < 0 || i > 9 || gQuick[i].text.empty()) return;
    const double now = Now();
    if (now - gQuickLastSend < 0.6) return;   // no spam
    gQuickLastSend = now;
    std::wstring msg = gPlayerName.empty() ? gQuick[i].text : gPlayerName + L": " + gQuick[i].text;
    if (msg.size() > 1900) msg.resize(1900);
    EnterCriticalSection(&gChatCs);
    if (gOutbox.size() < 3) gOutbox.push_back(msg);
    LeaveCriticalSection(&gChatCs);
    if (gChatWake) SetEvent(gChatWake);
    gQuickFlashIdx = i;
    gQuickFlashTime = now;
    gQuickShowTime = now;
}

static void ToggleQuick() {
    gQuickOn = !gQuickOn;
    SaveSettings();
    gOv[T_QUICK].sig = ~0ULL;
    if (gQuickOn) gQuickShowTime = Now();   // fully visible for a moment
    SetMessage(gQuickOn ? L"Quick phrases: ON" : L"Quick phrases: OFF");
    RegisterQuickKeys(true);                // may replace the message above if a key is taken
}

static void ToggleChat() {
    gChatOn = !gChatOn;   // the window is shown / hidden by Tick(), the thread starts / stops polling
    SaveSettings();
    gOv[T_CHAT].sig = ~0ULL;
    if (gChatOn) gChatNewTime = Now();   // fully visible for a moment
    SetMessage(gChatOn ? L"Discord chat: ON" : L"Discord chat: OFF");
}

static void ToggleEdge() {
    gEdgeOn = !gEdgeOn;   // the indicator windows redraw themselves (their signature includes the switch)
    SaveSettings();
    if (gEdgeOn) {   // test: show the gradient on the three edges right away
        const double t = Now();
        for (int i = 0; i < 3; i++) { gAudio.shotLevel[i] = -20.f; gAudio.shotTime[i] = t; }
    }
    SetMessage(gEdgeOn ? L"Edge gradient: ON" : L"Edge gradient: OFF (plain rectangles)");
}

static void ResetSettings() {
    gQuickOn = false;
    RegisterQuickKeys(false);
    gChatOn = false;
    gEdgeOn = true;
    gFlashOn = true;
    gGraphOn = true;
    gFlashMin = kDefaultFlashMin;
    gBarsDb = kDefaultBarsDb;
    gShotDb = kDefaultShotDb;
    gSuddenness = kDefaultSuddenness;
    gCenterZone = kDefaultCenterZone;
    gDuration = kDefaultDuration;
    SaveSettings();
    SetMessage(L"Settings reset to defaults");
}

static void ResetPositions() {
    for (auto& o : gOv) {
        if (o.type == T_FLASH) continue;
        if (o.type == T_PANEL) { o.savedPos = false; continue; }
        o.x = o.defX; o.y = o.defY;
        SetWindowPos(o.hwnd, nullptr, o.x, o.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SavePositions();
    SetMessage(L"Positions reset");
}

// ------------------------------------------------------------ colors: actions
static HICON CreateTrayIcon();

static void InvalidateAll() {
    for (auto& o : gOv) o.sig = ~0ULL;   // forces a redraw of every window
}

static void UpdateTrayIcon() {
    HICON icon = CreateTrayIcon();
    if (!icon) return;
    HICON old = gNid.hIcon;
    gNid.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &gNid);
    if (old) DestroyIcon(old);
}

// Opens AudioSight.ini in the default editor (Notepad if no association).
static void EditColors() {
    EnsureColorSection();
    HINSTANCE r = ShellExecuteW(nullptr, L"open", gIniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        std::wstring args = L"\"" + gIniPath + L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    SetMessage(L"Edit [colors], save the file: changes apply live");
}

static void ResetColors() {
    EnsureColorSection();
    gTheme = Theme();
    for (int i = 0; i < kColorCount; i++) {
        wchar_t v[16];
        FormatColor(kColorEntries[i].def, kColorEntries[i].def, v, 16);
        WritePrivateProfileStringW(L"colors", kColorEntries[i].key, v, gIniPath.c_str());
    }
    InvalidateAll();
    UpdateTrayIcon();
    SetMessage(L"Colors reset to defaults");
}

// Global hotkeys use RegisterHotKey (the standard Windows mechanism).
static void HotkeyAction(int i) {
    switch (i) {
        case 0: ToggleMove(); break;
        case 1: AdjustBars(-5.f); break;
        case 2: AdjustBars(+5.f); break;
        case 3: AdjustShots(-3.f); break;
        case 4: AdjustShots(+3.f); break;
        case 5: TogglePanel(); break;
    }
}
static void RegisterHotkeys(HWND h) {
    static const int vks[6] = {kVkMove, kVkBarsUp, kVkBarsDown, kVkShotsUp, kVkShotsDown, kVkPanel};
    for (int i = 0; i < 6; i++) RegisterHotKey(h, 1 + i, MOD_NOREPEAT, vks[i]);
}

// ------------------------------------------------------------ gear hit testing
// The bar container is click-through, except while the (visible) mouse cursor
// is over its gear column. A hidden cursor (in-game) never makes it clickable.
static bool PointInGear(POINT p) {
    const Overlay* b = &gOv[T_BAR];
    const int x0 = b->x + (int)(kGearX * b->scale);
    return p.x >= x0 && p.x < b->x + b->w && p.y >= b->y && p.y < b->y + b->h;
}
static bool CursorOverGear() {
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return false;
    return PointInGear(ci.ptScreenPos);
}

// ------------------------------------------------------------ panel mouse input
static int PanelHit(float lx, float ly) {
    if (lx >= 336 && lx <= 362 && ly >= 9 && ly <= 35) return kCloseButton;
    if (lx >= kTrackX0 - 6 && lx <= kTrackX1 + 6 && ly >= kToggleY && ly <= kToggleY + kToggleH) return kToggleButton;
    if (lx >= kTrackX0 - 6 && lx <= kTrackX1 + 6 && ly >= kToggle2Y && ly <= kToggle2Y + kToggleH) return kGraphToggleButton;
    if (lx >= kTrackX0 - 6 && lx <= kTrackX1 + 6 && ly >= kToggle3Y && ly <= kToggle3Y + kToggleH) return kEdgeToggleButton;
    if (lx >= kTrackX0 - 6 && lx <= kTrackX1 + 6 && ly >= kToggle4Y && ly <= kToggle4Y + kToggleH) return kChatToggleButton;
    if (lx >= kTrackX0 - 6 && lx <= kTrackX1 + 6 && ly >= kToggle5Y && ly <= kToggle5Y + kToggleH) return kQuickToggleButton;
    for (int i = 0; i < kButtonCount; i++)
        if (lx >= kBtnX[i] && lx <= kBtnX[i] + kBtnW && ly >= kBtnY[i] && ly <= kBtnY[i] + kBtnH) return i;
    return -1;
}
static void SliderFromX(int i, float lx) {
    const SliderDef& d = kSliders[i];
    float t = Clampf((lx - kTrackX0) / (kTrackX1 - kTrackX0), 0.f, 1.f);
    SetSlider(i, d.lo + t * (d.hi - d.lo));
}
static void PanelAction(int b) {
    switch (b) {
        case 0: ToggleMove(); break;
        case 1: ResetPositions(); break;
        case 2: EditColors(); break;
        case 3: ResetColors(); break;
        case 4: ResetSettings(); break;
        case 5: PostQuitMessage(0); break;
        case kCloseButton: TogglePanel(); break;
        case kToggleButton: ToggleFlash(); break;
        case kGraphToggleButton: ToggleGraph(); break;
        case kEdgeToggleButton: ToggleEdge(); break;
        case kChatToggleButton: ToggleChat(); break;
        case kQuickToggleButton: ToggleQuick(); break;
    }
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Overlay* o = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            break;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // never steal focus from the game
        case WM_SETCURSOR:
            if (o && o->moveMode) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return TRUE; }
            if (o && o->type == T_BAR) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        case WM_LBUTTONDOWN: {
            if (!o) return 0;
            if (o->type == T_PANEL) {
                const float lx = GET_X_LPARAM(lp) / o->scale, ly = GET_Y_LPARAM(lp) / o->scale;
                for (int i = 0; i < 5; i++) {
                    const float y0 = kSliderY0 + i * kSliderStep;
                    if (ly >= y0 + 18 && ly <= y0 + 48 && lx >= kTrackX0 - 12 && lx <= kTrackX1 + 12) {
                        gActiveSlider = i;
                        SliderFromX(i, lx);
                        SetCapture(h);
                        return 0;
                    }
                }
                int b = PanelHit(lx, ly);
                if (b >= 0) { PanelAction(b); return 0; }
                if (ly < 44) {   // drag the panel by its title bar
                    POINT p; GetCursorPos(&p);
                    o->dragging = true;
                    o->dragOffset = {p.x - o->x, p.y - o->y};
                    SetCapture(h);
                }
                return 0;
            }
            if (o->moveMode) {
                POINT p; GetCursorPos(&p);
                o->dragging = true;
                o->dragOffset = {p.x - o->x, p.y - o->y};
                SetCapture(h);
            } else if (o->type == T_BAR) {
                o->pressed = true;   // click on the gear column
                SetCapture(h);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!o) return 0;
            if (o->type == T_PANEL) {
                if (!o->tracking) {
                    TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
                    TrackMouseEvent(&tme);
                    o->tracking = true;
                }
                const float lx = GET_X_LPARAM(lp) / o->scale, ly = GET_Y_LPARAM(lp) / o->scale;
                if (gActiveSlider >= 0) SliderFromX(gActiveSlider, lx);
                else if (o->dragging) {
                    POINT p; GetCursorPos(&p);
                    o->x = p.x - o->dragOffset.x;
                    o->y = p.y - o->dragOffset.y;
                    SetWindowPos(h, nullptr, o->x, o->y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else gHoverButton = PanelHit(lx, ly);
                return 0;
            }
            if (o->dragging) {
                POINT p; GetCursorPos(&p);
                o->x = p.x - o->dragOffset.x;
                o->y = p.y - o->dragOffset.y;
                SetWindowPos(h, nullptr, o->x, o->y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (o) { o->tracking = false; if (o->type == T_PANEL) gHoverButton = -1; }
            return 0;
        case WM_LBUTTONUP:
            if (!o) return 0;
            if (o->type == T_PANEL) {
                if (gActiveSlider >= 0) { gActiveSlider = -1; SaveSettings(); }
                if (o->dragging) { o->dragging = false; o->savedPos = true; SavePositions(); }
                if (GetCapture() == h) ReleaseCapture();
                return 0;
            }
            if (o->dragging) { o->dragging = false; ReleaseCapture(); }
            else if (o->type == T_BAR && o->pressed) {
                o->pressed = false;
                ReleaseCapture();
                POINT p; GetCursorPos(&p);
                if (PointInGear(p)) TogglePanel();
            }
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool CreateOverlay(Overlay* o, HINSTANCE hi) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = o->w;
    bi.bmiHeader.biHeight = -o->h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    o->mem = CreateCompatibleDC(nullptr);
    o->dib = CreateDIBSection(o->mem, &bi, DIB_RGB_COLORS, &o->bits, nullptr, 0);
    if (!o->dib) return false;
    SelectObject(o->mem, o->dib);
    // GDI+ draws directly into the DIB (premultiplied alpha, as required by UpdateLayeredWindow)
    o->bmp = new Bitmap(o->w, o->h, o->w * 4, PixelFormat32bppPARGB, (BYTE*)o->bits);
    o->g = new Graphics(o->bmp);
    o->g->SetSmoothingMode(SmoothingModeAntiAlias);
    o->g->SetTextRenderingHint(TextRenderingHintAntiAlias);
    DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (o->type != T_PANEL) ex |= WS_EX_TRANSPARENT;   // click-through (the panel is interactive)
    o->hwnd = CreateWindowExW(ex, L"DAO_Overlay", L"", WS_POPUP, o->x, o->y, o->w, o->h,
                              nullptr, nullptr, hi, o);
    return o->hwnd != nullptr;
}

// ------------------------------------------------------------ main loop
static void Tick() {
    static double last = Now();
    static int shotCounter = 0;
    static int shotRev = 0;
    static float targetBalance = 0.f;
    static int topCounter = 0;

    double now = Now();
    float dt = (float)(now - last);
    last = now;
    if (dt < 0.f) dt = 0.f;
    if (dt > 0.1f) dt = 0.1f;

    const float barsFloor = gBarsDb;
    float dbL = gAudio.dbL, dbR = gAudio.dbR;
    if (now - gAudio.lastDataTime > 0.1) dbL = dbR = -100.f;   // no data = silence

    // volume bars (fast attack, smooth release)
    auto frac = [&](float db) { return Clampf((db - barsFloor) / (kBarsMaxDb - barsFloor), 0.f, 1.f); };
    float fL = frac(dbL), fR = frac(dbR);
    float release = 1.f - powf(0.88f, dt * 60.f);
    gBar.smoothL = fL > gBar.smoothL ? fL : gBar.smoothL + (fL - gBar.smoothL) * release;
    gBar.smoothR = fR > gBar.smoothR ? fR : gBar.smoothR + (fR - gBar.smoothR) * release;
    gBar.litL = (int)(gBar.smoothL * kSegments + 0.5f);
    gBar.litR = (int)(gBar.smoothR * kSegments + 0.5f);

    // level shown in the settings panel
    float target = max(max(dbL, dbR), -90.f);
    gBar.levelDb = target > gBar.levelDb ? target : gBar.levelDb + (target - gBar.levelDb) * release;

    // the direction marker moves ONLY when a shot is detected
    int count = gAudio.shotCount;
    if (count != shotCounter) {
        shotCounter = count;
        gBar.lastShotTime = now;   // starts the flash and the marker pulse
    }
    int rev = gAudio.shotRev;
    if (rev != shotRev) {
        shotRev = rev;
        // The track covers -90 .. +90 degrees. The angle comes from the L/R level RATIO
        // (difference in dB), so it does not depend on the sensitivity sliders nor on how
        // loud the shot is. It assumes a constant-power pan law, the most common one in
        // games: L = cos(p), R = sin(p) with p = 0..90 degrees, center = 45 degrees.
        //   equal levels -> 0 deg, only left -> -90 deg, only right -> +90 deg,
        //   e.g. right 10 dB below left -> about 55 deg to the left.
        const float diffDb = Clampf((float)gAudio.shotR - (float)gAudio.shotL, -60.f, 60.f);
        const float p = atanf(powf(10.f, diffDb / 20.f));     // 0 .. pi/2 (amplitude ratio R / L)
        targetBalance = Clampf(p / (0.78539816f) - 1.f, -1.f, 1.f);
        // Flash intensity follows the loudness of the shot (louder = closer = brighter):
        // flash_min_intensity at the detection threshold, 1.0 at kFlashFullDb.
        float loud = max((float)gAudio.shotL, (float)gAudio.shotR);
        float shotFloor = gShotDb;
        float loudness = Clampf((loud - shotFloor) / max(3.f, kFlashFullDb - shotFloor), 0.f, 1.f);
        gBar.flashIntensity = gFlashMin + (1.f - gFlashMin) * loudness;
        if (gDebug) {
            wchar_t buf[128];
            _snwprintf(buf, 128, L"Shot: L %.0f / R %.0f dB -> %.0f deg %ls, flash %.0f%%",
                       (float)gAudio.shotL, (float)gAudio.shotR, fabsf(targetBalance) * 90.f,
                       targetBalance < -0.005f ? L"left" : (targetBalance > 0.005f ? L"right" : L""),
                       gBar.flashIntensity * 100.f);
            SetMessage(buf);
        }
    }
    gBar.balance += (targetBalance - gBar.balance) * (1.f - powf(0.75f, dt * 60.f));

    // the container becomes clickable only while the cursor is over the gear
    bool hover = !gMove && CursorOverGear();
    if (hover != gGearHover) {
        gGearHover = hover;
        SetClickThrough(&gOv[T_BAR], !hover);
    }

    bool active = max(dbL, dbR) > barsFloor || (now - gBar.lastShotTime) < 2.0 || gGearHover || gPanelOpen;
    gBar.fade += ((active ? 1.f : kRestOpacity) - gBar.fade) * (1.f - powf(0.88f, dt * 60.f));

    // --- bar container
    Overlay* bar = &gOv[T_BAR];
    float age = (float)(now - gBar.lastShotTime);
    unsigned pulse = age >= 0.6f ? 12u : (unsigned)(age / 0.6f * 12.f);
    bool messageActive = now < gBar.messageEnd;
    unsigned long long sig =
        (unsigned long long)gBar.litL | ((unsigned long long)gBar.litR << 5) |
        ((unsigned long long)(int)((gBar.balance + 1.f) * 500.f) << 10) |
        ((unsigned long long)(messageActive ? 1 : 0) << 20) |
        ((unsigned long long)pulse << 21) |
        ((unsigned long long)(gMove ? 1 : 0) << 26) |
        ((unsigned long long)((gGearHover || gPanelOpen) ? 1 : 0) << 27) |
        ((unsigned long long)(gBar.serial & 0xFFFFF) << 28);
    Refresh(bar, sig, gMove ? 255 : (int)(gBar.fade * 255.f + 0.5f), now);

    // --- screen-edge indicators: full intensity for 25% of the duration, then fade out
    const double duration = gDuration, hold = duration * 0.25;
    auto fadeOf = [&](double since) -> float {
        if (since < hold) return 1.f;
        if (since < duration) return (float)(1.0 - (since - hold) / (duration - hold));
        return 0.f;
    };
    // With the gradient, the intensity follows the loudness of the shot (louder = closer =
    // stronger), with the same scale as the flash around the bar container.
    auto edgeIntensity = [&](int i) -> float {
        const float loud = gAudio.shotLevel[i];
        const float shotFloor = gShotDb;
        const float loudness = Clampf((loud - shotFloor) / max(3.f, kFlashFullDb - shotFloor), 0.f, 1.f);
        return gFlashMin + (1.f - gFlashMin) * loudness;
    };
    for (int i = 0; i < 3; i++) {
        float v = fadeOf(now - gAudio.shotTime[i]);
        if (gEdgeOn) v *= edgeIntensity(i);
        const unsigned long long esig = (gMove ? 1ULL : 0ULL) | (gEdgeOn ? 2ULL : 0ULL);
        Refresh(&gOv[T_LEFT + i], esig, gMove ? 255 : (int)(v * 255.f + 0.5f), now);
    }

    // --- flash around the bar container: same timing as the edge indicators
    {
        Overlay* fl = &gOv[T_FLASH];
        const int nx = gOv[T_BAR].x - gGlowPx, ny = gOv[T_BAR].y - gGlowPx;
        if (fl->x != nx || fl->y != ny) {   // follow the bar when it is moved
            fl->x = nx; fl->y = ny;
            Present(fl, max(fl->lastAlpha, 0));
        }
        float v = gFlashOn ? fadeOf(now - gBar.lastShotTime) * gBar.flashIntensity : 0.f;
        int alpha = gMove ? (gFlashOn ? 255 : 0) : (int)(v * 255.f + 0.5f);   // move mode: permanent preview
        Refresh(fl, 2ULL, alpha, now);
    }

    // --- sound graph (last kGraphSeconds seconds, scrolling in real time)
    {
        static double lastLoud = -999.0;
        static int graphShown = -1;
        Overlay* gr = &gOv[T_GRAPH];
        if (max(dbL, dbR) > barsFloor) lastLoud = now;
        if ((int)gGraphOn != graphShown) {   // show / hide the window when the switch changes
            graphShown = gGraphOn ? 1 : 0;
            ShowWindow(gr->hwnd, gGraphOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        if (gGraphOn) {
            // While nothing audible happened during the last seconds the graph is a flat
            // line: it is then drawn once instead of every frame (saves CPU).
            const bool idle = now - lastLoud > kGraphSeconds + 0.3;
            const unsigned long long frame = idle ? 0ULL : (unsigned long long)(now * 60.0) + 1ULL;
            const unsigned long long gsig = frame * 1000003ULL
                + (unsigned long long)(int)(-(float)gBarsDb) * 131ULL
                + (unsigned long long)(int)(-(float)gShotDb) * 7ULL
                + (gMove ? 100000000ULL : 0ULL);
            Refresh(gr, gsig, gMove ? 255 : (int)(gBar.fade * 255.f + 0.5f), now);
        }
    }

    // --- Discord chat: always a bit transparent, fully opaque while a new message just arrived
    {
        static int chatShown = -1;
        Overlay* ch = &gOv[T_CHAT];
        const bool chatOn = gChatOn;
        if ((int)chatOn != chatShown) {
            chatShown = chatOn ? 1 : 0;
            ShowWindow(ch->hwnd, chatOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        if (chatOn) {
            const float age = (float)(now - gChatNewTime);
            const float hold = 6.f, fadeTime = 3.f;
            const float k = age < hold ? 1.f : Clampf(1.f - (age - hold) / fadeTime, 0.f, 1.f);
            const float opacity = gChatRest + (1.f - gChatRest) * k;
            const unsigned long long csig = ((unsigned long long)(unsigned)(int)gChatRev << 2) | (gMove ? 1ULL : 0ULL);
            Refresh(ch, csig, gMove ? 255 : (int)(opacity * 255.f + 0.5f), now);
        }
    }

    // --- quick phrases window: always a bit transparent, fully opaque right after a key press
    {
        static int quickShown = -1;
        Overlay* qk = &gOv[T_QUICK];
        const bool qOn = gQuickOn;
        if ((int)qOn != quickShown) {
            quickShown = qOn ? 1 : 0;
            ShowWindow(qk->hwnd, qOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        if (qOn) {
            const bool errActive = now - gQuickErrTime < 6.0;
            const float age = (float)(now - gQuickShowTime);
            const float k = (age < 2.f || errActive) ? 1.f : Clampf(1.f - (age - 2.f) / 1.5f, 0.f, 1.f);
            const float opacity = gQuickRest + (1.f - gQuickRest) * k;
            const int fl = (gQuickFlashIdx >= 0 && now - gQuickFlashTime < 0.7) ? gQuickFlashIdx + 1 : 0;
            const unsigned long long qsig = ((unsigned long long)(unsigned)(int)gQuickRev << 8) |
                                            ((unsigned long long)fl << 3) | (errActive ? 2ULL : 0ULL) | (gMove ? 1ULL : 0ULL);
            Refresh(qk, qsig, gMove ? 255 : (int)(opacity * 255.f + 0.5f), now);
        }
    }

    // --- settings panel
    if (gPanelOpen) Refresh(&gOv[T_PANEL], PanelSignature(), 255, now);

    // live reload of the colors when AudioSight.ini is saved
    static double lastIniCheck = 0.0;
    if (now - lastIniCheck > 1.0) {
        lastIniCheck = now;
        if (IniChanged()) {
            Theme before = gTheme;
            LoadTheme();
            LoadQuick();                 // phrases and keys apply live
            RegisterQuickKeys(false);
            gOv[T_QUICK].sig = ~0ULL;
            if (memcmp(&before, &gTheme, sizeof(Theme)) != 0) { InvalidateAll(); UpdateTrayIcon(); }
        }
    }

    // keep the windows on top of the game
    if (++topCounter >= 120) {
        topCounter = 0;
        for (auto& o : gOv)
            SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static LRESULT CALLBACK MessageProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_TIMER:
            Tick();
            return 0;
        case WM_HOTKEY:
            if (wp >= 20 && wp < 30) QuickSend((int)wp - 20);
            else HotkeyAction((int)wp - 1);
            return 0;
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING | (gPanelOpen ? MF_CHECKED : 0), ID_PANEL, L"Settings (F7)");
                AppendMenuW(menu, MF_STRING | (gMove ? MF_CHECKED : 0), ID_MOVE, L"Move mode (F8)");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_BARS_UP, L"Bars: more sensitive (F9)");
                AppendMenuW(menu, MF_STRING, ID_BARS_DOWN, L"Bars: less sensitive (F10)");
                AppendMenuW(menu, MF_STRING, ID_SHOTS_UP, L"Shots: more sensitive (F11)");
                AppendMenuW(menu, MF_STRING, ID_SHOTS_DOWN, L"Shots: less sensitive (F12)");
                AppendMenuW(menu, MF_STRING | (gFlashOn ? MF_CHECKED : 0), ID_FLASH, L"Shot flash");
                AppendMenuW(menu, MF_STRING | (gGraphOn ? MF_CHECKED : 0), ID_GRAPH, L"Sound graph");
                AppendMenuW(menu, MF_STRING | (gEdgeOn ? MF_CHECKED : 0), ID_EDGE, L"Screen-edge gradient");
                AppendMenuW(menu, MF_STRING | (gChatOn ? MF_CHECKED : 0), ID_CHAT, L"Discord chat");
                AppendMenuW(menu, MF_STRING | (gQuickOn ? MF_CHECKED : 0), ID_QUICK, L"Quick phrases");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit");
                POINT pt; GetCursorPos(&pt);
                SetForegroundWindow(h);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
                PostMessageW(h, WM_NULL, 0, 0);
                DestroyMenu(menu);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_PANEL: TogglePanel(); break;
                case ID_MOVE: ToggleMove(); break;
                case ID_BARS_UP: AdjustBars(-5.f); break;
                case ID_BARS_DOWN: AdjustBars(+5.f); break;
                case ID_SHOTS_UP: AdjustShots(-3.f); break;
                case ID_SHOTS_DOWN: AdjustShots(+3.f); break;
                case ID_FLASH: ToggleFlash(); break;
                case ID_GRAPH: ToggleGraph(); break;
                case ID_EDGE: ToggleEdge(); break;
                case ID_CHAT: ToggleChat(); break;
                case ID_QUICK: ToggleQuick(); break;
                case ID_QUIT: PostQuitMessage(0); break;
            }
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// Notification area icon, drawn at runtime (no resource file needed)
static HICON CreateTrayIcon() {
    Bitmap b(32, 32, PixelFormat32bppARGB);
    Graphics g(&b);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    FillRR(&g, Col(gTheme.trayIconBackground), 0, 0, 32, 32, 7);
    for (int i = 0; i < 4; i++) {
        float h = 5.f + i * 3.f;
        FillRR(&g, Col(gTheme.leftColor), 14.f - i * 3.5f, 16.f - h / 2, 2.6f, h, 1.f);
        FillRR(&g, Col(gTheme.rightColor), 15.4f + i * 3.5f, 16.f - h / 2, 2.6f, h, 1.f);
    }
    HICON ic = nullptr;
    b.GetHICON(&ic);
    return ic;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int) {
    // single instance
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DirectionalAudioOverlay");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"AudioSight is already running (see the notification area icon).",
                    L"AudioSight", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    SetProcessDPIAware();
    Now();  // initialize the clock before starting threads

    // settings file next to the executable
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    gIniPath = exe;
    gIniPath = gIniPath.substr(0, gIniPath.find_last_of(L"\\/") + 1) + L"AudioSight.ini";

    InitializeCriticalSection(&gChatCs);
    gBarsDb = Clampf(IniFloat(L"settings", L"bars_threshold_db", kDefaultBarsDb), -90.f, -35.f);
    gShotDb = Clampf(IniFloat(L"settings", L"shots_threshold_db", kDefaultShotDb), -60.f, -5.f);
    gSuddenness = Clampf(IniFloat(L"settings", L"suddenness_db", kDefaultSuddenness), 4.f, 24.f);
    gCenterZone = Clampf(IniFloat(L"settings", L"center_zone_db", kDefaultCenterZone), 1.f, 10.f);
    gDuration = Clampf(IniFloat(L"settings", L"indicator_duration_s", kDefaultDuration), 0.5f, 3.f);
    gDebug = IniInt(L"settings", L"debug", 0) != 0;   // debug=1 shows each detected shot in the bar
    gFlashOn = IniInt(L"settings", L"flash_enabled", 1) != 0;
    gGraphOn = IniInt(L"settings", L"graph_enabled", 1) != 0;
    gEdgeOn = IniInt(L"settings", L"edge_gradient_enabled", 1) != 0;
    gChatOn = IniInt(L"settings", L"chat_enabled", 0) != 0;
    gQuickOn = IniInt(L"settings", L"quick_enabled", 0) != 0;
    gQuickW = Clampf((float)IniInt(L"settings", L"quick_width", 300), 180.f, 700.f);
    gQuickRest = Clampf(IniFloat(L"settings", L"quick_rest_opacity", 0.5f), 0.1f, 1.f);
    gChatW = Clampf((float)IniInt(L"settings", L"chat_width", 420), 240.f, 1000.f);
    gChatH = Clampf((float)IniInt(L"settings", L"chat_height", 240), 100.f, 800.f);
    gChatRest = Clampf(IniFloat(L"settings", L"chat_rest_opacity", 0.55f), 0.1f, 1.f);
    gFlashMin = Clampf(IniFloat(L"settings", L"flash_min_intensity", kDefaultFlashMin), 0.f, 1.f);
    EnsureColorSection();   // writes the documented [colors] section on first run
    EnsureDiscordSection(); // same for the [discord] section (chat setup)
    EnsureQuickSection();   // and for the [quick] section (quick phrases)
    LoadQuick();
    LoadTheme();
    IniChanged();           // remember the file time (for live reload)
    for (int i = 0; i < 3; i++) { gAudio.shotTime[i] = -999.0; gAudio.shotLevel[i] = -100.f; }

    GdiplusStartupInput gsi;
    ULONG_PTR gdiToken;
    GdiplusStartup(&gdiToken, &gsi, nullptr);
    gFont = new FontFamily(L"Segoe UI");
    if (gFont->GetLastStatus() != Ok) { delete gFont; gFont = FontFamily::GenericSansSerif()->Clone(); }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DAO_Overlay";
    RegisterClassW(&wc);
    WNDCLASSW wm = {};
    wm.lpfnWndProc = MessageProc;
    wm.hInstance = hi;
    wm.lpszClassName = L"DAO_Message";
    RegisterClassW(&wm);

    // --- sizes and default positions (primary screen)
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const float s = sh / 1080.f;                    // indicators scale with the screen height
    const float sb = max(0.75f, s);                 // bar / panel scale (never too small)
    const int thick = (int)(kEdgeDepth * s + 0.5f), length = (int)(kEdgeLength * s + 0.5f);
    const int barW = (int)(kBarW * sb + 0.5f), barH = (int)(kBarH * sb + 0.5f);

    gOv[T_BAR].type = T_BAR;  gOv[T_BAR].name = L"bar";
    gOv[T_BAR].w = barW; gOv[T_BAR].h = barH; gOv[T_BAR].scale = sb;
    // the volume bars (not the gear column) are centered on the screen
    gOv[T_BAR].x = (int)(sw / 2.f - (kContentW / 2) * sb + 0.5f); gOv[T_BAR].y = (int)(70 * s);

    gOv[T_LEFT].type = T_LEFT; gOv[T_LEFT].name = L"edge_left"; gOv[T_LEFT].label = L"L"; gOv[T_LEFT].scale = s;
    gOv[T_LEFT].w = thick; gOv[T_LEFT].h = length; gOv[T_LEFT].x = 0; gOv[T_LEFT].y = (sh - length) / 2;

    gOv[T_RIGHT].type = T_RIGHT; gOv[T_RIGHT].name = L"edge_right"; gOv[T_RIGHT].label = L"R"; gOv[T_RIGHT].scale = s;
    gOv[T_RIGHT].w = thick; gOv[T_RIGHT].h = length; gOv[T_RIGHT].x = sw - thick; gOv[T_RIGHT].y = (sh - length) / 2;

    gOv[T_TOP].type = T_TOP; gOv[T_TOP].name = L"edge_top"; gOv[T_TOP].label = L"TOP"; gOv[T_TOP].scale = s;
    gOv[T_TOP].w = length; gOv[T_TOP].h = thick; gOv[T_TOP].x = (sw - length) / 2; gOv[T_TOP].y = 0;

    gGlowPx = (int)(kFlashGlow * sb + 0.5f);
    gOv[T_FLASH].type = T_FLASH; gOv[T_FLASH].name = L"flash";
    gOv[T_FLASH].w = barW + 2 * gGlowPx; gOv[T_FLASH].h = barH + 2 * gGlowPx; gOv[T_FLASH].scale = sb;

    // sound graph: bottom center of the screen
    gOv[T_GRAPH].type = T_GRAPH; gOv[T_GRAPH].name = L"graph";
    gOv[T_GRAPH].w = (int)(kGraphW * sb + 0.5f); gOv[T_GRAPH].h = (int)(kGraphH * sb + 0.5f); gOv[T_GRAPH].scale = sb;
    gOv[T_GRAPH].x = (sw - gOv[T_GRAPH].w) / 2;
    gOv[T_GRAPH].y = sh - gOv[T_GRAPH].h - (int)(36 * s + 0.5f);

    // quick phrases: bottom right of the screen
    gOv[T_QUICK].type = T_QUICK; gOv[T_QUICK].name = L"quick";
    gOv[T_QUICK].w = (int)(gQuickW * sb + 0.5f); gOv[T_QUICK].h = (int)(kQuickH * sb + 0.5f); gOv[T_QUICK].scale = sb;
    gOv[T_QUICK].x = sw - gOv[T_QUICK].w - (int)(24 * s + 0.5f);
    gOv[T_QUICK].y = sh - gOv[T_QUICK].h - (int)(60 * s + 0.5f);

    // Discord chat: bottom left of the screen
    gOv[T_CHAT].type = T_CHAT; gOv[T_CHAT].name = L"chat";
    gOv[T_CHAT].w = (int)(gChatW * sb + 0.5f); gOv[T_CHAT].h = (int)(gChatH * sb + 0.5f); gOv[T_CHAT].scale = sb;
    gOv[T_CHAT].x = (int)(24 * s + 0.5f);
    gOv[T_CHAT].y = sh - gOv[T_CHAT].h - (int)(60 * s + 0.5f);

    gOv[T_PANEL].type = T_PANEL; gOv[T_PANEL].name = L"panel";
    gOv[T_PANEL].w = (int)(kPanelW * sb + 0.5f); gOv[T_PANEL].h = (int)(kPanelH * sb + 0.5f); gOv[T_PANEL].scale = sb;

    for (auto& o : gOv) { o.defX = o.x; o.defY = o.y; }

    // --- saved positions (only if still on a screen)
    for (auto& o : gOv) {
        if (o.type == T_FLASH) continue;   // follows the bar
        wchar_t kx[32], ky[32];
        _snwprintf(kx, 32, L"%ls_x", o.name);
        _snwprintf(ky, 32, L"%ls_y", o.name);
        bool fx = false, fy = false;
        int x = IniInt(L"positions", kx, o.x, &fx);
        int y = IniInt(L"positions", ky, o.y, &fy);
        if (fx && fy) {
            POINT p = {x + 5, y + 5};
            if (MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) { o.x = x; o.y = y; o.savedPos = true; }
        }
    }

    gOv[T_FLASH].x = gOv[T_BAR].x - gGlowPx;
    gOv[T_FLASH].y = gOv[T_BAR].y - gGlowPx;

    for (auto& o : gOv) {
        if (!CreateOverlay(&o, hi)) {
            MessageBoxW(nullptr, L"Could not create the overlay window.", L"AudioSight",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    // --- hidden message window: timer, hotkeys, notification area icon
    gMsgWnd = CreateWindowExW(0, L"DAO_Message", L"AudioSight", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, hi, nullptr);
    RegisterHotkeys(gMsgWnd);
    RegisterQuickKeys(true);
    gNid.cbSize = sizeof(gNid);
    gNid.hWnd = gMsgWnd;
    gNid.uID = 1;
    gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gNid.uCallbackMessage = WM_TRAY;
    gNid.hIcon = CreateTrayIcon();
    if (!gNid.hIcon) gNid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy(gNid.szTip, L"AudioSight");
    Shell_NotifyIconW(NIM_ADD, &gNid);

    // first draw (the panel stays hidden until the gear is clicked)
    Tick();
    for (auto& o : gOv) if (o.type != T_PANEL && (o.type != T_GRAPH || gGraphOn) && (o.type != T_CHAT || gChatOn) && (o.type != T_QUICK || gQuickOn)) ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);

    HANDLE thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
    gChatWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE chatThread = CreateThread(nullptr, 0, ChatThread, nullptr, 0, nullptr);
    SetTimer(gMsgWnd, 1, 16, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // --- cleanup
    gRun = false;
    SavePositions();
    SaveSettings();
    Shell_NotifyIconW(NIM_DELETE, &gNid);
    for (int i = 1; i <= 6; i++) UnregisterHotKey(gMsgWnd, i);
    for (int i = 20; i < 30; i++) UnregisterHotKey(gMsgWnd, i);
    if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
    if (chatThread) { WaitForSingleObject(chatThread, 3000); CloseHandle(chatThread); }
    for (auto& o : gOv) {
        if (o.hwnd) DestroyWindow(o.hwnd);
        delete o.g;
        delete o.bmp;
        if (o.dib) DeleteObject(o.dib);
        if (o.mem) DeleteDC(o.mem);
    }
    GdiplusShutdown(gdiToken);
    if (mutex) CloseHandle(mutex);
    return 0;
}
