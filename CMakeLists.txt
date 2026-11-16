cmake_minimum_required(VERSION 3.20)
project(YandexMusicPlayer VERSION 1.2.0 LANGUAGES CXX)

# C++17 + /await — режим, под который рассчитаны заголовки C++/WinRT из
# Windows SDK (они же ставит Visual Studio 2022).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Один .exe, без рантаймов и зависимостей кроме системных библиотек Windows.
add_executable(YandexMusicPlayer WIN32
    src/main.cpp
    src/smtc.cpp
    src/webview.cpp
    resources.rc   # иконка + версия в свойствах exe
)

target_include_directories(YandexMusicPlayer PRIVATE src)

target_compile_definitions(YandexMusicPlayer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Install the Microsoft.Web.WebView2 NuGet SDK or point this variable at it.
set(WEBVIEW2_SDK_DIR "${CMAKE_BINARY_DIR}/webview2" CACHE PATH "WebView2 NuGet SDK directory")
option(YM_REQUIRE_WEBVIEW2 "Fail if the WebView2 SDK is missing" OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WEBVIEW2_ARCH x64)
else()
    set(WEBVIEW2_ARCH x86)
endif()
set(WEBVIEW2_LOADER "${WEBVIEW2_SDK_DIR}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib")
if(EXISTS "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h" AND EXISTS "${WEBVIEW2_LOADER}")
    target_include_directories(YandexMusicPlayer PRIVATE "${WEBVIEW2_SDK_DIR}/build/native/include")
    target_compile_definitions(YandexMusicPlayer PRIVATE YM_HAS_WEBVIEW2)
    target_link_libraries(YandexMusicPlayer PRIVATE "${WEBVIEW2_LOADER}" oleaut32 shlwapi version)
    message(STATUS "WebView2 SDK enabled")
elseif(YM_REQUIRE_WEBVIEW2)
    message(FATAL_ERROR "WebView2 SDK not found: ${WEBVIEW2_SDK_DIR}")
else()
    message(WARNING "WebView2 SDK not found; expanded mode opens an external browser")
endif()

if(MSVC)
        target_compile_options(YandexMusicPlayer PRIVATE /W4 /permissive- /utf-8 /await)
    # Заголовкам C++/WinRT из SDK нужны <experimental/coroutine> и /await;
    # в MSVC 14.4x это помечено устаревшим — гасим предупреждение.
    target_compile_definitions(YandexMusicPlayer PRIVATE
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
        _SILENCE_CXX17_COROUTINE_DEPRECATION_WARNING
    )
    # Статическая линковка CRT: exe работает без VC++ Redistributable.
    set_property(TARGET YandexMusicPlayer PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(YandexMusicPlayer PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    ole32
    shell32
    uuid
    # Точки входа WinRT (RoGetActivationFactory, RoOriginateLanguageException).
    windowsapp
)

# Релиз по умолчанию: /O2, без отладочных символов в exe.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
