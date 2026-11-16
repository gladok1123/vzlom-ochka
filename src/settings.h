// settings.h — настройки плеера в %LOCALAPPDATA%\YandexMusicPlayer\settings.ini.
// Используем Win32 INI-API: файл создаётся сам, парсер писать не нужно.
#pragma once

#include <windows.h>

#include <string>

struct Settings {
    // Выбранная пользователем сессия (SourceAppUserModelId) или пусто = авто.
    std::wstring preferredSource;
    // Окно поверх всех остальных.
    bool topmost = false;
    // Окно прикреплено к рабочему столу (как виджет).
    bool desktopWidget = true;
    // Позиция окна.
    bool hasPosition = false;
    int x = 0;
    int y = 0;
};

namespace settings {

inline std::wstring FilePath() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    dir += L"\\YandexMusicPlayer";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\settings.ini";
    return dir;
}

inline Settings Load() {
    const std::wstring path = FilePath();
    Settings s;
    wchar_t val[MAX_PATH]{};
    GetPrivateProfileStringW(L"player", L"source", L"", val, MAX_PATH, path.c_str());
    s.preferredSource = val;
    s.topmost = GetPrivateProfileIntW(L"player", L"topmost", 0, path.c_str()) != 0;
    s.desktopWidget = GetPrivateProfileIntW(L"player", L"desktop", 1, path.c_str()) != 0;
    s.hasPosition = GetPrivateProfileIntW(L"player", L"haspos", 0, path.c_str()) != 0;
    s.x = static_cast<int>(GetPrivateProfileIntW(L"player", L"x", 0, path.c_str()));
    s.y = static_cast<int>(GetPrivateProfileIntW(L"player", L"y", 0, path.c_str()));
    return s;
}

inline void Save(const Settings& s) {
    const std::wstring path = FilePath();
    WritePrivateProfileStringW(L"player", L"source", s.preferredSource.c_str(), path.c_str());
    WritePrivateProfileStringW(L"player", L"topmost", s.topmost ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"player", L"desktop", s.desktopWidget ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"player", L"haspos", s.hasPosition ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"player", L"x", std::to_wstring(s.x).c_str(), path.c_str());
    WritePrivateProfileStringW(L"player", L"y", std::to_wstring(s.y).c_str(), path.c_str());
}

} // namespace settings
