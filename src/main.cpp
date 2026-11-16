// main.cpp — лёгкий плеер для Яндекс Музыки (Windows, без Electron).
//
// Архитектура:
//   Яндекс Музыка = веб-плеер в браузере. Браузер при воспроизведении
//   регистрируется в Windows как SMTC-сессия (см. smtc.h), а этот плеер:
//     1. в фоне опрашивает SMTC (название, обложка, позиция),
//     2. рисует компактное окно на Direct2D (акрил, скругления),
//     3. по клику/горячей клавише шлёт SMTC-команды Play/Pause/Next/Seek.
//   Никаких рантаймов: один .exe на Win32 + Direct2D + WinRT.
//   Размер ~0.5–1 МБ, потребление памяти — единицы мегабайт.
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
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

#include "smtc.h"
#include "volume.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kClassName[] = L"YandexMusicPlayer";
constexpr int kBaseWidth = 460;   // логические пиксели при 96 DPI
constexpr int kBaseHeight = 212;
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_UPDATE = WM_APP + 2;

HWND g_hwnd = nullptr;
HINSTANCE g_hInst = nullptr;

std::atomic<bool> g_running{true};
std::mutex g_stateMutex;
MediaState g_state;
std::chrono::steady_clock::time_point g_stateTime{};
SmtcClient g_smtc;

float g_volume = 0.8f; // 0..1, мастер-громкость Windows

// Состояние мыши для перетаскивания окна и слайдеров.
bool g_dragWindow = false;
bool g_dragVolume = false;
POINT g_dragOffset{};
POINT g_lastMouse{};
bool g_hoverProgress = false;

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

bool CreateDeviceResources() {
    if (g_d2d.rt)
        return true;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory),
                                   reinterpret_cast<void**>(g_d2d.factory.GetAddressOf()));
    if (FAILED(hr))
        return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
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
        D2D1::HwndRenderTargetProperties(g_hwnd, size),
        g_d2d.rt.GetAddressOf());
    if (FAILED(hr))
        return false;

    auto makeBrush = [&hr](const D2D1::ColorF& c, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& b) {
        hr = g_d2d.rt->CreateSolidColorBrush(c, b.GetAddressOf());
        return SUCCEEDED(hr);
    };
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 1), g_d2d.whiteBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 0.65f), g_d2d.grayBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 0.45f), g_d2d.dimBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1.0f, 0.83f, 0.0f, 1.0f), g_d2d.yellowBrush)) return false; // Яндекс-жёлтый
    if (!makeBrush(D2D1::ColorF(0, 0, 0, 1), g_d2d.blackBrush)) return false;
    if (!makeBrush(D2D1::ColorF(0.14f, 0.14f, 0.17f, 1.0f), g_d2d.bgBrush)) return false;
    if (!makeBrush(D2D1::ColorF(1, 1, 1, 0.18f), g_d2d.trackBrush)) return false;

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
        hr = g_d2d.dwriteFactory->CreateTextFormat(
            font, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", f.GetAddressOf());
        if (FAILED(hr))
            return false;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Обрезка длинных названий с многоточием. IDWriteTextFormat::
        // SetTrimming требует inline-объект с самим знаком, иначе он не
        // рисуется (текст просто обрежется по краю).
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(g_d2d.dwriteFactory->CreateEllipsisTrimmingSign(
                f.Get(), ellipsis.GetAddressOf()))) {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            trimming.delimiter = 0;
            trimming.sign = 0x2026; // символ «…» — просто кодовая точка
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
        L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 64.0f, L"",
        g_d2d.noteFormat.GetAddressOf());
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
    g_d2d.bgGradient.Reset();
    g_d2d.bgStops.Reset();
    g_d2d.coverBitmap.Reset();
    g_d2d.coverBitmapVersion = 0;
}

// --------------------------------------------------------------- WIC: обложка
// Декодировать JPEG/PNG из памяти в D2D-битмап. Вызывать только из UI-потока.
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
    if (FAILED(g_wicFactory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
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
};

float DpiScale() {
    UINT dpi = 96;
    if (g_hwnd) {
        dpi = GetDpiForWindow(g_hwnd);
        if (dpi == 0)
            dpi = 96;
    }
    return static_cast<float>(dpi) / 96.0f;
}

Layout ComputeLayout() {
    Layout L;
    L.scale = DpiScale();
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    L.w = static_cast<float>(rc.right - rc.left);
    L.h = static_cast<float>(rc.bottom - rc.top);
    const float s = L.scale;
    // Все размеры ниже — в логических пикселях (96 DPI).
    const float winW = L.w / s;
    auto R = [s](float x, float y, float w, float h) {
        return D2D1_RECT_F{x * s, y * s, (x + w) * s, (y + h) * s};
    };

    const float textX = 152.0f;
    // Колонка текста: до кнопок свернуть/закрыть (они занимают 52 px справа).
    const float textW = winW - textX - 16.0f - 52.0f;

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

    L.minBtn = R(winW - 56, 8, 20, 20);
    L.closeBtn = R(winW - 32, 8, 20, 20);
    return L;
}

bool Hit(const D2D1_RECT_F& r, POINT p) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

// Ползунки тонкие (6 px), попасть в них мышью сложно — расширяем зону
// попадания по вертикали.
bool HitPadded(const D2D1_RECT_F& r, POINT p, float padY) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top - padY && p.y <= r.bottom + padY;
}

std::wstring FormatTime(int64_t ms) {
    if (ms < 0)
        ms = 0;
    int64_t totalSec = ms / 1000;
    int64_t m = totalSec / 60;
    int64_t sec = totalSec % 60;
    wchar_t buf[32];
    swprintf_s(buf, L"%lld:%02lld", static_cast<long long>(m),
               static_cast<long long>(sec));
    return buf;
}

// Экстраполированная позиция: SMTC опрашивается раз в 400 мс, а ползунок
// должен ехать плавно — добавляем прошедшее время, если играет.
int64_t CurrentPositionMs(const MediaState& st) {
    int64_t pos = st.positionMs;
    if (st.isPlaying && st.durationMs > 0) {
        auto now = std::chrono::steady_clock::now();
        int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - g_stateTime)
                              .count();
        pos += elapsed;
        if (pos > st.durationMs)
            pos = st.durationMs;
    }
    return pos;
}

// --------------------------------------------------------------- Отрисовка
void DrawIconPlay(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float h = (r.bottom - r.top) * 0.28f;
    float w = h * 0.85f;
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
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float h = (r.bottom - r.top) * 0.28f;
    float bw = h * 0.32f;
    float gap = h * 0.28f;
    g_d2d.rt->FillRectangle(
        D2D1::RectF(cx - gap - bw, cy - h, cx - gap, cy + h), brush);
    g_d2d.rt->FillRectangle(
        D2D1::RectF(cx + gap, cy - h, cx + gap + bw, cy + h), brush);
}

void DrawIconPrev(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float h = (r.bottom - r.top) * 0.26f;
    float w = h * 0.85f;
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
    g_d2d.rt->FillRectangle(D2D1::RectF(cx - w * 0.9f, cy - h, cx - w * 0.9f + h * 0.18f, cy + h), brush);
}

void DrawIconNext(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float h = (r.bottom - r.top) * 0.26f;
    float w = h * 0.85f;
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
    g_d2d.rt->FillRectangle(D2D1::RectF(cx + w * 0.9f - h * 0.18f, cy - h, cx + w * 0.9f, cy + h), brush);
}

void DrawIconVolume(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush, bool muted) {
    float x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
    // Корпус динамика.
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
        // Крестик вместо волн.
        float cx = x + w * 0.75f, cy = y + h * 0.5f, d = h * 0.16f;
        g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), brush, 1.6f);
        g_d2d.rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), brush, 1.6f);
    } else {
        // Две дуги-волны.
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arc1, arc2;
        g_d2d.factory->CreatePathGeometry(arc1.GetAddressOf());
        g_d2d.factory->CreatePathGeometry(arc2.GetAddressOf());
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> s1, s2;
        arc1->Open(s1.GetAddressOf());
        s1->BeginFigure(D2D1::Point2F(x + w * 0.62f, y + h * 0.32f), D2D1_FIGURE_BEGIN_HOLLOW);
        s1->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w * 0.62f, y + h * 0.68f),
                                   D2D1::SizeF(w * 0.28f, h * 0.36f), -35.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s1->EndFigure(D2D1_FIGURE_END_OPEN);
        s1->Close();
        arc2->Open(s2.GetAddressOf());
        s2->BeginFigure(D2D1::Point2F(x + w * 0.66f, y + h * 0.22f), D2D1_FIGURE_BEGIN_HOLLOW);
        s2->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w * 0.66f, y + h * 0.78f),
                                   D2D1::SizeF(w * 0.44f, h * 0.56f), -35.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s2->EndFigure(D2D1_FIGURE_END_OPEN);
        s2->Close();
        g_d2d.rt->DrawGeometry(arc1.Get(), brush, 1.4f);
        g_d2d.rt->DrawGeometry(arc2.Get(), brush, 1.4f);
    }
}

void DrawIconRefresh(const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float rad = (r.bottom - r.top) * 0.28f;
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
    // Стрелка.
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

void DrawRoundedRect(const D2D1_ROUNDED_RECT& rr, ID2D1Brush* brush, bool fill) {
    if (fill)
        g_d2d.rt->FillRoundedRectangle(rr, brush);
    else
        g_d2d.rt->DrawRoundedRectangle(rr, brush, 1.0f);
}

// ID2D1RenderTarget::DrawText не имеет перегрузки без длины строки,
// поэтому оборачиваем вызов: длину считаем сами.
void DrawTextBlock(ID2D1RenderTarget* rt, const wchar_t* text, IDWriteTextFormat* fmt,
                   const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    if (!rt || !text || !fmt || !brush)
        return;
    rt->DrawText(text, static_cast<UINT32>(wcslen(text)), fmt, rect, brush,
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Render() {
    if (!CreateDeviceResources())
        return;
    Layout L = ComputeLayout();

    MediaState st;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        st = g_state;
    }

    // Обложка: передекодировать, если версия сменилась.
    if (st.coverVersion != g_d2d.coverBitmapVersion || g_d2d.coverBitmapEmpty != st.coverBytes.empty()) {
        DecodeCover(st.coverBytes, st.coverVersion);
    }

    g_d2d.rt->BeginDraw();
    g_d2d.rt->Clear(D2D1::ColorF(0.07f, 0.07f, 0.09f, 1.0f));

    float s = L.scale;
    // Фон окна: вертикальный градиент на весь клиент.
    if (g_d2d.bgGradient) {
        g_d2d.bgGradient->SetStartPoint(D2D1::Point2F(0, 0));
        g_d2d.bgGradient->SetEndPoint(D2D1::Point2F(0, L.h));
        g_d2d.rt->FillRectangle(D2D1::RectF(0, 0, L.w, L.h), g_d2d.bgGradient.Get());
    } else {
        g_d2d.rt->FillRectangle(D2D1::RectF(0, 0, L.w, L.h), g_d2d.bgBrush.Get());
    }
    // Тонкая светлая линия сверху — эффект "стекла".
    g_d2d.rt->DrawLine(D2D1::Point2F(16 * s, 1.0f), D2D1::Point2F(L.w - 16 * s, 1.0f),
                       g_d2d.dimBrush.Get(), 1.0f);

    bool hoverPlay = Hit(L.playBtn, g_lastMouse);
    bool hoverPrev = Hit(L.prevBtn, g_lastMouse);
    bool hoverNext = Hit(L.nextBtn, g_lastMouse);
    bool hoverVol = Hit(L.volIcon, g_lastMouse);
    bool hoverSource = Hit(L.sourceBtn, g_lastMouse);
    bool hoverClose = Hit(L.closeBtn, g_lastMouse);
    bool hoverMin = Hit(L.minBtn, g_lastMouse);

    if (!st.hasSession) {
        // Пустое состояние: подсказка.
        D2D1_RECT_F noteRect = D2D1::RectF(L.cover.left, L.cover.top + 8 * s,
                                           L.cover.right, L.cover.top + 72 * s);
        DrawTextBlock(g_d2d.rt.Get(), L"♪", g_d2d.noteFormat.Get(), noteRect,
                           g_d2d.dimBrush.Get());
        D2D1_RECT_F msg = D2D1::RectF(L.title.left, 30 * s, L.title.right, 54 * s);
        DrawTextBlock(g_d2d.rt.Get(), L"Яндекс Музыка не играет", g_d2d.titleFormat.Get(), msg,
                           g_d2d.whiteBrush.Get());
        D2D1_RECT_F hint = D2D1::RectF(L.title.left, 56 * s, L.title.right, 100 * s);
        // Многострочная подсказка: временный формат с переносом.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> hintFmt;
        g_d2d.dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"", hintFmt.GetAddressOf());
        if (hintFmt) {
            hintFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            hintFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_LEADING);
            DrawTextBlock(g_d2d.rt.Get(),
                L"Откройте music.yandex.ru в браузере и нажмите Play — "
                L"плеер подхватит трек автоматически.",
                hintFmt.Get(), hint, g_d2d.grayBrush.Get());
        }
    } else {
        // --- Обложка (со скруглением через клип) ---
        D2D1_ROUNDED_RECT coverRR{L.cover, 10.0f * s, 10.0f * s};
        if (g_d2d.coverBitmap) {
            Microsoft::WRL::ComPtr<ID2D1Layer> layer;
            g_d2d.rt->CreateLayer(layer.GetAddressOf());
            Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geo;
            g_d2d.factory->CreateRoundedRectangleGeometry(coverRR, geo.GetAddressOf());
            layer->PushClip(geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                            D2D1::Point2F(0, 0));
            D2D1_SIZE_F bmpSize = g_d2d.coverBitmap->GetSize();
            // Вписать битмап в квадрат обложки с сохранением пропорций (cover).
            float dstW = L.cover.right - L.cover.left;
            float dstH = L.cover.bottom - L.cover.top;
            float scaleBmp = std::max(dstW / bmpSize.width, dstH / bmpSize.height);
            float drawW = bmpSize.width * scaleBmp;
            float drawH = bmpSize.height * scaleBmp;
            float dx = L.cover.left + (dstW - drawW) * 0.5f;
            float dy = L.cover.top + (dstH - drawH) * 0.5f;
            g_d2d.rt->DrawBitmap(g_d2d.coverBitmap.Get(),
                                 D2D1::RectF(dx, dy, dx + drawW, dy + drawH));
            layer->PopClip();
        } else {
            // Плейсхолдер: тёмный квадрат + нота.
            DrawRoundedRect(coverRR, g_d2d.trackBrush.Get(), true);
            D2D1_RECT_F noteRect = L.cover;
            DrawTextBlock(g_d2d.rt.Get(), L"♪", g_d2d.noteFormat.Get(), noteRect,
                               g_d2d.dimBrush.Get());
        }

        // --- Текст ---
        std::wstring title = st.title.empty() ? L"Без названия" : st.title;
        std::wstring artist = st.artist.empty() ? L"Неизвестный исполнитель" : st.artist;
        DrawTextBlock(g_d2d.rt.Get(), title.c_str(), g_d2d.titleFormat.Get(), L.title,
                           g_d2d.whiteBrush.Get());
        DrawTextBlock(g_d2d.rt.Get(), artist.c_str(), g_d2d.artistFormat.Get(), L.artist,
                           g_d2d.grayBrush.Get());
        std::wstring sub = st.album.empty() ? st.sourceLabel : (st.album + L"  •  " + st.sourceLabel);
        DrawTextBlock(g_d2d.rt.Get(), sub.c_str(), g_d2d.smallFormat.Get(), L.album,
                           g_d2d.dimBrush.Get());
        std::wstring srcLine = L"Источник: " + st.sourceLabel;
        DrawTextBlock(g_d2d.rt.Get(), srcLine.c_str(), g_d2d.smallFormat.Get(), L.source,
                           g_d2d.dimBrush.Get());
        DrawIconRefresh(L.sourceBtn, hoverSource ? g_d2d.whiteBrush.Get() : g_d2d.dimBrush.Get());

        // --- Прогресс ---
        int64_t pos = CurrentPositionMs(st);
        float frac = (st.durationMs > 0)
                         ? static_cast<float>(pos) / static_cast<float>(st.durationMs)
                         : 0.0f;
        frac = std::clamp(frac, 0.0f, 1.0f);
        float progW = L.progress.right - L.progress.left;
        float progH = L.progress.bottom - L.progress.top;
        D2D1_ROUNDED_RECT progBg{L.progress, progH * 0.5f, progH * 0.5f};
        DrawRoundedRect(progBg, g_d2d.trackBrush.Get(), true);
        if (frac > 0.001f) {
            D2D1_RECT_F fillRect = L.progress;
            fillRect.right = L.progress.left + progW * frac;
            D2D1_ROUNDED_RECT progFg{fillRect, progH * 0.5f, progH * 0.5f};
            DrawRoundedRect(progFg, g_d2d.yellowBrush.Get(), true);
        }
        if (g_hoverProgress) {
            float kx = L.progress.left + progW * frac;
            float ky = (L.progress.top + L.progress.bottom) * 0.5f;
            g_d2d.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ky), 5.0f * s, 5.0f * s),
                                  g_d2d.yellowBrush.Get());
        }
        std::wstring cur = FormatTime(pos);
        std::wstring tot = FormatTime(st.durationMs);
        DrawTextBlock(g_d2d.rt.Get(), cur.c_str(), g_d2d.timeFormat.Get(), L.timeCur,
                           g_d2d.grayBrush.Get());
        // Время total — по правому краю.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> rightFmt;
        g_d2d.dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"", rightFmt.GetAddressOf());
        if (rightFmt) {
            rightFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            rightFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextBlock(g_d2d.rt.Get(), tot.c_str(), rightFmt.Get(), L.timeTotal,
                          g_d2d.dimBrush.Get());
        }
    }

    // --- Кнопки транспорта ---
    // Prev
    if (hoverPrev)
        g_d2d.rt->FillEllipse(D2D1::Ellipse(
                                  D2D1::Point2F((L.prevBtn.left + L.prevBtn.right) * 0.5f,
                                                (L.prevBtn.top + L.prevBtn.bottom) * 0.5f),
                                  18 * s, 18 * s),
                              g_d2d.trackBrush.Get());
    DrawIconPrev(L.prevBtn, (st.hasSession && st.canPrev) ? g_d2d.whiteBrush.Get() : g_d2d.dimBrush.Get());
    // Next
    if (hoverNext)
        g_d2d.rt->FillEllipse(D2D1::Ellipse(
                                  D2D1::Point2F((L.nextBtn.left + L.nextBtn.right) * 0.5f,
                                                (L.nextBtn.top + L.nextBtn.bottom) * 0.5f),
                                  18 * s, 18 * s),
                              g_d2d.trackBrush.Get());
    DrawIconNext(L.nextBtn, (st.hasSession && st.canNext) ? g_d2d.whiteBrush.Get() : g_d2d.dimBrush.Get());
    // Play/Pause — большая жёлтая кнопка.
    {
        float pr = (L.playBtn.right - L.playBtn.left) * 0.5f;
        D2D1_POINT_2F pc{(L.playBtn.left + L.playBtn.right) * 0.5f,
                         (L.playBtn.top + L.playBtn.bottom) * 0.5f};
        ID2D1SolidColorBrush* playBrush = g_d2d.yellowBrush.Get();
        g_d2d.rt->FillEllipse(D2D1::Ellipse(pc, pr, pr), playBrush);
        if (hoverPlay) {
            g_d2d.rt->DrawEllipse(D2D1::Ellipse(pc, pr, pr), g_d2d.whiteBrush.Get(), 1.5f);
        }
        if (st.isPlaying)
            DrawIconPause(L.playBtn, g_d2d.blackBrush.Get());
        else
            DrawIconPlay(L.playBtn, g_d2d.blackBrush.Get());
    }
    // Громкость.
    bool muted = (g_volume <= 0.001f);
    DrawIconVolume(L.volIcon, hoverVol ? g_d2d.whiteBrush.Get() : g_d2d.grayBrush.Get(), muted);
    {
        float vw = L.volSlider.right - L.volSlider.left;
        float vh = L.volSlider.bottom - L.volSlider.top;
        D2D1_ROUNDED_RECT vbg{L.volSlider, vh * 0.5f, vh * 0.5f};
        DrawRoundedRect(vbg, g_d2d.trackBrush.Get(), true);
        if (g_volume > 0.001f) {
            D2D1_RECT_F vf = L.volSlider;
            vf.right = L.volSlider.left + vw * g_volume;
            D2D1_ROUNDED_RECT vfg{vf, vh * 0.5f, vh * 0.5f};
            DrawRoundedRect(vfg, g_d2d.whiteBrush.Get(), true);
        }
    }

    // --- Min / Close ---
    auto drawGlyph = [&](const D2D1_RECT_F& r, bool isClose, bool hover) {
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float d = 5.0f * s;
        ID2D1SolidColorBrush* b = hover ? g_d2d.whiteBrush.Get() : g_d2d.dimBrush.Get();
        if (isClose) {
            g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), b, 1.6f);
            g_d2d.rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), b, 1.6f);
        } else {
            g_d2d.rt->DrawLine(D2D1::Point2F(cx - d, cy), D2D1::Point2F(cx + d, cy), b, 1.6f);
        }
    };
    drawGlyph(L.minBtn, false, hoverMin);
    drawGlyph(L.closeBtn, true, hoverClose);

    HRESULT hr = g_d2d.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

// -------------------------------------------------------------- Медиа-клавиши
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

// Команды SMTC нельзя вызывать из UI-потока: WinRT-объекты живут в MTA
// потока опроса, а блокирующий .get() в STA (в котором живёт окно с своим
// циклом сообщений) может подвесить приложение. Поэтому UI только кладёт
// команду в очередь, а выполняет её поток опроса — задержка меньше 400 мс,
// потому что очередь будит его условной переменной.
enum class Cmd { PlayPause, Next, Prev, Seek, CycleSource };

struct Command {
    Cmd type;
    int64_t arg = 0;
};

std::mutex g_cmdMutex;
std::condition_variable g_cmdCv;
std::vector<Command> g_cmdQueue;

void PostCommand(Cmd type, int64_t arg = 0) {
    {
        std::lock_guard<std::mutex> lk(g_cmdMutex);
        g_cmdQueue.push_back({type, arg});
    }
    g_cmdCv.notify_one();
}

void DoPlayPause() { PostCommand(Cmd::PlayPause); }
void DoNext() { PostCommand(Cmd::Next); }
void DoPrev() { PostCommand(Cmd::Prev); }

// Выполнить команду. Вызывается только из потока опроса (MTA).
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
    }
}

// ------------------------------------------------------------ Фон: опрос SMTC
void PollThread() {
    // Единственный поток, который трогает SMTC: и опрос, и команды.
    g_smtc.Initialize();
    while (g_running.load()) {
        // 1) Выполнить накопившиеся команды (почти всегда пусто).
        std::vector<Command> cmds;
        {
            std::lock_guard<std::mutex> lk(g_cmdMutex);
            cmds.swap(g_cmdQueue);
        }
        for (const auto& c : cmds)
            ExecuteCommand(c);

        // 2) Опросить состояние.
        MediaState st = g_smtc.Poll();
        {
            std::lock_guard<std::mutex> lk(g_stateMutex);
            g_state = std::move(st);
            g_stateTime = std::chrono::steady_clock::now();
        }
        if (g_hwnd)
            PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0);

        // 3) Спать до следующего опроса — или до новой команды.
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
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Яндекс Музыка — плеер");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void TrayRemove() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Не называем это ShowWindow: так зовётся Win32-функция, и перегрузка
// ломает вызовы вида ::ShowWindow(hwnd, SW_SHOW).
void ShowPlayerWindow(bool show) {
    if (show) {
        ::ShowWindow(g_hwnd, SW_SHOW);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    } else {
        ::ShowWindow(g_hwnd, SW_HIDE);
    }
}

// --------------------------------------------------- Внешний вид окна
void SetupWindowAppearance(HWND hwnd) {
    // Тёмный заголовок/рамка (Win10 1809+; на старых версиях просто
    // возвращает ошибку — не критично).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    // Скругление углов на Windows 11. На Windows 10 атрибут неизвестен,
    // и вызов тихо проваливается — окно останется прямоугольным.
    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));

    // Тень окна, чтобы не висело "плоско" на рабочем столе.
    MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
}

void SetVolumeFromX(float x) {
    Layout L = ComputeLayout();
    float vw = L.volSlider.right - L.volSlider.left;
    if (vw <= 0)
        return;
    float v = (x - L.volSlider.left) / vw;
    v = std::clamp(v, 0.0f, 1.0f);
    g_volume = v;
    volume::SetMaster(v);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SeekFromX(float x) {
    Layout L = ComputeLayout();
    MediaState st;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        st = g_state;
    }
    if (!st.hasSession || st.durationMs <= 0)
        return;
    float pw = L.progress.right - L.progress.left;
    if (pw <= 0)
        return;
    float frac = std::clamp((x - L.progress.left) / pw, 0.0f, 1.0f);
    PostCommand(Cmd::Seek, static_cast<int64_t>(frac * static_cast<float>(st.durationMs)));
}

// ------------------------------------------------------------------- WndProc
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SetupWindowAppearance(hwnd);
        TrayAdd();
        // Горячие клавиши: Ctrl+Alt+Space / ← / → / M
        RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
        RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_LEFT);
        RegisterHotKey(hwnd, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_RIGHT);
        RegisterHotKey(hwnd, 4, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
        float v = volume::GetMaster();
        if (v >= 0.0f)
            g_volume = v;
        SetTimer(hwnd, 1, 1000, nullptr); // обновлять громкость из системы
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
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wParam == 1) {
            float v = volume::GetMaster();
            if (v >= 0.0f && std::fabs(v - g_volume) > 0.01f) {
                g_volume = v;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_SIZE: {
        if (g_d2d.rt) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            g_d2d.rt->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_d2d.rt) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            g_d2d.rt->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        g_lastMouse.x = GET_X_LPARAM(lParam);
        g_lastMouse.y = GET_Y_LPARAM(lParam);
        Layout L = ComputeLayout();
        g_hoverProgress = HitPadded(L.progress, g_lastMouse, 8.0f * L.scale);
        if (g_dragWindow) {
            POINT cur{};
            GetCursorPos(&cur);
            SetWindowPos(hwnd, nullptr, cur.x - g_dragOffset.x, cur.y - g_dragOffset.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (g_dragVolume) {
            SetVolumeFromX(static_cast<float>(g_lastMouse.x));
        } else {
            // Курсор-рука над кнопками.
            bool overBtn = Hit(L.playBtn, g_lastMouse) || Hit(L.prevBtn, g_lastMouse) ||
                           Hit(L.nextBtn, g_lastMouse) || Hit(L.volIcon, g_lastMouse) ||
                           HitPadded(L.volSlider, g_lastMouse, 8.0f * L.scale) ||
                           HitPadded(L.progress, g_lastMouse, 8.0f * L.scale) ||
                           Hit(L.sourceBtn, g_lastMouse) || Hit(L.closeBtn, g_lastMouse) ||
                           Hit(L.minBtn, g_lastMouse);
            SetCursor(LoadCursor(nullptr, overBtn ? IDC_HAND : IDC_ARROW));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        g_lastMouse = p;
        Layout L = ComputeLayout();
        if (Hit(L.closeBtn, p)) {
            ShowPlayerWindow(false); // крестик = в трей, выход — из меню трея
            return 0;
        }
        if (Hit(L.minBtn, p)) {
            ShowPlayerWindow(false);
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
            // ToggleMute сам возвращает уровень обратно при размьюте,
            // поэтому после вызова просто перечитываем фактическую громкость.
            if (volume::ToggleMute() >= 0) {
                float v = volume::GetMaster();
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
            SeekFromX(static_cast<float>(p.x));
            return 0;
        }
        if (Hit(L.sourceBtn, p) || Hit(L.source, p)) {
            PostCommand(Cmd::CycleSource);
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
        g_dragWindow = false;
        g_dragVolume = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        Layout L = ComputeLayout();
        if (Hit(L.cover, p))
            DoPlayPause();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float step = (delta > 0) ? 0.05f : -0.05f;
        g_volume = std::clamp(g_volume + step, 0.0f, 1.0f);
        volume::SetMaster(g_volume);
        InvalidateRect(hwnd, nullptr, FALSE);
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
                float v = volume::GetMaster();
                if (v >= 0.0f)
                    g_volume = v;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_APP_TRAY: {
        const UINT trayMsg = static_cast<UINT>(lParam);
        if (trayMsg == WM_LBUTTONDBLCLK) {
            // Двойной клик по иконке — показать окно.
            ShowPlayerWindow(true);
        } else if (trayMsg == WM_RBUTTONUP || trayMsg == WM_CONTEXTMENU) {
            POINT pt{};
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Показать / скрыть");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, L"Выход");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenuW(menu, TPM_RETURNCMD | TPM_LEFTALIGN,
                                      pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) {
                ShowPlayerWindow(!IsWindowVisible(hwnd));
            } else if (cmd == 2) {
                DestroyWindow(hwnd);
            }
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
        KillTimer(hwnd, 1);
        TrayRemove();
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInst;

    // COM для WIC/CoreAudio.
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

    // SMTC инициализируется в потоке опроса (MTA) — см. PollThread().

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // рисуем сами через Direct2D
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Не удалось зарегистрировать класс окна.",
                    L"Яндекс Музыка — плеер", MB_ICONERROR);
        return 1;
    }

    // Масштаб системы до создания окна. GetDpiForMonitor требует shcore.lib,
    // поэтому берём GetDpiForSystem (Win10 1607+) — на старте этого достаточно,
    // дальше окно живёт по WM_DPICHANGED / GetDpiForWindow.
    float scale = static_cast<float>(GetDpiForSystem()) / 96.0f;
    if (scale < 1.0f)
        scale = 1.0f;
    int winW = static_cast<int>(kBaseWidth * scale);
    int winH = static_cast<int>(kBaseHeight * scale);

    // Старт справа внизу, над панелью задач.
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    int x = 100, y = 100;
    if (GetMonitorInfoW(mon, &mi)) {
        x = mi.rcWork.right - winW - 24;
        y = mi.rcWork.bottom - winH - 24;
    }

    // WS_EX_TOOLWINDOW — не показываться в панели задач и Alt+Tab.
    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, kClassName, L"Яндекс Музыка — плеер",
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, x, y, winW, winH, nullptr, nullptr,
        hInst, nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Не удалось создать окно.", L"Яндекс Музыка — плеер",
                    MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(g_hwnd, SW_SHOW);
    (void)nCmdShow;

    // Фоновый опрос SMTC + очередь команд.
    std::thread pollThread(PollThread);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Поток опроса сам выходит по g_running (не дольше 400 мс).
    pollThread.join();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
