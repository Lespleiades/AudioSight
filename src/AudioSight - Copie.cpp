// =====================================================================
//  AudioSight: Directional Audio Overlay
//
//  A small Windows overlay that shows, on screen, from which side the
//  sounds of a game are coming (left / right / center). Made as an
//  accessibility aid for players who are deaf in one ear or hard of
//  hearing and cannot localize sounds such as gunshots or footsteps.
//
//  - Captures the system audio output with WASAPI loopback
//  - Draws everything with GDI+ in click-through "layered" windows
//    (per-pixel transparency, always on top)
//  - No dependencies: a single small .exe
//
//  What is displayed:
//    * a volume bar for the left and right channels
//    * a marker that stays on the direction of the last detected shot
//    * three indicators (left / right / top) on the screen edges that
//      flash briefly when a loud, sudden sound (shot) is detected
//    * a gear icon (inside the bar container) that opens the settings
//      panel with sliders and a live level meter
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
static const float  kBalanceFullDb       = 12.0f;   // L/R difference that puts the marker at the end
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
static void SaveSettings() {
    IniWriteFloat(L"settings", L"bars_threshold_db", gBarsDb);
    IniWriteFloat(L"settings", L"shots_threshold_db", gShotDb);
    IniWriteFloat(L"settings", L"suddenness_db", gSuddenness);
    IniWriteFloat(L"settings", L"center_zone_db", gCenterZone);
    IniWriteFloat(L"settings", L"indicator_duration_s", gDuration);
}

// ------------------------------------------------------- shared audio state
// Written by the audio thread, read by the UI thread.
struct AudioState {
    std::atomic<float>  dbL{-100.f}, dbR{-100.f};   // level of the last analysis block
    std::atomic<double> lastDataTime{0.0};          // when audio data last arrived
    std::atomic<double> shotTime[3];                // 0 = left, 1 = right, 2 = top (center)
    std::atomic<float>  lastDiff{0.f};              // R - L (dB) of the last shot
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
        gAudio.lastDiff = diff;
        gAudio.shotL = l;
        gAudio.shotR = r;
        gAudio.shotCount++;
        d.lastShot = t;
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
enum Type { T_BAR = 0, T_LEFT, T_RIGHT, T_TOP, T_PANEL, NB_OV };
static const BYTE BR_ = 180, BG_ = 25, BB_ = 225;    // blue   (left)
static const BYTE OR_ = 40, OG_ = 225, OB_ = 25;    // green (right)

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
    if (bright) { SolidBrush hl(Color(40, 255, 255, 255)); g->FillEllipse(&hl, cx - 15.f, cy - 15.f, 30.f, 30.f); }
    const Color c = bright ? Color(255, 255, 255, 255) : Color(255, 200, 206, 218);
    g->TranslateTransform(cx, cy);
    for (int k = 0; k < 8; k++) {
        g->RotateTransform(45.f);
        FillRR(g, c, -2.0f, -10.0f, 4.0f, 5.5f, 1.1f);
    }
    { SolidBrush b(c); g->FillEllipse(&b, -7.f, -7.f, 14.f, 14.f); }
    g->SetCompositingMode(CompositingModeSourceCopy);   // punch the hole with the background color
    { SolidBrush hole(Color(170, 12, 14, 20)); g->FillEllipse(&hole, -3.f, -3.f, 6.f, 6.f); }
    g->SetCompositingMode(CompositingModeSourceOver);
    g->Restore(st);
}

static void DrawBar(Overlay* o, double now) {
    Graphics* g = o->g;
    g->ResetTransform();
    g->ScaleTransform(o->scale, o->scale);
    const float W = kContentW, H = kBarH;

    // container
    FillRR(g, Color(170, 12, 14, 20), 0.5f, 0.5f, kBarW - 1, H - 1, 14);
    { Pen p(Color(40, 255, 255, 255), 1.f); StrokeRR(g, p, 0.5f, 0.5f, kBarW - 1, H - 1, 14); }

    const float center = W / 2, margin = 18, gap = 12, spacing = 3;
    const float sideWidth = center - margin - gap / 2;
    const float segW = (sideWidth - spacing * (kSegments - 1)) / kSegments;
    const float barY = 16, barH = 28;

    auto side = [&](int lit, BYTE r, BYTE gg, BYTE b, bool right) {
        for (int i = 0; i < kSegments; i++) {
            float off = i * (segW + spacing);
            float x = right ? center + gap / 2 + off : center - gap / 2 - off - segW;
            Color c;
            if (i < lit) {
                float k = 1.f + 0.4f * i / kSegments;   // outer segments are brighter
                c = Color(255, (BYTE)min(255.f, r * k), (BYTE)min(255.f, gg * k), (BYTE)min(255.f, b * k));
            } else {
                c = Color(35, r, gg, b);
            }
            FillRR(g, c, x, barY, segW, barH, 2.5f);
        }
    };
    side(gBar.litL, BR_, BG_, BB_, false);
    side(gBar.litR, OR_, OG_, OB_, true);

    Text(g, L"L", 13, true, Color(255, BR_, BG_, BB_), margin, barY + barH + 4, 20, 16, StringAlignmentNear);
    Text(g, L"R", 13, true, Color(255, OR_, OG_, OB_), W - margin - 20, barY + barH + 4, 20, 16, StringAlignmentFar);

    // direction track + marker (only moves when a shot is detected)
    const float trackY = barY + barH + 13;
    { Pen p(Color(50, 255, 255, 255), 2.f); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
      g->DrawLine(&p, margin + 26, trackY, W - margin - 26, trackY); }
    { Pen p(Color(90, 255, 255, 255), 2.f); g->DrawLine(&p, center, trackY - 5, center, trackY + 5); }

    const float half = (W - 2 * (margin + 26)) / 2;
    const float mx = center + gBar.balance * half;
    const float t = (gBar.balance + 1.f) / 2.f;
    Color markerColor(255, (BYTE)(BR_ * (1 - t) + OR_ * t), (BYTE)(BG_ * (1 - t) + OG_ * t), (BYTE)(BB_ * (1 - t) + OB_ * t));
    float age = (float)(now - gBar.lastShotTime);
    float radius = 6.f + 4.f * max(0.f, 1.f - age / 0.6f);   // the marker swells on every shot
    { SolidBrush sb(markerColor); g->FillEllipse(&sb, mx - radius, trackY - radius, 2 * radius, 2 * radius);
      Pen p(Color(220, 255, 255, 255), 1.5f); g->DrawEllipse(&p, mx - radius, trackY - radius, 2 * radius, 2 * radius); }

    // gear column
    { Pen p(Color(40, 255, 255, 255), 1.f); g->DrawLine(&p, kGearX, 14.f, kGearX, H - 14.f); }
    DrawGearIcon(g, kGearX + (kBarW - kGearX) / 2, H / 2, gGearHover || gPanelOpen);

    if (o->moveMode) {
        Pen p(Color(255, 255, 220, 0), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, kBarW - 4, H - 4, 10);
        Text(g, L"MOVE MODE - drag with the mouse - press F8 to confirm", 11, true,
             Color(255, 255, 220, 0), 0, 1, W, 14, StringAlignmentCenter);
    } else if (now < gBar.messageEnd) {
        Text(g, gBar.message.c_str(), 11, false, Color(230, 255, 255, 255), 0, 1, W, 14, StringAlignmentCenter);
    }
}

static void DrawIndicator(Overlay* o) {
    Graphics* g = o->g;
    g->ResetTransform();
    const float w = (float)o->w - 2, h = (float)o->h - 2;
    if (o->moveMode) {
        FillRR(g, Color(150, 255, 149, 0), 1, 1, w, h, 8);
        Pen p(Color(255, 255, 220, 0), 2.f); p.SetDashStyle(DashStyleDash);
        StrokeRR(g, p, 2, 2, w - 2, h - 2, 8);
        Text(g, o->label, 13, true, Color(255, 255, 255, 255), 0, 0, (float)o->w, (float)o->h, StringAlignmentCenter);
    } else {
        FillRR(g, Color(235, 255, 149, 0), 1, 1, w, h, 8);
        Pen p(Color(120, 255, 255, 255), 1.5f);
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
    BYTE r, g, b;       // accent color
};
static const SliderDef kSliders[5] = {
    {L"Volume bars sensitivity", L"Right: quieter sounds (footsteps...) show up", -35.f, -90.f, 0, L" dB", 0, 200, 255},
    {L"Shot detection sensitivity", L"Right: quieter or more distant shots are detected", -5.f, -60.f, 0, L" dB", 255, 149, 0},
    {L"Sound suddenness (shots)", L"Right: also accepts less abrupt sounds", 24.f, 4.f, 0, L" dB", 255, 149, 0},
    {L"\"Center\" zone (top indicator)", L"Right: more shots are classified as \"top\"", 1.f, 10.f, 0, L" dB", 170, 150, 255},
    {L"Indicator duration", L"How long a shot stays visible on the screen edges", 0.5f, 3.0f, 1, L" s", 170, 150, 255},
};
static const float kPanelW = 380.f, kPanelH = 534.f, kTrackX0 = 18.f, kTrackX1 = 362.f;
static const float kSliderY0 = 52.f, kSliderStep = 64.f;
static const float kBtnX[4] = {18.f, 195.f, 18.f, 195.f}, kBtnY[4] = {446.f, 446.f, 486.f, 486.f};
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

    FillRR(g, Color(240, 14, 16, 24), 0.5f, 0.5f, W - 1, H - 1, 14);
    { Pen p(Color(60, 255, 255, 255), 1.f); StrokeRR(g, p, 0.5f, 0.5f, W - 1, H - 1, 14); }
    Text(g, L"Overlay settings", 15, true, Color(255, 255, 255, 255), 18, 8, 280, 30, StringAlignmentNear);

    // close button
    if (gHoverButton == 4) FillRR(g, Color(255, 80, 42, 52), 336, 9, 26, 26, 7);
    { Pen p(Color(255, 220, 225, 235), 2.f); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
      g->DrawLine(&p, 344.f, 17.f, 354.f, 27.f); g->DrawLine(&p, 354.f, 17.f, 344.f, 27.f); }
    { Pen p(Color(40, 255, 255, 255), 1.f); g->DrawLine(&p, 18.f, 44.f, 362.f, 44.f); }

    // sliders
    for (int i = 0; i < 5; i++) {
        const SliderDef& d = kSliders[i];
        const float y0 = kSliderY0 + i * kSliderStep, ty = y0 + 32.f;
        const Color accent(255, d.r, d.g, d.b);
        Text(g, d.label, 13, false, Color(240, 255, 255, 255), kTrackX0, y0, 260, 20, StringAlignmentNear);
        wchar_t val[48];
        _snwprintf(val, 48, d.decimals ? L"%.1f%ls" : L"%.0f%ls", GetSlider(i), d.unit);
        Text(g, val, 13, true, accent, 200, y0, 162, 20, StringAlignmentFar);

        const float tx = kTrackX0 + SliderT(i) * (kTrackX1 - kTrackX0);
        FillRR(g, Color(255, 52, 56, 76), kTrackX0, ty - 3, kTrackX1 - kTrackX0, 6, 3);
        if (tx - kTrackX0 > 1.f) FillRR(g, accent, kTrackX0, ty - 3, tx - kTrackX0, 6, 3);
        const float r = (i == gActiveSlider) ? 10.f : 8.5f;
        { SolidBrush wb(Color(255, 255, 255, 255)); g->FillEllipse(&wb, tx - r, ty - r, 2 * r, 2 * r);
          Pen p(accent, 2.f); g->DrawEllipse(&p, tx - r, ty - r, 2 * r, 2 * r); }
        Text(g, d.hint, 11, false, Color(255, 150, 158, 175), kTrackX0, y0 + 42, kTrackX1 - kTrackX0, 16, StringAlignmentNear);
    }

    // live level meter
    const float level = gBar.levelDb;
    wchar_t buf[64];
    if (level <= -89.f) _snwprintf(buf, 64, L"Current level: silence");
    else _snwprintf(buf, 64, L"Current level: %.0f dB", level);
    Text(g, buf, 12, false, Color(240, 255, 255, 255), kTrackX0, 378, 180, 18, StringAlignmentNear);
    float lastShot = LastShotDb();
    if (lastShot < -900.f) _snwprintf(buf, 64, L"Last shot: none");
    else _snwprintf(buf, 64, L"Last shot: %.0f dB", lastShot);
    Text(g, buf, 12, false, Color(240, 255, 255, 255), 190, 378, 172, 18, StringAlignmentFar);

    const float meterW = kTrackX1 - kTrackX0, meterY = 402.f;
    auto xOf = [&](float db) { return kTrackX0 + Clampf((db + 90.f) / 90.f, 0.f, 1.f) * meterW; };
    FillRR(g, Color(255, 40, 44, 62), kTrackX0, meterY, meterW, 8, 4);
    float fillW = xOf(level) - kTrackX0;
    if (fillW > 1.f) FillRR(g, level >= gShotDb ? Color(255, 255, 149, 0) : Color(255, 0, 200, 255), kTrackX0, meterY, fillW, 8, 4);
    auto tick = [&](float db, Color c) { Pen p(c, 2.f); float x = xOf(db); g->DrawLine(&p, x, meterY - 4, x, meterY + 12); };
    tick(gBarsDb, Color(255, 0, 200, 255));
    tick(gShotDb, Color(255, 255, 149, 0));
    if (lastShot > -900.f) tick(lastShot, Color(255, 255, 255, 255));
    const float lx[3] = {18.f, 120.f, 225.f};
    const wchar_t* lt[3] = {L"bars threshold", L"shots threshold", L"last shot"};
    const Color lc[3] = {Color(255, 0, 200, 255), Color(255, 255, 149, 0), Color(255, 255, 255, 255)};
    for (int i = 0; i < 3; i++) {
        FillRR(g, lc[i], lx[i], 423.f, 8, 8, 2);
        Text(g, lt[i], 11, false, Color(255, 150, 158, 175), lx[i] + 12, 418, 100, 18, StringAlignmentNear);
    }

    // buttons
    const wchar_t* labels[4] = {gMove ? L"Confirm placement" : L"Move elements",
                                L"Reset positions", L"Reset settings", L"Quit"};
    for (int i = 0; i < 4; i++) {
        const bool hov = gHoverButton == i;
        Color fill = hov ? Color(255, 64, 70, 98) : Color(255, 40, 44, 62);
        Color border(70, 255, 255, 255);
        if (i == 0 && gMove) { fill = Color(255, 90, 78, 20); border = Color(255, 255, 220, 0); }
        if (i == 3) fill = hov ? Color(255, 130, 42, 50) : Color(255, 84, 34, 42);
        FillRR(g, fill, kBtnX[i], kBtnY[i], kBtnW, kBtnH, 8);
        Pen p(border, 1.f);
        StrokeRR(g, p, kBtnX[i], kBtnY[i], kBtnW, kBtnH, 8);
        Text(g, labels[i], 12, false, Color(255, 255, 255, 255), kBtnX[i], kBtnY[i], kBtnW, kBtnH, StringAlignmentCenter);
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
    if (o->type == T_PANEL) return;
    o->moveMode = on;
    SetClickThrough(o, !on);
    o->sig = ~0ULL;
}

// ------------------------------------------------------------ actions / tray
static HWND gMsgWnd = nullptr;
static NOTIFYICONDATAW gNid = {};
static const UINT WM_TRAY = WM_APP + 1;
enum { ID_PANEL = 100, ID_MOVE, ID_BARS_UP, ID_BARS_DOWN, ID_SHOTS_UP, ID_SHOTS_DOWN, ID_QUIT };

static void SavePositions() {
    for (auto& o : gOv) {
        if (o.type == T_PANEL && !o.savedPos) continue;
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

static void ResetSettings() {
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
        if (o.type == T_PANEL) { o.savedPos = false; continue; }
        o.x = o.defX; o.y = o.defY;
        SetWindowPos(o.hwnd, nullptr, o.x, o.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SavePositions();
    SetMessage(L"Positions reset");
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
    if (lx >= 336 && lx <= 362 && ly >= 9 && ly <= 35) return 4;   // close
    for (int i = 0; i < 4; i++)
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
        case 2: ResetSettings(); break;
        case 3: PostQuitMessage(0); break;
        case 4: TogglePanel(); break;
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
        targetBalance = Clampf(gAudio.lastDiff / kBalanceFullDb, -1.f, 1.f);
        gBar.lastShotTime = now;
        if (gDebug) {
            wchar_t buf[128];
            _snwprintf(buf, 128, L"Shot: L %.0f / R %.0f dB (threshold %.0f)",
                       (float)gAudio.shotL, (float)gAudio.shotR, (float)gShotDb);
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
    for (int i = 0; i < 3; i++) {
        double since = now - gAudio.shotTime[i];
        float v;
        if (since < hold) v = 1.f;
        else if (since < duration) v = (float)(1.0 - (since - hold) / (duration - hold));
        else v = 0.f;
        Refresh(&gOv[T_LEFT + i], gMove ? 1ULL : 0ULL, gMove ? 255 : (int)(v * 255.f + 0.5f), now);
    }

    // --- settings panel
    if (gPanelOpen) Refresh(&gOv[T_PANEL], PanelSignature(), 255, now);

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
    FillRR(&g, Color(255, 12, 14, 20), 0, 0, 32, 32, 7);
    for (int i = 0; i < 4; i++) {
        float h = 5.f + i * 3.f;
        FillRR(&g, Color(255, BR_, BG_, BB_), 14.f - i * 3.5f, 16.f - h / 2, 2.6f, h, 1.f);
        FillRR(&g, Color(255, OR_, OG_, OB_), 15.4f + i * 3.5f, 16.f - h / 2, 2.6f, h, 1.f);
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

    gOv[T_PANEL].type = T_PANEL; gOv[T_PANEL].name = L"panel";
    gOv[T_PANEL].w = (int)(kPanelW * sb + 0.5f); gOv[T_PANEL].h = (int)(kPanelH * sb + 0.5f); gOv[T_PANEL].scale = sb;

    for (auto& o : gOv) { o.defX = o.x; o.defY = o.y; }

    // --- saved positions (only if still on a screen)
    for (auto& o : gOv) {
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
