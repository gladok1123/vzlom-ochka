@echo off
chcp 65001 >nul
REM build.bat — сборка плеера одной командой.
REM Требования: Visual Studio 2022 (Desktop development with C++) + CMake
REM (CMake ставится вместе с VS, искать его вручную не нужно).
REM Результат: build\Release\YandexMusicPlayer.exe (без рантаймов)

setlocal

REM --- 1. Найти Visual Studio и её окружение -------------------------------
set "VSDIR="
for %%D in (
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
) do (
    if exist "%%~D\Common7\Tools\VsDevCmd.bat" set "VSDIR=%%~D"
)

if defined VSDIR (
    echo [build] Visual Studio: %VSDIR%
    call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
) else (
    echo [build] Visual Studio 2022 не найдена в стандартных путях.
    echo [build] Если она установлена в другое место — запустите этот файл
    echo [build] из "x64 Native Tools Command Prompt for VS 2022".
)

REM --- 2. Найти CMake: сначала в PATH, потом внутри Visual Studio ----------
set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
    if defined VSDIR (
        if exist "%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)

"%CMAKE%" --version >nul 2>nul
if errorlevel 1 (
    echo [build] ОШИБКА: CMake не найден.
    echo [build] Установите Visual Studio 2022 с компонентом
    echo [build] "Desktop development with C++" ^(CMake идёт в комплекте^),
    echo [build] либо отдельный CMake с https://cmake.org
    exit /b 1
)

REM --- 3. Конфигурация и сборка -------------------------------------------
echo [build] Конфигурация...
"%CMAKE%" -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

echo [build] Сборка...
"%CMAKE%" --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo [build] Готово: build\Release\YandexMusicPlayer.exe
echo [build] Запустите его, откройте music.yandex.ru в браузере и нажмите Play.
endlocal
