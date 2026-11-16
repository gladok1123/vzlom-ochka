@echo off
REM build.bat — сборка плеера одной командой.
REM Требования: Visual Studio 2022 (Desktop development with C++) + CMake.
REM Результат: build\Release\YandexMusicPlayer.exe (~0.5-1 МБ, без рантаймов)

setlocal

REM Найти VsDevCmd, чтобы cmake/cl нашлись без ручной настройки PATH.
set "VSDEVCMD="
for %%D in ("%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools") do (
    if exist "%%~D\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%%~D\Common7\Tools\VsDevCmd.bat"
)
if defined VSDEVCMD (
    echo [build] Использую %VSDEVCMD%
    call "%VSDEVCMD%" -arch=x64 >nul
) else (
    echo [build] VsDevCmd не найден, пробую собрать в текущем PATH...
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [build] ОШИБКА: cmake не найден. Установите Visual Studio 2022
    echo [build] с компонентом "Desktop development with C++" ^(там есть CMake^).
    exit /b 1
)

echo [build] Конфигурация CMake...
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

echo [build] Сборка...
cmake --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo [build] Готово: build\Release\YandexMusicPlayer.exe
echo [build] Запустите его, откройте music.yandex.ru в браузере и нажмите Play.
endlocal
