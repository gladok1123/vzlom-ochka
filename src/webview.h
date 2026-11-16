// webview.h — встроенный браузер для развёрнутого режима.
//
// Используется WebView2 (Edge/Chromium), который уже есть в Windows 11 и в
// большинстве Windows 10 — то есть «движок» не тащим в exe, в отличие от
// Electron. Если SDK WebView2 не нашёлся на этапе сборки (нет пакета NuGet),
// сборка всё равно проходит, а кнопка разворота открывает music.yandex.ru в
// обычном браузере.
#pragma once

#include <windows.h>

#include <string>

#ifdef YM_HAS_WEBVIEW2
#include <wrl/client.h>
// Заголовок из NuGet-пакета Microsoft.Web.WebView2 (его находит CMake).
#include <WebView2.h>
#endif

class WebViewHost {
public:
    WebViewHost() = default;
    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    // true, если сборка умеет WebView2.
    static bool Supported();

    // Создать окружение и контроллер. Асинхронно: готовность — IsReady().
    bool Create(HWND parent, const RECT& bounds, const std::wstring& url,
                const std::wstring& userDataDir);

    void SetBounds(const RECT& bounds);
    void SetVisible(bool visible);
    void Reload();
    void Focus();
    // Сбросить состояние ошибки, чтобы попробовать снова (например, если
    // пользователь доустановил рантайм WebView2).
    void ResetFailure();

    bool IsReady() const { return ready_; }
    bool IsFailed() const { return failed_; }

    void Destroy();

private:
    void ApplyBounds();

    HWND parent_ = nullptr;
    RECT bounds_{};
    std::wstring url_;
    bool visible_ = false;
    bool ready_ = false;
    bool failed_ = false;
    bool creating_ = false;

#ifdef YM_HAS_WEBVIEW2
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
#endif
};
