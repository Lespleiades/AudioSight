// =====================================================================
//  AudioSight: Directional Audio Overlay
//
//  Hotkeys (global, they work while the game has focus):
//    F7  : open / close the settings panel
//    F8  : move mode ON/OFF (drag the elements with the mouse)
//    F9  : volume bars more sensitive      F10 : less sensitive
//    F11 : shot detection more sensitive   F12 : less sensitive
//  Notification area icon: click for a menu (including "Quit").
//
//  Build (MinGW-w64):
//    g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++
//        -o AudioSight.exe AudioSight.cpp
//        -lgdiplus -lgdi32 -luser32 -lole32 -lshell32 -luuid
//  Build (Visual Studio, "x64 Native Tools Command Prompt"):
//    cl /O2 /EHsc /MT /DUNICODE /D_UNICODE AudioSight.cpp
//       /link /SUBSYSTEM:WINDOWS gdiplus.lib gdi32.lib user32.lib
//       ole32.lib shell32.lib
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
static float gFlashMin = kDefaultFlashMin;   // flash intensity of the quietest shot
static std::wstring gIniPath;

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
// * To change a default color: edit its value below and rebuild.
// * Users can override any color WITHOUT rebuilding, in the [colors] section
//   of AudioSight.ini (created on first run). Changes apply live as soon
//   as the file is saved.
// * To add a color: add one line here, then use gTheme.<field> where needed.
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
    X(indicator,          L"indicator_fill",        0xEB28E119, "Screen-edge shot indicators (left / right / top)") \
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
    X(trayIconBackground, L"tray_icon_background",  0xFF0C0E14, "Background of the notification area icon")

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
};
static AudioState gAudio;
static std::atomic<bool> gRun(true);

// Shot detector: a shot is a sound that is loud enough AND rises abruptly
// above the slowly-tracked background level.
struct Detector {
    double background = -80.0;
    double lastShot = 0.0;
    double peakEnd = 0.0;    // end of the peak measurement window of the last shot
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
    if (level > shotThreshold && level > d.background + suddenness && t - d.lastShot > kShotCooldown) {
        float diff = r - l;
        int side = diff < -centerZone ? 0 : (diff > centerZone ? 1 : 2);
        gAudio.shotTime[side] = t;
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
enum Type { T_FLASH = 0, T_BAR, T_LEFT, T_RIGHT, T_TOP, T_PANEL, NB_OV };

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

static void DrawIndicator(Overlay* o) {
    Graphics* g = o->g;
    g->ResetTransform();
    const float w = (float)o->w - 2, h = (float)o->h - 2;
    if (o->moveMode) {
        FillRR(g, ColA(gTheme.indicator, 150), 1, 1, w, h, 8);
        Pen p(Col(gTheme.moveMode), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, w - 2, h - 2, 8);
        Text(g, o->label, 13, true, Col(gTheme.indicatorLabel), 0, 0, (float)o->w, (float)o->h, StringAlignmentCenter);
    } else {
        FillRR(g, Col(gTheme.indicator), 1, 1, w, h, 8);
        Pen p(Col(gTheme.indicatorBorder), 1.5f);
        StrokeRR(g, p, 1, 1, w, h, 8);
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
static const int kButtonCount = 6, kCloseButton = 6, kToggleButton = 7;
static const float kToggleY = 376.f, kToggleH = 30.f;   // shot flash switch row
static const float kPanelW = 380.f, kPanelH = 614.f, kTrackX0 = 18.f, kTrackX1 = 362.f;
static const float kSliderY0 = 52.f, kSliderStep = 64.f;
static const float kBtnX[6] = {18.f, 195.f, 18.f, 195.f, 18.f, 195.f};
static const float kBtnY[6] = {486.f, 486.f, 526.f, 526.f, 566.f, 566.f};
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

    // shot flash switch
    {
        if (gHoverButton == kToggleButton)
            FillRR(g, ColA(gTheme.buttonHover, 120), kTrackX0 - 6, kToggleY, kTrackX1 - kTrackX0 + 12, kToggleH, 8);
        Text(g, L"Flash on shots (brighter when closer)", 13, false, Col(gTheme.panelText),
             kTrackX0, kToggleY, 290, kToggleH, StringAlignmentNear);
        const float sw = 40, sh = 20, sx = kTrackX1 - sw, sy = kToggleY + (kToggleH - sh) / 2;
        FillRR(g, Col(gFlashOn ? gTheme.toggleOn : gTheme.toggleOff), sx, sy, sw, sh, 10);
        SolidBrush thumb(Col(gTheme.toggleThumb));
        const float cx = gFlashOn ? sx + sw - 10 : sx + 10;
        g->FillEllipse(&thumb, cx - 8, sy + sh / 2 - 8, 16.f, 16.f);
    }

    // live level meter
    const float level = gBar.levelDb;
    wchar_t buf[64];
    if (level <= -89.f) _snwprintf(buf, 64, L"Current level: silence");
    else _snwprintf(buf, 64, L"Current level: %.0f dB", level);
    Text(g, buf, 12, false, Col(gTheme.panelText), kTrackX0, 418, 180, 18, StringAlignmentNear);
    float lastShot = LastShotDb();
    if (lastShot < -900.f) _snwprintf(buf, 64, L"Last shot: none");
    else _snwprintf(buf, 64, L"Last shot: %.0f dB", lastShot);
    Text(g, buf, 12, false, Col(gTheme.panelText), 190, 418, 172, 18, StringAlignmentFar);

    const float meterW = kTrackX1 - kTrackX0, meterY = 442.f;
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
        FillRR(g, Col(lc[i]), lx[i], 463.f, 8, 8, 2);
        Text(g, lt[i], 11, false, Col(gTheme.panelHint), lx[i] + 12, 458, 100, 18, StringAlignmentNear);
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
enum { ID_PANEL = 100, ID_MOVE, ID_BARS_UP, ID_BARS_DOWN, ID_SHOTS_UP, ID_SHOTS_DOWN, ID_FLASH, ID_QUIT };

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

static void ResetSettings() {
    gFlashOn = true;
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
        // The track covers -90 .. +90 degrees. The position is the difference between
        // the right and left fill levels of the volume bars (same scale as the LEDs):
        // 75% left and 25% right = 50% to the left = 45 degrees to the left.
        float fillL = frac(gAudio.shotL), fillR = frac(gAudio.shotR);
        targetBalance = Clampf(fillR - fillL, -1.f, 1.f);
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
    for (int i = 0; i < 3; i++) {
        float v = fadeOf(now - gAudio.shotTime[i]);
        Refresh(&gOv[T_LEFT + i], gMove ? 1ULL : 0ULL, gMove ? 255 : (int)(v * 255.f + 0.5f), now);
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

    // --- settings panel
    if (gPanelOpen) Refresh(&gOv[T_PANEL], PanelSignature(), 255, now);

    // live reload of the colors when AudioSight.ini is saved
    static double lastIniCheck = 0.0;
    if (now - lastIniCheck > 1.0) {
        lastIniCheck = now;
        if (IniChanged()) {
            Theme before = gTheme;
            LoadTheme();
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
            HotkeyAction((int)wp - 1);
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
        MessageBoxW(nullptr, L"Directional Audio Overlay is already running (see the notification area icon).",
                    L"Directional Audio Overlay", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    SetProcessDPIAware();
    Now();  // initialize the clock before starting threads

    // settings file next to the executable
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    gIniPath = exe;
    gIniPath = gIniPath.substr(0, gIniPath.find_last_of(L"\\/") + 1) + L"AudioSight.ini";

    gBarsDb = Clampf(IniFloat(L"settings", L"bars_threshold_db", kDefaultBarsDb), -90.f, -35.f);
    gShotDb = Clampf(IniFloat(L"settings", L"shots_threshold_db", kDefaultShotDb), -60.f, -5.f);
    gSuddenness = Clampf(IniFloat(L"settings", L"suddenness_db", kDefaultSuddenness), 4.f, 24.f);
    gCenterZone = Clampf(IniFloat(L"settings", L"center_zone_db", kDefaultCenterZone), 1.f, 10.f);
    gDuration = Clampf(IniFloat(L"settings", L"indicator_duration_s", kDefaultDuration), 0.5f, 3.f);
    gDebug = IniInt(L"settings", L"debug", 0) != 0;   // debug=1 shows each detected shot in the bar
    gFlashOn = IniInt(L"settings", L"flash_enabled", 1) != 0;
    gFlashMin = Clampf(IniFloat(L"settings", L"flash_min_intensity", kDefaultFlashMin), 0.f, 1.f);
    EnsureColorSection();   // writes the documented [colors] section on first run
    LoadTheme();
    IniChanged();           // remember the file time (for live reload)
    for (int i = 0; i < 3; i++) gAudio.shotTime[i] = -999.0;

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
    const int thick = (int)(36 * s + 0.5f), length = (int)(220 * s + 0.5f);
    const int barW = (int)(kBarW * sb + 0.5f), barH = (int)(kBarH * sb + 0.5f);

    gOv[T_BAR].type = T_BAR;  gOv[T_BAR].name = L"bar";
    gOv[T_BAR].w = barW; gOv[T_BAR].h = barH; gOv[T_BAR].scale = sb;
    // the volume bars (not the gear column) are centered on the screen
    gOv[T_BAR].x = (int)(sw / 2.f - (kContentW / 2) * sb + 0.5f); gOv[T_BAR].y = (int)(70 * s);

    gOv[T_LEFT].type = T_LEFT; gOv[T_LEFT].name = L"left"; gOv[T_LEFT].label = L"L";
    gOv[T_LEFT].w = thick; gOv[T_LEFT].h = length; gOv[T_LEFT].x = 0; gOv[T_LEFT].y = (sh - length) / 2;

    gOv[T_RIGHT].type = T_RIGHT; gOv[T_RIGHT].name = L"right"; gOv[T_RIGHT].label = L"R";
    gOv[T_RIGHT].w = thick; gOv[T_RIGHT].h = length; gOv[T_RIGHT].x = sw - thick; gOv[T_RIGHT].y = (sh - length) / 2;

    gOv[T_TOP].type = T_TOP; gOv[T_TOP].name = L"top"; gOv[T_TOP].label = L"TOP";
    gOv[T_TOP].w = length; gOv[T_TOP].h = thick; gOv[T_TOP].x = (sw - length) / 2; gOv[T_TOP].y = (int)(6 * s);

    gGlowPx = (int)(kFlashGlow * sb + 0.5f);
    gOv[T_FLASH].type = T_FLASH; gOv[T_FLASH].name = L"flash";
    gOv[T_FLASH].w = barW + 2 * gGlowPx; gOv[T_FLASH].h = barH + 2 * gGlowPx; gOv[T_FLASH].scale = sb;

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
            MessageBoxW(nullptr, L"Could not create the overlay window.", L"Directional Audio Overlay",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    // --- hidden message window: timer, hotkeys, notification area icon
    gMsgWnd = CreateWindowExW(0, L"DAO_Message", L"Directional Audio Overlay", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, hi, nullptr);
    RegisterHotkeys(gMsgWnd);
    gNid.cbSize = sizeof(gNid);
    gNid.hWnd = gMsgWnd;
    gNid.uID = 1;
    gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gNid.uCallbackMessage = WM_TRAY;
    gNid.hIcon = CreateTrayIcon();
    if (!gNid.hIcon) gNid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy(gNid.szTip, L"Directional Audio Overlay (click: menu)");
    Shell_NotifyIconW(NIM_ADD, &gNid);

    // first draw (the panel stays hidden until the gear is clicked)
    Tick();
    for (auto& o : gOv) if (o.type != T_PANEL) ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);

    HANDLE thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
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
    if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
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
