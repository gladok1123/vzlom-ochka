cmake_minimum_required(VERSION 3.20)
project(YandexMusicPlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Один .exe, без рантаймов и зависимостей кроме системных библиотек Windows.
add_executable(YandexMusicPlayer WIN32
    src/main.cpp
    src/smtc.cpp
)

target_include_directories(YandexMusicPlayer PRIVATE src)

# C++/WinRT: заголовки идут с Windows SDK, отдельный пакет не нужен.
target_compile_definitions(YandexMusicPlayer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(YandexMusicPlayer PRIVATE /W4 /permissive- /utf-8)
    # Статическая линковка CRT: exe работает без VC++ Redistributable.
    set_property(TARGET YandexMusicPlayer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(YandexMusicPlayer PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    ole32
    shell32
    uuid
)

# Релиз по умолчанию: /O2, без отладочных символов в exe.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
