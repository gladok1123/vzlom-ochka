// webview.cpp — реализация WebViewHost (см. webview.h).
#include "webview.h"

#ifdef YM_HAS_WEBVIEW2

#include <wrl.h>
#include <wrl/implements.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

bool WebViewHost::Supported() { return true; }

bool WebViewHost::Create(HWND parent, const RECT& bounds, const std::wstring& url,
                         const std::wstring& userDataDir) {
    if (ready_ || creating_)
        return true;
    if (failed_)
        return false;

    creating_ = true;
    parent_ = parent;
    bounds_ = bounds;
    url_ = url;

    // Пустая строка = официальный рантайм WebView2, установленный в системе.
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    creating_ = false;
                    failed_ = true;
                    return S_OK;
                }
                env_ = env;
                HRESULT hr2 = env_->CreateCoreWebView2Controller(
                    parent_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT {
                            creating_ = false;
                            if (FAILED(r2) || !controller) {
                                failed_ = true;
                                return S_OK;
                            }
                            controller_ = controller;
                            controller_->get_CoreWebView2(&webview_);
                            // Тёмный фон до загрузки страницы — чтобы не было
                            // белой вспышки на тёмном окне.
                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(controller_->QueryInterface(
                                    IID_PPV_ARGS(c2.GetAddressOf()))) &&
                                c2) {
                                COREWEBVIEW2_COLOR bg{255, 0x12, 0x12, 0x16};
                                c2->put_DefaultBackgroundColor(bg);
                            }
                            ApplyBounds();
                            controller_->put_IsVisible(visible_ ? TRUE : FALSE);
                            if (webview_)
                                webview_->Navigate(url_.c_str());
                            ready_ = true;
                            return S_OK;
                        })
                        .Get());
                if (FAILED(hr2)) {
                    creating_ = false;
                    failed_ = true;
                }
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        creating_ = false;
        failed_ = true;
        return false;
    }
    return true;
}

void WebViewHost::ApplyBounds() {
    if (controller_)
        controller_->put_Bounds(bounds_);
}

void WebViewHost::SetBounds(const RECT& bounds) {
    bounds_ = bounds;
    ApplyBounds();
}

void WebViewHost::SetVisible(bool visible) {
    visible_ = visible;
    if (controller_)
        controller_->put_IsVisible(visible ? TRUE : FALSE);
}

void WebViewHost::Reload() {
    if (webview_)
        webview_->Reload();
}

void WebViewHost::Focus() {
    if (controller_)
        controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebViewHost::ResetFailure() {
    failed_ = false;
    ready_ = false;
}

void WebViewHost::Destroy() {
    if (controller_) {
        controller_->Close();
        controller_.Reset();
    }
    webview_.Reset();
    env_.Reset();
    ready_ = false;
    creating_ = false;
}

#else // YM_HAS_WEBVIEW2 не определён: SDK WebView2 не найден при сборке.

bool WebViewHost::Supported() { return false; }

bool WebViewHost::Create(HWND, const RECT&, const std::wstring&, const std::wstring&) {
    failed_ = true;
    return false;
}

void WebViewHost::ApplyBounds() {}

void WebViewHost::SetBounds(const RECT&) {}

void WebViewHost::SetVisible(bool) {}

void WebViewHost::Reload() {}

void WebViewHost::Focus() {}

void WebViewHost::ResetFailure() {
    failed_ = false;
    ready_ = false;
}

void WebViewHost::Destroy() {}

#endif
