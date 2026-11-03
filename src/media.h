#pragma once
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <winhttp.h>
#include <shcore.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <cwctype>
#include "lyrics.h"

static const IID IID_IAsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

struct IAsyncInfo : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(unsigned int *id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int *status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *errorCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

template<typename TResult>
struct IAsyncOperation : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void **handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(TResult *results) = 0;
};

struct IGlobalSystemMediaTransportControlsSessionManagerStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOperation<void*>** operation) = 0;
};

struct ISessionList : public IInspectable { // IVectorView<GlobalSystemMediaTransportControlsSession>
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, void** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned* size) = 0;
};

struct IGSMTCSessionManager : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(void** session) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(ISessionList** sessions) = 0;
};

// Vtable order checked against the Windows.Media.Control WinRT metadata; every slot up to the last one we call must exist.
struct IGSMTCSession : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOperation<void*>** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(int mode, IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(double rate, IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(unsigned char active, IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64 position, IInspectable** operation) = 0; // 100 ns ticks
};

struct IGSMTCPlaybackInfo : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(void** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int* value) = 0; // 4 = Playing, 5 = Paused
};

struct IGSMTCTimeline : public IInspectable { // TimeSpan/DateTime are 100 ns INT64 ticks; DateTime counts from 1601 like FILETIME
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(INT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(INT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(INT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(INT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(INT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(INT64* value) = 0;
};

struct IGSMTCMediaProperties : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(void **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(void **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(void **value) = 0; // IRandomAccessStreamReference
};

struct IStreamRef : public IInspectable { // Windows.Storage.Streams.IRandomAccessStreamReference
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOperation<void*>** operation) = 0;
};

constexpr int ART_PX = 112; // cached cover size: 2x the 56 px tile, ~50 KB

class MediaManager {
public:
    const std::wstring idleTitle = L"Esperando reproduccion...";
    std::wstring currentTitle = idleTitle;
    std::wstring currentArtist = L"Cualquier app de audio";
    std::wstring currentApp = L"Windows Media";
    bool isPlaying = false;
    bool showLyrics = false; // lyrics toggle: panel under the controls + current line in the closed notch

    // Timeline snapshot: the app reports a position at a moment; Elapsed() runs it forward while playing.
    double position = 0, duration = 0;
    INT64 positionStamp = 0;

    // Synced lyrics, written by the fetch thread (guarded by lyricsMutex)
    std::mutex lyricsMutex;
    std::vector<LyricLine> lyricLines;
    std::wstring lyricStatus;
    std::wstring lyricKey; // title + artist the lines belong to
    bool lyricBusy = false;

    struct LyricView {
        int index = -1;
        std::wstring prev, cur, next, status;
        std::wstring line; // current line if it is sung (not a break): what the collapsed notch shows
        bool retry = false;
    };
    IGSMTCSessionManager* manager = NULL; // requested once and kept for the life of the process
    std::wstring lastSource; // AUMID of the session shown last

    // Cover art and the two ambient tints taken from it (left/right halves). Defaults: cool blue, warm gold.
    // Not freed in a destructor: g_App outlives GdiplusShutdown.
    Gdiplus::Bitmap* art = NULL;
    Gdiplus::Color tint[2] = { Gdiplus::Color(255, 40, 86, 200), Gdiplus::Color(255, 190, 150, 40) };
    std::wstring artKey;
    int artLoads = 0; // re-reads after a track change: some players publish the new cover a moment after the title

    using SessionCommand = HRESULT (STDMETHODCALLTYPE IGSMTCSession::*)(IInspectable**);

    // Drives the session Windows reports as current; media keys only when no app exposes one.
    void Send(SessionCommand cmd, BYTE mediaKey) {
        IGSMTCSession* s = CurrentSession();
        IInspectable* op = NULL;
        if (s && SUCCEEDED((s->*cmd)(&op)) && op) {
            op->Release(); // the command still runs after the handle is dropped
        } else {
            keybd_event(mediaKey, 0, KEYEVENTF_EXTENDEDKEY, 0);
            keybd_event(mediaKey, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
        }
        if (s) s->Release();
    }

    void PlayPause() {
        Send(&IGSMTCSession::TryTogglePlayPauseAsync, VK_MEDIA_PLAY_PAUSE);
        position = Elapsed(); // freeze/resume the lyric clock from here
        positionStamp = Now();
        isPlaying = !isPlaying; // optimistic; the next refresh reads the real state
    }

    void Next() { Send(&IGSMTCSession::TrySkipNextAsync, VK_MEDIA_NEXT_TRACK); }

    void Previous() { Send(&IGSMTCSession::TrySkipPreviousAsync, VK_MEDIA_PREV_TRACK); }

    void Seek(double seconds) {
        IGSMTCSession* s = CurrentSession();
        IInspectable* op = NULL;
        if (s && SUCCEEDED(s->TryChangePlaybackPositionAsync((INT64)(seconds * 1e7), &op)) && op) op->Release();
        if (s) s->Release();
        position = seconds; // move the clock (bar, lyrics) now; the next refresh reads the app's real position
        positionStamp = Now();
    }

    static INT64 Now() {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        return ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }

    double Elapsed() const {
        double e = position;
        if (isPlaying && positionStamp > 0) e += (std::max)(0.0, (Now() - positionStamp) / 1e7);
        return duration > 0 ? (std::min)(e, duration) : e;
    }

    void ToggleLyrics() {
        showLyrics = !showLyrics;
        if (showLyrics) RequestLyrics();
    }

    // Starts a background fetch when the track changed; results land under lyricsMutex.
    void RequestLyrics() {
        if (currentTitle == idleTitle) return;
        std::wstring key = currentTitle + L"\n" + currentArtist;
        std::lock_guard<std::mutex> lock(lyricsMutex);
        if (key == lyricKey) return;
        lyricKey = key;
        lyricLines.clear();
        if (duration <= 0) { // without a timeline the lines cannot follow the song
            lyricStatus = L"Esta app no comparte la posición de la canción.";
            lyricBusy = false;
            return;
        }
        lyricStatus = L"Buscando letras sincronizadas…";
        lyricBusy = true;
        std::wstring title = currentTitle, artist = currentArtist;
        int dur = (int)(duration + 0.5);
        std::thread([this, key, title, artist, dur]() {
            std::vector<LyricLine> lines;
            std::wstring status = FetchSyncedLyrics(title, artist, dur, lines);
            std::lock_guard<std::mutex> lock(lyricsMutex);
            if (key != lyricKey) return; // the track changed while this was in flight
            lyricLines = std::move(lines);
            lyricStatus = status;
            lyricBusy = false;
        }).detach();
    }

    void RetryLyrics() {
        {
            std::lock_guard<std::mutex> lock(lyricsMutex);
            lyricKey.clear();
        }
        RequestLyrics();
    }

    // What the lyrics panel and the closed notch show right now.
    LyricView Lyrics() {
        LyricView v;
        double at = Elapsed();
        std::lock_guard<std::mutex> lock(lyricsMutex);
        const auto& l = lyricLines;
        int n = (int)l.size();
        v.status = lyricStatus;
        v.retry = n == 0 && !lyricBusy && duration > 0;
        v.index = LyricIndex(l, at);
        if (v.index > 0) v.prev = l[v.index - 1].text;
        v.cur = v.index >= 0 ? l[v.index].text : (n > 0 ? L"♪" : L"");
        if (v.index + 1 < n) v.next = l[v.index + 1].text;
        if (v.index >= 0 && IsVocalLine(l[v.index].text)) v.line = l[v.index].text;
        return v;
    }

    // Spin briefly on a WinRT async op; local media calls finish in a few ms.
    static void Wait(IInspectable* op, int maxMs) {
        IAsyncInfo* info = NULL;
        op->QueryInterface(IID_IAsyncInfo, (void**)&info);
        int status = 0;
        for (int t = 0; info && SUCCEEDED(info->get_Status(&status)) && status == 0 && t < maxMs; t += 5) Sleep(5);
        if (info) info->Release();
    }

    static int Status(IGSMTCSession* s) {
        void* raw = NULL;
        int status = 0;
        if (SUCCEEDED(s->GetPlaybackInfo(&raw)) && raw) {
            ((IGSMTCPlaybackInfo*)raw)->get_PlaybackStatus(&status);
            ((IGSMTCPlaybackInfo*)raw)->Release();
        }
        return status;
    }

    static std::wstring SourceId(IGSMTCSession* s) {
        HSTRING h = NULL;
        std::wstring id;
        if (SUCCEEDED(s->get_SourceAppUserModelId(&h)) && h) {
            id = WindowsGetStringRawBuffer(h, NULL);
            WindowsDeleteString(h);
        }
        return id;
    }

    // AUMIDs come as "Spotify.exe", "C:\...\foobar2000.exe", "Publisher.App_hash!App", "MSEdge" or a hash (Firefox).
    static std::wstring FriendlyName(std::wstring id) {
        static const wchar_t* known[][2] = {
            { L"Spotify", L"Spotify" }, { L"MSEdge", L"Edge" }, { L"Chrome", L"Chrome" }, { L"308046B0AF4A39CB", L"Firefox" },
            { L"Firefox", L"Firefox" }, { L"Opera", L"Opera" }, { L"Brave", L"Brave" }, { L"VLC", L"VLC" },
            { L"ZuneMusic", L"Reproductor multimedia" }, { L"ZuneVideo", L"Pel\u00EDculas y TV" }, { L"AppleMusic", L"Apple Music" },
        };
        for (auto& k : known) if (id.find(k[0]) != std::wstring::npos) return k[1];
        if (id.find(L'!') != std::wstring::npos) id = id.substr(0, id.find(L'_')); // packaged app: drop hash and entry point
        size_t slash = id.find_last_of(L"\\/");
        if (slash != std::wstring::npos) id = id.substr(slash + 1);
        if (id.size() > 4 && _wcsicmp(id.c_str() + id.size() - 4, L".exe") == 0) id.resize(id.size() - 4);
        size_t dot = id.rfind(L'.');
        if (dot != std::wstring::npos && dot + 1 < id.size()) id = id.substr(dot + 1); // "Publisher.App" -> "App"
        if (!id.empty()) id[0] = towupper(id[0]);
        return id.empty() ? L"Windows Media" : id;
    }

    // Picks the session to show and control (caller releases), NULL when no app exposes one.
    // Windows' "current" session is just the last one that took focus, so prefer whatever is actually playing,
    // then the session shown last (pausing it must not jump to another app), then Windows' pick.
    IGSMTCSession* CurrentSession() {
        if (!manager) {
            HSTRING hs = NULL;
            WindowsCreateString(L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager", 70, &hs);
            IID staticsIid;
            IIDFromString(L"{2050c4ee-11a0-57de-aed7-c97c70338245}", &staticsIid);
            IGlobalSystemMediaTransportControlsSessionManagerStatics* pStatics = NULL;
            if (SUCCEEDED(RoGetActivationFactory(hs, staticsIid, (void**)&pStatics)) && pStatics) {
                IAsyncOperation<void*>* pOp = NULL;
                if (SUCCEEDED(pStatics->RequestAsync(&pOp)) && pOp) {
                    Wait(pOp, 500);
                    void* pMgrRaw = NULL;
                    if (SUCCEEDED(pOp->GetResults(&pMgrRaw)) && pMgrRaw) manager = (IGSMTCSessionManager*)pMgrRaw;
                    pOp->Release();
                }
                pStatics->Release();
            }
            WindowsDeleteString(hs);
        }
        if (!manager) return NULL;

        IGSMTCSession* playing = NULL;
        IGSMTCSession* previous = NULL;
        ISessionList* list = NULL;
        if (SUCCEEDED(manager->GetSessions(&list)) && list) {
            unsigned n = 0;
            list->get_Size(&n);
            for (unsigned i = 0; i < n && !playing; ++i) {
                void* item = NULL;
                if (FAILED(list->GetAt(i, &item)) || !item) continue;
                IGSMTCSession* s = (IGSMTCSession*)item;
                if (Status(s) == 4) playing = s;
                else if (!previous && !lastSource.empty() && SourceId(s) == lastSource) previous = s;
                else s->Release();
            }
            list->Release();
        }
        if (playing) {
            if (previous) previous->Release();
            return playing;
        }
        if (previous) return previous;

        void* pSessionRaw = NULL;
        if (FAILED(manager->GetCurrentSession(&pSessionRaw))) pSessionRaw = NULL;
        return (IGSMTCSession*)pSessionRaw;
    }

    // Adapts universally to ANY audio/music playing on Windows 10/11 (Sonora, Spotify, Chrome, Edge, VLC, Apple Music, etc.)
    void DetectActiveTrack() {
        IGSMTCSession* pSession = CurrentSession();
        if (!pSession) {
            currentTitle = idleTitle;
            currentArtist = L"Cualquier app de audio";
            currentApp = L"Windows Media";
            isPlaying = false;
            position = duration = 0;
            ClearArt();
            artKey.clear();
            FallbackScan();
            return;
        }

        lastSource = SourceId(pSession);
        currentApp = FriendlyName(lastSource);

        void* pInfoRaw = NULL;
        if (SUCCEEDED(pSession->GetPlaybackInfo(&pInfoRaw)) && pInfoRaw) {
            IGSMTCPlaybackInfo* pInfo = (IGSMTCPlaybackInfo*)pInfoRaw;
            int status = 0;
            if (SUCCEEDED(pInfo->get_PlaybackStatus(&status))) isPlaying = (status == 4);
            pInfo->Release();
        }

        void* pTimelineRaw = NULL;
        if (SUCCEEDED(pSession->GetTimelineProperties(&pTimelineRaw)) && pTimelineRaw) {
            IGSMTCTimeline* tl = (IGSMTCTimeline*)pTimelineRaw;
            INT64 start = 0, end = 0, pos = 0, updated = 0;
            tl->get_StartTime(&start);
            tl->get_EndTime(&end);
            tl->get_Position(&pos);
            tl->get_LastUpdatedTime(&updated);
            duration = (end - start) / 1e7;
            position = (pos - start) / 1e7;
            positionStamp = updated;
            tl->Release();
        }

        IAsyncOperation<void*>* pPropOp = NULL;
        if (SUCCEEDED(pSession->TryGetMediaPropertiesAsync(&pPropOp)) && pPropOp) {
            Wait(pPropOp, 100);
            void* pPropsRaw = NULL;
            if (SUCCEEDED(pPropOp->GetResults(&pPropsRaw)) && pPropsRaw) {
                IGSMTCMediaProperties* pProps = (IGSMTCMediaProperties*)pPropsRaw;
                HSTRING titleHs = NULL, artistHs = NULL;
                pProps->get_Title(&titleHs);
                pProps->get_Artist(&artistHs);
                bool detected = false;
                if (titleHs) {
                    std::wstring t = WindowsGetStringRawBuffer(titleHs, NULL);
                    if (!t.empty()) {
                        currentTitle = t;
                        detected = true;
                    }
                    WindowsDeleteString(titleHs);
                }
                if (artistHs) {
                    std::wstring a = WindowsGetStringRawBuffer(artistHs, NULL);
                    if (!a.empty()) currentArtist = a;
                    WindowsDeleteString(artistHs);
                }

                if (detected && showLyrics) RequestLyrics();

                std::wstring key = currentTitle + L"\n" + currentArtist;
                if (detected && key != artKey) {
                    artKey = key;
                    artLoads = 3;
                    ClearArt();
                }
                if (artLoads > 0) {
                    artLoads--;
                    LoadArt(pProps);
                }

                pProps->Release();
            }
            pPropOp->Release();
        }
        pSession->Release();
    }

    void ClearArt() {
        delete art;
        art = NULL;
        tint[0] = Gdiplus::Color(255, 40, 86, 200);
        tint[1] = Gdiplus::Color(255, 190, 150, 40);
    }

    // Decodes the session thumbnail into a small square cache (center-cropped) and re-derives the tints.
    void LoadArt(IGSMTCMediaProperties* props) {
        void* refRaw = NULL;
        if (FAILED(props->get_Thumbnail(&refRaw)) || !refRaw) return;
        IStreamRef* ref = (IStreamRef*)refRaw;
        IStream* stream = NULL;
        IAsyncOperation<void*>* op = NULL;
        if (SUCCEEDED(ref->OpenReadAsync(&op)) && op) {
            Wait(op, 300);
            void* rasRaw = NULL;
            if (SUCCEEDED(op->GetResults(&rasRaw)) && rasRaw) {
                if (FAILED(CreateStreamOverRandomAccessStream((IUnknown*)rasRaw, __uuidof(IStream), (void**)&stream))) stream = NULL;
                ((IUnknown*)rasRaw)->Release();
            }
            op->Release();
        }
        ref->Release();
        if (!stream) return;

        {
            Gdiplus::Bitmap full(stream);
            int w = (int)full.GetWidth(), h = (int)full.GetHeight();
            if (full.GetLastStatus() == Gdiplus::Ok && w > 0 && h > 0) {
                int side = (std::min)(w, h); // video thumbnails are 16:9; keep the middle square
                Gdiplus::Bitmap* cover = new Gdiplus::Bitmap(ART_PX, ART_PX, PixelFormat32bppARGB);
                Gdiplus::Graphics g(cover);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                g.DrawImage(&full, Gdiplus::Rect(0, 0, ART_PX, ART_PX), (w - side) / 2, (h - side) / 2, side, side, Gdiplus::UnitPixel);
                delete art;
                art = cover;
                ExtractTints();
            }
        }
        stream->Release();
    }

    // Saturation-weighted average of each half, lifted to a usable brightness so dark covers still glow.
    void ExtractTints() {
        Gdiplus::BitmapData d;
        Gdiplus::Rect r(0, 0, ART_PX, ART_PX);
        if (art->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &d) != Gdiplus::Ok) return;
        double acc[2][4] = {};
        for (int y = 0; y < ART_PX; ++y) {
            const BYTE* row = (const BYTE*)d.Scan0 + y * d.Stride;
            for (int x = 0; x < ART_PX; ++x) {
                const BYTE* p = row + x * 4; // B, G, R, A
                double w = ((std::max)({ p[0], p[1], p[2] }) - (std::min)({ p[0], p[1], p[2] })) + 16.0;
                double* a = acc[x * 2 / ART_PX];
                a[0] += p[2] * w;
                a[1] += p[1] * w;
                a[2] += p[0] * w;
                a[3] += w;
            }
        }
        art->UnlockBits(&d);
        for (int i = 0; i < 2; ++i) {
            double rr = acc[i][0] / acc[i][3], gg = acc[i][1] / acc[i][3], bb = acc[i][2] / acc[i][3];
            double k = 210.0 / (std::max)({ rr, gg, bb, 1.0 });
            tint[i] = Gdiplus::Color(255, (BYTE)(rr * k), (BYTE)(gg * k), (BYTE)(bb * k));
        }
    }

    void FallbackScan() {
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            if (!IsWindowVisible(hwnd)) return TRUE;
            wchar_t title[256];
            GetWindowTextW(hwnd, title, 256);
            std::wstring str(title);
            if (str.find(L" - YouTube") != std::wstring::npos || str.find(L"Spotify") != std::wstring::npos) {
                auto* self = reinterpret_cast<MediaManager*>(lParam);
                size_t sep = str.find(L" - ");
                if (sep != std::wstring::npos) {
                    self->currentArtist = str.substr(0, sep);
                    self->currentTitle = str.substr(sep + 3);
                    size_t yt = self->currentTitle.find(L" - YouTube");
                    if (yt != std::wstring::npos) self->currentTitle = self->currentTitle.substr(0, yt);
                    self->currentApp = L"Reproductor Web";
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
    }
};
