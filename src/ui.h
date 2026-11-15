#pragma once
#include <windows.h>
#include <gdiplus.h>
#include "types.h"
#include "pomodoro.h"
#include "todo.h"
#include "events.h"
#include "media.h"
#include "ai_client.h"
#include "tabs.h"
#include "shelf.h"
#include <cmath>
#include <ctime>

// Width of the concave "ears" where the screen edge flows into the notch; content sits between them.
constexpr int NOTCH_SHOULDER = 12;

constexpr int LYRIC_SLIDE_MS = 260;
constexpr int CLOSED_W = 220;     // collapsed content width with a title
constexpr int CLOSED_MAX_W = 560; // longer collapsed text widens the notch up to this, then ellipsizes
constexpr int CLOSED_CHROME = 104; // art tile + clock + padding around the centered text
constexpr int CLOSED_CHROME_LEFT = 60; // the same without a value on the right
constexpr int CLOSED_FIT_MIN = 130;    // narrowest pill of the views that hug their content (Sistema, Pomodoro)
constexpr int TAB_STEP = 38;          // icon tab row: distance between tabs and the highlight's width
constexpr int TAB_W = 34;
constexpr int TAB_ANIM_MS = 220;      // tab switch: highlight glide, content slide and fade
inline const Gdiplus::Color ACCENT(255, 124, 156, 255); // soft periwinkle: active tab, bars, sparklines, focus edges
constexpr int PANEL_W = 460;          // expanded content column
constexpr int MEDIA_COL_LYRICS = 300; // media controls column while lyrics share the panel
constexpr int LYRICS_PANEL_W = 740;   // panel width with lyrics: the lyrics column gets the other 440 px
constexpr int CAL_PANEL_W = 760;      // calendar tab content size (the expanded notch grows for it)
constexpr int CAL_PANEL_H = 400;
constexpr int CAL_POP_H = 524; // the panel grows while the event window is open so the window has room (card 458 + margins)

class CapriUI {
public:
    // Tab switch: when it started and which way the content slides (set by TabChanged)
    ULONGLONG tabChangedAt = 0;
    int tabFrom = 0, tabDir = 1;
    // Sistema: the last sample, and the CPU history for the sparkline (ring buffer of 60 samples)
    CapriSys sys = {};
    float cpuHist[60] = {};
    int cpuHistPos = 0;
    // Clima: the last forecast, the place it is for (found once; autoAsked/cityAsked are the settings it was asked with),
    // when it arrived; status 0 none, 1 loading, 2 ok, 3 error, 4 place unknown
    struct WeatherState { CapriWeather data = {}; std::wstring place; double lat = 0, lon = 0; bool located = false; bool autoAsked = true; std::wstring cityAsked; ULONGLONG at = 0; int status = 0; } weather;

    // Lyric line change animation state
    int lyricIndex = -2;
    std::wstring lyricText;
    ULONGLONG lyricChangedAt = 0;

    // Task list view state: first visible row, and the add field's text while typing
    size_t todoTop = 0;
    bool editing = false; // typing into a text field (tasks, or the calendar event window)
    std::wstring todoDraft;
    size_t todoParent = SIZE_MAX; // task the field adds a subtask to; SIZE_MAX adds a normal task
    static constexpr size_t TODO_ROWS = 4;
    // Archivos: first visible row, whether a drop or a click sends (true) or keeps/opens (false), and the footer note of the last send
    size_t shelfTop = 0;
    bool shelfSend = false;
    std::wstring shelfNote;
    // Thumbnails of the tiles painted so far (NULL = none to show), all at shelfThumbPx. Loaded on first paint, all freed
    // when the notch closes, the shelf is left or the tiles change size.
    std::vector<std::pair<std::wstring, HBITMAP>> shelfThumbCache;
    int shelfThumbPx = 0;
    // shortcut: read on the UI thread; Windows serves it from its thumbnail cache, but a huge new image can stall one frame. Upgrade to a worker thread if that shows.
    HBITMAP ShelfThumb(const std::wstring& path, int px) {
        if (px != shelfThumbPx) ShelfThumbsFree();
        shelfThumbPx = px;
        for (auto& t : shelfThumbCache) if (t.first == path) return t.second;
        HBITMAP bmp = NULL;
        IShellItemImageFactory* f = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), NULL, IID_IShellItemImageFactory, (void**)&f))) {
            f->GetImage({ px, px }, SIIGBF_RESIZETOFIT, &bmp);
            f->Release();
        }
        shelfThumbCache.push_back({ path, bmp }); // a failure is kept too, so it is not retried on every frame
        return bmp;
    }
    void ShelfThumbsFree() {
        for (auto& t : shelfThumbCache) if (t.second) DeleteObject(t.second);
        shelfThumbCache.clear();
        shelfThumbCache.shrink_to_fit();
    }

    // Calendar: view, selected day (YYYYMMDD, 0 = today) that every view is built around
    enum CalView { CAL_AGENDA, CAL_DAY, CAL_WEEK, CAL_MONTH };
    CalView calView = CAL_AGENDA;
    int calSel = 0;
    int calEvent = -1;     // selected event (index into events), shown in the detail strip
    // New/edit event window (kept while the panel is closed, so a draft survives): the draft, the event it rewrites (-1 = new), focused field
    bool calPop = false;
    CalEvent popEv{ 0, -1, -1, 0, 0, L"" };
    int popEdit = -1;
    int popFocus = 0; // 0 title, 1 notes
    int calHour = 8;       // first hour row of the day/week grid
    size_t calAgendaTop = 0;
    bool calHidden[CAL_CAT_COUNT] = {}; // categories switched off for this session
    // Sync state of each service (1 Google, 2 Outlook): the app sets it before every paint
    enum SyncDot { SYNC_OFF, SYNC_ON, SYNC_BUSY, SYNC_ERR };
    SyncDot syncDot[3] = { SYNC_OFF, SYNC_OFF, SYNC_OFF };
    bool linkGoogle = false; // the calendar link's host is Google's: its events are marked G, others O

    // Clickable areas, rebuilt on every calendar paint (so the click handler never repeats the layout); later = on top
    enum { HIT_DAY, HIT_PREV, HIT_NEXT, HIT_TODAY, HIT_VIEW, HIT_CAT, HIT_EVENT, HIT_SLOT,
           HIT_COLOR, HIT_MOVE, HIT_REPEAT, HIT_EDIT, HIT_DELETE, HIT_NEW,
           HIT_POP_BACK, HIT_POP_CARD, HIT_POP_TITLE, HIT_POP_NOTES, HIT_POP_DATE, HIT_POP_ALLDAY, HIT_POP_START, HIT_POP_END,
           HIT_POP_REPEAT, HIT_POP_COLOR, HIT_POP_ALERT, HIT_POP_SAVE, HIT_POP_DELETE, HIT_POP_CANCEL, HIT_POP_SOURCE,
           HIT_POP_TASK, HIT_TASK_DONE };
    struct CalHit { Gdiplus::RectF r; int kind, a, b; };
    std::vector<CalHit> calHits;
    std::vector<CalHit> tabHits; // clickable areas of the clock and weather tabs (rebuilt on every expanded paint)
    enum { HIT_W_RETRY, HIT_W_SETUP, HIT_S_MODE, HIT_S_ITEM, HIT_S_REMOVE }; // kinds in tabHits
    static constexpr const wchar_t* DAY_NAMES[] = { L"Domingo", L"Lunes", L"Martes", L"Miércoles", L"Jueves", L"Viernes", L"Sábado" };
    static constexpr const wchar_t* MONTH_NAMES[] = { L"enero", L"febrero", L"marzo", L"abril", L"mayo", L"junio", L"julio",
                                                      L"agosto", L"septiembre", L"octubre", L"noviembre", L"diciembre" };

    struct Month { int year, month, firstCol, days; }; // month 0-11; firstCol: column of day 1, weeks start on Monday
    static Month CalMonth(int year, int month0) {
        tm first = {};
        first.tm_year = year - 1900;
        first.tm_mon = month0; // out-of-range months roll over into other years
        first.tm_mday = 1;
        first.tm_hour = 12;    // midday: clear of DST jumps
        mktime(&first);
        tm last = first;
        last.tm_mon += 1;
        last.tm_mday = 0;      // day 0 of next month = last day of this one
        mktime(&last);
        return { first.tm_year + 1900, first.tm_mon, (first.tm_wday + 6) % 7, last.tm_mday };
    }

    // The date `days` away from y-m-d (month 0-11), normalized by mktime
    static tm CalDay(int year, int month0, int day, int days) {
        tm t = {};
        t.tm_year = year - 1900;
        t.tm_mon = month0;
        t.tm_mday = day + days;
        t.tm_hour = 12;
        mktime(&t);
        return t;
    }

    static int DateKey(int year, int month0, int day) { return year * 10000 + (month0 + 1) * 100 + day; }
    static int DateKey(const tm& t) { return DateKey(t.tm_year + 1900, t.tm_mon, t.tm_mday); }
    static int TodayKey() {
        SYSTEMTIME now;
        GetLocalTime(&now);
        return DateKey(now.wYear, now.wMonth - 1, now.wDay);
    }
    static tm KeyDay(int key, int days = 0) { return CalDay(key / 10000, key / 100 % 100 - 1, key % 100, days); }
    int SelKey() const { return calSel ? calSel : TodayKey(); }
    static std::wstring DayShort(int key, bool full = false) {
        tm t = KeyDay(key);
        return std::wstring(DAY_NAMES[t.tm_wday]).substr(0, full ? std::wstring::npos : 3) + L" " + std::to_wstring(t.tm_mday) + L" " +
               std::wstring(MONTH_NAMES[t.tm_mon]).substr(0, 3);
    }
    static std::wstring TimeRange(const CalEvent& e) {
        if (e.minute < 0) return L"Todo el día";
        return EventTime(e.minute) + (e.end >= 0 ? L"–" + EventTime(e.end) : std::wstring());
    }
    // A task's mark before its title in the calendar: open ○, done ✓ (escapes, so the source stays ASCII)
    static std::wstring EventLabel(const CalEvent& e) {
        return e.task == 1 ? L"○ " + e.title : e.task == 2 ? L"✓ " + e.title : e.title;
    }
    // Reminders and the idle pill skip a done task, and a dated task while tasksInCalendar is off
    static bool RemindsFor(const CalEvent& e) { return e.task == 0 || (e.task == 1 && g_settings.tasksInCalendar); }
    // The dated tasks the Tareas tab lists first (indices into ev.events, stored order); none while tasksInCalendar is off.
    // Callers take it fresh after every change to the events, since the indices move.
    static std::vector<size_t> DatedTasks(const EventManager& ev, int today) {
        std::vector<size_t> out;
        if (!g_settings.tasksInCalendar) return out;
        for (size_t i = 0; i < ev.events.size(); ++i) if (TaskListed(ev.events[i], today)) out.push_back(i);
        return out;
    }

    // Arrows (and the wheel in agenda/month): move the selection by a week, a day or a month (keeping the day where it exists)
    void CalPage(int dir, bool byMonth = false) {
        tm s = KeyDay(SelKey());
        if (byMonth || calView == CAL_MONTH) {
            Month m = CalMonth(s.tm_year + 1900, s.tm_mon + dir);
            CalSelect(DateKey(m.year, m.month, (std::min)(s.tm_mday, m.days)));
        } else {
            CalSelect(DateKey(KeyDay(SelKey(), calView == CAL_DAY ? dir : 7 * dir)));
        }
    }

    void CalSelect(int key) {
        calSel = key;
        calAgendaTop = 0;
        CalHome();
    }

    // First hour row: an hour before now when the day/week holds today, else 8:00
    void CalHome() {
        tm s = KeyDay(SelKey());
        int first = calView == CAL_WEEK ? DateKey(KeyDay(SelKey(), -((s.tm_wday + 6) % 7))) : SelKey();
        int last = calView == CAL_WEEK ? DateKey(KeyDay(first, 6)) : first, today = TodayKey();
        SYSTEMTIME now;
        GetLocalTime(&now);
        calHour = today >= first && today <= last ? (std::max)(0, (std::min)(15, now.wHour - 1)) : 8;
    }

    // Wheel: scrolls what the view has to scroll (agenda rows, grid hours), pages the month view
    void CalWheel(int dir) {
        if (calView == CAL_MONTH) CalPage(dir);
        else if (calView == CAL_AGENDA) calAgendaTop = (size_t)(std::max)(0, (int)calAgendaTop + dir);
        else calHour = (std::max)(0, (std::min)(15, calHour + dir));
    }

    // Transient banner over the collapsed pill (charger, event reminder), set by the app until noticeUntil
    std::wstring notice, noticeRight, noticeIcon;
    Gdiplus::Color noticeColor;
    ULONGLONG noticeUntil = 0;

    // True while the collapsed pill has something moving: the equalizer, or the idle eyes' blink window
    bool PillLive(bool expanded, bool musicPlaying) const {
        return !expanded && ((g_settings.equalizer && musicPlaying) || (g_settings.eyes && GetTickCount64() % 4500 < 200));
    }

    // Idle pill text: the next timed event today, else the open task count, else the date. todayTimed is (minute, title).
    static std::wstring IdleLine(const std::vector<std::pair<int, std::wstring>>& todayTimed, int nowMinute, int openTasks,
                                 const std::wstring& dateText) {
        for (const auto& e : todayTimed)
            if (e.first >= nowMinute) return EventTime(e.first) + L" · " + e.second;
        if (openTasks > 0) return std::to_wstring(openTasks) + (openTasks == 1 ? L" pendiente" : L" pendientes");
        return dateText;
    }

    bool Animating() const {
        if (!EffectivePerf().anim) return false;
        ULONGLONG t = GetTickCount64();
        return t - lyricChangedAt < LYRIC_SLIDE_MS || t - tabChangedAt < TAB_ANIM_MS;
    }

    // Starts the switch animation. Opening Sistema samples at once so memory and disk are not blank for a second.
    void TabChanged(int from, int to) {
        if (from == to) return;
        if (from == TAB_SHELF) ShelfThumbsFree();
        tabFrom = from;
        tabDir = to > from ? 1 : -1;
        tabChangedAt = GetTickCount64();
        if (to == TAB_SYSTEM) capri_sys_sample(&sys, g_settings.sysTemps, g_settings.sysPower);
    }

    // Media column for an expanded content width: full panel normally, shrinking to MEDIA_COL_LYRICS as the panel
    // widens for lyrics. Derived from the animated width so the controls glide instead of jumping.
    static int MediaColumn(int width) {
        if (width <= PANEL_W) return PANEL_W;
        float t = (std::min)(1.0f, (float)(width - PANEL_W) / (LYRICS_PANEL_W - PANEL_W));
        return PANEL_W - (int)((PANEL_W - MEDIA_COL_LYRICS) * t);
    }

    // Center of prev/play/next: the column middle, drifting left as the column narrows so the transport keeps
    // clear of the lyrics button at the column's right end.
    static float TransportCenter(int col) {
        return col / 2.0f - 25.0f * (PANEL_W - col) / (PANEL_W - MEDIA_COL_LYRICS);
    }

    static std::wstring FormatTime(double seconds) {
        int t = (int)seconds;
        wchar_t buf[16];
        wsprintfW(buf, L"%d:%02d", t / 60, t % 60);
        return buf;
    }

    // Collapsed notch text last drawn, and the content width that fits it whole
    std::wstring pillText;
    int pillWidth = CLOSED_W;

    // The sung line shown in the collapsed notch, empty when the title should show instead.
    static std::wstring ClosedLyric(MediaManager& media, PomodoroManager& pomo) {
        if (!g_settings.pillLyrics || !media.showLyrics || !media.isPlaying || pomo.state.isRunning) return L"";
        return media.Lyrics().line;
    }

    // Collapsed content width that fits `text` in the closed-notch font (same font and 96-DPI units as Render).
    static int ClosedWidthFor(const std::wstring& text, int minW = CLOSED_W, int chrome = CLOSED_CHROME) {
        if (text.empty()) return minW;
        Gdiplus::Bitmap bmp(1, 1, PixelFormat32bppARGB);
        bmp.SetResolution(96.0f, 96.0f);
        Gdiplus::Graphics g(&bmp);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font f(&ff, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::RectF box;
        g.MeasureString(text.c_str(), -1, &f, Gdiplus::PointF(0.0f, 0.0f), &box);
        return (std::max)(minW, (std::min)(CLOSED_MAX_W, (int)ceilf(box.Width) + chrome));
    }

    void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float width, float height, float radius) {
        float d = radius * 2.0f;
        path.AddArc(x, y, d, d, 180, 90);
        path.AddArc(x + width - d, y, d, d, 270, 90);
        path.AddArc(x + width - d, y + height - d, d, d, 0, 90);
        path.AddArc(x, y + height - d, d, d, 90, 90);
        path.CloseFigure();
    }

    // Notch contour: flat top flush with the screen, concave shoulders, rounded bottom corners.
    void AddNotch(Gdiplus::GraphicsPath& p, float w, float h, float r) {
        float s = (float)NOTCH_SHOULDER, l = s, rt = w - s;
        r = fminf(r, h - s);
        p.AddLine(0.0f, 0.0f, w, 0.0f);
        p.AddBezier(w, 0.0f, rt, 0.0f, rt, s * 0.38f, rt, s);
        p.AddLine(rt, s, rt, h - r);
        p.AddBezier(rt, h - r, rt, h - r * 0.448f, rt - r * 0.448f, h, rt - r, h);
        p.AddLine(rt - r, h, l + r, h);
        p.AddBezier(l + r, h, l + r * 0.448f, h, l, h - r * 0.448f, l, h - r);
        p.AddLine(l, h - r, l, s);
        p.AddBezier(l, s, l, s * 0.38f, l, 0.0f, 0.0f, 0.0f);
        p.CloseFigure();
    }

    void Glow(Gdiplus::Graphics& g, float cx, float cy, float rx, float ry, Gdiplus::Color c) {
        Gdiplus::GraphicsPath e;
        e.AddEllipse(cx - rx, cy - ry, rx * 2.0f, ry * 2.0f);
        Gdiplus::PathGradientBrush b(&e);
        b.SetCenterColor(c);
        Gdiplus::Color clear(0, c.GetR(), c.GetG(), c.GetB());
        int n = 1;
        b.SetSurroundColors(&clear, &n);
        b.SetBlendBellShape(1.0f, 1.0f);
        g.FillPath(&b, &e);
    }

    // Texture-fill instead of clipping: path clips in GDI+ are not anti-aliased, the fill edge is.
    void FillArt(Gdiplus::Graphics& g, Gdiplus::Bitmap* art, float x, float y, float size, float radius) {
        Gdiplus::GraphicsPath p;
        AddRoundRect(p, x, y, size, size, radius);
        Gdiplus::TextureBrush tb(art, Gdiplus::WrapModeClamp);
        tb.TranslateTransform(x, y);
        tb.ScaleTransform(size / art->GetWidth(), size / art->GetHeight());
        g.FillPath(&tb, &p);
    }

    const wchar_t* IconFontName() {
        static const wchar_t* name = nullptr;
        if (!name) {
            Gdiplus::FontFamily fluent(L"Segoe Fluent Icons");
            name = fluent.IsAvailable() ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets"; // Win11 : Win10
        }
        return name;
    }

    void Render(void* pvBits, int stride, int pixelWidth, int pixelHeight, float scale, bool expanded, NotchTab activeTab,
                PomodoroManager& pomo, TodoManager& todo, EventManager& events, MediaManager& media, AICompanion& ai, AudioWakeEngine& wake, ShelfManager& shelf) {
        using namespace Gdiplus;

        const Perf perf = EffectivePerf(); // read once per frame: animations and glow off take the plain paths below

        // Wrap DIB section in GDI+ Graphics with exact scanline stride
        Bitmap bitmap(pixelWidth, pixelHeight, stride, PixelFormat32bppARGB, (BYTE*)pvBits);
        bitmap.SetResolution(96.0f, 96.0f); // else point-sized fonts get the monitor DPI on top of `scale`
        Graphics g(&bitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit); // ClearType tints glyph edges on a layered surface
        g.Clear(Color(0, 0, 0, 0));

        // Layout stays in 96-DPI units; drawing happens at the monitor's real pixel density
        g.ScaleTransform(scale, scale);
        int fullWidth = (int)lroundf(pixelWidth / scale);
        int height = (int)lroundf(pixelHeight / scale);

        // === Notch body: near-black glass ===
        GraphicsPath notchPath;
        AddNotch(notchPath, (float)fullWidth, (float)(height - 1), expanded ? 22.0f : 16.0f);

        PointF diagA(-1.0f, -1.0f), diagB((float)fullWidth + 1.0f, (float)height + 1.0f);
        if (perf.glow) {
            LinearGradientBrush glass(diagA, diagB, Color(255, 18, 19, 23), Color(255, 1, 2, 3));
            Color glassColors[] = { Color(255, 18, 19, 23), Color(255, 8, 9, 12), Color(255, 1, 2, 3) };
            REAL glassPos[] = { 0.0f, 0.46f, 1.0f };
            glass.SetInterpolationColors(glassColors, glassPos, 3);
            g.FillPath(&glass, &notchPath);
        } else {
            SolidBrush glass(Color(255, 8, 9, 12));
            g.FillPath(&glass, &notchPath);
        }

        if (expanded && perf.glow) {
            // Ambient light pooling at the base, tinted by the cover (blue/gold when nothing plays)
            g.SetClip(&notchPath);
            const Color& l = media.tint[0];
            const Color& r = media.tint[1];
            Glow(g, fullWidth * 0.10f, (float)height, fullWidth * 0.55f, height * 0.75f, Color(72, l.GetR(), l.GetG(), l.GetB()));
            Glow(g, fullWidth * 0.95f, (float)height, fullWidth * 0.50f, height * 0.70f, Color(80, r.GetR(), r.GetG(), r.GetB()));
            g.ResetClip();
        }

        // Hairline edge, skipped on the top line so the notch melts into the screen edge
        g.SetClip(RectF(0.0f, 1.0f, (float)fullWidth, (float)height));
        if (perf.glow) {
            LinearGradientBrush edgeBrush(diagA, diagB, Color(66, 255, 255, 255), Color(34, 255, 255, 255));
            Color edgeColors[] = { Color(66, 255, 255, 255), Color(9, 255, 255, 255), Color(34, 255, 255, 255) };
            REAL edgePos[] = { 0.0f, 0.4f, 1.0f };
            edgeBrush.SetInterpolationColors(edgeColors, edgePos, 3);
            Pen edgePen(&edgeBrush, 1.0f);
            g.DrawPath(&edgePen, &notchPath);
        } else {
            Pen edgePen(Color(30, 255, 255, 255), 1.0f);
            g.DrawPath(&edgePen, &notchPath);
        }
        g.ResetClip();

        // Content is laid out between the shoulders
        g.TranslateTransform((float)NOTCH_SHOULDER, 0.0f);
        int width = fullWidth - 2 * NOTCH_SHOULDER;

        // Fonts
        FontFamily fontFamily(L"Segoe UI");
        FontFamily iconFamily(IconFontName());
        Font fontSmall(&fontFamily, 9.0f, FontStyleRegular, UnitPoint);
        Font fontSmallBold(&fontFamily, 9.0f, FontStyleBold, UnitPoint);
        Font fontMedium(&fontFamily, 11.0f, FontStyleBold, UnitPoint);
        Font fontTitle(&fontFamily, 12.5f, FontStyleBold, UnitPoint);
        Font fontBig(&fontFamily, 22.0f, FontStyleBold, UnitPoint);
        Font iconSmall(&iconFamily, 8.0f, FontStyleRegular, UnitPoint);
        Font iconFont(&iconFamily, 11.0f, FontStyleRegular, UnitPoint);
        Font iconBig(&iconFamily, 18.0f, FontStyleRegular, UnitPoint);

        SolidBrush textWhite(Color(255, 242, 242, 245));
        SolidBrush textDim(Color(255, 142, 145, 152));
        SolidBrush ink(Color(255, 8, 9, 12));
        SolidBrush accentGreen(Color(255, 74, 222, 128));
        SolidBrush surface(Color(255, 22, 23, 27));
        Pen surfaceEdge(Color(26, 255, 255, 255), 1.0f);

        StringFormat center, leftMid, rightMid;
        center.SetAlignment(StringAlignmentCenter);
        leftMid.SetAlignment(StringAlignmentNear);
        rightMid.SetAlignment(StringAlignmentFar);
        for (StringFormat* f : { &center, &leftMid, &rightMid }) {
            f->SetLineAlignment(StringAlignmentCenter);
            f->SetTrimming(StringTrimmingEllipsisCharacter);
            f->SetFormatFlags(StringFormatFlagsNoWrap);
        }

        const wchar_t* tabIcons[] = { L"", L"", L"", L"",L"",L"",L"", L"\uE8B7" };
        if (!expanded) {
            // === COLLAPSED NOTCH === left tile / centered text / right value; the first state that applies wins
            ULONGLONG t = GetTickCount64();
            auto tile = [&](Color c) {
                GraphicsPath p;
                AddRoundRect(p, 10.0f, 7.0f, 20.0f, 20.0f, 6.0f);
                SolidBrush b(c);
                g.FillPath(&b, &p);
            };
            RectF leftSlot(10.0f, 7.0f, 20.0f, 20.0f), rightSlot((float)(width - 60), 0.0f, 50.0f, (float)height);
            std::wstring centerText, rightText;
            SolidBrush* rightBrush = &textWhite;
            bool fit = false, rightUsed = true;
            bool pomoOn = pomo.state.isRunning, music = media.currentTitle != media.idleTitle;
            SYSTEMTIME st;
            GetLocalTime(&st);

            SolidBrush noticeBrush(noticeColor);
            if (noticeUntil > t) {
                g.DrawString(noticeIcon.c_str(), -1, &iconFont, leftSlot, &center, &noticeBrush);
                centerText = notice;
                rightText = noticeRight;
                rightBrush = &noticeBrush;
            } else if (pomoOn || (activeTab == TAB_POMODORO && g_settings.pillPomoIdle)) { // running, or its tab was left selected: the timer at a glance
                tile(Color(255, 150, 45, 45));
                g.DrawString(tabIcons[TAB_POMODORO], -1, &iconSmall, leftSlot, &center, &textWhite);
                centerText = pomo.state.isBreak ? L"Descanso" : L"Enfoque";
                if (!pomoOn) centerText += pomo.state.remainingSeconds < pomo.state.totalSeconds ? L" · en pausa" : L" · listo";
                if (g_settings.pillPomoSessions) centerText += L" · " + std::to_wstring(pomo.state.sessionsCompleted) + (pomo.state.sessionsCompleted == 1 ? L" sesión" : L" sesiones");
                rightText = pomo.GetFormattedTime();
                fit = true;
                float frac = pomo.state.totalSeconds > 0 ? 1.0f - (float)pomo.state.remainingSeconds / pomo.state.totalSeconds : 0.0f;
                Pen trackPen(Color(30, 255, 255, 255), 2.0f), fillPen(Color(255, 239, 68, 68), 2.0f);
                float lineW = (float)(width - 32), y = (float)(height - 4);
                if (g_settings.pillPomoBar) g.DrawLine(&trackPen, 16.0f, y, 16.0f + lineW, y);
                if (g_settings.pillPomoBar && frac > 0.0f) g.DrawLine(&fillPen, 16.0f, y, 16.0f + lineW * (std::min)(1.0f, frac), y);
            } else if (activeTab == TAB_SYSTEM && g_settings.pillSys) {
                // Sistema left selected: its figures at a glance (the app keeps sampling while this shows); the pill widens to fit
                tile(Color(255, 34, 37, 48));
                SolidBrush accentB(ACCENT);
                g.DrawString(tabIcons[TAB_SYSTEM], -1, &iconSmall, leftSlot, &center, &accentB);
                auto whole = [](float v) { return std::to_wstring((int)lroundf(v)); };
                bool temps = g_settings.sysTemps;
                auto part = [&](const std::wstring& s) { centerText += (centerText.empty() ? L"" : L"  ·  ") + s; }; // only what Settings → Sistema leaves on
                if (g_settings.pillSysCpu) part(L"CPU " + whole(sys.cpu) + L" %" + (temps && sys.cpuTemp >= 0 ? L" " + whole(sys.cpuTemp) + L"°" : L""));
                if (g_settings.pillSysGpu && temps && sys.gpuTemp >= 0) part(L"GPU " + whole(sys.gpuTemp) + L"°");
                if (g_settings.pillSysRam && sys.memTotalGb > 0) part(L"RAM " + whole(100.0f * sys.memUsedGb / sys.memTotalGb) + L" %");
                if (g_settings.pillSysDisk && sys.diskTotalGb > 0) part(L"C: " + whole(sys.diskFreeGb) + L" GB");
                if (centerText.empty()) centerText = L"Sistema";
                float w = fabsf(sys.batteryW);
                if (!g_settings.pillSysPower) {
                } else if (g_settings.sysPower && sys.batteryWKnown && w >= 0.05f) { // one decimal under 10 W so it fits the slot
                    wchar_t buf[16];
                    swprintf(buf, 16, w < 10.0f ? L"%.1f W" : L"%.0f W", w);
                    rightText = (sys.batteryW > 0 ? L"+" : L"−") + std::wstring(buf);
                } else if (sys.battery >= 0) {
                    rightText = std::to_wstring(sys.battery) + L" %";
                }
                fit = true;
                rightUsed = !rightText.empty();
            } else if (music) {
                if (media.art && g_settings.pillArt) {
                    FillArt(g, media.art, 10.0f, 7.0f, 20.0f, 6.0f);
                } else {
                    tile(Color(255, 44, 48, 58));
                    g.DrawString(L"", -1, &iconSmall, leftSlot, &center, &textWhite);
                }
                // The sung line replaces the title (the notch widens to fit it); instrumental breaks fall back to the title
                centerText = ClosedLyric(media, pomo);
                if (centerText.empty()) centerText = media.currentTitle + (g_settings.pillArtist && !media.currentArtist.empty() ? L" · " + media.currentArtist : L"");
                // Equalizer: 4 bars dancing while playing, flat at 4 px when paused
                const Color& tn = media.tint[1];
                SolidBrush bar(Color(255, (tn.GetR() + 255) / 2, (tn.GetG() + 255) / 2, (tn.GetB() + 255) / 2));
                for (int i = 0; g_settings.equalizer && i < 4; ++i) {
                    float h = media.isPlaying ? 4.0f + 10.0f * (0.5f + 0.5f * sinf(t * 0.012f + i * 1.7f)) : 4.0f;
                    GraphicsPath p;
                    AddRoundRect(p, (float)(width - 30 + i * 5), (height - h) / 2.0f, 3.0f, h, 1.5f);
                    g.FillPath(&bar, &p);
                }
            } else {
                // Idle: Nusa's eyes (blink every 4.5 s, shut after 2 min without input), then what comes next today
                LASTINPUTINFO li = { sizeof(li) };
                bool asleep = GetLastInputInfo(&li) && GetTickCount() - li.dwTime > (DWORD)g_settings.sleepMin * 60000;
                if (g_settings.eyes) {
                    float eyeH = asleep ? 2.0f : (t % 4500) < 140 ? 2.0f : 9.0f;
                    SolidBrush eye(Color(255, 255, 255, 255));
                    for (float x : { 13.0f, 22.0f }) g.FillEllipse(&eye, x, 17.0f - eyeH / 2.0f, 5.0f, eyeH);
                    if (asleep) g.DrawString(L"z", -1, &fontSmall, PointF(28.0f, 2.0f), &textDim);
                } else {
                    g.DrawString(L"", -1, &iconSmall, leftSlot, &center, &textDim); // eyes off: a faint calendar in their place
                }

                std::vector<std::pair<int, std::wstring>> timed;
                for (size_t i : events.On(TodayKey()))
                    if (events.events[i].minute >= 0 && RemindsFor(events.events[i])) timed.push_back({ events.events[i].minute, events.events[i].title });
                int open = 0;
                for (const auto& task : todo.tasks) open += task.completed ? 0 : 1;
                centerText = IdleLine(timed, st.wHour * 60 + st.wMinute, open,
                                      std::wstring(DAY_NAMES[st.wDayOfWeek]) + L" " + std::to_wstring(st.wDay) + L" de " + MONTH_NAMES[st.wMonth - 1]);
                wchar_t clock[8];
                wsprintfW(clock, L"%02d:%02d", st.wHour, st.wMinute);
                rightText = clock;
            }
            // Measured only when what is shown changes; the app resizes the pill to pillWidth. The views composed in Settings
            // (fit) hug their content, so switching a figure on or off there widens or narrows the pill.
            std::wstring key = centerText + (fit ? L"\x1" : L"") + (rightUsed ? L"" : L"\x2");
            if (key != pillText) {
                pillText = key;
                pillWidth = ClosedWidthFor(centerText, fit ? CLOSED_FIT_MIN : CLOSED_W, rightUsed ? CLOSED_CHROME : CLOSED_CHROME_LEFT);
            }
            g.DrawString(centerText.c_str(), -1, &fontSmall, RectF(36.0f, 0.0f, (float)(width - (rightUsed ? 92 : 48)), (float)height), &center, &textWhite);
            g.DrawString(rightText.c_str(), -1, &fontSmallBold, rightSlot, &rightMid, rightBrush);
        } else {
            // === EXPANDED NOTCH VIEW ===
            // 1. Icon tab row; active tab sits in a soft rounded square
            // One highlight glides from the previous tab to the active one (tabEase 1 when settled)
            tabHits.clear();
            float tabP = perf.anim ? (std::min)(1.0f, (float)(GetTickCount64() - tabChangedAt) / TAB_ANIM_MS) : 1.0f;
            float tabEase = 1.0f - (1.0f - tabP) * (1.0f - tabP) * (1.0f - tabP);
            GraphicsPath tabPath;
            AddRoundRect(tabPath, 16.0f + (tabFrom + (activeTab - tabFrom) * tabEase) * TAB_STEP, 8.0f, (float)TAB_W, 30.0f, 10.0f);
            SolidBrush tabActiveBg(Color(255, 34, 37, 48));
            g.FillPath(&tabActiveBg, &tabPath);
            Pen tabOutline(Color(60, 124, 156, 255), 1.0f);
            g.DrawPath(&tabOutline, &tabPath);
            SolidBrush accent(ACCENT);
            for (int i = 0; i < TAB_COUNT; ++i)
                g.DrawString(tabIcons[i], -1, &iconFont, RectF(16.0f + i * TAB_STEP, 8.0f, (float)TAB_W, 30.0f), &center, i == activeTab ? &accent : &textDim);
            // Settings gear at the right end of the row, on every tab
            g.DrawString(L"", -1, &iconSmall, RectF((float)(width - 44), 8.0f, 32.0f, 30.0f), &center, &textDim);

            // 2. Tab Content Views
            // The content slides in from the side the tab switch came from
            GraphicsState gs = g.Save();
            g.TranslateTransform(tabDir * 28.0f * (1.0f - tabEase), 0.0f);
            if (activeTab == TAB_MEDIA) {
                const int col = MediaColumn(width); // controls column; the lyrics aside takes the rest to its right

                // Album art tile
                GraphicsPath art;
                AddRoundRect(art, 20.0f, 52.0f, 56.0f, 56.0f, 12.0f);
                if (perf.glow) {
                    LinearGradientBrush artBg(PointF(20.0f, 52.0f), PointF(77.0f, 109.0f), Color(255, 120, 136, 180), Color(255, 30, 36, 56));
                    g.FillPath(&artBg, &art);
                } else {
                    SolidBrush artBg(Color(255, 44, 48, 58));
                    g.FillPath(&artBg, &art);
                }
                g.DrawString(L"", -1, &iconBig, RectF(20.0f, 52.0f, 56.0f, 56.0f), &center, &textWhite);

                if (media.art) FillArt(g, media.art, 20.0f, 52.0f, 56.0f, 12.0f);

                // Track title, then "artist · app"
                std::wstring badgeText = media.currentApp.empty() ? L"Windows Media" : media.currentApp;
                std::wstring subLine = media.currentArtist + L" · " + badgeText;
                g.DrawString(media.currentTitle.c_str(), -1, &fontTitle, RectF(88.0f, 56.0f, (float)(col - 108), 24.0f), &leftMid, &textWhite);
                g.DrawString(subLine.c_str(), -1, &fontSmall, RectF(88.0f, 80.0f, (float)(col - 108), 20.0f), &leftMid, &textDim);

                // Playback controls: bare skip glyphs around a white play disc
                float midX = TransportCenter(col);
                g.DrawString(L"", -1, &iconFont, RectF(midX - 80.0f, 115.0f, 46.0f, 40.0f), &center, &textWhite);
                SolidBrush disc(Color(255, 245, 245, 247));
                g.FillEllipse(&disc, midX - 20.0f, 115.0f, 40.0f, 40.0f);
                g.DrawString(media.isPlaying ? L"" : L"", -1, &iconFont, RectF(midX - 20.0f, 115.0f, 40.0f, 40.0f), &center, &ink);
                g.DrawString(L"", -1, &iconFont, RectF(midX + 34.0f, 115.0f, 46.0f, 40.0f), &center, &textWhite);

                // Lyrics toggle on the transport row, right-aligned; filled while the aside is open
                RectF lyricsBtn((float)(col - 60), 117.0f, 40.0f, 36.0f);
                if (media.showLyrics) {
                    GraphicsPath pill;
                    AddRoundRect(pill, lyricsBtn.X, lyricsBtn.Y, lyricsBtn.Width, lyricsBtn.Height, 10.0f);
                    SolidBrush on(Color(255, 44, 46, 52));
                    g.FillPath(&on, &pill);
                }
                g.DrawString(L"", -1, &iconFont, lyricsBtn, &center, media.showLyrics ? &textWhite : &textDim);

                // Timeline: elapsed and total over a thin bar with a knob; click it to seek
                if (media.duration > 0) {
                    double elapsed = media.Elapsed();
                    float frac = (float)(std::min)(1.0, (std::max)(0.0, elapsed / media.duration));
                    float bx = 20.0f, bw = (float)(col - 40), by = 188.0f;
                    Font fontTime(&fontFamily, 7.5f, FontStyleRegular, UnitPoint);
                    g.DrawString(FormatTime(elapsed).c_str(), -1, &fontTime, RectF(bx, 164.0f, 80.0f, 16.0f), &leftMid, &textDim);
                    g.DrawString(FormatTime(media.duration).c_str(), -1, &fontTime, RectF(bx + bw - 80.0f, 164.0f, 80.0f, 16.0f), &rightMid, &textDim);
                    Pen trackPen(Color(255, 58, 60, 66), 3.0f);
                    Pen fillPen(Color(255, 242, 242, 245), 3.0f);
                    for (Pen* pen : { &trackPen, &fillPen }) {
                        pen->SetStartCap(LineCapRound);
                        pen->SetEndCap(LineCapRound);
                    }
                    g.DrawLine(&trackPen, bx, by, bx + bw, by);
                    if (frac > 0.0f) g.DrawLine(&fillPen, bx, by, bx + bw * frac, by);
                    SolidBrush knob(Color(255, 245, 245, 247));
                    g.FillEllipse(&knob, bx + bw * frac - 6.0f, by - 6.0f, 12.0f, 12.0f);
                }

                if (media.showLyrics) {
                    // nabyscape side layout: divider, then dim previous line, large current line, dimmer next line, status footer
                    MediaManager::LyricView lv = media.Lyrics();
                    if (lv.index != lyricIndex || lv.cur != lyricText) {
                        lyricIndex = lv.index;
                        lyricText = lv.cur;
                        lyricChangedAt = GetTickCount64();
                    }
                    // "Slide": the new line rises 10 px and fades in over 260 ms, ease-out
                    float p = perf.anim ? (std::min)(1.0f, (GetTickCount64() - lyricChangedAt) / (float)LYRIC_SLIDE_MS) : 1.0f;
                    float ease = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);

                    Font fontLyric(&fontFamily, 15.0f, FontStyleBold, UnitPoint);
                    Font fontLyricPrev(&fontFamily, 9.5f, FontStyleRegular, UnitPoint);
                    Font fontLyricNext(&fontFamily, 10.0f, FontStyleRegular, UnitPoint);
                    Font fontTiny(&fontFamily, 7.0f, FontStyleRegular, UnitPoint);
                    Font fontTinyBold(&fontFamily, 7.0f, FontStyleBold, UnitPoint);
                    SolidBrush prevBrush(Color(102, 242, 242, 245));
                    SolidBrush nextBrush(Color(153, 242, 242, 245));
                    SolidBrush curBrush(Color((BYTE)(255 * (0.3f + 0.7f * ease)), 255, 255, 255));
                    StringFormat wrapLeft; // current line may take two lines, like nabyscape's 52 px max
                    wrapLeft.SetLineAlignment(StringAlignmentCenter);
                    wrapLeft.SetTrimming(StringTrimmingEllipsisWord);

                    Pen divider(Color(255, 48, 49, 55), 1.0f);
                    g.DrawLine(&divider, (float)col, 56.0f, (float)col, 214.0f);
                    float lx = (float)col + 20.0f, lw = (float)(width - col - 40);
                    g.DrawString(lv.prev.c_str(), -1, &fontLyricPrev, RectF(lx, 56.0f, lw, 20.0f), &leftMid, &prevBrush);
                    g.DrawString(lv.cur.empty() ? L"Letras" : lv.cur.c_str(), -1, &fontLyric,
                                 RectF(lx, 82.0f + 10.0f * (1.0f - ease), lw, 58.0f), &wrapLeft, &curBrush);
                    g.DrawString(lv.next.c_str(), -1, &fontLyricNext, RectF(lx, 146.0f, lw, 20.0f), &leftMid, &nextBrush);
                    g.DrawString(lv.status.c_str(), -1, &fontTiny, RectF(lx, 196.0f, lw - 70.0f, 18.0f), &leftMid, &textDim);
                    if (lv.retry) g.DrawString(L"Reintentar", -1, &fontTinyBold, RectF(lx + lw - 70.0f, 196.0f, 70.0f, 18.0f), &rightMid, &textWhite);
                }

            } else if (activeTab == TAB_POMODORO) {
                // Ring timer on the left
                float cx = 110.0f, cy = 132.0f, rr = 60.0f;
                Pen track(Color(255, 46, 48, 54), 3.0f);
                g.DrawEllipse(&track, cx - rr, cy - rr, rr * 2.0f, rr * 2.0f);
                float progress = 1.0f - ((float)pomo.state.remainingSeconds / (float)pomo.state.totalSeconds);
                if (progress > 0.0f) {
                    Pen arc(ACCENT, 3.0f);
                    arc.SetStartCap(LineCapRound);
                    arc.SetEndCap(LineCapRound);
                    g.DrawArc(&arc, cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, -90.0f, 360.0f * progress);
                }
                std::wstring timeStr = pomo.GetFormattedTime();
                g.DrawString(timeStr.c_str(), -1, &fontBig, RectF(cx - rr, cy - 24.0f, rr * 2.0f, 36.0f), &center, &textWhite);
                std::wstring modeLine = (pomo.state.isBreak ? L"Descanso · " + std::to_wstring(g_settings.breakMin) : L"Enfoque · " + std::to_wstring(g_settings.focusMin)) + L" min";
                g.DrawString(modeLine.c_str(), -1, &fontSmall, RectF(cx - rr, cy + 12.0f, rr * 2.0f, 18.0f), &center, &textDim);

                // Right column: mode, status, round controls
                float colX = 210.0f;
                g.DrawString(pomo.state.isBreak ? L"Descanso" : L"Enfoque", -1, &fontTitle, RectF(colX, 62.0f, 140.0f, 26.0f), &leftMid, &textWhite);
                g.DrawString(pomo.state.isRunning ? L"En curso" : L"Listo", -1, &fontSmall, RectF(colX, 62.0f, (float)width - colX - 20.0f, 26.0f), &rightMid, &textDim);

                SolidBrush disc(Color(255, 245, 245, 247));
                g.FillEllipse(&disc, colX, 104.0f, 44.0f, 44.0f);
                g.DrawString(pomo.state.isRunning ? L"" : L"", -1, &iconFont, RectF(colX, 104.0f, 44.0f, 44.0f), &center, &ink);
                Pen ringBtn(Color(255, 58, 60, 66), 1.2f);
                g.DrawEllipse(&ringBtn, colX + 56.0f, 104.0f, 44.0f, 44.0f);
                g.DrawString(L"", -1, &iconFont, RectF(colX + 56.0f, 104.0f, 44.0f, 44.0f), &center, &textDim);

                wchar_t sessBuf[64];
                wsprintfW(sessBuf, L"Sesiones completadas hoy: %d", pomo.state.sessionsCompleted);
                g.DrawString(sessBuf, -1, &fontSmall, RectF(colX, 164.0f, (float)width - colX - 20.0f, 20.0f), &leftMid, &textDim);

            } else if (activeTab == TAB_TASKS) {
                // Date heading + open count
                SYSTEMTIME st;
                GetLocalTime(&st);
                wchar_t dateBuf[64];
                wsprintfW(dateBuf, L"Hoy · %02d/%02d/%04d", st.wDay, st.wMonth, st.wYear);
                g.DrawString(dateBuf, -1, &fontTitle, RectF(20.0f, 48.0f, 240.0f, 26.0f), &leftMid, &textWhite);
                // Rows: the dated tasks first (r < D is events[dated[r]]), then todo.tasks[r - D]
                const int todayKey = TodayKey();
                const std::vector<size_t> dated = DatedTasks(events, todayKey);
                const size_t D = dated.size(), N = D + todo.tasks.size();
                todoTop = (std::min)(todoTop, N > TODO_ROWS ? N - TODO_ROWS : 0); // the list may have shrunk in the calendar
                int open = 0;
                for (const auto& t : todo.tasks) open += t.completed ? 0 : 1;
                for (size_t i : dated) open += events.events[i].task == 1;
                wchar_t openBuf[64];
                if (N > TODO_ROWS) // scrolled with the mouse wheel
                    wsprintfW(openBuf, L"%d pendientes · %d-%d de %d", open, (int)todoTop + 1,
                              (int)(std::min)(todoTop + TODO_ROWS, N), (int)N);
                else
                    wsprintfW(openBuf, L"%d pendientes", open);
                g.DrawString(openBuf, -1, &fontSmall, RectF(20.0f, 48.0f, (float)(width - 40), 26.0f), &rightMid, &textDim);

                // Tasks list with round checkboxes; click the circle/text to toggle, the X to delete
                Pen checkRing(Color(255, 70, 72, 78), 1.2f);
                Font fontDone(&fontFamily, 9.0f, FontStyleStrikeout, UnitPoint);
                float itemY = 82.0f;
                if (N == 0)
                    g.DrawString(L"Sin tareas. Escribe una abajo.", -1, &fontSmall, RectF(24.0f, itemY, (float)(width - 48), 24.0f), &leftMid, &textDim);
                for (size_t r = todoTop; r < N && r < todoTop + TODO_ROWS; ++r) {
                    const bool isDated = r < D; // a calendar task: its event's title and done state, no subtasks, no + button
                    const TaskItem item = isDated ? TaskItem{ events.events[dated[r]].title, events.events[dated[r]].task == 2 } : todo.tasks[r - D];
                    float in = item.sub ? 22.0f : 0.0f, textR = 44.0f; // subtasks indent; text stops before the buttons
                    if (isDated) {
                        const CalEvent& ev = events.events[dated[r]];
                        std::wstring when = ev.date == todayKey ? L"Hoy" : ev.date == TmKey(KeyTm(todayKey, 1)) ? L"Mañana" : DayShort(ev.date);
                        if (ev.minute >= 0) when += L" · " + EventTime(ev.minute);
                        SolidBrush redText(Color(255, 239, 68, 68)), catBrush(Color(255, CAL_CATS[ev.color].r, CAL_CATS[ev.color].g, CAL_CATS[ev.color].b));
                        g.DrawString(when.c_str(), -1, &fontSmall, RectF((float)(width - 172), itemY, 124.0f, 24.0f), &rightMid, // wide enough for "Mié 11 nov · 09:00"
                                     ev.task == 1 && ev.date < todayKey ? &redText : &textDim); // open and past its day: overdue
                        g.FillEllipse(&catBrush, width - 190.0f, itemY + 8.0f, 8.0f, 8.0f);
                        textR = 204.0f;
                    } else if (!item.sub) {
                        g.DrawString(L"", -1, &iconSmall, RectF((float)(width - 68), itemY, 24.0f, 24.0f), &center, &textDim);
                        textR = 68.0f;
                        auto p = todo.SubProgress(r - D);
                        if (p.second) {
                            wchar_t prog[16];
                            wsprintfW(prog, L"%d/%d", p.first, p.second);
                            g.DrawString(prog, -1, &fontSmall, RectF((float)(width - 112), itemY, 40.0f, 24.0f), &rightMid, &textDim);
                            textR = 112.0f;
                        }
                    }
                    g.DrawString(L"", -1, &iconSmall, RectF((float)(width - 44), itemY, 24.0f, 24.0f), &center, &textDim);
                    if (item.completed) {
                        SolidBrush done(Color(255, 245, 245, 247));
                        g.FillEllipse(&done, 24.0f + in, itemY + 5.0f, 14.0f, 14.0f);
                        g.DrawString(L"", -1, &iconSmall, RectF(24.0f + in, itemY + 5.0f, 14.0f, 14.0f), &center, &ink);
                    } else {
                        g.DrawEllipse(&checkRing, 24.0f + in, itemY + 5.0f, 14.0f, 14.0f);
                    }
                    g.DrawString(item.text.c_str(), -1, item.completed ? &fontDone : &fontSmall, RectF(48.0f + in, itemY, width - 52.0f - in - textR, 24.0f), &leftMid, item.completed ? &textDim : &textWhite);
                    itemY += 26.0f;
                }

                // Add-task field
                GraphicsPath bAdd;
                AddRoundRect(bAdd, 20.0f, 196.0f, (float)(width - 40), 30.0f, 8.0f);
                g.FillPath(&surface, &bAdd);
                Pen focusEdge(ACCENT, 1.0f);
                g.DrawPath(editing ? &focusEdge : &surfaceEdge, &bAdd);
                float fx = 32.0f;
                if ((editing || !todoDraft.empty()) && todoParent < todo.tasks.size()) { // subtask: name its task first
                    std::wstring label = L"↳ " + todo.tasks[todoParent].text + L":";
                    RectF box;
                    g.MeasureString(label.c_str(), -1, &fontSmall, PointF(0.0f, 0.0f), &box);
                    float lw = (std::min)(box.Width, 150.0f);
                    g.DrawString(label.c_str(), -1, &fontSmall, RectF(fx, 196.0f, lw, 30.0f), &leftMid, &textDim);
                    fx += lw + 4.0f;
                }
                if (editing || !todoDraft.empty()) {
                    // shortcut: only the tail of a long draft is shown so the caret stays visible; no cursor movement
                    std::wstring shown = todoDraft.size() > 60 ? L"…" + todoDraft.substr(todoDraft.size() - 59) : todoDraft;
                    if (editing) shown += L"|";
                    g.DrawString(shown.c_str(), -1, &fontSmall, RectF(fx, 196.0f, width - 48.0f - fx, 30.0f), &leftMid, &textWhite);
                } else {
                    g.DrawString(L"Añadir tarea... (ej. Informe mañana 10:00)", -1, &fontSmall, RectF(fx, 196.0f, width - 48.0f - fx, 30.0f), &leftMid, &textDim);
                }
                g.DrawString(L"", -1, &iconSmall, RectF((float)(width - 48), 196.0f, 24.0f, 30.0f), &center, &textDim);

            } else if (activeTab == TAB_CALENDAR) {
                // Left: mini-month, category filter, new-event button.
                // Right: view switch, agenda / day / week / month around the selected day, and the selected event's strip.
                // Every clickable area is registered in calHits as it is drawn, so the click handler never repeats the layout.
                SYSTEMTIME now;
                GetLocalTime(&now);
                int todayKey = TodayKey(), selKey = SelKey();
                tm sel = KeyDay(selKey), monday = KeyDay(selKey, -((sel.tm_wday + 6) % 7));
                calHits.clear();
                auto hit = [&](float x, float y, float w, float h, int kind, int a = 0, int b = 0) { calHits.push_back({ RectF(x, y, w, h), kind, a, b }); };
                auto catColor = [&](int c, BYTE alpha = 255) { return Color(alpha, CAL_CATS[c].r, CAL_CATS[c].g, CAL_CATS[c].b); };
                auto visible = [&](int key) {
                    std::vector<size_t> v;
                    for (size_t i : events.On(key))
                        if (!calHidden[events.events[i].color] && (events.events[i].task == 0 || g_settings.tasksInCalendar)) v.push_back(i);
                    return v;
                };
                auto fillRound = [&](Brush* b, float x, float y, float w, float h, float r) {
                    GraphicsPath p;
                    AddRoundRect(p, x, y, w, h, r);
                    g.FillPath(b, &p);
                };
                auto dot = [&](float cx, float cy, float d, Color c) {
                    SolidBrush b(c);
                    g.FillEllipse(&b, cx - d / 2.0f, cy - d / 2.0f, d, d);
                };
                // Small text centered on a line: a tight layout rect would clip the glyphs
                auto textAt = [&](const std::wstring& s, Font* f, float x, float cy, float w, Brush* b) {
                    g.DrawString(s.c_str(), -1, f, RectF(x, cy - 8.0f, w, 16.0f), &leftMid, b);
                };
                Font fontTiny(&fontFamily, 8.0f, FontStyleRegular, UnitPoint);
                Font fontMicro(&fontFamily, 7.0f, FontStyleRegular, UnitPoint);
                SolidBrush chipBg(Color(255, 44, 46, 52)), todayFill(Color(255, 245, 245, 247)), colBg(Color(255, 22, 23, 27)),
                           colToday(Color(255, 38, 40, 46)), faint(Color(255, 72, 74, 80));
                Pen selRing(Color(150, 255, 255, 255), 1.2f), focusEdge(ACCENT, 1.0f);
                Pen nowPen(Color(255, 239, 68, 68), 1.5f), hourLine(Color(255, 48, 49, 55), 1.0f);
                SolidBrush nowDot(Color(255, 239, 68, 68));

                // --- Mini-month: today filled, selection ringed, a dot in the first visible event's color
                Month mm = CalMonth(sel.tm_year + 1900, sel.tm_mon);
                g.DrawString(L"", -1, &iconSmall, RectF(20.0f, 44.0f, 26.0f, 22.0f), &center, &textDim);
                g.DrawString(L"", -1, &iconSmall, RectF(204.0f, 44.0f, 26.0f, 22.0f), &center, &textDim);
                hit(20.0f, 44.0f, 26.0f, 22.0f, HIT_PREV, 1);
                hit(204.0f, 44.0f, 26.0f, 22.0f, HIT_NEXT, 1);
                std::wstring miniTitle = std::wstring(MONTH_NAMES[mm.month]) + L" " + std::to_wstring(mm.year);
                g.DrawString(miniTitle.c_str(), -1, &fontSmallBold, RectF(46.0f, 44.0f, 158.0f, 22.0f), &center, &textWhite);
                const wchar_t* heads[] = { L"L", L"M", L"X", L"J", L"V", L"S", L"D" };
                for (int c = 0; c < 7; ++c) g.DrawString(heads[c], -1, &fontTiny, RectF(20.0f + c * 30.0f, 66.0f, 30.0f, 16.0f), &center, &textDim);
                for (int d = 1; d <= mm.days; ++d) {
                    int cell = mm.firstCol + d - 1, key = DateKey(mm.year, mm.month, d);
                    float cx = 20.0f + (cell % 7) * 30.0f, cy = 82.0f + (cell / 7) * 20.0f;
                    if (key == todayKey) g.FillEllipse(&todayFill, cx + 6.0f, cy + 0.5f, 18.0f, 18.0f);
                    else if (key == selKey) g.DrawEllipse(&selRing, cx + 6.0f, cy + 0.5f, 18.0f, 18.0f);
                    g.DrawString(std::to_wstring(d).c_str(), -1, &fontSmall, RectF(cx, cy, 30.0f, 19.0f), &center,
                                 key == todayKey ? &ink : cell % 7 >= 5 ? &textDim : &textWhite);
                    std::vector<size_t> vis = visible(key);
                    if (!vis.empty()) dot(cx + 15.0f, cy + 18.0f, 3.0f, catColor(events.events[vis[0]].color));
                    hit(cx, cy, 30.0f, 20.0f, HIT_DAY, key);
                }

                // --- Categories: click toggles whether their events show anywhere
                g.DrawString(L"Categorías", -1, &fontSmall, RectF(20.0f, 214.0f, 210.0f, 18.0f), &leftMid, &textDim);
                for (int c = 0; c < CAL_CAT_COUNT; ++c) {
                    float cx = 20.0f + (c % 2) * 110.0f, cy = 234.0f + (c / 2) * 24.0f;
                    if (!calHidden[c]) {
                        fillRound(&chipBg, cx, cy, 100.0f, 22.0f, 8.0f);
                    } else {
                        GraphicsPath outline;
                        AddRoundRect(outline, cx, cy, 100.0f, 22.0f, 8.0f);
                        g.DrawPath(&surfaceEdge, &outline);
                    }
                    dot(cx + 11.0f, cy + 11.0f, 8.0f, catColor(c, calHidden[c] ? 90 : 255));
                    g.DrawString(CAL_CATS[c].name, -1, &fontSmall, RectF(cx + 22.0f, cy, 76.0f, 22.0f), &leftMid, calHidden[c] ? &textDim : &textWhite);
                    hit(cx, cy, 100.0f, 22.0f, HIT_CAT, c);
                }

                // --- New event button: opens the event window
                fillRound(&todayFill, 20.0f, 314.0f, 210.0f, 28.0f, 14.0f);
                g.DrawString(L"\uE710", -1, &iconSmall, RectF(72.0f, 314.0f, 18.0f, 28.0f), &center, &ink);
                g.DrawString(L"Nuevo evento", -1, &fontSmallBold, RectF(94.0f, 314.0f, 120.0f, 28.0f), &leftMid, &ink);
                hit(20.0f, 314.0f, 210.0f, 28.0f, HIT_NEW);

                Pen divider(Color(255, 48, 49, 55), 1.0f);
                g.DrawLine(&divider, 240.0f, 46.0f, 240.0f, 388.0f);

                // --- Header: view switch, way back to today, range title and arrows
                const wchar_t* viewNames[] = { L"Agenda", L"Día", L"Semana", L"Mes" };
                const float viewW[] = { 56.0f, 40.0f, 56.0f, 40.0f };
                float vx = 250.0f;
                for (int i = 0; i < 4; ++i) {
                    if (calView == i) fillRound(&chipBg, vx, 47.0f, viewW[i], 24.0f, 8.0f);
                    g.DrawString(viewNames[i], -1, &fontSmall, RectF(vx, 47.0f, viewW[i], 24.0f), &center, calView == i ? &textWhite : &textDim);
                    hit(vx, 47.0f, viewW[i], 24.0f, HIT_VIEW, i);
                    vx += viewW[i] + 4.0f;
                }
                if (selKey != todayKey) {
                    fillRound(&chipBg, vx + 6.0f, 47.0f, 40.0f, 24.0f, 8.0f);
                    g.DrawString(L"Hoy", -1, &fontSmall, RectF(vx + 6.0f, 47.0f, 40.0f, 24.0f), &center, &textWhite);
                    hit(vx + 6.0f, 47.0f, 40.0f, 24.0f, HIT_TODAY);
                }
                auto range = [&](const tm& a, const tm& b) {
                    return std::to_wstring(a.tm_mday) + L" " + std::wstring(MONTH_NAMES[a.tm_mon]).substr(0, 3) + L" – " + std::to_wstring(b.tm_mday) +
                           L" " + std::wstring(MONTH_NAMES[b.tm_mon]).substr(0, 3) + L" " + std::to_wstring(b.tm_year + 1900);
                };
                std::wstring title;
                if (calView == CAL_AGENDA) {
                    title = range(sel, KeyDay(selKey, 13));
                } else if (calView == CAL_WEEK) {
                    title = range(monday, KeyDay(DateKey(monday), 6));
                } else if (calView == CAL_DAY) {
                    title = std::wstring(DAY_NAMES[sel.tm_wday]) + L" " + std::to_wstring(sel.tm_mday) + L" de " + MONTH_NAMES[sel.tm_mon];
                } else {
                    title = MONTH_NAMES[mm.month];
                    title[0] = towupper(title[0]);
                    title += L" " + std::to_wstring(mm.year);
                }
                g.DrawString(title.c_str(), -1, &fontSmallBold, RectF(510.0f, 47.0f, 176.0f, 24.0f), &rightMid, &textWhite);
                g.DrawString(L"", -1, &iconSmall, RectF(690.0f, 47.0f, 24.0f, 24.0f), &center, &textDim);
                g.DrawString(L"", -1, &iconSmall, RectF(714.0f, 47.0f, 24.0f, 24.0f), &center, &textDim);
                hit(690.0f, 47.0f, 24.0f, 24.0f, HIT_PREV);
                hit(714.0f, 47.0f, 24.0f, 24.0f, HIT_NEXT);

                if (calView == CAL_AGENDA) {
                    // The next 14 days from the selection; only days with events, the selected one always
                    struct Row { int key, ev; }; // ev: -1 day header, -2 "Sin eventos", else the event
                    std::vector<Row> rows;
                    for (int d = 0; d < 14; ++d) {
                        int key = d ? TmKey(KeyTm(selKey, d)) : selKey;
                        std::vector<size_t> vis = visible(key);
                        if (vis.empty() && d) continue;
                        rows.push_back({ key, -1 });
                        if (vis.empty()) rows.push_back({ key, -2 });
                        for (size_t i : vis) rows.push_back({ key, (int)i });
                    }
                    calAgendaTop = (std::min)(calAgendaTop, rows.size() - 1);
                    float ry = 80.0f;
                    for (size_t r = calAgendaTop; r < rows.size(); ++r) {
                        const Row& row = rows[r];
                        float rh = row.ev == -1 ? 20.0f : 22.0f;
                        if (ry + rh + (row.ev == -1 ? 22.0f : 0.0f) > 346.0f) break; // no day header without its first row
                        if (row.ev == -1) {
                            std::wstring name = row.key == todayKey ? L"Hoy" : row.key == TmKey(KeyTm(todayKey, 1)) ? L"Mañana" : DayShort(row.key, true);
                            g.DrawString(name.c_str(), -1, &fontSmallBold, RectF(256.0f, ry, 470.0f, rh), &leftMid, row.key == todayKey ? &textWhite : &textDim);
                            hit(250.0f, ry, 490.0f, rh, HIT_DAY, row.key);
                        } else if (row.ev == -2) {
                            g.DrawString(L"Sin eventos", -1, &fontSmall, RectF(268.0f, ry, 300.0f, rh), &leftMid, &textDim);
                        } else {
                            const CalEvent& e = events.events[row.ev];
                            if (calEvent == row.ev) fillRound(&chipBg, 250.0f, ry, 490.0f, rh, 6.0f);
                            SolidBrush bar(catColor(e.color));
                            g.FillRectangle(&bar, 256.0f, ry + 4.0f, 3.0f, 14.0f);
                            g.DrawString(TimeRange(e).c_str(), -1, &fontSmall, RectF(268.0f, ry, 96.0f, rh), &leftMid, &textDim);
                            g.DrawString(EventLabel(e).c_str(), -1, &fontSmall, RectF(368.0f, ry, 316.0f, rh), &leftMid, &textWhite);
                            if (e.src) g.DrawString((e.src == 1 || (e.src == 3 && linkGoogle)) ? L"G" : L"O", -1, &fontTiny, RectF(712.0f, ry, 20.0f, rh), &center, &textDim);
                            if (e.repeat) g.DrawString(L"", -1, &iconSmall, RectF(684.0f, ry, 24.0f, rh), &center, &textDim);
                            hit(250.0f, ry, 490.0f, rh, HIT_EVENT, row.ev);
                        }
                        ry += rh;
                    }
                } else if (calView == CAL_DAY || calView == CAL_WEEK) {
                    // 9 hour rows of 25 px from calHour; the day (or 7 columns) with timed events as blocks, all-day ones in a strip
                    const int n = calView == CAL_WEEK ? 7 : 1, HOURS = 9;
                    const float gx = 296.0f, cw = 444.0f / n, top = 120.0f, hh = 25.0f;
                    for (int i = 0; i < HOURS; ++i) {
                        wchar_t hb[8];
                        wsprintfW(hb, L"%02d:00", calHour + i);
                        textAt(hb, &fontTiny, 250.0f, top + i * hh, 44.0f, &textDim);
                    }
                    for (int c = 0; c < n; ++c) {
                        tm dd = n == 1 ? sel : KeyDay(DateKey(monday), c);
                        int key = DateKey(dd);
                        float cx = gx + c * cw;
                        std::wstring label = n == 1 ? DayShort(key, true) : std::wstring(DAY_NAMES[dd.tm_wday]).substr(0, 3) + L" " + std::to_wstring(dd.tm_mday);
                        if (key == todayKey) fillRound(&todayFill, cx + 4.0f, 80.0f, cw - 8.0f, 20.0f, 8.0f);
                        else if (key == selKey) {
                            GraphicsPath ring;
                            AddRoundRect(ring, cx + 4.0f, 80.0f, cw - 8.0f, 20.0f, 8.0f);
                            g.DrawPath(&selRing, &ring);
                        }
                        g.DrawString(label.c_str(), -1, &fontSmall, RectF(cx, 80.0f, cw, 20.0f), &center,
                                     key == todayKey ? &ink : n == 7 && c >= 5 ? &textDim : &textWhite);
                        hit(cx, 80.0f, cw, 20.0f, HIT_DAY, key);

                        fillRound(key == todayKey ? &colToday : &colBg, cx + 2.0f, top, cw - 4.0f, HOURS * hh, 6.0f);
                        for (int i = 0; i < HOURS; ++i) {
                            if (i) g.DrawLine(&hourLine, cx + 4.0f, top + i * hh, cx + cw - 4.0f, top + i * hh);
                            hit(cx, top + i * hh, cw, hh, HIT_SLOT, key, (calHour + i) * 60);
                        }

                        std::vector<size_t> vis = visible(key);
                        int allDay = 0, firstAll = -1;
                        for (size_t i : vis)
                            if (events.events[i].minute < 0 && !allDay++) firstAll = (int)i;
                        if (allDay) { // the first all-day event, with how many more share the day
                            const CalEvent& e = events.events[firstAll];
                            SolidBrush soft(catColor(e.color, 90)), solid(catColor(e.color));
                            fillRound(&soft, cx + 3.0f, 102.0f, cw - 6.0f, 16.0f, 4.0f);
                            g.FillRectangle(&solid, cx + 3.0f, 105.0f, 3.0f, 10.0f);
                            textAt(EventLabel(e) + (allDay > 1 ? L" +" + std::to_wstring(allDay - 1) : std::wstring()), &fontTiny, cx + 9.0f, 110.0f, cw - 14.0f, &textWhite);
                            hit(cx + 3.0f, 102.0f, cw - 6.0f, 16.0f, HIT_EVENT, firstAll);
                        }

                        // Timed events: overlapping ones split the column into lanes (greedy, by start)
                        struct Blk { size_t i; int s, e, lane, lanes; };
                        std::vector<Blk> blks;
                        std::vector<int> laneEnd;
                        size_t clusterStart = 0;
                        int clusterEnd = 0;
                        auto closeCluster = [&]() {
                            for (size_t k = clusterStart; k < blks.size(); ++k) blks[k].lanes = (int)laneEnd.size();
                            clusterStart = blks.size();
                            laneEnd.clear();
                        };
                        for (size_t i : vis) {
                            const CalEvent& e = events.events[i];
                            if (e.minute < 0) continue;
                            int s = e.minute, en = e.end > s ? e.end : s + 60;
                            if (s >= clusterEnd) closeCluster();
                            size_t l = 0;
                            while (l < laneEnd.size() && laneEnd[l] > s) ++l;
                            if (l == laneEnd.size()) laneEnd.push_back(en);
                            else laneEnd[l] = en;
                            clusterEnd = (std::max)(clusterEnd, en);
                            blks.push_back({ i, s, en, (int)l, 0 });
                        }
                        closeCluster();

                        g.SetClip(RectF(cx, top, cw, HOURS * hh));
                        for (const Blk& b : blks) {
                            const CalEvent& e = events.events[b.i];
                            float lw = (cw - 6.0f) / (float)b.lanes, bx = cx + 3.0f + b.lane * lw, bw = lw - 1.0f;
                            float by = top + (b.s - calHour * 60) * hh / 60.0f, bh = (std::max)(14.0f, (b.e - b.s) * hh / 60.0f - 1.0f);
                            if (by + bh <= top || by >= top + HOURS * hh) continue;
                            SolidBrush soft(catColor(e.color, 90)), solid(catColor(e.color));
                            GraphicsPath bp;
                            AddRoundRect(bp, bx, by, bw, bh, 5.0f);
                            g.FillPath(&soft, &bp);
                            if (calEvent == (int)b.i) g.DrawPath(&selRing, &bp);
                            g.FillRectangle(&solid, bx, by + 3.0f, 3.0f, bh - 6.0f);
                            if (bh >= 34.0f) { // room for two lines: time over title
                                textAt(EventTime(b.s), &fontTiny, bx + 7.0f, by + 9.0f, bw - 9.0f, &textDim);
                                textAt(EventLabel(e), &fontTiny, bx + 7.0f, by + 23.0f, bw - 9.0f, &textWhite);
                            } else {
                                textAt((cw > 100.0f ? EventTime(b.s) + L" " : std::wstring()) + EventLabel(e), &fontTiny, bx + 7.0f, by + bh / 2.0f, bw - 9.0f, &textWhite);
                            }
                            float y0 = (std::max)(by, top), y1 = (std::min)(by + bh, top + HOURS * hh);
                            hit(bx, y0, bw, y1 - y0, HIT_EVENT, (int)b.i);
                        }
                        if (key == todayKey) {
                            float ny = top + (now.wHour * 60 + now.wMinute - calHour * 60) * hh / 60.0f;
                            if (ny >= top && ny <= top + HOURS * hh) {
                                g.DrawLine(&nowPen, cx + 2.0f, ny, cx + cw - 2.0f, ny);
                                g.FillEllipse(&nowDot, cx - 1.0f, ny - 3.0f, 6.0f, 6.0f);
                            }
                        }
                        g.ResetClip();
                    }
                } else {
                    // 6 rows x 7 of 70x41 cells: day number, up to two event bars and "+N" for the rest
                    const float cw = 490.0f / 7.0f, ch = 41.0f;
                    for (int c = 0; c < 7; ++c) g.DrawString(heads[c], -1, &fontTiny, RectF(250.0f + c * cw, 78.0f, cw, 16.0f), &center, &textDim);
                    for (int cell = 0; cell < 42; ++cell) {
                        tm dd = KeyDay(DateKey(mm.year, mm.month, 1), cell - mm.firstCol);
                        int key = DateKey(dd);
                        bool inMonth = dd.tm_mon == mm.month;
                        float cx = 250.0f + (cell % 7) * cw, cy = 96.0f + (cell / 7) * ch;
                        if (inMonth) fillRound(key == todayKey ? &colToday : &colBg, cx + 1.0f, cy + 1.0f, cw - 2.0f, ch - 2.0f, 6.0f);
                        if (key == todayKey) fillRound(&todayFill, cx + 4.0f, cy + 3.0f, 18.0f, 16.0f, 8.0f);
                        else if (key == selKey) {
                            GraphicsPath ring;
                            AddRoundRect(ring, cx + 4.0f, cy + 3.0f, 18.0f, 16.0f, 8.0f);
                            g.DrawPath(&selRing, &ring);
                        }
                        g.DrawString(std::to_wstring(dd.tm_mday).c_str(), -1, &fontSmall, RectF(cx + 4.0f, cy + 3.0f, 18.0f, 16.0f), &center,
                                     key == todayKey ? &ink : !inMonth ? &faint : cell % 7 >= 5 ? &textDim : &textWhite);
                        std::vector<size_t> vis = visible(key);
                        for (size_t k = 0; k < vis.size() && k < 2; ++k) {
                            const CalEvent& e = events.events[vis[k]];
                            SolidBrush soft(catColor(e.color, 80));
                            float by = cy + 20.0f + k * 10.0f;
                            fillRound(&soft, cx + 3.0f, by, cw - 6.0f, 9.0f, 3.0f);
                            textAt(EventLabel(e), &fontMicro, cx + 6.0f, by + 4.5f, cw - 10.0f, &textWhite);
                        }
                        if (vis.size() > 2) {
                            std::wstring more = L"+" + std::to_wstring(vis.size() - 2);
                            g.DrawString(more.c_str(), -1, &fontMicro, RectF(cx + cw - 28.0f, cy + 3.0f, 24.0f, 16.0f), &rightMid, &textDim);
                        }
                        hit(cx, cy, cw, ch, HIT_DAY, key, 1);
                    }
                }

                // --- Detail strip: the selected event and what can be done to it
                if (calEvent >= (int)events.events.size()) calEvent = -1;
                if (calEvent >= 0) { // the strip only exists while an event is selected
                    GraphicsPath strip;
                    AddRoundRect(strip, 250.0f, 352.0f, 490.0f, 36.0f, 10.0f);
                    g.FillPath(&surface, &strip);
                    g.DrawPath(&surfaceEdge, &strip);
                    const CalEvent& e = events.events[calEvent];
                    SolidBrush bar(catColor(e.color));
                    fillRound(&bar, 262.0f, 360.0f, 3.0f, 20.0f, 1.5f);
                    g.DrawString(EventLabel(e).c_str(), -1, &fontSmallBold, RectF(272.0f, 353.0f, 166.0f, 18.0f), &leftMid, &textWhite);
                    textAt(DayShort(e.date) + L" · " + TimeRange(e), &fontTiny, 272.0f, 378.0f, 166.0f, &textDim);
                    for (int c = 0; c < CAL_CAT_COUNT; ++c) { // category: the current one ringed
                        float dx = 454.0f + c * 16.0f;
                        dot(dx, 370.0f, 10.0f, catColor(c));
                        if (c == e.color) g.DrawEllipse(&selRing, dx - 7.0f, 363.0f, 14.0f, 14.0f);
                        hit(dx - 8.0f, 352.0f, 16.0f, 36.0f, HIT_COLOR, c);
                    }
                    // A calendar link's event is read-only: move, repeat, edit and delete are shown grey and do nothing; only the colours work
                    const bool ro = e.src == 3;
                    SolidBrush disabled(Color(255, 70, 72, 78)), &act = ro ? disabled : textDim;
                    g.DrawString(L"", -1, &iconSmall, RectF(548.0f, 352.0f, 20.0f, 36.0f), &center, &act);
                    g.DrawString(L"", -1, &iconSmall, RectF(568.0f, 352.0f, 20.0f, 36.0f), &center, &act);
                    if (!ro) hit(548.0f, 352.0f, 20.0f, 36.0f, HIT_MOVE, -1);
                    if (!ro) hit(568.0f, 352.0f, 20.0f, 36.0f, HIT_MOVE, 1);
                    fillRound(&chipBg, 592.0f, 359.0f, 92.0f, 22.0f, 8.0f);
                    if (e.task) { // the strip is full: a task's done toggle takes the repeat chip's place (its repeat is set in the event window)
                        g.DrawString(e.task == 2 ? L"Reabrir" : L"Completar", -1, &fontSmall, RectF(592.0f, 359.0f, 92.0f, 22.0f), &center, &textWhite);
                        if (!ro) hit(592.0f, 359.0f, 92.0f, 22.0f, HIT_TASK_DONE);
                    } else {
                        // A series already in the service keeps its repeat: it is shown, dimmed, and cannot be changed here
                        bool repeatLocked = ro || (e.src && !e.rid.empty());
                        g.DrawString(CAL_REPEAT[e.repeat], -1, &fontSmall, RectF(592.0f, 359.0f, 92.0f, 22.0f), &center, e.repeat && !repeatLocked ? &textWhite : &textDim);
                        if (!repeatLocked) hit(592.0f, 359.0f, 92.0f, 22.0f, HIT_REPEAT);
                    }
                    g.DrawString(L"", -1, &iconSmall, RectF(688.0f, 352.0f, 24.0f, 36.0f), &center, &act);
                    g.DrawString(L"", -1, &iconSmall, RectF(712.0f, 352.0f, 24.0f, 36.0f), &center, &act);
                    if (!ro) hit(688.0f, 352.0f, 24.0f, 36.0f, HIT_EDIT);
                    if (!ro) hit(712.0f, 352.0f, 24.0f, 36.0f, HIT_DELETE);
                }

                // --- New / edit event window over everything: dimmed notch, then the card. Its hit areas replace the calendar's.
                if (calPop) {
                    const CalEvent& pe = popEv;
                    bool timed = pe.minute >= 0, noTitle = pe.title.find_first_not_of(L' ') == std::wstring::npos;
                    g.TranslateTransform(-(float)NOTCH_SHOULDER, 0.0f); // notchPath is in untranslated coordinates
                    SolidBrush dim(Color(150, 0, 0, 0));
                    g.FillPath(&dim, &notchPath);
                    g.TranslateTransform((float)NOTCH_SHOULDER, 0.0f);
                    calHits.clear();
                    hit(0.0f, 0.0f, (float)width, (float)height, HIT_POP_BACK);
                    // Card 500x458; labels in a column at LX, controls from V, rows 42 px apart
                    const float CX = 130.0f, CY = 50.0f, CW = 500.0f, CH = 458.0f, LX = 158.0f, RX = 602.0f, V = 246.0f;
                    hit(CX, CY, CW, CH, HIT_POP_CARD);

                    SolidBrush cardBg(Color(255, 24, 25, 30)), redText(Color(255, 239, 68, 68));
                    Pen cardEdge(Color(40, 255, 255, 255), 1.0f);
                    GraphicsPath card;
                    AddRoundRect(card, CX, CY, CW, CH, 16.0f);
                    g.FillPath(&cardBg, &card);
                    g.DrawPath(&cardEdge, &card);

                    auto label = [&](float y, const wchar_t* s) { g.DrawString(s, -1, &fontSmall, RectF(LX, y, V - LX, 24.0f), &leftMid, &textDim); };
                    auto chip = [&](float x, float y, float w, const wchar_t* s, bool on, int kind, int a = 0, bool live = true) {
                        fillRound(on && live ? &todayFill : &chipBg, x, y, w, 24.0f, 9.0f);
                        g.DrawString(s, -1, &fontSmall, RectF(x, y, w, 24.0f), &center, !live ? &textDim : on ? &ink : &textWhite);
                        if (live) hit(x, y, w, 24.0f, kind, a);
                    };
                    auto arrow = [&](float x, float y, const wchar_t* glyph, int kind, int a) {
                        fillRound(&chipBg, x, y, 24.0f, 24.0f, 9.0f);
                        g.DrawString(glyph, -1, &iconSmall, RectF(x, y, 24.0f, 24.0f), &center, &textWhite);
                        hit(x, y, 24.0f, 24.0f, kind, a);
                    };
                    // Text field: only the tail of long text is shown so the caret stays visible (shortcut: no cursor movement)
                    auto field = [&](float x, float y, float w, float h, const std::wstring& text, size_t keep, const wchar_t* hint, Font* f, int idx) {
                        GraphicsPath fp;
                        AddRoundRect(fp, x, y, w, h, 9.0f);
                        g.FillPath(&surface, &fp);
                        bool focus = popFocus == idx;
                        g.DrawPath(focus ? &focusEdge : &surfaceEdge, &fp);
                        std::wstring shown = text.size() > keep ? L"…" + text.substr(text.size() - keep + 1) : text;
                        if (focus && editing) shown += L"|";
                        if (text.empty()) g.DrawString(hint, -1, f, RectF(x + 20.0f, y, w - 28.0f, h), &leftMid, &textDim);
                        g.DrawString(shown.c_str(), -1, f, RectF(x + 14.0f, y, w - 24.0f, h), &leftMid, &textWhite);
                        hit(x, y, w, h, idx ? HIT_POP_NOTES : HIT_POP_TITLE);
                    };

                    field(LX, 72.0f, RX - LX, 40.0f, pe.title, 44, L"Añadir título", &fontTitle, 0);

                    // Calendar: picked only when creating (Capri and the connected services); editing shows the current one, dimmed
                    const float yCal = 152.0f, yDate = 194.0f, yTime = 236.0f, yRep = 278.0f, yCat = 320.0f, yAlert = 362.0f, yNotes = 404.0f, yBtn = 460.0f;
                    label(yCal, L"Calendario");
                    const wchar_t* srcNames[] = { L"Capri", L"Google", L"Outlook" };
                    const float srcW[] = { 64.0f, 72.0f, 80.0f };
                    float sx = V;
                    const bool taskOn = pe.task != 0; // a task is local: no service can be picked
                    for (int s = 0; s < 3; ++s) {
                        if (popEdit < 0 ? s > 0 && syncDot[s] == SYNC_OFF : s != pe.src) continue;
                        if (popEdit < 0) chip(sx, yCal, srcW[s], srcNames[s], pe.src == s, HIT_POP_SOURCE, s, !taskOn);
                        else chip(sx, yCal, srcW[s], srcNames[s], true, HIT_POP_SOURCE, s, false);
                        sx += srcW[s] + 8.0f;
                    }
                    // Tarea: right end of the row (inside the form's right edge). An event already in a service cannot become one.
                    chip(RX - 64.0f, yCal, 64.0f, L"Tarea", taskOn, HIT_POP_TASK, 0, popEdit < 0 || pe.src == 0);

                    // Date
                    label(yDate, L"Fecha");
                    arrow(V, yDate, L"", HIT_POP_DATE, DateKey(KeyDay(pe.date, -1)));
                    g.DrawString(DayShort(pe.date).c_str(), -1, &fontSmallBold, RectF(V + 26.0f, yDate, 90.0f, 24.0f), &center, &textWhite);
                    arrow(V + 118.0f, yDate, L"", HIT_POP_DATE, DateKey(KeyDay(pe.date, 1)));
                    chip(V + 156.0f, yDate, 50.0f, L"Hoy", pe.date == todayKey, HIT_POP_DATE, todayKey);
                    chip(V + 214.0f, yDate, 68.0f, L"Mañana", pe.date == DateKey(KeyDay(todayKey, 1)), HIT_POP_DATE, DateKey(KeyDay(todayKey, 1)));

                    // Time: all-day switch, then start and end with 15-minute arrows
                    label(yTime, L"Hora");
                    fillRound(timed ? &chipBg : &todayFill, V, yTime + 4.0f, 30.0f, 16.0f, 8.0f);
                    { SolidBrush knob(timed ? Color(255, 142, 145, 152) : Color(255, 8, 9, 12)); g.FillEllipse(&knob, timed ? V + 3.0f : V + 17.0f, yTime + 7.0f, 10.0f, 10.0f); }
                    g.DrawString(L"Todo el día", -1, &fontSmall, RectF(V + 38.0f, yTime, 80.0f, 24.0f), &leftMid, &textWhite);
                    hit(V, yTime, 118.0f, 24.0f, HIT_POP_ALLDAY);
                    if (timed) {
                        arrow(V + 128.0f, yTime, L"", HIT_POP_START, -15);
                        g.DrawString(EventTime(pe.minute).c_str(), -1, &fontSmallBold, RectF(V + 152.0f, yTime, 46.0f, 24.0f), &center, &textWhite);
                        arrow(V + 198.0f, yTime, L"", HIT_POP_START, 15);
                        g.DrawString(L"–", -1, &fontSmall, RectF(V + 222.0f, yTime, 20.0f, 24.0f), &center, &textDim);
                        arrow(V + 242.0f, yTime, L"", HIT_POP_END, -15);
                        g.DrawString(EventTime(pe.end).c_str(), -1, &fontSmallBold, RectF(V + 266.0f, yTime, 46.0f, 24.0f), &center, &textWhite);
                        arrow(V + 312.0f, yTime, L"", HIT_POP_END, 15);
                    }

                    // Repeat (dimmed for a series already in the service)
                    label(yRep, L"Repetir");
                    const wchar_t* repNames[] = { L"No", L"Diario", L"Semanal", L"Mensual", L"Anual" };
                    const float repW[] = { 44.0f, 58.0f, 66.0f, 66.0f, 54.0f };
                    bool repeatLocked = pe.src && !pe.rid.empty();
                    for (int i = 0, x = (int)V; i < 5; x += (int)repW[i++] + 8) chip((float)x, yRep, repW[i], repNames[i], pe.repeat == i, HIT_POP_REPEAT, i, !repeatLocked);

                    // Category
                    label(yCat, L"Categoría");
                    for (int c = 0; c < CAL_CAT_COUNT; ++c) {
                        float dx = V + 10.0f + c * 26.0f;
                        dot(dx, yCat + 12.0f, 14.0f, catColor(c));
                        if (c == pe.color) g.DrawEllipse(&selRing, dx - 9.0f, yCat + 3.0f, 18.0f, 18.0f);
                        hit(dx - 12.0f, yCat, 24.0f, 24.0f, HIT_POP_COLOR, c);
                    }
                    g.DrawString(CAL_CATS[pe.color].name, -1, &fontSmall, RectF(V + 164.0f, yCat, RX - V - 164.0f, 24.0f), &leftMid, &textWhite);

                    // Alert: only for timed events. Each time is its own toggle (bits, see AlertMask); Ninguno (bit 0) is on when none is
                    label(yAlert, L"Aviso");
                    const wchar_t* alertNames[] = { L"Ninguno", L"Al empezar", L"10 min", L"30 min", L"1 h" };
                    const int alertBits[] = { 0, 1, 2, 4, 8 };
                    const float alertW[] = { 62.0f, 80.0f, 56.0f, 56.0f, 44.0f };
                    const int alertMask = AlertMask(pe.alert, pe.moreAlerts);
                    for (int i = 0, x = (int)V; i < 5; x += (int)alertW[i++] + 8)
                        chip((float)x, yAlert, alertW[i], alertNames[i], timed && (alertBits[i] ? (alertMask & alertBits[i]) != 0 : alertMask == 0), HIT_POP_ALERT, alertBits[i], timed);

                    // Notes
                    label(yNotes + 3.0f, L"Notas");
                    field(V, yNotes, RX - V, 30.0f, pe.notes, 52, L"Añadir notas", &fontSmall, 1);

                    // Buttons
                    if (popEdit >= 0) {
                        fillRound(&chipBg, LX, yBtn, 90.0f, 32.0f, 16.0f);
                        g.DrawString(L"Eliminar", -1, &fontSmallBold, RectF(LX, yBtn, 90.0f, 32.0f), &center, &redText);
                        hit(LX, yBtn, 90.0f, 32.0f, HIT_POP_DELETE);
                    }
                    fillRound(&chipBg, RX - 188.0f, yBtn, 90.0f, 32.0f, 16.0f);
                    g.DrawString(L"Cancelar", -1, &fontSmallBold, RectF(RX - 188.0f, yBtn, 90.0f, 32.0f), &center, &textWhite);
                    hit(RX - 188.0f, yBtn, 90.0f, 32.0f, HIT_POP_CANCEL);
                    SolidBrush saveFill(noTitle ? Color(80, 245, 245, 247) : Color(255, 245, 245, 247));
                    fillRound(&saveFill, RX - 90.0f, yBtn, 90.0f, 32.0f, 16.0f);
                    g.DrawString(L"Guardar", -1, &fontSmallBold, RectF(RX - 90.0f, yBtn, 90.0f, 32.0f), &center, &ink);
                    hit(RX - 90.0f, yBtn, 90.0f, 32.0f, HIT_POP_SAVE);
                }

            } else if (activeTab == TAB_AI) {
                g.DrawString(L"Compañero Capri", -1, &fontTitle, RectF(20.0f, 44.0f, 240.0f, 26.0f), &leftMid, &textWhite);
                g.DrawString(L"Cloud / Local", -1, &fontSmall, RectF(20.0f, 44.0f, (float)(width - 40), 26.0f), &rightMid, &textDim);

                // On-demand Whistle local model toggle (loads/unloads 16.9 MB)
                GraphicsPath bWhistle;
                AddRoundRect(bWhistle, 20.0f, 72.0f, (float)(width - 40), 24.0f, 8.0f);
                SolidBrush whistleBg(wake.isModelLoaded ? Color(255, 16, 44, 28) : Color(255, 22, 23, 27));
                g.FillPath(&whistleBg, &bWhistle);
                g.DrawPath(&surfaceEdge, &bWhistle);
                SolidBrush dot(wake.isModelLoaded ? Color(255, 74, 222, 128) : Color(255, 90, 92, 98));
                g.FillEllipse(&dot, 32.0f, 80.0f, 8.0f, 8.0f);
                g.DrawString(wake.isModelLoaded ? L"Whistle activo · 16.9 MB en RAM · clic para descargar"
                                                : L"Whistle local · en disco (0 MB en RAM) · clic para cargar",
                             -1, &fontSmall, RectF(48.0f, 72.0f, (float)(width - 80), 24.0f), &leftMid, wake.isModelLoaded ? &accentGreen : &textDim);

                // Response card
                GraphicsPath card;
                AddRoundRect(card, 20.0f, 102.0f, (float)(width - 40), 82.0f, 10.0f);
                g.FillPath(&surface, &card);
                g.DrawPath(&surfaceEdge, &card);
                std::wstring response;
                {
                    std::lock_guard<std::mutex> lock(ai.aiMutex);
                    response = ai.lastResponse;
                }
                g.DrawString(response.c_str(), -1, &fontSmall, RectF(32.0f, 108.0f, (float)(width - 64), 70.0f), NULL, &textWhite);

                // Quick ask chips
                SolidBrush chipBg(Color(255, 34, 36, 41));
                GraphicsPath bQ1, bQ2;
                AddRoundRect(bQ1, 24.0f, 192.0f, 170.0f, 28.0f, 14.0f);
                AddRoundRect(bQ2, 204.0f, 192.0f, 170.0f, 28.0f, 14.0f);
                g.FillPath(&chipBg, &bQ1);
                g.FillPath(&chipBg, &bQ2);
                g.DrawString(L"Resumir día y tareas", -1, &fontSmall, RectF(24.0f, 192.0f, 170.0f, 28.0f), &center, &textWhite);
                g.DrawString(L"Consejo de productividad", -1, &fontSmall, RectF(204.0f, 192.0f, 170.0f, 28.0f), &center, &textWhite);

            } else if (activeTab == TAB_SYSTEM) {
                // 3x2 cards: CPU load and temperature (with a 60-sample sparkline), memory, disk C:, battery, battery power
                const float cw = (width - 56.0f) / 3.0f, ch = 82.0f;
                const float xs[] = { 20.0f, 20.0f + cw + 8.0f, 20.0f + 2.0f * (cw + 8.0f) }, ys[] = { 48.0f, 138.0f };
                SolidBrush hotRed(Color(255, 239, 68, 68)), offDim(Color(255, 90, 92, 98));
                auto one = [](float v) { int t = (int)lroundf(v * 10.0f); return std::to_wstring(t / 10) + L"." + std::to_wstring(t % 10); };
                // frac < 0: no bar (the power card writes its word under the value instead)
                auto card = [&](float x, float y, const wchar_t* label, Brush* labelBrush, const std::wstring& value, Brush* valueBrush, float frac, bool warn) {
                    GraphicsPath cp;
                    AddRoundRect(cp, x, y, cw, ch, 12.0f);
                    g.FillPath(&surface, &cp);
                    g.DrawPath(&surfaceEdge, &cp);
                    g.DrawString(label, -1, &fontSmall, RectF(x + 14.0f, y + 10.0f, cw - 28.0f, 18.0f), &leftMid, labelBrush);
                    g.DrawString(value.c_str(), -1, &fontMedium, RectF(x + 14.0f, y + 30.0f, cw - 28.0f, 26.0f), &leftMid, valueBrush);
                    if (frac < 0.0f) return;
                    frac = (std::min)(1.0f, (std::max)(0.0f, frac));
                    GraphicsPath track, fill;
                    AddRoundRect(track, x + 14.0f, y + ch - 14.0f, cw - 28.0f, 3.0f, 1.5f);
                    SolidBrush trackBrush(Color(255, 58, 60, 66));
                    g.FillPath(&trackBrush, &track);
                    if (frac > 0.0f) {
                        AddRoundRect(fill, x + 14.0f, y + ch - 14.0f, (std::max)(3.0f, (cw - 28.0f) * frac), 3.0f, 1.5f);
                        SolidBrush fillBrush(warn && frac > 0.9f ? Color(255, 239, 68, 68) : ACCENT);
                        g.FillPath(&fillBrush, &fill);
                    }
                };
                const bool cpuHot = g_settings.sysTemps && sys.cpuTemp >= 85.0f;
                std::wstring cpuText = std::to_wstring((int)lroundf(sys.cpu)) + L" %";
                if (g_settings.sysTemps && sys.cpuTemp >= 0) cpuText += L" · " + std::to_wstring((int)lroundf(sys.cpuTemp)) + L" °C";
                card(xs[0], ys[0], L"CPU", &textDim, cpuText, cpuHot ? &hotRed : &textWhite, sys.cpu / 100.0f, true);

                const bool gpuOn = g_settings.sysTemps && sys.gpuTemp >= 0;
                card(xs[1], ys[0], L"GPU", gpuOn ? &textDim : &offDim,
                     gpuOn ? std::to_wstring((int)lroundf(sys.gpuTemp)) + L" °C" : std::wstring(L"—"),
                     gpuOn && sys.gpuTemp >= 85.0f ? &hotRed : &textWhite, gpuOn ? sys.gpuTemp / 100.0f : 0.0f, true);

                const std::wstring memSep = sys.memTotalGb >= 100 ? L"/" : L" / "; // "128.0/255.9 GB" would not fit the card otherwise
                card(xs[2], ys[0], L"Memoria", &textDim, one(sys.memUsedGb) + memSep + one(sys.memTotalGb) + L" GB", &textWhite,
                     sys.memTotalGb > 0 ? sys.memUsedGb / sys.memTotalGb : 0.0f, true);

                card(xs[0], ys[1], L"Disco (C:)", &textDim, std::to_wstring((int)lroundf(sys.diskFreeGb)) + L" GB libres", &textWhite,
                     sys.diskTotalGb > 0 ? 1.0f - sys.diskFreeGb / sys.diskTotalGb : 0.0f, true);

                card(xs[1], ys[1], L"Batería", &textDim, sys.battery < 0 ? std::wstring(L"Sin batería") : std::to_wstring(sys.battery) + L" %", &textWhite,
                     sys.battery < 0 ? 0.0f : sys.battery / 100.0f, false); // a full battery is no warning
                if (sys.battery >= 0 && (sys.charging || sys.onAc)) // on the label line: "100 % · cargando" is wider than the card's text
                    g.DrawString(sys.charging ? L"cargando" : L"enchufada", -1, &fontSmall, RectF(xs[1] + 14.0f, ys[1] + 10.0f, cw - 28.0f, 18.0f), &rightMid, &textDim);

                const bool powerOn = g_settings.sysPower && sys.battery >= 0 && sys.batteryWKnown;
                std::wstring power = !powerOn ? std::wstring(L"—") : sys.batteryW == 0.0f ? std::wstring(L"0 W")
                                   : (sys.batteryW > 0 ? std::wstring(L"+") : std::wstring(L"−")) + one(fabsf(sys.batteryW)) + L" W";
                const wchar_t* word = !powerOn ? L"" : sys.batteryW > 0 ? L"Entrando" : sys.batteryW < 0 ? L"Saliendo" : L"";
                card(xs[2], ys[1], L"Energía", &textDim, power, &textWhite, -1.0f, false);
                g.DrawString(word, -1, &fontSmall, RectF(xs[2] + 14.0f, ys[1] + 56.0f, cw - 28.0f, 18.0f), &leftMid, &textDim);

                // CPU sparkline in the top right of its card, oldest sample on the left
                Gdiplus::PointF pts[60];
                for (int i = 0; i < 60; ++i)
                    pts[i] = PointF(xs[0] + cw * 0.5f + i * (cw * 0.5f - 14.0f) / 59.0f,
                                    ys[0] + 8.0f + 18.0f * (1.0f - cpuHist[(cpuHistPos + i) % 60] / 100.0f));
                Pen spark(Color(160, ACCENT.GetR(), ACCENT.GetG(), ACCENT.GetB()), 1.5f);
                g.DrawLines(&spark, pts, 60);

            } else if (activeTab == TAB_WEATHER) {
                // Place name in the header (no chevrons: the place is found for the user); then the forecast or its state
                // only with a forecast: a place from an earlier fetch must not sit above "No pude encontrar tu ubicación"
                g.DrawString(weather.status == 2 && !weather.place.empty() ? weather.place.c_str() : L"Clima", -1, &fontTitle, RectF(20.0f, 48.0f, width - 40.0f, 24.0f), &leftMid, &textWhite);

                if (weather.status == 2) {
                    const CapriWeather& d = weather.data;
                    Font fontHuge(&fontFamily, 30.0f, FontStyleBold, UnitPoint);
                    g.DrawString((std::to_wstring((int)lroundf(d.temp)) + L"°").c_str(), -1, &fontHuge, RectF(20.0f, 80.0f, 180.0f, 46.0f), &leftMid, &textWhite);
                    g.DrawString(Wide(capri_weather_text(d.code)).c_str(), -1, &fontSmall, RectF(20.0f, 128.0f, 180.0f, 18.0f), &leftMid, &textWhite);
                    g.DrawString((L"Máx " + std::to_wstring((int)lroundf(d.tMax)) + L"° · Mín " + std::to_wstring((int)lroundf(d.tMin)) + L"°").c_str(),
                                 -1, &fontSmall, RectF(20.0f, 150.0f, 180.0f, 18.0f), &leftMid, &textDim);

                    // Feels-like, humidity and wind in one card
                    GraphicsPath wc;
                    AddRoundRect(wc, 236.0f, 48.0f, width - 256.0f, 160.0f, 12.0f);
                    g.FillPath(&surface, &wc);
                    g.DrawPath(&surfaceEdge, &wc);
                    const wchar_t* labels[] = { L"Sensación", L"Humedad", L"Viento" };
                    std::wstring values[] = { std::to_wstring((int)lroundf(d.feels)) + L"°", std::to_wstring(d.humidity) + L" %",
                                              std::to_wstring((int)lroundf(d.wind)) + L" km/h" };
                    for (int i = 0; i < 3; ++i) {
                        float ry = 68.0f + i * 44.0f;
                        g.DrawString(labels[i], -1, &fontSmall, RectF(250.0f, ry, 120.0f, 24.0f), &leftMid, &textDim);
                        g.DrawString(values[i].c_str(), -1, &fontSmallBold, RectF(250.0f, ry, width - 284.0f, 24.0f), &rightMid, &textWhite);
                    }
                } else if (weather.status == 3 || weather.status == 4) {
                    // 4: no place found, the chip opens the city setting; 3: no connection, the chip retries
                    const bool unknown = weather.status == 4;
                    g.DrawString(unknown ? L"No pude encontrar tu ubicación" : L"Sin conexión", -1, &fontSmall, RectF(20.0f, 104.0f, width - 40.0f, 24.0f), &center, &textDim);
                    GraphicsPath rp;
                    AddRoundRect(rp, width / 2.0f - 60.0f, 136.0f, 120.0f, 28.0f, 14.0f);
                    SolidBrush retryBg(Color(255, 44, 46, 52));
                    g.FillPath(&retryBg, &rp);
                    g.DrawString(unknown ? L"Elegir ciudad" : L"Reintentar", -1, &fontSmallBold, RectF(width / 2.0f - 60.0f, 136.0f, 120.0f, 28.0f), &center, &textWhite);
                    tabHits.push_back({ RectF(width / 2.0f - 60.0f, 136.0f, 120.0f, 28.0f), unknown ? HIT_W_SETUP : HIT_W_RETRY, 0, 0 });
                } else {
                    g.DrawString(L"Cargando…", -1, &fontSmall, RectF(20.0f, 104.0f, width - 40.0f, 24.0f), &center, &textDim);
                }
            } else if (activeTab == TAB_SHELF) {
                // Header: the mode's name and the chip that switches it. Each mode has its own view: the shelf, or a drop zone.
                g.DrawString(shelfSend ? L"Enviar al móvil" : L"Estante", -1, &fontTitle, RectF(20.0f, 48.0f, 200.0f, 26.0f), &leftMid, &textWhite);
                // Switch of two icons, file (keep) and paper plane (send): the knob sits under the mode that is on
                RectF chip(width - 92.0f, 49.0f, 72.0f, 24.0f);
                GraphicsPath chipPath, knobPath;
                AddRoundRect(chipPath, chip.X, chip.Y, chip.Width, chip.Height, 12.0f);
                SolidBrush chipBg(Color(255, 44, 46, 52)), knob(ACCENT);
                g.FillPath(&chipBg, &chipPath);
                AddRoundRect(knobPath, chip.X + (shelfSend ? 37.0f : 2.0f), chip.Y + 2.0f, 33.0f, 20.0f, 10.0f);
                g.FillPath(&knob, &knobPath);
                g.DrawString(L"", -1, &iconSmall, RectF(chip.X + 2.0f, chip.Y, 33.0f, 24.0f), &center, shelfSend ? &textDim : &ink);
                g.DrawString(L"", -1, &iconSmall, RectF(chip.X + 37.0f, chip.Y, 33.0f, 24.0f), &center, shelfSend ? &ink : &textDim);
                tabHits.push_back({ chip, HIT_S_MODE, 0, 0 });

                if (shelfSend) {
                    // Send keeps nothing: only the drop zone and the state of the last send
                    GraphicsPath zone;
                    AddRoundRect(zone, 20.0f, 84.0f, width - 40.0f, 112.0f, 12.0f);
                    Pen dash(Color(255, 70, 72, 78), 1.2f);
                    dash.SetDashStyle(DashStyleDash);
                    g.DrawPath(&dash, &zone);
                    g.DrawString(L"", -1, &iconBig, RectF(20.0f, 100.0f, width - 40.0f, 40.0f), &center, &textDim);
                    g.DrawString(L"Suelta aquí un archivo para enviarlo con Quick Share", -1, &fontSmall, RectF(28.0f, 146.0f, width - 56.0f, 24.0f), &center, &textDim);
                    g.DrawString(shelfNote.c_str(), -1, &fontSmall, RectF(20.0f, 204.0f, width - 40.0f, 24.0f), &center, &textWhite);
                } else {
                    // The shelf's copies: a grid of thumbnails without names (up to 4 files: one row of big tiles; more: two rows
                    // of small ones), or rows of names. A click opens the copy, the X sends it to the Recycle Bin.
                    const bool thumbs = g_settings.shelfThumbs;
                    const size_t N = shelf.files.size();
                    const bool big = N <= 4;
                    const size_t cols = big ? 4 : 7, rows = thumbs ? (big ? 4 : 14) : 5; // rows: how many fit on screen
                    const float tileW = big ? 100.0f : 56.0f, tileH = big ? 120.0f : 58.0f, tileGap = big ? 6.6f : 4.6f;
                    const float rowH = 26.0f, textX = 48.0f;
                    shelfTop = (std::min)(shelfTop, N > rows ? N - rows : 0);
                    float itemY = 80.0f;
                    if (N == 0 && shelfNote.empty())
                        g.DrawString(L"Arrastra archivos al notch para guardar una copia aquí", -1, &fontSmall, RectF(20.0f, 120.0f, width - 40.0f, 24.0f), &center, &textDim);
                    for (size_t r = shelfTop; r < N && r < shelfTop + rows; ++r) {
                        if (thumbs) {
                            const size_t k = r - shelfTop;
                            RectF tile(20.0f + (k % cols) * (tileW + tileGap), 80.0f + (k / cols) * (tileH + 6.0f), tileW, tileH);
                            GraphicsPath tilePath;
                            AddRoundRect(tilePath, tile.X, tile.Y, tile.Width, tile.Height, 8.0f);
                            g.FillPath(&surface, &tilePath);
                            const float box = (std::min)(tileW, tileH) - 8.0f;
                            if (HBITMAP thumb = ShelfThumb(shelf.files[r], (int)(box * scale))) {
                                // shortcut: FromHBITMAP drops the alpha channel, so a file-type icon gets a black box (barely seen on the dark tile). Images are opaque.
                                Bitmap pic(thumb, (HPALETTE)NULL);
                                float tw = pic.GetWidth() / scale, th = pic.GetHeight() / scale; // already fitted inside the box with its aspect kept
                                g.DrawImage(&pic, tile.X + (tileW - tw) / 2.0f, tile.Y + (tileH - th) / 2.0f, tw, th);
                            } else {
                                g.DrawString(L"", -1, &iconBig, tile, &center, &textDim);
                            }
                            RectF xBox(tile.X + tileW - 19.0f, tile.Y + 3.0f, 16.0f, 16.0f); // the X sits on the tile's corner, over the picture
                            SolidBrush xBg(Color(210, 8, 9, 12));
                            g.FillEllipse(&xBg, xBox);
                            g.DrawString(L"", -1, &iconSmall, xBox, &center, &textWhite);
                            tabHits.push_back({ tile, HIT_S_ITEM, (int)r, 0 });
                            tabHits.push_back({ xBox, HIT_S_REMOVE, (int)r, 0 });
                            continue;
                        } else {
                            g.DrawString(L"", -1, &iconSmall, RectF(20.0f, itemY, 24.0f, rowH), &center, &textDim);
                        }
                        g.DrawString(ShelfManager::Name(shelf.files[r]).c_str(), -1, &fontSmall, RectF(textX, itemY, width - textX - 52.0f, rowH), &leftMid, &textWhite);
                        g.DrawString(L"", -1, &iconSmall, RectF(width - 44.0f, itemY, 24.0f, rowH), &center, &textDim);
                        tabHits.push_back({ RectF(20.0f, itemY, width - 68.0f, rowH), HIT_S_ITEM, (int)r, 0 });
                        tabHits.push_back({ RectF(width - 44.0f, itemY, 24.0f, rowH), HIT_S_REMOVE, (int)r, 0 });
                        itemY += rowH;
                    }

                    // Footer: a copy in progress or failed, else what a click does; with more files than fit, where the wheel is
                    std::wstring foot = !shelfNote.empty() ? shelfNote : N == 0 ? L"" : L"Clic en un archivo para abrirlo";
                    if (N > rows)
                        foot += L" · " + std::to_wstring(shelfTop + 1) + L"-" + std::to_wstring((std::min)(shelfTop + rows, N)) + L" de " + std::to_wstring(N);
                    g.DrawString(foot.c_str(), -1, &fontSmall, RectF(20.0f, 212.0f, width - 40.0f, 20.0f), &leftMid, &textDim);
                }
            }
            g.Restore(gs);

            // Veil over the content: fades during a tab switch, and while the notch is still opening (then it covers the tab row too)
            float openVeil = perf.anim ? 1.0f - std::clamp((height - 60) / 110.0f, 0.0f, 1.0f) : 0.0f;
            float veil = (std::max)(1.0f - tabEase, openVeil);
            if (veil > 0.0f) {
                GraphicsState vs = g.Save();
                g.TranslateTransform(-(float)NOTCH_SHOULDER, 0.0f); // notchPath is in untranslated coordinates
                g.SetClip(&notchPath);
                SolidBrush veilBrush(Color((BYTE)(255 * veil), 8, 9, 12));
                g.FillRectangle(&veilBrush, 0.0f, openVeil > 1.0f - tabEase ? 0.0f : 42.0f, (float)fullWidth, (float)height);
                g.Restore(vs);
            }
        }

        // Premultiply alpha in-place per scanline for Windows UpdateLayeredWindow
        for (int y = 0; y < pixelHeight; ++y) {
            BYTE* row = (BYTE*)pvBits + y * stride;
            for (int x = 0; x < pixelWidth; ++x) {
                BYTE* px = row + x * 4;
                BYTE a = px[3];
                px[0] = (BYTE)((px[0] * a) / 255);
                px[1] = (BYTE)((px[1] * a) / 255);
                px[2] = (BYTE)((px[2] * a) / 255);
            }
        }
    }
};
