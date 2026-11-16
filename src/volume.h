// volume.h — управление громкостью системы через Core Audio API.
// SMTC громкость не умеет, поэтому крутим мастер-громкость Windows.
// Всё в одном заголовке, без .cpp: пара маленьких inline-функций.
#pragma once

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <comdef.h>

namespace volume {

// Получить текущую мастер-громкость 0.0..1.0. -1 при ошибке.
inline float GetMaster() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return -1.0f;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device)
        return -1.0f;

    IAudioEndpointVolume* vol = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&vol));
    device->Release();
    if (FAILED(hr) || !vol)
        return -1.0f;

    float level = -1.0f;
    vol->GetMasterVolumeLevelScalar(&level);
    vol->Release();
    return level;
}

// Установить мастер-громкость 0.0..1.0. true при успехе.
inline bool SetMaster(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return false;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device)
        return false;

    IAudioEndpointVolume* vol = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&vol));
    device->Release();
    if (FAILED(hr) || !vol)
        return false;

    hr = vol->SetMasterVolumeLevelScalar(level, nullptr);
    vol->Release();
    return SUCCEEDED(hr);
}

// Переключить mute. Возвращает новое состояние mute или -1 при ошибке.
inline int ToggleMute() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return -1;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device)
        return -1;

    IAudioEndpointVolume* vol = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&vol));
    device->Release();
    if (FAILED(hr) || !vol)
        return -1;

    BOOL mute = FALSE;
    if (FAILED(vol->GetMute(&mute))) {
        vol->Release();
        return -1;
    }
    BOOL next = !mute;
    if (FAILED(vol->SetMute(next, nullptr))) {
        vol->Release();
        return -1;
    }
    vol->Release();
    return next ? 1 : 0;
}

} // namespace volume
