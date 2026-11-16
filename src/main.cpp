// main.cpp — лёгкий плеер для Яндекс Музыки (Windows, без Electron).
//
// Архитектура:
//   Яндекс Музыка = веб-плеер в браузере. Браузер при воспроизведении
//   регистрируется в Windows как SMTC-сессия (см. smtc.h), а этот плеер:
//     1. в фоне опрашивает SMTC (название, обложка, позиция),
//     2. рисует компактное окно на Direct2D,
//     3. по клику/горячей клавише шлёт SMTC-команды Play/Pause/Next/Seek.
//
//   Два режима окна:
//     • Mini     — компактная плашка 460×212, «виджет на рабочем столе».
//     • Expanded — разворачивается с анимацией в полноценное окно Яндекс
//                  Музыки (встроенный WebView2, то есть системный Edge —
//                  никакого Chromium внутри exe).
//
// Сборка: см. README.md и build.bat (CMake + Visual Studio 2022).

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10: GetDpiForWindow, Per-Monitor V2
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <wrl/client.h> // Microsoft::WRL::ComPtr

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings.h"
#include "smtc.h"
#include "volume.h"
#include "webview.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kClassName[] = L"YandexMusicPlayer";
constexpr wchar_t kMusicUrl[] = L"https://music.yandex.ru";
constexpr int kBaseWidth = 460;   // логические пиксели при 96 DPI
constexpr int kBaseHeight = 212;
constexpr int kExpandedWidth = 1180;
constexpr int kExpandedHeight = 820;
constexpr float kTopBarHeight = 44.0f; // шапка развёрнутого окна, лог. px
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_UPDATE = WM_APP + 2;
constexpr UINT_PTR kTimerVolume = 1;
constexpr UINT_PTR kTimerAnim = 2;
constexpr UINT_PTR kTimerTick = 3;

HWND g_hwnd = nullptr;
HINSTANCE g_hInst = nullptr;

std::atomic<bool> g_running{true};
std::mutex g_stateMutex;
MediaState g_state;              // наполняется потоком опроса
std::chrono::steady_clock::time_point g_stateTime{};
MediaState g_uiState;            // копия для UI-потока (без мьютекса)
SmtcClient g_smtc;
Settings g_settings;

float g_volume = 0.8f; // 0..1, мастер-громкость Windows

// Режимы окна и анимация перехода между ними.
enum class Mode { Mini, Expanded };
Mode g_mode = Mode::Mini;
WebViewHost g_webview;

struct Anim {
    bool active = false;
    bool expanding = false;
    RECT from{};
    RECT to{};
    DWORD start = 0;
    DWORD duration = 280;
    float t = 0.0f; // сглаженный прогресс 0..1
};
Anim g_anim;
RECT g_miniRect{}; // куда возвращаться при сворачивании

bool g_desktopWidget = false; // окно прикреплено к рабочему столу (WorkerW)
bool g_topmost = false;

// Мышь: перетаскивание окна и слайдеров.
bool g_dragWindow = false;
bool g_dragVolume = false;
bool g_dragProgress = false;
bool g_hoverProgress = false;
int64_t g_dragSeekMs = 0; // предпросмотр перемотки, пока тянем полоску
POINT g_dragOffset{};
POINT g_lastMouse{};

// Выбор источника ("какого именно процесса слушаем").
bool g_pickerOpen = false;
int g_pickerHover = -1;

bool g_trayAdded = false;

// Длительность трека запоминаем: часть браузеров отдаёт её с задержкой.
int64_t g_knownDuration = 0;
std::wstring g_knownTrackKey;

// ---------------------------------------------------------------- Утилиты
float DpiScale();

int64_t NowUtcTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart);
}

std::wstring TrackKey(const MediaState& st) { return st.title + L"\n" + st.artist; }

float EaseOutCubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

int LerpI(int a, int b, float t) { return a + static_cast<int>(lroundf((b - a) * t)); }

MONITORINFO MonitorInfoOf(HWND hwnd) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                        : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    return mi;
}

// ---------------------------------------------------------------- D2D ресурсы
struct D2DResources {
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> whiteBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> grayBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dimBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> yellowBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> blackBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> trackBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> bgStops;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> bgGradient;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> artistFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> timeFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> noteFormat;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> coverBitmap;
    uint64_t coverBitmapVersion = 0;
    bool coverBitmapEmpty = true; // true = плейсхолдер ещё не нарисован
};

D2DResources g_d2d;
Microsoft::WRL::ComPtr<IWICImagingFactory> g_wicFactory;

float DpiScale() {
    UINT dpi = 96;
    if (g_hwnd) {
        dpi = GetDpiForWindow(g_hwnd);
        if (dpi == 0)
            dpi = 96;
    }
    return static_cast<float>(dpi) / 96.0f;
}

bool CreateDeviceResources() {
    if (g_d2d.rt)
        return true;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory),
                                   reinterpret_cast<void**>(g_d2d.factory.GetAddressOf()));
    if (FAILED(hr))
        return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(g_d2d.dwriteFactory.GetAddressOf()));
    if (FAILED(hr))
        return false;

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    // ALPHA_MODE_IGNORE — единственный режим, который реально работает для
    // HwndRenderTarget (прозрачности у HWND-цели нет, окно рисуем плотным).
    hr = g_d2d.factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
        D2D1::HwndRenderTargetProperties(g_hwnd, size), g_d2d.rt.GetAddressOf());
    if (FAILED(hr))
        return false;

    auto makeBrush = [&hr](const D2D1::ColorF& c,
                           Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& b) {
        hr = g_d2d.rt->CreateSolidColorBrush(c, b.GetAddressOf());
        return SUCCEEDED(hr);
    };
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 1), g_d2d.whiteBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 0.65f), g_d2d.grayBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 0.45f), g_d2d.dimBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1.0f, 0.83f, 0.0f, 1.0f), g_d2d.yellowBrush)) return false;
    if (!makeBrush(D2D1::ColorF(0, 0, 0, 1), g_d2d.blackBrush)) return false;
    if (!makeBrush(D2D1::ColorF(0.14f, 0.14f, 0.17f, 1.0f), g_d2d.bgBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 0.18f), g_d2d.trackBrush)) return false;
    if (!makeBrush(D2D1::ColorF(0.11f, 0.11f, 0.13f, 0.98f), g_d2d.panelBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 0.08f), g_d2d.hoverBrush)) return false;

    // Вертикальный градиент фона: чуть светлее сверху, темнее снизу.
    D2D1_GRADIENT_STOP stops[2]{};
    stops[0].position = 0.0f;
    stops[0].color = D2D1::ColorF(0.16f, 0.16f, 0.19f, 1.0f);
    stops[1].position = 1.0f;
    stops[1].color = D2D1::ColorF(0.07f, 0.07f, 0.09f, 1.0f);
    hr = g_d2d.rt->CreateGradientStopCollection(stops, 2, g_d2d.bgStops.GetAddressOf());
    if (FAILED(hr))
        return false;
    hr = g_d2d.rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
                                            D2D1::Point2F(0, static_cast<float>(size.height))),
        g_d2d.bgStops.Get(), g_d2d.bgGradient.GetAddressOf());
    if (FAILED(hr))
        return false;

    auto makeFormat = [&hr](const wchar_t* font, float size, DWRITE_FONT_WEIGHT w,
                            Microsoft::WRL::ComPtr<IDWriteTextFormat>& f) {
        hr = g_d2d.dwriteFactory->CreateTextFormat(font, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                                   f.GetAddressOf());
        if (FAILED(hr))
            return false;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Обрезка длинных названий с многоточием: SetTrimming требует
        // inline-объект со знаком, иначе текст просто обрежется по краю.
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(g_d2d.dwriteFactory->CreateEllipsisTrimmingSign(
                f.Get(), ellipsis.GetAddressOf()))) {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            trimming.delimiter = 0;
            f->SetTrimming(&trimming, ellipsis.Get());
        }
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return true;
    };

    if (!makeFormat(L"Segoe UI", 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g_d2d.titleFormat)) return false;
    if (!makeFormat(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, g_d2d.artistFormat)) return false;
    if (!makeFormat(L"Segoe UI", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, g_d2d.smallFormat)) return false;
    if (!makeFormat(L"Segoe UI", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, g_d2d.timeFormat)) return false;

    hr = g_d2d.dwriteFactory->CreateTextFormat(
        L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 64.0f, L"", g_d2d.noteFormat.GetAddressOf());
    if (FAILED(hr))
        return false;
    g_d2d.noteFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_d2d.noteFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_d2d.noteFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    return true;
}

void DiscardDeviceResources() {
    g_d2d.rt.Reset();
    g_d2d.whiteBrush.Reset();
    g_d2d.grayBrush.Reset();
    g_d2d.dimBrush.Reset();
    g_d2d.yellowBrush.Reset();
    g_d2d.blackBrush.Reset();
    g_d2d.bgBrush.Reset();
    g_d2d.trackBrush.Reset();
    g_d2d.panelBrush.Reset();
    g_d2d.hoverBrush.Reset();
    g_d2d.bgGradient.Reset();
    g_d2d.bgStops.Reset();
    g_d2d.coverBitmap.Reset();
    g_d2d.coverBitmapVersion = 0;
}

// --------------------------------------------------------------- WIC: обложка
void DecodeCover(const std::vector<uint8_t>& bytes, uint64_t version) {
    g_d2d.coverBitmap.Reset();
    g_d2d.coverBitmapVersion = version;
    g_d2d.coverBitmapEmpty = bytes.empty();
    if (bytes.empty() || !g_wicFactory || !g_d2d.rt)
        return;

    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(g_wicFactory->CreateStream(stream.GetAddressOf())))
        return;
    if (FAILED(stream->InitializeFromMemory(const_cast<uint8_t*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))))
        return;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(g_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                     WICDecodeMetadataCacheOnDemand,
                                                     decoder.GetAddressOf())))
        return;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
        return;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(g_wicFactory->CreateFormatConverter(converter.GetAddressOf())))
        return;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return;

    g_d2d.rt->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                                        g_d2d.coverBitmap.GetAddressOf());
}

// ------------------------------------------------------------------- Layout
struct Layout {
    float scale = 1.0f;
    float w = 0, h = 0;
    D2D1_RECT_F cover{};
    D2D1_RECT_F title{};
    D2D1_RECT_F artist{};
    D2D1_RECT_F album{};
    D2D1_RECT_F source{};
    D2D1_RECT_F sourceBtn{};
    D2D1_RECT_F progress{};
    D2D1_RECT_F timeCur{};
    D2D1_RECT_F timeTotal{};
    D2D1_RECT_F prevBtn{};
    D2D1_RECT_F playBtn{};
    D2D1_RECT_F nextBtn{};
    D2D1_RECT_F volIcon{};
    D2D1_RECT_F volSlider{};
    D2D1_RECT_F minBtn{};
    D2D1_RECT_F closeBtn{};
    D2D1_RECT_F expandBtn{};
    D2D1_RECT_F pinBtn{};
    // Развёрнутый режим.
    D2D1_RECT_F topBar{};
    D2D1_RECT_F collapseBtn{};
    D2D1_RECT_F topTitle{};
};

Layout ComputeMiniLayout() {
    Layout L;
    L.scale = DpiScale();
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    L.w = static_cast<float>(rc.right - rc.left);
    L.h = static_cast<float>(rc.bottom - rc.top);
    const float s = L.scale;
    const float winW = L.w / s;
    auto R = [s](float x, float y, float w, float h) {
        return D2D1_RECT_F{x * s, y * s, (x + w) * s, (y + h) * s};
    };

    const float textX = 152.0f;
    // Справа сверху четыре кнопки (развернуть, закрепить, свернуть, закрыть).
    const float textW = winW - textX - 16.0f - 104.0f;

    L.cover = R(16, 16, 120, 120);
    L.title = R(textX, 18, textW, 24);
    L.artist = R(textX, 46, textW, 20);
    L.album = R(textX, 70, textW, 16);
    L.source = R(textX, 92, textW - 24, 16);
    L.sourceBtn = R(textX + textW - 20, 90, 20, 20);
    L.progress = R(textX, 116, textW, 6);
    L.timeCur = R(textX, 124, 60, 14);
    L.timeTotal = R(textX + textW - 60, 124, 60, 14);

    L.prevBtn = R(textX + 8, 146, 36, 36);
    L.playBtn = R(textX + 52, 140, 48, 48);
    L.nextBtn = R(textX + 108, 146, 36, 36);
    L.volIcon = R(textX + 152, 152, 24, 24);
    L.volSlider = R(textX + 180, 161, textW - 180, 6);

    L.expandBtn = R(winW - 104, 8, 20, 20);
    L.pinBtn = R(winW - 80, 8, 20, 20);
    L.minBtn = R(winW - 56, 8, 20, 20);
    L.closeBtn = R(winW - 32, 8, 20, 20);
    return L;
}

Layout ComputeExpandedLayout() {
    Layout L;
    L.scale = DpiScale();
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    L.w = static_cast<float>(rc.right - rc.left);
    L.h = static_cast<float>(rc.bottom - rc.top);
    const float s = L.scale;
    const float winW = L.w / s;
    auto R = [s](float x, float y, float w, float h) {
        return D2D1_RECT_F{x * s, y * s, (x + w) * s, (y + h) * s};
    };
    L.topBar = R(0, 0, winW, kTopBarHeight);
    L.collapseBtn = R(12, 12, 22, 22);
    L.topTitle = R(44, 12, winW - 44 - 104, 22);
    L.pinBtn = R(winW - 80, 12, 22, 22);
    L.closeBtn = R(winW - 44, 12, 22, 22);
    return L;
}

bool Hit(const D2D1_RECT_F& r, POINT p) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

// Ползунки тонкие (6 px) — расширяем зону попадания по вертикали.
bool HitPadded(const D2D1_RECT_F& r, POINT p, float padY) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top - padY && p.y <= r.bottom + padY;
}

// Список сессий для выбора источника: панель поверх плашки.
struct PickerLayout {
    D2D1_RECT_F panel{};
    std::vector<D2D1_RECT_F> items;
    bool empty = true;
};

PickerLayout ComputePicker() {
    PickerLayout P;
    const Layout L = ComputeMiniLayout();
    const float s = L.scale;
    const size_t count = std::min<size_t>(g_uiState.sources.size(), 4);
    const float rowH = 30.0f;
    const float left = 152.0f;
    const float width = (L.w / s) - left - 16.0f;
    const float top = 86.0f;
    const float height = 8.0f + rowH * static_cast<float>(count ? count : 1);
    P.panel = D2D1::RectF(left * s, top * s, (left + width) * s, (top + height) * s);
    P.empty = count == 0;
    for (size_t i = 0; i < count; ++i) {
        const float y = top + 4.0f + rowH * static_cast<float>(i);
        P.items.push_back(D2D1::RectF((left + 4.0f) * s, y * s, (left + width - 4.0f) * s,
                                      (y + rowH) * s));
    }
    return P;
}

// -------------------------------------------------------------- Время/позиция
std::wstring FormatTime(int64_t ms) {
    if (ms < 0)
        ms = 0;
    const int64_t totalSec = ms / 1000;
    wchar_t buf[32];
    swprintf_s(buf, L"%lld:%02lld", static_cast<long long>(totalSec / 60),
               static_cast<long long>(totalSec % 60));
    return buf;
}

// Позиция сейчас = позиция из SMTC + время с момента её обновления.
// Раньше считалось от момента опроса — из-за этого полоска отставала на
// один-два интервала опроса, а у браузеров, которые обновляют Position редко,
// «прошедшее время» вообще замирало.
int64_t PositionNow(const MediaState& st) {
    if (!st.hasSession)
        return 0;
    int64_t pos = st.positionMs;
    if (st.isPlaying) {
        int64_t elapsedMs = 0;
        if (st.lastUpdatedTicks > 0) {
            const int64_t ticks = NowUtcTicks();
            if (ticks > st.lastUpdatedTicks)
                elapsedMs = (ticks - st.lastUpdatedTicks) / 10000;
        }
        // Совсем старое обновление (пауза, сон системы) — не экстраполируем.
        if (elapsedMs < 0 || elapsedMs > 2 * 60 * 60 * 1000)
            elapsedMs = 0;
        const double rate = (st.playbackRate > 0.05) ? st.playbackRate : 1.0;
        pos += static_cast<int64_t>(llround(static_cast<double>(elapsedMs) * rate));
    }
    if (st.durationMs > 0)
        pos = std::min(pos, st.durationMs);
    return std::max<int64_t>(pos, 0);
}

// --------------------------------------------------------------- Отрисовка
void DrawTextBlock(ID2D1RenderTarget* rt, const wchar_t* text, IDWriteTextFormat* fmt,
                   const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    if (!rt || !text || !fmt || !brush)
        return;
    rt->DrawText(text, static_cast<UINT32>(wcslen(text)), fmt, rect, brush,
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawRoundedRect(const D2D1_ROUNDED_RECT& rr, ID2D1Brush* brush, bool fill) {
    if (fill)
        g_d2d.rt->FillRoundedRectangle(rr, brush);
    else
        g_d2d.rt->DrawRoundedRectangle(rr, brush, 1.0f);
}

void DrawIconPlay(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float h = (r.bottom - r.top) * 0.28f;
    const float w = h * 0.85f;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
    g_d2d.factory->CreatePathGeometry(geo.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    geo->Open(sink.GetAddressOf());
    sink->BeginFigure(D2D1::Point2F(cx - w * 0.4f, cy - h), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(cx - w * 0.4f, cy + h));
    sink->AddLine(D2D1::Point2F(cx + w * 0.8f, cy));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    g_d2d.rt->FillGeometry(geo.Get(), brush);
}

void DrawIconPause(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float h = (r.bottom - r.top) * 0.28f;
    const float bw = h * 0.32f;
    const float gap = h * 0.28f;
    g_d2d.rt->FillRectangle(D2D1::RectF(cx - gap - bw, cy - h, cx - gap, cy + h), brush);
    g_d2d.rt->FillRectangle(D2D1::RectF(cx + gap, cy - h, cx + gap + bw, cy + h), brush);
}

void DrawIconPrev(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float h = (r.bottom - r.top) * 0.26f;
    const float w = h * 0.85f;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
    g_d2d.factory->CreatePathGeometry(geo.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    geo->Open(sink.GetAddressOf());
    sink->BeginFigure(D2D1::Point2F(cx + w * 0.5f, cy - h), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(cx + w * 0.5f, cy + h));
    sink->AddLine(D2D1::Point2F(cx - w * 0.6f, cy));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    g_d2d.rt->FillGeometry(geo.Get(), brush);
    g_d2d.rt->FillRectangle(
        D2D1::RectF(cx - w * 0.9f, cy - h, cx - w * 0.9f + h * 0.18f, cy + h), brush);
}

void DrawIconNext(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float h = (r.bottom - r.top) * 0.26f;
    const float w = h * 0.85f;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
    g_d2d.factory->CreatePathGeometry(geo.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    geo->Open(sink.GetAddressOf());
    sink->BeginFigure(D2D1::Point2F(cx - w * 0.5f, cy - h), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(cx - w * 0.5f, cy + h));
    sink->AddLine(D2D1::Point2F(cx + w * 0.6f, cy));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    g_d2d.rt->FillGeometry(geo.Get(), brush);
    g_d2d.rt->FillRectangle(
        D2D1::RectF(cx + w * 0.9f - h * 0.18f, cy - h, cx + w * 0.9f, cy + h), brush);
}

void DrawIconVolume(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush, bool muted) {
    const float x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
    g_d2d.factory->CreatePathGeometry(geo.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    geo->Open(sink.GetAddressOf());
    sink->BeginFigure(D2D1::Point2F(x + w * 0.1f, y + h * 0.38f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + w * 0.32f, y + h * 0.38f));
    sink->AddLine(D2D1::Point2F(x + w * 0.55f, y + h * 0.18f));
    sink->AddLine(D2D1::Point2F(x + w * 0.55f, y + h * 0.82f));
    sink->AddLine(D2D1::Point2F(x + w * 0.32f, y + h * 0.62f));
    sink->AddLine(D2D1::Point2F(x + w * 0.1f, y + h * 0.62f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    g_d2d.rt->FillGeometry(geo.Get(), brush);
    if (muted) {
        const float cx = x + w * 0.75f, cy = y + h * 0.5f, d = h * 0.16f;
        g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), brush, 1.6f);
        g_d2d.rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), brush, 1.6f);
    } else {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arc1, arc2;
        g_d2d.factory->CreatePathGeometry(arc1.GetAddressOf());
        g_d2d.factory->CreatePathGeometry(arc2.GetAddressOf());
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> s1, s2;
        arc1->Open(s1.GetAddressOf());
        s1->BeginFigure(D2D1::Point2F(x + w * 0.62f, y + h * 0.32f), D2D1_FIGURE_BEGIN_HOLLOW);
        s1->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w * 0.62f, y + h * 0.68f),
                                    D2D1::SizeF(w * 0.28f, h * 0.36f), -35.0f,
                                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s1->EndFigure(D2D1_FIGURE_END_OPEN);
        s1->Close();
        arc2->Open(s2.GetAddressOf());
        s2->BeginFigure(D2D1::Point2F(x + w * 0.66f, y + h * 0.22f), D2D1_FIGURE_BEGIN_HOLLOW);
        s2->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w * 0.66f, y + h * 0.78f),
                                    D2D1::SizeF(w * 0.44f, h * 0.56f), -35.0f,
                                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s2->EndFigure(D2D1_FIGURE_END_OPEN);
        s2->Close();
        g_d2d.rt->DrawGeometry(arc1.Get(), brush, 1.4f);
        g_d2d.rt->DrawGeometry(arc2.Get(), brush, 1.4f);
    }
}

void DrawIconRefresh(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float rad = (r.bottom - r.top) * 0.28f;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
    g_d2d.factory->CreatePathGeometry(geo.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    geo->Open(sink.GetAddressOf());
    sink->BeginFigure(D2D1::Point2F(cx + rad * 0.7f, cy - rad * 0.7f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx - rad * 0.7f, cy + rad * 0.7f),
                                  D2D1::SizeF(rad * 1.4f, rad * 1.4f), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    g_d2d.rt->DrawGeometry(geo.Get(), brush, 1.5f);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> arrow;
    g_d2d.factory->CreatePathGeometry(arrow.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> asink;
    arrow->Open(asink.GetAddressOf());
    asink->BeginFigure(D2D1::Point2F(cx + rad * 1.15f, cy - rad * 1.05f), D2D1_FIGURE_BEGIN_FILLED);
    asink->AddLine(D2D1::Point2F(cx + rad * 0.35f, cy - rad * 1.05f));
    asink->AddLine(D2D1::Point2F(cx + rad * 0.75f, cy - rad * 0.25f));
    asink->EndFigure(D2D1_FIGURE_END_CLOSED);
    asink->Close();
    g_d2d.rt->FillGeometry(arrow.Get(), brush);
}

// Стрелка «развернуть» (в правый нижний угол).
void DrawIconExpand(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    const float d = (r.bottom - r.top) * 0.30f;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), brush, 1.6f);
    g_d2d.rt->DrawLine(D2D1::Point2F(cx + d, cy + d), D2D1::Point2F(cx - d * 0.1f, cy + d), brush, 1.6f);
    g_d2d.rt->DrawLine(D2D1::Point2F(cx + d, cy + d), D2D1::Point2F(cx + d, cy - d * 0.1f), brush, 1.6f);
}

// Стрелка «свернуть» (в левый верхний угол).
void DrawIconCollapse(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    const float d = (r.bottom - r.top) * 0.30f;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), brush, 1.6f);
    g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d * 0.1f, cy - d), brush, 1.6f);
    g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx - d, cy + d * 0.1f), brush, 1.6f);
}

// Булавка: залитая — «поверх всех окон», контур — обычный режим.
void DrawIconPin(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush, bool filled) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float d = (r.bottom - r.top) * 0.26f;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
    g_d2d.factory->CreatePathGeometry(geo.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    geo->Open(sink.GetAddressOf());
    sink->BeginFigure(D2D1::Point2F(cx - d * 0.8f, cy - d), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + d * 0.9f, cy - d),
                                        D2D1::Point2F(cx + d * 0.9f, cy + d * 0.35f),
                                        D2D1::Point2F(cx + d * 0.1f, cy + d * 0.4f)));
    sink->AddLine(D2D1::Point2F(cx + d * 0.1f, cy + d));
    sink->AddLine(D2D1::Point2F(cx - d * 0.1f, cy + d));
    sink->AddLine(D2D1::Point2F(cx - d * 0.1f, cy + d * 0.4f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - d * 0.9f, cy + d * 0.35f),
                                        D2D1::Point2F(cx - d * 0.9f, cy - d),
                                        D2D1::Point2F(cx + d * 0.8f, cy - d)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    if (filled)
        g_d2d.rt->FillGeometry(geo.Get(), brush);
    else
        g_d2d.rt->DrawGeometry(geo.Get(), brush, 1.4f);
}

// Кнопка «×» / «—» и прочие крестики.
void DrawGlyphClose(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush, float scale) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float d = 5.0f * scale;
    g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), brush, 1.6f);
    g_d2d.rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), brush, 1.6f);
}

void DrawGlyphMin(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush, float scale) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float d = 5.0f * scale;
    g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy), D2D1::Point2F(cx + d, cy), brush, 1.6f);
}

// Обложка в прямоугольник с сохранением пропорций (object-fit: cover).
void DrawCoverInto(const D2D1_RECT_F& dst, float radius) {
    const D2D1_ROUNDED_RECT rr{dst, radius, radius};
    if (!g_d2d.coverBitmap) {
        DrawRoundedRect(rr, g_d2d.trackBrush.Get(), true);
        const D2D1_RECT_F noteRect = dst;
        DrawTextBlock(g_d2d.rt.Get(), L"♪", g_d2d.noteFormat.Get(), noteRect,
                      g_d2d.dimBrush.Get());
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1Layer> layer;
    g_d2d.rt->CreateLayer(layer.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geo;
    g_d2d.factory->CreateRoundedRectangleGeometry(rr, geo.GetAddressOf());
    D2D1_LAYER_PARAMETERS params =
        D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_d2d.rt->PushLayer(params, layer.Get());

    const D2D1_SIZE_F bmp = g_d2d.coverBitmap->GetSize();
    const float dstW = dst.right - dst.left;
    const float dstH = dst.bottom - dst.top;
    const float scaleBmp = std::max(dstW / bmp.width, dstH / bmp.height);
    const float drawW = bmp.width * scaleBmp;
    const float drawH = bmp.height * scaleBmp;
    const float dx = dst.left + (dstW - drawW) * 0.5f;
    const float dy = dst.top + (dstH - drawH) * 0.5f;
    g_d2d.rt->DrawBitmap(g_d2d.coverBitmap.Get(), D2D1::RectF(dx, dy, dx + drawW, dy + drawH));

    g_d2d.rt->PopLayer();
}

void DrawBackground() {
    const RECT rc = [] {
        RECT r{};
        GetClientRect(g_hwnd, &r);
        return r;
    }();
    const float w = static_cast<float>(rc.right), h = static_cast<float>(rc.bottom);
    if (g_d2d.bgGradient) {
        g_d2d.bgGradient->SetStartPoint(D2D1::Point2F(0, 0));
        g_d2d.bgGradient->SetEndPoint(D2D1::Point2F(0, h));
        g_d2d.rt->FillRectangle(D2D1::RectF(0, 0, w, h), g_d2d.bgGradient.Get());
    } else {
        g_d2d.rt->FillRectangle(D2D1::RectF(0, 0, w, h), g_d2d.bgBrush.Get());
    }
    const float s = DpiScale();
    g_d2d.rt->DrawLine(D2D1::Point2F(16 * s, 1.0f), D2D1::Point2F(w - 16 * s, 1.0f),
                       g_d2d.dimBrush.Get(), 1.0f);
}

// --------------------------------------------------------------- Мини-режим
void DrawWindowButtons(const Layout& L, bool collapsedStyle); // определение ниже

void RenderPickerOverlay(const Layout& L) {
    const PickerLayout P = ComputePicker();
    const float s = L.scale;
    D2D1_ROUNDED_RECT panel{P.panel, 10.0f * s, 10.0f * s};
    DrawRoundedRect(panel, g_d2d.panelBrush.Get(), true);
    DrawRoundedRect(panel, g_d2d.trackBrush.Get(), false);

    if (P.empty) {
        D2D1_RECT_F r = P.panel;
        r.left += 10 * s;
        r.right -= 10 * s;
        DrawTextBlock(g_d2d.rt.Get(), L"Нет активных сессий — включите музыку в браузере",
                      g_d2d.smallFormat.Get(), r, g_d2d.dimBrush.Get());
        return;
    }

    for (size_t i = 0; i < P.items.size(); ++i) {
        const SourceEntry& src = g_uiState.sources[i];
        if (static_cast<int>(i) == g_pickerHover) {
            D2D1_ROUNDED_RECT hr{P.items[i], 7.0f * s, 7.0f * s};
            DrawRoundedRect(hr, g_d2d.hoverBrush.Get(), true);
        }
        const bool selected = (!g_settings.preferredSource.empty() &&
                               g_settings.preferredSource == src.appId);
        if (selected) {
            const float cy = (P.items[i].top + P.items[i].bottom) * 0.5f;
            g_d2d.rt->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(P.items[i].left + 8 * s, cy), 3.0f * s, 3.0f * s),
                g_d2d.yellowBrush.Get());
        }
        std::wstring line = src.label;
        if (!src.title.empty()) {
            line += L"  ·  ";
            line += src.title;
            if (!src.artist.empty()) {
                line += L" — ";
                line += src.artist;
            }
        }
        D2D1_RECT_F tr = P.items[i];
        tr.left += 16 * s;
        tr.right -= 6 * s;
        DrawTextBlock(g_d2d.rt.Get(), line.c_str(), g_d2d.smallFormat.Get(), tr,
                      src.playing ? g_d2d.whiteBrush.Get() : g_d2d.grayBrush.Get());
    }
}

void RenderMini() {
    const Layout L = ComputeMiniLayout();
    const MediaState& st = g_uiState;
    DrawBackground();
    const float s = L.scale;

    const bool hoverPlay = Hit(L.playBtn, g_lastMouse);
    const bool hoverVol = Hit(L.volIcon, g_lastMouse);
    const bool hoverSource = Hit(L.sourceBtn, g_lastMouse);

    if (!st.hasSession) {
        D2D1_RECT_F noteRect = D2D1::RectF(L.cover.left, L.cover.top + 8 * s, L.cover.right,
                                           L.cover.top + 72 * s);
        DrawTextBlock(g_d2d.rt.Get(), L"♪", g_d2d.noteFormat.Get(), noteRect,
                      g_d2d.dimBrush.Get());
        D2D1_RECT_F msg = D2D1::RectF(L.title.left, 30 * s, L.title.right + 100 * s, 54 * s);
        DrawTextBlock(g_d2d.rt.Get(), L"Яндекс Музыка не играет", g_d2d.titleFormat.Get(), msg,
                      g_d2d.whiteBrush.Get());
        D2D1_RECT_F hint = D2D1::RectF(L.title.left, 56 * s, L.title.right + 100 * s, 100 * s);
        Microsoft::WRL::ComPtr<IDWriteTextFormat> hintFmt;
        g_d2d.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              12.0f, L"", hintFmt.GetAddressOf());
        if (hintFmt) {
            hintFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            hintFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DrawTextBlock(g_d2d.rt.Get(),
                          L"Откройте music.yandex.ru в браузере и нажмите Play — "
                          L"плеер подхватит трек сам.",
                          hintFmt.Get(), hint, g_d2d.grayBrush.Get());
        }
    } else {
        DrawCoverInto(L.cover, 10.0f * s);

        const std::wstring title = st.title.empty() ? L"Без названия" : st.title;
        const std::wstring artist =
            st.artist.empty() ? L"Неизвестный исполнитель" : st.artist;
        DrawTextBlock(g_d2d.rt.Get(), title.c_str(), g_d2d.titleFormat.Get(), L.title,
                      g_d2d.whiteBrush.Get());
        DrawTextBlock(g_d2d.rt.Get(), artist.c_str(), g_d2d.artistFormat.Get(), L.artist,
                      g_d2d.grayBrush.Get());
        const std::wstring sub =
            st.album.empty() ? st.sourceLabel : (st.album + L"  •  " + st.sourceLabel);
        DrawTextBlock(g_d2d.rt.Get(), sub.c_str(), g_d2d.smallFormat.Get(), L.album,
                      g_d2d.dimBrush.Get());
        const std::wstring srcLine = L"Источник: " + st.sourceLabel;
        DrawTextBlock(g_d2d.rt.Get(), srcLine.c_str(), g_d2d.smallFormat.Get(), L.source,
                      g_d2d.dimBrush.Get());
        DrawIconRefresh(L.sourceBtn, hoverSource ? g_d2d.whiteBrush.Get() : g_d2d.dimBrush.Get());

        // --- Прогресс (с предпросмотром при перетаскивании) ---
        const int64_t dur = st.durationMs > 0 ? st.durationMs : g_knownDuration;
        const int64_t pos = g_dragProgress ? g_dragSeekMs : PositionNow(st);
        float frac = (dur > 0) ? static_cast<float>(pos) / static_cast<float>(dur) : 0.0f;
        frac = std::clamp(frac, 0.0f, 1.0f);
        const float progW = L.progress.right - L.progress.left;
        const float progH = L.progress.bottom - L.progress.top;
        D2D1_ROUNDED_RECT progBg{L.progress, progH * 0.5f, progH * 0.5f};
        DrawRoundedRect(progBg, g_d2d.trackBrush.Get(), true);
        if (frac > 0.001f) {
            D2D1_RECT_F fillRect = L.progress;
            fillRect.right = L.progress.left + progW * frac;
            D2D1_ROUNDED_RECT progFg{fillRect, progH * 0.5f, progH * 0.5f};
            DrawRoundedRect(progFg, g_d2d.yellowBrush.Get(), true);
        }
        if (g_hoverProgress || g_dragProgress) {
            const float kx = L.progress.left + progW * frac;
            const float ky = (L.progress.top + L.progress.bottom) * 0.5f;
            g_d2d.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ky), 5.0f * s, 5.0f * s),
                                  g_d2d.yellowBrush.Get());
        }
        DrawTextBlock(g_d2d.rt.Get(), FormatTime(pos).c_str(), g_d2d.timeFormat.Get(), L.timeCur,
                      g_d2d.grayBrush.Get());
        Microsoft::WRL::ComPtr<IDWriteTextFormat> rightFmt;
        g_d2d.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              11.0f, L"", rightFmt.GetAddressOf());
        if (rightFmt) {
            rightFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            rightFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextBlock(g_d2d.rt.Get(), FormatTime(dur).c_str(), rightFmt.Get(), L.timeTotal,
                          g_d2d.dimBrush.Get());
        }
    }

    // --- Транспорт ---
    if (Hit(L.prevBtn, g_lastMouse))
        g_d2d.rt->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F((L.prevBtn.left + L.prevBtn.right) * 0.5f,
                                        (L.prevBtn.top + L.prevBtn.bottom) * 0.5f),
                          18 * s, 18 * s),
            g_d2d.trackBrush.Get());
    DrawIconPrev(L.prevBtn, (st.hasSession && st.canPrev) ? g_d2d.whiteBrush.Get()
                                                          : g_d2d.dimBrush.Get());
    if (Hit(L.nextBtn, g_lastMouse))
        g_d2d.rt->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F((L.nextBtn.left + L.nextBtn.right) * 0.5f,
                                        (L.nextBtn.top + L.nextBtn.bottom) * 0.5f),
                          18 * s, 18 * s),
            g_d2d.trackBrush.Get());
    DrawIconNext(L.nextBtn, (st.hasSession && st.canNext) ? g_d2d.whiteBrush.Get()
                                                          : g_d2d.dimBrush.Get());
    {
        const float pr = (L.playBtn.right - L.playBtn.left) * 0.5f;
        const D2D1_POINT_2F pc{(L.playBtn.left + L.playBtn.right) * 0.5f,
                               (L.playBtn.top + L.playBtn.bottom) * 0.5f};
        g_d2d.rt->FillEllipse(D2D1::Ellipse(pc, pr, pr), g_d2d.yellowBrush.Get());
        if (hoverPlay)
            g_d2d.rt->DrawEllipse(D2D1::Ellipse(pc, pr, pr), g_d2d.whiteBrush.Get(), 1.5f);
        if (st.isPlaying)
            DrawIconPause(L.playBtn, g_d2d.blackBrush.Get());
        else
            DrawIconPlay(L.playBtn, g_d2d.blackBrush.Get());
    }

    // --- Громкость ---
    const bool muted = (g_volume <= 0.001f);
    DrawIconVolume(L.volIcon, hoverVol ? g_d2d.whiteBrush.Get() : g_d2d.grayBrush.Get(), muted);
    {
        const float vw = L.volSlider.right - L.volSlider.left;
        const float vh = L.volSlider.bottom - L.volSlider.top;
        D2D1_ROUNDED_RECT vbg{L.volSlider, vh * 0.5f, vh * 0.5f};
        DrawRoundedRect(vbg, g_d2d.trackBrush.Get(), true);
        if (g_volume > 0.001f) {
            D2D1_RECT_F vf = L.volSlider;
            vf.right = L.volSlider.left + vw * g_volume;
            D2D1_ROUNDED_RECT vfg{vf, vh * 0.5f, vh * 0.5f};
            DrawRoundedRect(vfg, g_d2d.whiteBrush.Get(), true);
        }
    }

    // --- Кнопки окна (развернуть, пин, свернуть, закрыть) ---
    DrawWindowButtons(L, false);

    if (g_pickerOpen)
        RenderPickerOverlay(L);
}

// ---------------------------------------------------------- Отрисовка шапки
void DrawWindowButtons(const Layout& L, bool collapsedStyle) {
    const float s = L.scale;
    const bool hoverClose = Hit(L.closeBtn, g_lastMouse);
    const bool hoverPin = Hit(L.pinBtn, g_lastMouse);

    if (collapsedStyle) {
        const bool hoverCollapse = Hit(L.collapseBtn, g_lastMouse);
        if (hoverCollapse)
            g_d2d.rt->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F((L.collapseBtn.left + L.collapseBtn.right) * 0.5f,
                                            (L.collapseBtn.top + L.collapseBtn.bottom) * 0.5f),
                              14 * s, 14 * s),
                g_d2d.trackBrush.Get());
        DrawIconCollapse(L.collapseBtn,
                         hoverCollapse ? g_d2d.whiteBrush.Get() : g_d2d.grayBrush.Get());
        DrawIconPin(L.pinBtn, g_topmost ? g_d2d.yellowBrush.Get() : g_d2d.dimBrush.Get(),
                    g_topmost);
        DrawGlyphClose(L.closeBtn,
                       hoverClose ? g_d2d.whiteBrush.Get() : g_d2d.dimBrush.Get(), s);
        (void)hoverPin;
        return;
    }
    DrawIconExpand(L.expandBtn, Hit(L.expandBtn, g_lastMouse) ? g_d2d.whiteBrush.Get()
                                                             : g_d2d.dimBrush.Get());
    DrawIconPin(L.pinBtn, g_topmost ? g_d2d.yellowBrush.Get() : g_d2d.dimBrush.Get(), g_topmost);
    DrawGlyphMin(L.minBtn, Hit(L.minBtn, g_lastMouse) ? g_d2d.whiteBrush.Get()
                                                      : g_d2d.dimBrush.Get(), s);
    DrawGlyphClose(L.closeBtn, hoverClose ? g_d2d.whiteBrush.Get() : g_d2d.dimBrush.Get(), s);
}

// -------------------------------------------------- Развёрнутый режим: шапка
void RenderExpandedBar() {
    const Layout L = ComputeExpandedLayout();
    const float s = L.scale;
    // Фон рисуем только в шапке: остальное закрывает встроенный браузер.
    if (g_d2d.bgGradient) {
        g_d2d.bgGradient->SetStartPoint(D2D1::Point2F(0, 0));
        g_d2d.bgGradient->SetEndPoint(D2D1::Point2F(0, L.topBar.bottom));
        g_d2d.rt->FillRectangle(
            D2D1::RectF(0, 0, L.w, L.topBar.bottom), g_d2d.bgGradient.Get());
    } else {
        g_d2d.rt->FillRectangle(D2D1::RectF(0, 0, L.w, L.topBar.bottom), g_d2d.bgBrush.Get());
    }
    g_d2d.rt->DrawLine(D2D1::Point2F(0, L.topBar.bottom - 1),
                       D2D1::Point2F(L.w, L.topBar.bottom - 1), g_d2d.trackBrush.Get(), 1.0f);

    const MediaState& st = g_uiState;
    std::wstring line = L"Яндекс Музыка";
    if (st.hasSession && !st.title.empty()) {
        line += L"  —  ";
        line += st.title;
        if (!st.artist.empty()) {
            line += L" · ";
            line += st.artist;
        }
    }
    DrawTextBlock(g_d2d.rt.Get(), line.c_str(), g_d2d.artistFormat.Get(), L.topTitle,
                  g_d2d.whiteBrush.Get());

    DrawWindowButtons(L, true);
    (void)s;
}

// --------------------------------------------- Кадр перехода между режимами
void RenderTransition(float t) {
    DrawBackground();
    const RECT rc = [&] {
        RECT r{};
        GetClientRect(g_hwnd, &r);
        return r;
    }();
    const float w = static_cast<float>(rc.right), h = static_cast<float>(rc.bottom);

    // Обложка «перетекает» из маленькой в большую и обратно.
    const float s = DpiScale();
    const float miniSize = 120.0f * s;
    const float bigSize = std::min(w, h) * 0.55f;
    const float size = miniSize + (bigSize - miniSize) * t;
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const D2D1_RECT_F coverRect{cx - size * 0.5f, cy - size * 0.5f, cx + size * 0.5f,
                                cy + size * 0.5f};
    DrawCoverInto(coverRect, (10.0f + 14.0f * t) * s);

    (void)w;
    (void)h;
}

// ------------------------------------------------------------------ Draw
void Render() {
    if (!CreateDeviceResources())
        return;
    g_d2d.rt->BeginDraw();
    g_d2d.rt->Clear(D2D1::ColorF(0.07f, 0.07f, 0.09f, 1.0f));

    if (g_anim.active)
        RenderTransition(g_anim.t);
    else if (g_mode == Mode::Expanded)
        RenderExpandedBar();
    else
        RenderMini();

    const HRESULT hr = g_d2d.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
}

// ------------------------------------------------------------ Очередь команд
// SMTC трогаем только из потока опроса: WinRT-объекты живут в MTA, а
// блокирующий .get() в UI-потоке (STA с циклом сообщений) может подвесить окно.
enum class Cmd { PlayPause, Next, Prev, Seek, CycleSource, SelectSource };

struct Command {
    Cmd type;
    int64_t arg = 0;
    std::wstring text;
};

std::mutex g_cmdMutex;
std::condition_variable g_cmdCv;
std::vector<Command> g_cmdQueue;

void PostCommand(Cmd type, int64_t arg = 0, std::wstring text = L"") {
    {
        std::lock_guard<std::mutex> lk(g_cmdMutex);
        g_cmdQueue.push_back({type, arg, std::move(text)});
    }
    g_cmdCv.notify_one();
}

void DoPlayPause() { PostCommand(Cmd::PlayPause); }
void DoNext() { PostCommand(Cmd::Next); }
void DoPrev() { PostCommand(Cmd::Prev); }

// Запасной вариант, если SMTC-команда не прошла: системные медиа-клавиши.
void SendMediaKey(WORD vk) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void ExecuteCommand(const Command& c) {
    switch (c.type) {
    case Cmd::PlayPause:
        if (!g_smtc.PlayPause())
            SendMediaKey(VK_MEDIA_PLAY_PAUSE);
        break;
    case Cmd::Next:
        if (!g_smtc.Next())
            SendMediaKey(VK_MEDIA_NEXT_TRACK);
        break;
    case Cmd::Prev:
        if (!g_smtc.Previous())
            SendMediaKey(VK_MEDIA_PREV_TRACK);
        break;
    case Cmd::Seek:
        g_smtc.Seek(c.arg);
        break;
    case Cmd::CycleSource:
        g_smtc.CycleSource();
        break;
    case Cmd::SelectSource:
        g_smtc.SetPreferredSource(c.text);
        break;
    }
}

// ------------------------------------------------------------ Фон: опрос SMTC
void PollThread() {
    g_smtc.Initialize();
    while (g_running.load()) {
        std::vector<Command> cmds;
        {
            std::lock_guard<std::mutex> lk(g_cmdMutex);
            cmds.swap(g_cmdQueue);
        }
        for (const auto& c : cmds)
            ExecuteCommand(c);

        MediaState st = g_smtc.Poll();
        {
            std::lock_guard<std::mutex> lk(g_stateMutex);
            g_state = std::move(st);
            g_stateTime = std::chrono::steady_clock::now();
        }
        if (g_hwnd)
            PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0);

        std::unique_lock<std::mutex> lk(g_cmdMutex);
        g_cmdCv.wait_for(lk, std::chrono::milliseconds(400), [] {
            return !g_cmdQueue.empty() || !g_running.load();
        });
    }
}

// ------------------------------------------------------------------- Трей
void TrayAdd() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
    if (!nid.hIcon)
        nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Яндекс Музыка — плеер");
    if (Shell_NotifyIconW(NIM_ADD, &nid))
        g_trayAdded = true;
}

void TrayRemove() {
    if (!g_trayAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// Не называем это ShowWindow: так зовётся Win32-функция.
void ShowPlayerWindow(bool show) {
    if (show) {
        ::ShowWindow(g_hwnd, SW_SHOW);
        if (!g_desktopWidget) {
            SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
    } else {
        ::ShowWindow(g_hwnd, SW_HIDE);
    }
}

void SaveWindowPos() {
    if (g_desktopWidget || g_mode != Mode::Mini || g_anim.active)
        return;
    RECT rc{};
    if (!GetWindowRect(g_hwnd, &rc))
        return;
    g_settings.hasPosition = true;
    g_settings.x = rc.left;
    g_settings.y = rc.top;
    settings::Save(g_settings);
}

// --------------------------------------------------- Поверх всех / виджет
bool ApplyDesktopWidget(bool on, bool persist); // определение ниже

void ApplyTopmost(bool on) {
    if (on && g_desktopWidget)
        ApplyDesktopWidget(false, /*persist=*/true);
    g_topmost = on;
    if (g_desktopWidget) // у дочернего окна рабочего стола topmost не работает
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    else
        SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_settings.topmost = on;
    settings::Save(g_settings);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

HWND g_foundWorkerW = nullptr;

BOOL CALLBACK EnumWorkerWProc(HWND top, LPARAM) {
    // Ищем окно, в котором лежит иконка рабочего стола, и берём следующий
    // за ним WorkerW — именно в него «встраиваются» виджеты рабочего стола.
    HWND shell = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (shell) {
        g_foundWorkerW = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        return FALSE;
    }
    return TRUE;
}

// Тот же приём, что используют Rainmeter и обои рабочего стола.
HWND FindDesktopWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
        return nullptr;
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
    g_foundWorkerW = nullptr;
    EnumWindows(EnumWorkerWProc, 0);
    return g_foundWorkerW;
}

bool ApplyDesktopWidget(bool on, bool persist = true) {
    if (on == g_desktopWidget)
        return true;

    if (on) {
        HWND workerw = FindDesktopWorkerW();
        if (!workerw)
            return false;

        RECT rc{};
        if (!GetWindowRect(g_hwnd, &rc))
            return false;
        POINT tl{rc.left, rc.top};
        ScreenToClient(workerw, &tl);

        if (g_topmost) {
            g_topmost = false;
            g_settings.topmost = false;
        }

        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN);
        SetParent(g_hwnd, workerw);
        SetWindowPos(g_hwnd, nullptr, tl.x, tl.y, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_desktopWidget = true;
    } else {
        RECT rc{};
        GetWindowRect(g_hwnd, &rc);
        SetParent(g_hwnd, nullptr);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_TOP, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_desktopWidget = false;
    }

    if (persist) {
        g_settings.desktopWidget = g_desktopWidget;
        settings::Save(g_settings);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return true;
}

// --------------------------------------------------- Разворот и анимация
RECT ExpandedRect() {
    const MONITORINFO mi = MonitorInfoOf(g_hwnd);
    const float s = DpiScale();
    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int workH = mi.rcWork.bottom - mi.rcWork.top;
    int w = static_cast<int>(kExpandedWidth * s);
    int h = static_cast<int>(kExpandedHeight * s);
    w = std::min(w, workW - 80);
    h = std::min(h, workH - 80);
    RECT r{};
    r.left = mi.rcWork.left + (workW - w) / 2;
    r.top = mi.rcWork.top + (workH - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

RECT WebViewBounds() {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    rc.top += static_cast<int>(kTopBarHeight * DpiScale());
    return rc;
}

std::wstring WebViewDataDir() {
    std::wstring path = settings::FilePath(); // ...\settings.ini
    const size_t pos = path.find_last_of(L'\\');
    if (pos != std::wstring::npos)
        path = path.substr(0, pos);
    return path + L"\\WebView2";
}

void EnsureWebView() {
    if (!WebViewHost::Supported())
        return;
    if (g_webview.IsFailed())
        g_webview.ResetFailure();
    const RECT bounds = WebViewBounds();
    g_webview.Create(g_hwnd, bounds, kMusicUrl, WebViewDataDir());
    g_webview.SetBounds(bounds);
    g_webview.SetVisible(true);
    g_webview.Focus();
}

void StartTransition(bool expand); // предварительное объявление

void FinishTransition() {
    if (g_anim.expanding) {
        g_mode = Mode::Expanded;
        if (!WebViewHost::Supported()) {
            // Собрано без WebView2 — открываем страницу в обычном браузере.
            ShellExecuteW(nullptr, L"open", kMusicUrl, nullptr, nullptr, SW_SHOWNORMAL);
            StartTransition(false);
            return;
        }
        EnsureWebView();
    } else {
        g_mode = Mode::Mini;
        g_webview.SetVisible(false);
        if (g_settings.desktopWidget && !g_desktopWidget)
            ApplyDesktopWidget(true, /*persist=*/false);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void TickAnimation() {
    if (!g_anim.active)
        return;
    const DWORD now = GetTickCount();
    float raw = static_cast<float>(now - g_anim.start) / static_cast<float>(g_anim.duration);
    if (raw > 1.0f)
        raw = 1.0f;
    g_anim.t = EaseOutCubic(raw);

    RECT r;
    r.left = LerpI(g_anim.from.left, g_anim.to.left, g_anim.t);
    r.top = LerpI(g_anim.from.top, g_anim.to.top, g_anim.t);
    r.right = LerpI(g_anim.from.right, g_anim.to.right, g_anim.t);
    r.bottom = LerpI(g_anim.from.bottom, g_anim.to.bottom, g_anim.t);
    SetWindowPos(g_hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    if (raw >= 1.0f) {
        g_anim.active = false;
        KillTimer(g_hwnd, kTimerAnim);
        FinishTransition();
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void StartTransition(bool expand) {
    if (g_anim.active)
        return;
    if (expand && g_mode != Mode::Mini)
        return;
    if (!expand && g_mode != Mode::Expanded)
        return;

    g_pickerOpen = false;
    g_dragProgress = false;
    g_dragWindow = false;

    // Встроенный браузер во время анимации прячем: он не умеет полупрозрачность.
    g_webview.SetVisible(false);
    if (expand && g_desktopWidget)
        ApplyDesktopWidget(false, /*persist=*/false);

    RECT from{};
    GetWindowRect(g_hwnd, &from);
    if (expand)
        g_miniRect = from;

    g_anim.active = true;
    g_anim.expanding = expand;
    g_anim.from = from;
    g_anim.to = expand ? ExpandedRect() : g_miniRect;
    g_anim.start = GetTickCount();
    g_anim.duration = expand ? 320 : 260;
    g_anim.t = 0.0f;
    SetTimer(g_hwnd, kTimerAnim, 15, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------ Слайдеры
void SetVolumeFromX(float x) {
    const Layout L = ComputeMiniLayout();
    const float vw = L.volSlider.right - L.volSlider.left;
    if (vw <= 0)
        return;
    const float v = std::clamp((x - L.volSlider.left) / vw, 0.0f, 1.0f);
    g_volume = v;
    volume::SetMaster(v);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

int64_t DurationForUi() {
    return g_uiState.durationMs > 0 ? g_uiState.durationMs : g_knownDuration;
}

void SetSeekFromX(float x) {
    const Layout L = ComputeMiniLayout();
    const float pw = L.progress.right - L.progress.left;
    const int64_t dur = DurationForUi();
    if (pw <= 0 || dur <= 0)
        return;
    const float frac = std::clamp((x - L.progress.left) / pw, 0.0f, 1.0f);
    g_dragSeekMs = static_cast<int64_t>(frac * static_cast<float>(dur));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void CommitSeek() {
    if (!g_dragProgress)
        return;
    g_dragProgress = false;
    if (DurationForUi() > 0)
        PostCommand(Cmd::Seek, g_dragSeekMs);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------- Обновление UI-состояния
void RefreshUiState() {
    MediaState st;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        st = g_state;
    }

    // Длительность: часть браузеров отдаёт её не сразу и иногда сбрасывает в 0 —
    // запоминаем последнее известное значение для текущего трека.
    const std::wstring key = TrackKey(st);
    if (key != g_knownTrackKey) {
        g_knownTrackKey = key;
        g_knownDuration = 0;
    }
    if (st.durationMs > 0)
        g_knownDuration = st.durationMs;
    else if (st.hasSession && g_knownDuration > 0)
        st.durationMs = g_knownDuration;

    g_uiState = std::move(st);

    // WebView2 не смог стартовать (нет рантайма) — открываем браузер и назад.
    if (g_mode == Mode::Expanded && g_webview.IsFailed()) {
        StartTransition(false);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------- Меню трея
void ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Показать / скрыть");
    AppendMenuW(menu, MF_STRING, 2, g_mode == Mode::Mini ? L"Развернуть" : L"Свернуть в плашку");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_topmost ? MF_CHECKED : 0), 3, L"Поверх всех окон");
    AppendMenuW(menu, MF_STRING | (g_desktopWidget ? MF_CHECKED : 0), 4,
                L"Виджет на рабочем столе");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"Выход");

    SetForegroundWindow(g_hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hwnd,
                                  nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case 1:
        ShowPlayerWindow(!IsWindowVisible(g_hwnd));
        break;
    case 2:
        StartTransition(g_mode == Mode::Mini);
        break;
    case 3:
        ApplyTopmost(!g_topmost);
        break;
    case 4:
        if (g_mode != Mode::Mini) {
            StartTransition(false);
            break;
        }
        ApplyDesktopWidget(!g_desktopWidget);
        break;
    case 5:
        DestroyWindow(g_hwnd);
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------- WndProc
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Тёмный заголовок/рамка, скругления Windows 11, тень окна.
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        int corner = 2; // DWMWP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner,
                              sizeof(corner));
        MARGINS margins{1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        TrayAdd();
        RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
        RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_LEFT);
        RegisterHotKey(hwnd, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_RIGHT);
        RegisterHotKey(hwnd, 4, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
        const float v = volume::GetMaster();
        if (v >= 0.0f)
            g_volume = v;
        SetTimer(hwnd, kTimerVolume, 1000, nullptr);
        // Полоска прогресса должна ехать плавно, а не рывками раз в 400 мс.
        SetTimer(hwnd, kTimerTick, 60, nullptr);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_APP_UPDATE:
        RefreshUiState();
        return 0;
    case WM_TIMER: {
        if (wParam == kTimerVolume) {
            const float v = volume::GetMaster();
            if (v >= 0.0f && std::fabs(v - g_volume) > 0.01f) {
                g_volume = v;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (wParam == kTimerAnim) {
            TickAnimation();
        } else if (wParam == kTimerTick) {
            if (g_mode == Mode::Mini && !g_anim.active && !g_dragProgress &&
                g_uiState.isPlaying) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_SIZE: {
        if (g_d2d.rt) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            g_d2d.rt->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        }
        if (g_mode == Mode::Expanded && g_webview.IsReady())
            g_webview.SetBounds(WebViewBounds());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_d2d.rt) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            g_d2d.rt->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        }
        if (g_mode == Mode::Expanded && g_webview.IsReady())
            g_webview.SetBounds(WebViewBounds());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        g_lastMouse.x = GET_X_LPARAM(lParam);
        g_lastMouse.y = GET_Y_LPARAM(lParam);

        if (g_anim.active)
            return 0;

        if (g_dragWindow) {
            POINT cur{};
            GetCursorPos(&cur);
            SetWindowPos(hwnd, nullptr, cur.x - g_dragOffset.x, cur.y - g_dragOffset.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (g_dragVolume) {
            SetVolumeFromX(static_cast<float>(g_lastMouse.x));
            return 0;
        }
        if (g_dragProgress) {
            SetSeekFromX(static_cast<float>(g_lastMouse.x));
            return 0;
        }

        if (g_mode == Mode::Mini) {
            const Layout L = ComputeMiniLayout();
            g_hoverProgress = HitPadded(L.progress, g_lastMouse, 8.0f * L.scale);
            g_pickerHover = -1;
            if (g_pickerOpen) {
                const PickerLayout P = ComputePicker();
                for (size_t i = 0; i < P.items.size(); ++i) {
                    if (Hit(P.items[i], g_lastMouse))
                        g_pickerHover = static_cast<int>(i);
                }
            }
            const bool overBtn =
                Hit(L.playBtn, g_lastMouse) || Hit(L.prevBtn, g_lastMouse) ||
                Hit(L.nextBtn, g_lastMouse) || Hit(L.volIcon, g_lastMouse) ||
                HitPadded(L.volSlider, g_lastMouse, 8.0f * L.scale) ||
                HitPadded(L.progress, g_lastMouse, 8.0f * L.scale) ||
                Hit(L.sourceBtn, g_lastMouse) || Hit(L.source, g_lastMouse) ||
                Hit(L.closeBtn, g_lastMouse) || Hit(L.minBtn, g_lastMouse) ||
                Hit(L.expandBtn, g_lastMouse) || Hit(L.pinBtn, g_lastMouse);
            SetCursor(LoadCursor(nullptr, overBtn ? IDC_HAND : IDC_ARROW));
        } else {
            const Layout L = ComputeExpandedLayout();
            const bool overBtn = Hit(L.collapseBtn, g_lastMouse) || Hit(L.pinBtn, g_lastMouse) ||
                                 Hit(L.closeBtn, g_lastMouse);
            SetCursor(LoadCursor(nullptr, overBtn ? IDC_HAND : IDC_ARROW));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (g_anim.active)
            return 0;
        const POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        g_lastMouse = p;

        if (g_mode == Mode::Expanded) {
            const Layout L = ComputeExpandedLayout();
            if (Hit(L.collapseBtn, p)) {
                StartTransition(false);
                return 0;
            }
            if (Hit(L.pinBtn, p)) {
                ApplyTopmost(!g_topmost);
                return 0;
            }
            if (Hit(L.closeBtn, p)) {
                ShowPlayerWindow(false);
                return 0;
            }
            // Тянем за шапку.
            g_dragWindow = true;
            POINT cur{};
            GetCursorPos(&cur);
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            g_dragOffset.x = cur.x - rc.left;
            g_dragOffset.y = cur.y - rc.top;
            SetCapture(hwnd);
            return 0;
        }

        const Layout L = ComputeMiniLayout();

        // Открытый список источников забирает клик первым.
        if (g_pickerOpen) {
            const PickerLayout P = ComputePicker();
            for (size_t i = 0; i < P.items.size(); ++i) {
                if (Hit(P.items[i], p)) {
                    const std::wstring appId = g_uiState.sources[i].appId;
                    g_settings.preferredSource = appId;
                    settings::Save(g_settings);
                    PostCommand(Cmd::SelectSource, 0, appId);
                    g_pickerOpen = false;
                    g_pickerHover = -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            g_pickerOpen = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (Hit(L.closeBtn, p)) {
            ShowPlayerWindow(false);
            return 0;
        }
        if (Hit(L.minBtn, p)) {
            ShowPlayerWindow(false);
            return 0;
        }
        if (Hit(L.expandBtn, p)) {
            StartTransition(true);
            return 0;
        }
        if (Hit(L.pinBtn, p)) {
            ApplyTopmost(!g_topmost);
            return 0;
        }
        if (Hit(L.playBtn, p)) {
            DoPlayPause();
            return 0;
        }
        if (Hit(L.prevBtn, p)) {
            DoPrev();
            return 0;
        }
        if (Hit(L.nextBtn, p)) {
            DoNext();
            return 0;
        }
        if (Hit(L.volIcon, p)) {
            if (volume::ToggleMute() >= 0) {
                const float v = volume::GetMaster();
                if (v >= 0.0f)
                    g_volume = v;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (HitPadded(L.volSlider, p, 8.0f * L.scale)) {
            g_dragVolume = true;
            SetCapture(hwnd);
            SetVolumeFromX(static_cast<float>(p.x));
            return 0;
        }
        if (HitPadded(L.progress, p, 8.0f * L.scale)) {
            g_dragProgress = true;
            SetCapture(hwnd);
            SetSeekFromX(static_cast<float>(p.x));
            return 0;
        }
        if (Hit(L.sourceBtn, p) || Hit(L.source, p)) {
            g_pickerOpen = !g_pickerOpen;
            g_pickerHover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Иначе — перетаскивание окна.
        g_dragWindow = true;
        POINT cur{};
        GetCursorPos(&cur);
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        g_dragOffset.x = cur.x - rc.left;
        g_dragOffset.y = cur.y - rc.top;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        CommitSeek();
        g_dragWindow = false;
        g_dragVolume = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        if (g_anim.active || g_mode != Mode::Mini)
            return 0;
        const POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const Layout L = ComputeMiniLayout();
        if (Hit(L.cover, p))
            DoPlayPause();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (g_mode != Mode::Mini)
            return 0;
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const float step = (delta > 0) ? 0.05f : -0.05f;
        g_volume = std::clamp(g_volume + step, 0.0f, 1.0f);
        volume::SetMaster(g_volume);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            if (g_pickerOpen) {
                g_pickerOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_mode == Mode::Expanded) {
                StartTransition(false);
            }
        }
        return 0;
    }
    case WM_HOTKEY: {
        if (wParam == 1)
            DoPlayPause();
        else if (wParam == 2)
            DoPrev();
        else if (wParam == 3)
            DoNext();
        else if (wParam == 4) {
            if (volume::ToggleMute() >= 0) {
                const float v = volume::GetMaster();
                if (v >= 0.0f)
                    g_volume = v;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_EXITSIZEMOVE:
        SaveWindowPos();
        return 0;
    case WM_APP_TRAY: {
        const UINT trayMsg = static_cast<UINT>(lParam);
        if (trayMsg == WM_LBUTTONDBLCLK) {
            ShowPlayerWindow(true);
        } else if (trayMsg == WM_RBUTTONUP || trayMsg == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;
    }
    case WM_CLOSE:
        ShowPlayerWindow(false); // в трей, не выходить
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, 1);
        UnregisterHotKey(hwnd, 2);
        UnregisterHotKey(hwnd, 3);
        UnregisterHotKey(hwnd, 4);
        KillTimer(hwnd, kTimerVolume);
        KillTimer(hwnd, kTimerAnim);
        KillTimer(hwnd, kTimerTick);
        g_webview.Destroy();
        SaveWindowPos();
        TrayRemove();
        g_running.store(false);
        g_cmdCv.notify_all();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInst;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Per-Monitor DPI v2 — чтобы окно не мылилось на 125%/150% масштабе.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto pSetCtx = reinterpret_cast<SetCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (pSetCtx)
            pSetCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IWICImagingFactory,
                     reinterpret_cast<void**>(g_wicFactory.GetAddressOf()));

    g_settings = settings::Load();
    g_topmost = g_settings.topmost;

    // SMTC инициализируется в потоке опроса (MTA) — см. PollThread().
    // Выбор источника передаём до старта потока: гонки нет.
    if (!g_settings.preferredSource.empty())
        g_smtc.SetPreferredSource(g_settings.preferredSource);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = nullptr; // рисуем сами через Direct2D
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Не удалось зарегистрировать класс окна.",
                    L"Яндекс Музыка — плеер", MB_ICONERROR);
        return 1;
    }

    // Масштаб системы до создания окна (GetDpiForSystem — Win10 1607+).
    float scale = static_cast<float>(GetDpiForSystem()) / 96.0f;
    if (scale < 1.0f)
        scale = 1.0f;
    const int winW = static_cast<int>(kBaseWidth * scale);
    const int winH = static_cast<int>(kBaseHeight * scale);

    const MONITORINFO mi = MonitorInfoOf(nullptr);
    int x = mi.rcWork.right - winW - 24;
    int y = mi.rcWork.bottom - winH - 24;
    if (g_settings.hasPosition) {
        x = g_settings.x;
        y = g_settings.y;
    }

    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, kClassName, L"Яндекс Музыка — плеер",
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, x, y, winW, winH, nullptr, nullptr, hInst,
        nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Не удалось создать окно.", L"Яндекс Музыка — плеер",
                    MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(g_hwnd, SW_SHOW);
    (void)nCmdShow;
    if (g_topmost)
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g_settings.desktopWidget)
        ApplyDesktopWidget(true);

    std::thread pollThread(PollThread);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    pollThread.join();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
