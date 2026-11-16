cmake_minimum_required(VERSION 3.20)
project(YandexMusicPlayer VERSION 1.0.0 LANGUAGES CXX)

# C++17 + /await: именно под этот режим рассчитаны заголовки C++/WinRT из
# Windows SDK. В C++20 они не собираются на новых MSVC (C3779).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Один .exe, без рантаймов и зависимостей кроме системных библиотек Windows.
add_executable(YandexMusicPlayer WIN32
    src/main.cpp
    src/smtc.cpp
)

target_include_directories(YandexMusicPlayer PRIVATE src)

target_compile_definitions(YandexMusicPlayer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# ---------------------------------------------------------------------------
# C++/WinRT.
#
# Заголовки winrt/*.h лежат и в Windows SDK, но та версия (2.0.19xxx) писалась
# под старый MSVC с /await и на актуальных компиляторах падает (C3779 /
# deprecated <experimental/coroutine>). Поэтому по умолчанию скачиваем
# официальный пакет Microsoft.Windows.CppWinRT с nuget.org и подставляем его
# заголовки. Если сети нет — молча остаёмся на SDK-версии.
#
# Отключить: cmake ... -DCPPWINRT_FROM_NUGET=OFF
# ---------------------------------------------------------------------------
option(CPPWINRT_FROM_NUGET "Скачать свежий C++/WinRT с nuget.org" OFF)

set(CPPWINRT_ROOT "${CMAKE_BINARY_DIR}/cppwinrt")
set(_cppwinrt_versions
    "2.0.240111.5"
    "2.0.231028.2"
    "2.0.220110.5"
    "2.0.210503.1"
)

if(CPPWINRT_FROM_NUGET AND NOT EXISTS "${CPPWINRT_ROOT}/include/winrt/base.h")
    foreach(_ver IN LISTS _cppwinrt_versions)
        if(EXISTS "${CPPWINRT_ROOT}/include/winrt/base.h")
            break()
        endif()
        set(_url "https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/${_ver}")
        set(_nupkg "${CMAKE_BINARY_DIR}/cppwinrt-${_ver}.nupkg")
        set(_got FALSE)

        message(STATUS "C++/WinRT: качаю пакет ${_ver}...")
        # 1) Встроенный загрузчик CMake.
        file(DOWNLOAD "${_url}" "${_nupkg}" STATUS _dl_status TIMEOUT 120)
        list(GET _dl_status 0 _dl_code)
        if(_dl_code EQUAL 0 AND EXISTS "${_nupkg}")
            set(_got TRUE)
        else()
            # 2) Запасной вариант — curl.exe (есть в Windows 10/11 из коробки):
            #    у него другой TLS-стек, и он часто проходит там, где падает CMake.
            find_program(CURL_EXE NAMES curl curl.exe)
            if(CURL_EXE)
                message(STATUS "C++/WinRT: CMake не смог, пробую curl...")
                execute_process(
                    COMMAND "${CURL_EXE}" -sSL --max-time 120 -o "${_nupkg}" "${_url}"
                    RESULT_VARIABLE _curl_res)
                if(_curl_res EQUAL 0 AND EXISTS "${_nupkg}")
                    set(_got TRUE)
                endif()
            endif()
        endif()

        if(_got)
            file(ARCHIVE_EXTRACT INPUT "${_nupkg}" DESTINATION "${CPPWINRT_ROOT}")
            if(NOT EXISTS "${CPPWINRT_ROOT}/include/winrt/base.h")
                message(STATUS "C++/WinRT: в пакете ${_ver} нет include/winrt/base.h")
            endif()
        else()
            message(STATUS "C++/WinRT: пакет ${_ver} скачать не удалось")
        endif()
    endforeach()
endif()

if(EXISTS "${CPPWINRT_ROOT}/include/winrt/base.h")
    message(STATUS "C++/WinRT: использую заголовки из ${CPPWINRT_ROOT}/include")
    target_include_directories(YandexMusicPlayer PRIVATE "${CPPWINRT_ROOT}/include")
else()
    message(WARNING
        "C++/WinRT: свежие заголовки не скачались, беру версию из Windows SDK. "
        "Если сборка упадёт с ошибками в winrt/*.h — нужен доступ в интернет "
        "или установленный пакет Microsoft.Windows.CppWinRT.")
endif()

if(MSVC)
    target_compile_options(YandexMusicPlayer PRIVATE /W4 /permissive- /utf-8 /await)
    # MSVC 14.5x считает <experimental/coroutine> (нужен старым заголовкам
    # C++/WinRT) устаревшим и по умолчанию выдаёт ошибку — глушим предупреждение.
    target_compile_definitions(YandexMusicPlayer PRIVATE
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
        _SILENCE_CXX17_COROUTINE_DEPRECATION_WARNING
    )
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
