// smtc.cpp — реализация SmtcClient. См. smtc.h за объяснением архитектуры.
#include "smtc.h"

#include <algorithm>
#include <cwctype>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

// На уровне файла (не внутри анонимного неймспейса), чтобы имена были
// видны и в функциях-членах SmtcClient ниже.
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;

namespace {

// winrt::Windows::Foundation::TimeSpan в C++/WinRT — это
// std::chrono::duration<int64_t, ratio<1, 10000000>>, поэтому
// duration_cast к миллисекундам работает напрямую.
int64_t ToMs(winrt::Windows::Foundation::TimeSpan ts) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(ts).count();
}

std::wstring ToWString(winrt::hstring const& s) {
    return std::wstring(s.begin(), s.end());
}

// Человекочитаемое имя источника для UI.
// SourceAppUserModelId у браузеров — это имя exe/пакета, приводим к виду
// "Chrome", "Microsoft Edge", "Firefox" и т.д. Неизвестное — как есть.
std::wstring PrettySourceName(const std::wstring& appId) {
    std::wstring lower = appId;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    auto contains = [&lower](const wchar_t* needle) {
        return lower.find(needle) != std::wstring::npos;
    };

    if (contains(L"chrome") && !contains(L"chromium"))
        return L"Chrome";
    if (contains(L"msedge") || contains(L"edge"))
        return L"Microsoft Edge";
    if (contains(L"firefox"))
        return L"Firefox";
    if (contains(L"opera"))
        return L"Opera";
    if (contains(L"brave"))
        return L"Brave";
    if (contains(L"yandex"))
        return L"Яндекс Браузер";
    if (contains(L"vivaldi"))
        return L"Vivaldi";
    if (contains(L"spotify"))
        return L"Spotify";
    if (contains(L"vlc"))
        return L"VLC";
    if (contains(L"wmplayer") || contains(L"groove") || contains(L"zunemusic"))
        return L"Groove Music";
    if (contains(L"itunes"))
        return L"iTunes";
    if (contains(L"youtube"))
        return L"YouTube Music";
    if (appId.empty())
        return L"Неизвестно";
    return appId;
}

// Прочитать thumbnail сессии в байты. Пустой вектор = обложки нет.
// Свойства передаются снаружи: TryGetMediaPropertiesAsync — самый дорогой
// вызов SMTC, лишний раз его дёргать незачем.
std::vector<uint8_t> ReadThumbnail(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties const& props) {
    std::vector<uint8_t> out;
    try {
        auto thumb = props.Thumbnail();
        if (!thumb)
            return out;

        auto stream = thumb.OpenReadAsync().get();
        uint64_t size = stream.Size();
        if (size == 0 || size > 16 * 1024 * 1024)
            return out;

        Buffer buf(static_cast<uint32_t>(size));
        auto readBuf = stream.ReadAsync(buf, static_cast<uint32_t>(size),
                                        InputStreamOptions::None)
                           .get();
        if (!readBuf)
            return out;

        DataReader reader = DataReader::FromBuffer(readBuf);
        uint32_t readSize = reader.UnconsumedBufferLength();
        if (readSize == 0)
            return out;

        out.resize(readSize);
        winrt::array_view<uint8_t> view(out.data(), readSize);
        reader.ReadBytes(view);
    } catch (...) {
        out.clear();
    }
    return out;
}

} // namespace

bool SmtcClient::Initialize() {
    // Квартира — на поток: каждый поток, который трогает SMTC, должен сам
    // вызвать init_apartment. thread_local-флаг избавляет от повторных
    // (и всегда провальных) вызовов на каждом опросе.
    static thread_local bool apartmentReady = false;
    if (!apartmentReady) {
        try {
            // MTA: в многопоточной квартире блокирующий .get() на
            // IAsyncOperation не дедлокается (в STA может зависнуть).
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
            // Квартира уже инициализирована — это нормально.
        }
        apartmentReady = true;
    }
    if (initialized_)
        return true;
    try {
        manager_ = Manager::RequestAsync().get();
        initialized_ = (manager_ != nullptr);
    } catch (...) {
        initialized_ = false;
    }
    return initialized_;
}

SmtcClient::Session SmtcClient::FindSession(const std::wstring& appId) {
    if (!manager_)
        return nullptr;
    try {
        for (auto const& s : manager_.GetSessions()) {
            if (ToWString(s.SourceAppUserModelId()) == appId)
                return s;
        }
    } catch (...) {
    }
    return nullptr;
}

SmtcClient::Session SmtcClient::PickSession(Manager& manager) {
    // 1) Ручной выбор пользователя, если такая сессия ещё жива.
    if (!preferredSource_.empty()) {
        if (auto s = FindSession(preferredSource_))
            return s;
    }
    // 2) Системная "текущая" сессия (та, к которой привязаны медиа-клавиши).
    try {
        if (auto cur = manager.GetCurrentSession())
            return cur;
    } catch (...) {
    }
    // 3) Первая играющая сессия, 4) иначе — просто первая доступная.
    try {
        auto sessions = manager.GetSessions();
        for (auto const& s : sessions) {
            try {
                if (s.GetPlaybackInfo().PlaybackStatus() ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                    return s;
            } catch (...) {
            }
        }
        if (sessions.Size() > 0)
            return sessions.GetAt(0);
    } catch (...) {
    }
    return nullptr;
}

MediaState SmtcClient::Poll() {
    MediaState st;
    if (!Initialize())
        return st;

    Manager manager = manager_;
    std::vector<Session> sessions;
    try {
        for (auto const& s : manager.GetSessions())
            sessions.push_back(s);
    } catch (...) {
        return st;
    }
    for (auto const& s : sessions) {
        try {
            st.availableSources.push_back(ToWString(s.SourceAppUserModelId()));
        } catch (...) {
        }
    }

    Session session = PickSession(manager);
    if (!session)
        return st;

    st.hasSession = true;
    try {
        st.sourceAppId = ToWString(session.SourceAppUserModelId());
    } catch (...) {
    }
    st.sourceLabel = PrettySourceName(st.sourceAppId);

    // Метаданные одним запросом (он самый дорогой в SMTC) — и оттуда же
    // берём обложку, но только при смене трека.
    try {
        auto props = session.TryGetMediaPropertiesAsync().get();
        st.title = ToWString(props.Title());
        st.artist = ToWString(props.Artist());
        st.album = ToWString(props.AlbumTitle());

        // Статус воспроизведения и доступность кнопок.
        try {
            auto info = session.GetPlaybackInfo();
            st.isPlaying =
                (info.PlaybackStatus() ==
                 GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
            auto controls = info.Controls();
            st.canPlayPause = controls.IsPlayPauseEnabled() ||
                              controls.IsPlayEnabled() || controls.IsPauseEnabled();
            st.canNext = controls.IsNextEnabled();
            st.canPrev = controls.IsPreviousEnabled();
        } catch (...) {
        }

        const bool trackChanged = (st.title != lastTitle_) || (st.artist != lastArtist_);
        if (trackChanged || lastCoverBytes_.empty()) {
            auto bytes = ReadThumbnail(props);
            if (bytes != lastCoverBytes_) {
                // Другая картинка (или её не стало) — новая версия для UI.
                lastCoverBytes_ = std::move(bytes);
                ++coverVersion_;
            }
            lastTitle_ = st.title;
            lastArtist_ = st.artist;
        }
    } catch (...) {
    }

    // Таймлайн: позиция и длительность.
    try {
        auto timeline = session.GetTimelineProperties();
        st.positionMs = ToMs(timeline.Position());
        const int64_t endMs = ToMs(timeline.EndTime());
        const int64_t startMs = ToMs(timeline.StartTime());
        st.durationMs = (endMs > startMs) ? (endMs - startMs) : endMs;
        if (st.durationMs < 0)
            st.durationMs = 0;
        st.canSeek = st.durationMs > 0;
    } catch (...) {
    }

    st.coverBytes = lastCoverBytes_;
    st.coverVersion = coverVersion_;
    return st;
}

bool SmtcClient::PlayPause() {
    if (!Initialize())
        return false;
    try {
        Session s = PickSession(manager_);
        if (!s)
            return false;
        s.TryPlayPauseAsync().get();
        return true;
    } catch (...) {
        return false;
    }
}

bool SmtcClient::Play() {
    if (!Initialize())
        return false;
    try {
        Session s = PickSession(manager_);
        if (!s)
            return false;
        s.TryPlayAsync().get();
        return true;
    } catch (...) {
        return false;
    }
}

bool SmtcClient::Pause() {
    if (!Initialize())
        return false;
    try {
        Session s = PickSession(manager_);
        if (!s)
            return false;
        s.TryPauseAsync().get();
        return true;
    } catch (...) {
        return false;
    }
}

bool SmtcClient::Next() {
    if (!Initialize())
        return false;
    try {
        Session s = PickSession(manager_);
        if (!s)
            return false;
        s.TrySkipNextAsync().get();
        return true;
    } catch (...) {
        return false;
    }
}

bool SmtcClient::Previous() {
    if (!Initialize())
        return false;
    try {
        Session s = PickSession(manager_);
        if (!s)
            return false;
        s.TrySkipPreviousAsync().get();
        return true;
    } catch (...) {
        return false;
    }
}

bool SmtcClient::Seek(int64_t positionMs) {
    if (!Initialize() || positionMs < 0)
        return false;
    try {
        Session s = PickSession(manager_);
        if (!s)
            return false;
        // TrySeekAsync принимает TimeSpan (100-нс тики). Периоды у
        // std::chrono::milliseconds и TimeSpan разные, поэтому duration_cast.
        using winrt::Windows::Foundation::TimeSpan;
        TimeSpan ts = std::chrono::duration_cast<TimeSpan>(
            std::chrono::milliseconds(positionMs));
        s.TrySeekAsync(ts).get();
        return true;
    } catch (...) {
        return false;
    }
}

void SmtcClient::SetPreferredSource(const std::wstring& appId) {
    preferredSource_ = appId;
}

std::wstring SmtcClient::CycleSource() {
    if (!Initialize())
        return {};
    std::vector<std::wstring> sources;
    try {
        for (auto const& s : manager_.GetSessions())
            sources.push_back(ToWString(s.SourceAppUserModelId()));
    } catch (...) {
        return {};
    }
    if (sources.empty())
        return {};
    if (sources.size() == 1) {
        preferredSource_ = sources[0];
        return preferredSource_;
    }
    // Найти текущий и взять следующий по кругу.
    size_t idx = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i] == preferredSource_) {
            idx = (i + 1) % sources.size();
            break;
        }
    }
    preferredSource_ = sources[idx];
    return preferredSource_;
}
