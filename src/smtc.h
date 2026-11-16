// smtc.h — обёртка над System Media Transport Controls (SMTC) Windows.
//
// Зачем это нужно:
//   Яндекс Музыка в Windows — это веб-плеер (music.yandex.ru) в браузере.
//   Когда браузер играет музыку, он сам регистрируется в Windows как
//   медиа-сессия SMTC и отдаёт системе: название трека, исполнителя,
//   обложку, позицию/длительность и статус Play/Pause.
//   Любая программа может эти данные прочитать и отправить команды
//   Play / Pause / Next / Previous / Seek — без хаков, без Electron,
//   без внедрения в браузер. Именно это и делает SmtcClient.
//
// Потокобезопасность: НЕ потокобезопасен. Внутри — объекты WinRT из MTA,
//   поэтому и Poll(), и команды должны вызываться из одного и того же
//   фонового потока (см. PollThread в main.cpp). UI-поток только читает
//   готовое состояние через мьютекс и складывает команды в очередь.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Обязательный набор: без Windows.Foundation.Collections.h объявления
// методов WinRT-коллекций (Size/GetAt/begin) остаются без определений, и
// MSVC выдаёт невнятное "a function that returns 'auto' cannot be used
// before it is defined" (C3779).
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

// Одна медиасессия Windows (для выбора «процесса» в настройках плеера).
struct SourceEntry {
    std::wstring appId;   // SourceAppUserModelId: chrome.exe, msedge.exe и т.п.
    std::wstring label;   // человекочитаемое имя: Chrome, Firefox...
    std::wstring title;   // что сейчас в этой сессии играет
    std::wstring artist;
    bool playing = false;
};

struct MediaState {
    bool hasSession = false;

    // Источник: SourceAppUserModelId сессии, например L"Chrome", L"msedge.exe".
    // Для браузера это имя браузера — URL страницы SMTC не отдаёт, поэтому
    // если открыто несколько вкладок/браузеров, источник переключается вручную.
    std::wstring sourceAppId;
    std::wstring sourceLabel; // человекочитаемое имя для UI

    std::wstring title;
    std::wstring artist;
    std::wstring album;

    // Сырые байты обложки (JPEG/PNG), декодирует UI-поток через WIC.
    // coverVersion растёт при каждой смене обложки — по нему UI понимает,
    // что надо передекодировать картинку.
    std::vector<uint8_t> coverBytes;
    uint64_t coverVersion = 0;

    bool isPlaying = false;
    int64_t positionMs = 0;     // позиция на момент последнего обновления SMTC
    int64_t durationMs = 0;
    // Момент последнего обновления позиции в тиках UTC (100 нс, эпоха 1601).
    // По нему UI считает реальную позицию: 0 = источник не сообщает время.
    int64_t lastUpdatedTicks = 0;
    // Скорость воспроизведения (1.0 — обычная).
    double playbackRate = 1.0;

    bool canPlayPause = false;
    bool canNext = false;
    bool canPrev = false;
    bool canSeek = false;

    // Все доступные сессии: из них UI строит список выбора источника.
    std::vector<SourceEntry> sources;
};

class SmtcClient {
public:
    SmtcClient() = default;
    SmtcClient(const SmtcClient&) = delete;
    SmtcClient& operator=(const SmtcClient&) = delete;

    // Запросить GlobalSystemMediaTransportControlsSessionManager.
    // Возвращает false, если SMTC недоступен (старый Windows и т.п.).
    bool Initialize();

    // Опросить все сессии, выбрать активную и вернуть её состояние.
    // Вызывать из фонового потока каждые 300–500 мс.
    MediaState Poll();

    // Команды управления. Возвращают false, если нет активной сессии.
    bool PlayPause();
    bool Play();
    bool Pause();
    bool Next();
    bool Previous();
    bool Seek(int64_t positionMs);

    // Выбрать источник вручную (SourceAppUserModelId).
    // Пустая строка = автоматический выбор (сейчас играющая сессия).
    void SetPreferredSource(const std::wstring& appId);
    std::wstring PreferredSource() const { return preferredSource_; }

    // Переключить источник на следующий доступный (для кнопки в UI).
    // Возвращает новый выбранный appId или пустую строку, если сессий нет.
    std::wstring CycleSource();

private:
    using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;

    // Найти сессию по appId, или null, если такой нет.
    Session FindSession(const std::wstring& appId);
    // Выбрать сессию по правилам: preferred > текущая системная > играющая > первая.
    Session PickSession(Manager& manager);

    Manager manager_{nullptr};
    std::wstring preferredSource_;
    // Кэш последней обложки, чтобы не дёргать thumbnail на каждый опрос.
    std::vector<uint8_t> lastCoverBytes_;
    uint64_t coverVersion_ = 0;
    // Название/исполнитель прошлого трека — по ним ловим смену трека.
    std::wstring lastTitle_;
    std::wstring lastArtist_;
    bool initialized_ = false;
};
