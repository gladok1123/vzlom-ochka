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
# Заголовки winrt/*.h лежат в Windows SDK, но та версия (2.0.19xxx) писалась
# под MSVC 2019 и на актуальных компиляторах не собирается (C3779: методы
# с auto-возвратом). Поэтому берём официальный Microsoft.Windows.CppWinRT
# (он же ставит Visual Studio) и генерируем проекции им — ровно то, что делает
# MSBuild-интеграция пакета.
#
# Сети нет? Ставим -DCPPWINRT_GENERATE=OFF и остаёмся на SDK-заголовках
# (с VS 2022 17.x они работают).
# ---------------------------------------------------------------------------
option(CPPWINRT_GENERATE "Сгенерировать свежие проекции C++/WinRT (нужна сеть)" ON)

if(CPPWINRT_GENERATE)
    set(_cppwinrt_versions "2.0.240111.5" "2.0.231028.2" "2.0.220110.5")
    set(_pkg "${CMAKE_BINARY_DIR}/cppwinrt-pkg")
    set(_gen "${CMAKE_BINARY_DIR}/cppwinrt-generated")

    if(NOT EXISTS "${_pkg}/bin/cppwinrt.exe")
        foreach(_ver IN LISTS _cppwinrt_versions)
            if(EXISTS "${_pkg}/bin/cppwinrt.exe")
                break()
            endif()
            set(_url "https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/${_ver}")
            set(_nupkg "${CMAKE_BINARY_DIR}/cppwinrt-${_ver}.nupkg")
            set(_got FALSE)

            message(STATUS "C++/WinRT: качаю пакет ${_ver}...")
            file(DOWNLOAD "${_url}" "${_nupkg}" STATUS _dl_status TIMEOUT 120)
            list(GET _dl_status 0 _dl_code)
            if(_dl_code EQUAL 0 AND EXISTS "${_nupkg}")
                set(_got TRUE)
            else()
                # Запасной загрузчик: curl.exe есть в Windows 10/11 из коробки
                # и использует другой TLS-стек, чем CMake.
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
                file(ARCHIVE_EXTRACT INPUT "${_nupkg}" DESTINATION "${_pkg}")
            endif()
        endforeach()
    endif()

    if(EXISTS "${_pkg}/bin/cppwinrt.exe" AND NOT EXISTS "${_gen}/winrt/base.h")
        # Windows.winmd — объединённый контракт установленного Windows SDK.
        # Путь к Windows SDK: переменная окружения из Developer Command Prompt
        # либо стандартные каталоги установки. ($ENV{ProgramFiles(x86)} не
        # используем — CMake путается на скобках в имени переменной.)
        set(_sdk_roots
            "$ENV{WindowsSdkDir}"
            "C:/Program Files (x86)/Windows Kits/10"
            "$ENV{ProgramFiles}/Windows Kits/10"
            "$ENV{ProgramW6432}/Windows Kits/10"
        )
        set(_winmd_candidates)
        foreach(_root IN LISTS _sdk_roots)
            if(_root AND EXISTS "${_root}/UnionMetadata")
                file(GLOB _found "${_root}/UnionMetadata/*/Windows.winmd")
                if(_found)
                    list(APPEND _winmd_candidates ${_found})
                endif()
            endif()
        endforeach()
        if(_winmd_candidates)
            list(SORT _winmd_candidates)
            list(GET _winmd_candidates -1 _winmd)
            message(STATUS "C++/WinRT: генерирую проекции из ${_winmd}")
            execute_process(
                COMMAND "${_pkg}/bin/cppwinrt.exe" -input "${_winmd}" -output "${_gen}"
                RESULT_VARIABLE _gen_res
                OUTPUT_VARIABLE _gen_out
                ERROR_VARIABLE _gen_out)
            if(NOT _gen_res EQUAL 0)
                message(STATUS "C++/WinRT: генератор вернул ${_gen_res}: ${_gen_out}")
            endif()
        else()
            message(STATUS "C++/WinRT: Windows.winmd не найден в ${_sdk_root}")
        endif()
    endif()

    if(EXISTS "${_gen}/winrt/base.h")
        message(STATUS "C++/WinRT: использую сгенерированные заголовки ${_gen}")
        target_include_directories(YandexMusicPlayer PRIVATE "${_gen}")
    else()
        message(WARNING
            "C++/WinRT: свежие заголовки получить не удалось, беру версию из "
            "Windows SDK. С Visual Studio 2022 17.x это работает; на более "
            "новом MSVC возможны ошибки C3779 в winrt/*.h.")
    endif()
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
