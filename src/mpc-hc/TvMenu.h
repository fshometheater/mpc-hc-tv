/*
 * TV mode for MPC-HC (personal fork).
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <mvrInterfaces.h>
#include <vector>
#include <array>
#include "TvInfo.h"

// Info > Scopes (waveform, tone curve; TvScopes.cpp) is held back from tv-mode for now: 0 = no Scopes
// tab and nothing measured. Work on it continues on the feature/tv-scopes branch, where this is 1.
#ifndef TV_SCOPES
#define TV_SCOPES 0
#endif

class CMainFrame;

// Remote-navigable TV overlay drawn over madVR through madVR's own bitmap OSD
// (IMadVROsdServices::OsdSetBitmap), so no window is put on top of the video and
// keyboard focus never leaves the main frame. Inert unless started with /tv.
//
// Control bar (bottom of the screen), opened with Down, Enter (play/pause) or
// Left/Right (starts seeking):
//   seek row:    current time ---o--------- duration
//   button row:  |<  -10  >/||  +30  >|   Ends at 21:48        CC  audio  settings
// Up/Down switch rows (Up from the seek row hides the bar), Left/Right move between
// buttons or move a seek preview (tap 10 s, held keys go faster; the jump happens
// 0.8 s after the last press or at once with Enter, Back cancels it). Enter presses
// a button. The bar hides after 5 s while playing and stays up while paused.
//
// Lists (centered panel): "Settings" (gear button: Audio >, Subtitles >, Subtitle delay,
// TV options >, MPC-HC options, Desktop mode, Close), the Audio / Subtitles track lists
// and "TV options" (CTvMode's TvOpt list: when to show the controls, hide delays, skip
// and seek steps, delay step, sizes). Back goes up one level, back to
// the bar when opened from it. Selecting a track applies it at once and closes.
// On the Playback list, Left/Right on Audio / Subtitles step through tracks in place
// (applied after a short pause, or at once when the list closes or opens fully).
//
// Panels fade in/out, lists close after 10 s without a key, and everything follows
// output size changes. Commands bindable in Options > Player > Keys: "TV Mode: Menu"
// (toggles the bar), "TV Mode: Audio List", "TV Mode: Subtitle List".
class CTvMenu
{
public:
    static CTvMenu& Instance();

    // Hooks called from CMainFrame (marked "// TV MODE" there).
    bool PreTranslateMessage(CMainFrame* pMainFrame, const MSG* pMsg);
    void OnCloseMedia();
    void OnPaused(CMainFrame* pMainFrame);      // end of CMainFrame::OnPlayPause (any way of pausing)
    void OnMediaLoaded(CMainFrame* pMainFrame); // end of CMainFrame::OnFilePostOpenmedia
    // ID_TV_MENU (toggle bar), ID_TV_AUDIO, ID_TV_SUBTITLES: bindable in Options > Player > Keys.
    void OnCommand(CMainFrame* pMainFrame, UINT nID);

    bool IsOpen() const { return m_bOpen; }
    void ForceClose(); // leaving TV Mode: close at once, no fade

private:
    CTvMenu() = default;
    ~CTvMenu();
    CTvMenu(const CTvMenu&) = delete;
    CTvMenu& operator=(const CTvMenu&) = delete;

    // MiniSeek: Left/Right seeking with only a small time indicator ("Show controls when seeking" off)
    // VideoInfo / AudioInfo: read-only info panels (Basic / Extended / Nightmare, Left/Right)
    // ColourPicker: custom accent colour (Hue / Saturation / Lightness bars, remote and mouse)
    enum class Page { None, Bar, MiniSeek, Main, Audio, Subtitles, Options, VideoInfo, AudioInfo, ColourPicker, SubStyle, OptionsSection,
                      MadVR, Lav, Video, Advanced, Chapters, Pause, Scopes,
                      Quit }; // Quit: Back with nothing open (exit, fullscreen, restart)
    // Settings-type pages: may be as tall as the band between 5 % from the top and the control bar
    static bool IsBandPage(Page p) {
        return p == Page::Main || p == Page::Options || p == Page::OptionsSection || p == Page::MadVR || p == Page::Lav ||
               p == Page::Chapters ||
               p == Page::Video || p == Page::Advanced;
    }
    static bool IsInfoPage(Page p) { return p == Page::VideoInfo || p == Page::AudioInfo; }

    enum class RowKind {
        OpenAudio,
        OpenSubtitles,
        SubtitleDelay,
        AudioDelay,
        OpenOptions,   // "TV options >" page
        OpenPicker,    // "Custom colour >" (colour picker page)
        Option,        // one TvOpt (data), Left/Right or Enter step through its choices
        ResetOptions,  // all TV options back to defaults
        MpcOptions,    // MPC-HC's own Options dialog (mouse)
        DesktopMode,   // leave TV Mode
        ExitPlayer,    // close MPC-HC (File > Exit)
        ToggleFullscreen, // enter / leave fullscreen (View > Full Screen)
        RestartPlayer, // close MPC-HC and start it again with the same file at the same place
        ReopenToApply, // Video page: reopen the file so a new renderer is used
        Close,
        AudioTrack,
        SubtitleOff,
        SubtitleTrack,
        Info, // not selectable content, e.g. "No audio tracks"
        Heading, // section title on the TV options page; never focused
        OpenSubStyle, // "Subtitle style" page (from the Subtitles list and TV options)
        ResetSubStyle, // subtitle style back to this build's defaults
        OpenSection,   // TV options: open one submenu (data = section index)
        BarItemRow,    // Bar buttons submenu: one bar button (data = index in BarLayout())
        KeyRow,        // Remote keys submenu: one action (data = index in the action list)
        MadVrSetting,  // madVR panel: a madVR setting changed with Left/Right (data = index in the list)
        MadVrStats,    // madVR panel: statistics on / off (Ctrl+J)
        MadVrOpenSub,  // madVR panel: open a submenu (data = submenu)
        MadVrAction,   // madVR panel: a button (data = MadVrActionId)
        MadVrModeTags, // madVR display modes: one line of mode tags (data = first mode, data2 = count)
        OpenLav,       // Settings: LAV Audio (data 0) / LAV Video (data 1) page
        LavSetting,    // LAV page: one setting (data = index in the LAV table), Left/Right or OK change it
        LavAction,     // LAV page: open LAV's own settings window (data 0 audio, 1 video)
        OpenMadVr,     // Settings / Video: the madVR panel
        VideoRenderer, // Video: MPC-HC's video renderer (Left/Right; applies from the next file)
        ShadersRow,    // Video: MPC-HC's pixel shaders on / off
        OpenVideo,     // Settings: the Video page
        OpenAdvanced,  // Settings: the full dialogs (mouse)
        AddSeparator,  // Control bar > Buttons: add a separator (then placed with Up / Down)
        MadVrDevice,   // madVR panel, several displays: which madVR device the settings are for (Left/Right)
        ChapterRow,    // Chapters list: one chapter (data = index in GetChapters()), OK jumps there
        ArrowRow,      // Button / key mapping: an arrow's tap / hold action during playback (data = slot 0-7)
    };
    // Rows whose value is changed in place; while faded (they need another setting on) they can't be
    static bool IsSettingRow(RowKind k) {
        return k == RowKind::Option || k == RowKind::LavSetting || k == RowKind::MadVrSetting;
    }

    struct Row {
        RowKind kind = RowKind::Info;
        CStringW text;     // main text
        CStringW value;    // right-aligned value (main list)
        CStringW detail;   // smaller second line (track lists)
        bool checked = false;
        int data = -1;     // track index
        int data2 = -1;    // madVR on/off row: the value shown and adjusted next to it
        CStringW prefix;   // drawn just left of the "< value >" (e.g. "On")
        bool faded = false; // madVR: only applies while an on / off option is on; drawn faded
        bool skip = false;  // shown, but the focus passes over it (e.g. the only display)
        int toggle = -1;    // an on / off value drawn as a switch: 1 on, 0 off (-1: plain value)
        CStringW icon;      // Segoe MDL2 glyph left of the text (Settings page)
        CStringW help;      // one line explaining it, under the list while it's focused (TV options)
        int col = 0;       // two-column pages: 0 left, 1 right
        int line = 0;      // line within its column (set by BuildRows; = row index for one column)
    };
    // Rows the focus (keys and mouse) passes over: headings, and settings that are faded because
    // they need another setting on
    static bool IsSkipped(const Row& r) { return r.kind == RowKind::Heading || r.faded || r.skip; }
    // A switch row's identity across rebuilds (its kind and data), for the slide when it flips
    static int ToggleKey(const Row& r) { return static_cast<int>(r.kind) * 100000 + r.data; }
    int m_nToggleAnimKey = -1;
    ULONGLONG m_nToggleAnimStart = 0;
    bool m_bTwoColumns = false;  // current page is laid out in two columns

    // Stats = madVR's statistics (presses Ctrl+J). Volume = mute button + level slider.
    // Values are stored in the "BarButtons" setting: only append new ones.
    // Shaders = MPC-HC's pixel shaders on / off (Play > Shaders), off by default.
    enum class BarButton { Prev, Back10, PlayPause, Fwd30, Next, Stats, VideoInfo, AudioInfo, Subtitles, Audio, Settings, Volume,
                           Shaders, Video, Separator, Chapters, Fullscreen, Power, Count }; // Separator: a divider in the bar (any number)
    int m_nLoadedRenderer = -1; // the renderer the current file was opened with (-1: no file)
    static bool ShadersOn(CMainFrame* pMainFrame);
    static void ToggleShaders(CMainFrame* pMainFrame);
    // Back from a page opened inside Audio / Video (LAV, madVR): to that page, which then keeps
    // its own way back (bar or Settings)
    struct Crumb { Page page; int focus; Page parent; };
    std::vector<Crumb> m_crumbs;
    static bool IsMadVrRenderer();
    // madVR panel (the madVR bar button): live madVR settings through IMadVRSettings
    CStringW MadVrActiveDevice(CStringW* deviceName);  // "devices\<id>" of the display in use, "" if unknown
    CStringW m_strMadVrDevice;                        // found when the madVR panel opens
    CStringW m_strMadVrDeviceName;
    bool m_bResetArmed = false;        // a Reset row was pressed once: the next OK resets
    CStringW m_strMadVrDeviceActive;   // the device of the display in use (m_strMadVrDevice may be another one)
    CStringW m_strMadVrDeviceOverride; // another display picked on the madVR panel ("" = the one in use)
    std::vector<std::pair<CStringW, CStringW>> MadVrDeviceList(); // madVR's devices: path, name
    CStringW MadVrHdrSummary();        // HDR mode as shown in the HDR submenu ("" if unknown)
    CStringW m_strMadVrProblem;                       // why no device was chosen (shown in the panel)
    bool MadVrGet(int index, CStringW& value);
    CStringW MadVrPath(int index);
    int m_nMadVrSub = 0;        // madVR panel submenu (0 = the panel itself)
    int m_nMadVrSubFocus = 0;   // row to return to in the panel
    // LAV Audio / Video pages: settings changed through LAV's own interface on the running
    // filter, then saved the way MPC-HC's Options save them
    int m_nLavPage = 0; // 0 audio, 1 video
    CComPtr<IBaseFilter> LavFilter(bool video);
    void LavStep(int defIndex, int dir);
    void LavSave(bool video);
    CStringW LavSummary(bool video);
    void LavWindow(bool video);
    bool m_bMadVrCompact = false; // live-adjust strip: only the focused madVR row, at the top
    int m_nMadVrPart = -1;        // editing a madVR row: the picked part (MvPart), -1 = not editing
    std::vector<std::pair<RECT, int>> m_hitMadVrParts; // mouse: parts of the edited row
    std::vector<int> MadVrParts(int row) const;
    void StartMadVrEdit();
    void EndMadVrEdit();
    void MadVrEditStep(int dir);
    void MoveColumn(int dir);
    bool HandleMadVrKey(UINT vk, bool isRepeat);
    bool MadVrClick(int row, POINT pt);
    enum MadVrActionId { MvActionSettingsWindow = 0, MvActionFillModes,
                         MvActionAdd2160, MvActionAdd1440, MvActionAdd1080, MvActionAddCurrentRes, MvActionAddCurrentMode,
                         MvActionClearModes };
    // Display modes submenu: madVR's list as tags (Left / Right pick, OK shows an x, OK again removes)
    std::vector<std::array<INT32, 4>> m_madVrModes; // width, height, refresh, interlaced (as madVR stores them)
    int m_nModeTag = 0;
    bool m_bModeTagArmed = false;
    std::vector<std::pair<RECT, int>> m_hitTags;
    std::vector<std::array<INT32, 4>> MadVrModesList();
    bool MadVrSetModes(std::vector<std::array<INT32, 4>> list);
    std::vector<std::array<INT32, 4>> MadVrModesFor(int action);
    bool HandleModeTagKey(UINT vk, bool isRepeat);
    void ScrollFocusIntoView();
    // Display mode list (devices\<id>\displayModes\displayModesData: 16 bytes per mode = int32
    // width, height, refresh (Windows' rounded value, e.g. 23 for 23.976), interlaced)
    CStringW MadVrModesInMadVr();   // what madVR has now, as madVR's text ("2160p23, 2160p24")
    CStringW MadVrModesAvailable(); // all refresh rates Windows offers at the current resolution
    std::vector<BYTE> MadVrModesAvailableData(); // the same, in madVR's stored form
    static std::vector<int> MadVrRatesAt(LPCWSTR device, const DEVMODEW& cur);
    bool MadVrWriteDisplayModes(IMadVRSettings* s, const std::vector<BYTE>& data);
    void MadVrAction(int action);
    void RebuildKeepingMadVrFocus(int defIndex);
    bool MadVrVisible(int index);   // shown now (e.g. tone mapping values only while tone mapping)
    int MadVrShowState(int index);  // 0 hidden, 1 shown, 2 shown faded (its on / off is off)
    static int MadVrSettingCount();
    static CStringW MadVrLabel(int index);
    CStringW MadVrValueText(int index);
    void StepMadVrSetting(int index, int dir);
    void ToggleMadVrStats();
    static std::vector<BarButton> VisibleButtons(); // left to right

    // Bar layout ("Bar buttons" submenu): order and on/off per button. Transport buttons
    // (Prev .. Next) sit on the left, the rest on the right; each side keeps its own order.
    struct BarItem {
        BarButton id;
        bool on;
    };
    static std::vector<BarItem> BarLayout();
    static void SaveBarLayout(const std::vector<BarItem>& items);
    static bool IsLeftButton(BarButton b) { return b <= BarButton::Next; }
    static bool CanHide(BarButton b) { return b != BarButton::PlayPause && b != BarButton::Settings; }
    static LPCWSTR BarButtonName(BarButton b);
    // Remote keys submenu: waiting for a key press for this action (-1 = not listening)
    int m_nListenKey = -1;
    CStringW m_strKeyMessage;    // shown in the listening row (e.g. "used by the menus")
    bool HandleKeyCapture(const MSG* pMsg); // true = the key was taken by the listening row
    static CStringW KeyText(int actionId);  // current key of an action, "Not set" if none
    static void AssignKey(int actionId, UINT vk, BYTE fVirt); // vk 0 = clear
    static UINT ExtraKey(bool back);        // TV-only extra Back / OK key (0 = none)
    static UINT BarKey();                   // TV-only key that shows the control bar (default Down; 0 = none)
    // Arrow keys during playback (no menu open): tap / hold actions, slots 0-7 = Left, Left hold,
    // Right, Right hold, Up, Up hold, Down, Down hold
    static int ArrowIndex(UINT vk);
    static bool IsJumpCommand(int id);      // MPC-HC's six jumps
    static std::map<UINT, int> HoldMap();   // key -> command held 0.6 s (TvMode "HoldKeys")
    static void SaveHoldMap(const std::map<UINT, int>& m);
    static int HoldFor(UINT vk);            // the hold command of a key, 0 = none
    static void ClearAction(int action);    // every key off an action (MPC-HC key, arrows, holds)
    // Every key of an action, for its row's text and the Edit tags: its MPC-HC key (or TV-only key),
    // the arrows set for it, the keys held for it
    struct KeyBinding {
        int type = 0;   // 0 the key (MPC-HC's or the TV-only one), 1 an arrow, 2 a hold
        UINT vk = 0;    // hold: the key held
        int slot = -1;  // arrow: its ArrowAction slot
        CStringW label; // "Space", "Right", "hold Red"
    };
    static std::vector<KeyBinding> KeyBindings(int action);
    static void RemoveBinding(int action, const KeyBinding& b);
    static int HoldCommand(int action);     // what a hold of this action runs (0 = it can't have one)
    enum KeyChoice { kChoiceSet = 0, kChoiceHold, kChoiceEdit, kChoiceClear };
    static std::vector<int> KeyChoices(int action); // the row's choices: Set, Set hold, Edit (2+ keys), Clear
    int m_nKeyEditRow = -1;                 // Button / key mapping: row showing its choices
    int m_nKeyEditPart = 0;                 // the chosen one (index into KeyChoices)
    int m_nKeyTag = -1;                     // Edit: the picked key tag (-1 = not editing the keys)
    bool m_bListenHold = false;             // the key being waited for becomes a hold
    bool HandleKeyEditKey(UINT vk, bool isRepeat);
    static int ArrowAction(int slot);
    static void SetArrowAction(int slot, int action);
    static std::vector<int> ArrowChoices(bool hold);
    static CStringW ArrowActionName(int action);
    bool RunArrowAction(int action, UINT vk, bool isRepeat, bool deferred);
    void ReleaseArrow();
    void ArmArrowTimer();
    static void CALLBACK OnArrowTimer(HWND hWnd, UINT msg, UINT_PTR id, DWORD time);
    UINT m_nArrowPendingVk = 0;             // an arrow with a hold action, waiting for key-up / hold
    ULONGLONG m_nArrowDownAt = 0, m_nArrowLastKey = 0;
    bool m_bArrowHoldDone = false;
    UINT_PTR m_nArrowTimer = 0;
    static bool NeedsBarFallback();         // no key / automatic way to bring up the bar
public:
    // MPC-HC's right-click menu, just before it opens: "Show control bar" at the top while
    // NeedsBarFallback() (TV Mode, file loaded); removed again otherwise
    void PrepareContextMenu(CMainFrame* pMainFrame, CMenu* pMenu);
private:
    int m_nMovingItem = -1;      // Bar buttons submenu: layout index being moved with Up/Down (-1 = none)
    void MoveBarItem(int dir);

    // Volume on the bar
    RECT m_hitVolume = {};       // slider (output coordinates)
    int m_hitVolumeX0 = 0, m_hitVolumeX1 = 0;
    bool m_bDragVolume = false;
    void SetVolumeFromX(int x);

    // Chapters of the current file (for the seek line), refreshed while the bar is drawn
    struct Chapter {
        REFERENCE_TIME rt;
        CStringW name;
    };
    std::vector<Chapter> GetChapters() const;

    // Mouse: hover moves the focus, clicks press / select, drag on the seek row seeks,
    // wheel scrolls lists, right-click or a click outside a list = Back. Clicks on the
    // overlay never reach MPC-HC (no click-to-pause through the bar).
    bool HandleMouse(CMainFrame* pMainFrame, const MSG* pMsg);
    bool ToOutput(const MSG* pMsg, POINT& pt) const; // message position -> output (OSD) coordinates
    int HitRow(POINT pt) const;                       // list row under pt, -1 = none
    bool HitButton(POINT pt, BarButton& button) const;
    bool HitSeek(POINT pt) const;
    REFERENCE_TIME TimeAtX(int x) const;
    void RequestRender();                             // Render, at most ~30 times a second

    bool HandleKeyDown(UINT vk, bool isRepeat);
    bool HandleBarKey(UINT vk, bool isRepeat);
    bool HandleMiniSeekKey(UINT vk, bool isRepeat);
    bool Open(CMainFrame* pMainFrame, Page page, Page parent);
    void Close();
    void RemoveNow(); // no fade: file closing etc.
    void ShowPage(Page page, int focusRow);
    void OpenChild(Page page, int focusRow); // from the current page, Back returns to it
    void BuildRows();          // builds the page's rows, then their column / line layout
    void BuildRowsContent();
    int LineCount() const;     // lines of the tallest column
    int MaxRows() const;   // rows shown at once: 8, or about 80 % of the screen height for TV options
    void Move(int delta, bool mayWrap = false); // mayWrap: a fresh Up / Down press (WrapAround option)
    void Activate();
    void Back();
    void Render();
    void RenderPanel();
    void RenderBar(bool preview = false);
    void RenderInfo();
    void RenderPicker();
    bool HandlePickerKey(UINT vk, bool isRepeat);
    void PickerAdjust(int row, int delta);  // row 0 hue, 1 saturation, 2 lightness
    int m_nPickerTarget = 0;                // what the picker edits: 0 custom accent, 1 custom background
    void PickerGet(int& h, int& s, int& l) const;
    void PickerSet(int h, int s, int l);
    void PickerSetFromX(int row, int x);     // mouse on a bar
    void BuildInfo();        // (re)reads the info lines for the current info page and level
    int InfoLevel() const;   // remembered per panel
    void SetInfoLevel(int level);
    void UpdateBarPreview(); // bar / sample subtitle shown while a related TV option is focused
    void RenderSubtitlePreview();
    void SendSubtitlePreview(float fade);
    // Sample subtitle cache (straight-alpha BGRA pixels), rebuilt only when the style changes
    std::vector<BYTE> m_subCache;
    CStringW m_subCacheKey;
    int m_subCacheW = 0, m_subCacheH = 0, m_subCacheX = 0, m_subCacheY = 0;
    float m_subCacheFade = -1.0f;
    void RenderMiniSeek();
    void RemoveOsd();

    // Control bar
    void PressBarButton(BarButton button);
    void Scrub(int dir, bool isRepeat);
    void ApplySeek();
    bool StepGlide();                    // held Left / Right: move the preview by this frame's time
    void ArmPauseTimer(int ms);          // pause screen check in ms (0 = stop)
    static void CALLBACK OnPauseTimer(HWND hWnd, UINT msg, UINT_PTR id, DWORD time);
    void CheckPauseScreen();             // paused and quiet long enough: show the pause screen
    void RenderPauseScreen();
    UINT_PTR m_nPauseTimer = 0;
    POINT m_ptPauseMouse = {};           // cursor (screen) when the pause screen came up
    void BarOk();                        // OK on the bar (seek row / focused button)
    void ReleaseOk();                    // OK let go: the press that was waiting to see if it's a hold
    bool m_bOkPending = false;           // OK pressed on the bar, not yet let go or held long enough
    ULONGLONG m_nOkDownAt = 0;
    ULONGLONG m_nOkLastKey = 0;          // last OK key-down (first or repeat) while pending
    REFERENCE_TIME SnapToChapter(REFERENCE_TIME from, REFERENCE_TIME target, int dir) const; // ChapterSnap
    void CancelSeek();
    bool IsPlaying();

    // Left/Right on the Playback list's Audio / Subtitles rows
    static constexpr int kNoPending = INT_MIN;
    void CycleTrack(RowKind kind, int dir);
    bool AdjustRow(int dir); // Left/Right (or a click on a row's arrows); false = row has no value to change
    int RowArrowAt(int row, POINT pt) const; // mouse over a row's value arrows (see m_nRowArrowHover)
    void ApplyPending();
    void CancelApplyTimer();
    static void CALLBACK OnApplyTimer(HWND hWnd, UINT msg, UINT_PTR id, DWORD time);

    // One timer while something is visible: fade steps, auto-hide, seek apply, bar
    // time refresh, following output size changes.
    void StartTick();
    void StopTick();
    void OnTick();
    static void CALLBACK OnTickTimer(HWND hWnd, UINT msg, UINT_PTR id, DWORD time);

    static CComPtr<IMadVROsdServices> FindMadVrOsd(CMainFrame* pMainFrame);
    static bool IsTargetWindow(CMainFrame* pMainFrame, HWND hWnd);

    bool m_bOpen = false;
    Page m_page = Page::None;
    Page m_parent = Page::None;     // where Back goes from a track list (None = close)
    Page m_mainParent = Page::None; // where Back goes from the Playback list
    int m_nFocus = 0;
    int m_nMainFocus = 0;       // focus to restore when coming back to the Settings list
    int m_nScroll = 0;          // first visible row
    float m_fScrollShown = 0.0f;  // where the list is drawn, in lines (springs towards m_fScrollTarget)
    float m_fScrollTarget = 0.0f; // where the mouse wants it (fractional while the wheel / scrollbar is in use)
    float m_fScrollVel = 0.0f;    // lines per second
    ULONGLONG m_nScrollInputAt = 0;
    bool m_bScrollGlide = false;  // mouse scrolling is moving the list (m_fScrollShown is in charge)
    void SetScrollTarget(float target, bool follow);
    std::vector<Row> m_rows;
    CMainFrame* m_pMainFrame = nullptr;
    CComPtr<IMadVROsdServices> m_pOsd;   // where the overlay is drawn: madVR, or our own windows
    CComPtr<IMadVROsdServices> m_pMadVr; // the madVR filter when it's the renderer (its settings, info)
    CComPtr<IMadVROsdServices> m_pWinOsd; // the own-window overlay (TvOsdWindow), made when first needed
    HWND m_hWinOsdVideo = nullptr;
    bool m_bWindowOsd = false;
    POINT m_lastOrigin = { LONG_MIN, LONG_MIN }; // own windows: follow the video window when it moves
    ULONGLONG m_nLastTick = 0;           // animation steps scale with the real time between ticks
    ULONGLONG m_nBackDownAt = 0;         // Back pressed (held down = close everything)
    bool m_bSwallowBack = false;         // ...then its remaining repeats are eaten until it's released
    bool ChooseOverlay();                // picks m_pOsd from the TV option and what's playing
    int BarHeightAt(int outH) const;     // control bar height (as RenderBar draws it)
    void SettingsBand(int outH, int& top, int& bottom) const; // where settings pages may sit
    void SwitchOverlay();
    void SwitchInfoKind();               // info panel: video <-> audio
    RECT m_hitInfoTab[3] = {};           // info panel tabs (mouse): 0 video, 1 audio, 2 scopes (madVR only)
    void ShowInfoTab(int tab);           // Video / Audio / Scopes, keeping the way back
    void RestartPlayer();                // Quit page: a new player with this file and position, then exit
    int InfoTabCount() const { return (TV_SCOPES && m_pMadVr) ? 3 : 2; }
    void RenderScopes();                 // Info > Scopes: waveform, tone curve, history, stats (madVR)
    unsigned m_nScopesVersion = 0;       // last measurement drawn
    int MadVrDisplayNits();              // madVR's "display peak luminance" (tone mapping target), 0 if unknown
    HBITMAP m_hbmMenu = nullptr;
    HBITMAP m_hbmPreview = nullptr;  // control bar preview
    HBITMAP m_hbmSubPreview = nullptr; // sample subtitle (subtitle opacity)

    // Info panels (pinned to the top, full width): chip rows, then detail lines in columns, paged
    std::vector<TvInfoLine> m_info;
    std::vector<TvInfoChip> m_chips1, m_chips2; // source card, output card
    std::vector<TvInfoBadge> m_badges;
    CStringW m_strInfoFile;
    RECT m_hitLevelPill[3] = {};     // the level switch: click a level
    int m_nInfoScroll = 0;           // current page
    int m_nInfoPages = 1;            // from the last render
    int m_nInfoVisible = 0;          // unused for paging, kept for clamping
    ULONGLONG m_nLastInfoBuild = 0;
    RECT m_hitLevelPrev = {}, m_hitLevelNext = {}; // clickable level arrows

    // Colour picker (focus row = m_nFocus: 0 hue, 1 saturation, 2 lightness)
    RECT m_hitPickerBar[3] = {};     // gradient bars, output coordinates
    int m_nDragPicker = -1;          // bar being dragged with the mouse
    ULONGLONG m_nPickerHoldStart = 0;
    int m_nOptionsFocus = 0;         // TV options submenu row to return to (from the colour picker)
    int m_nSection = 0;              // TV options submenu being shown (OptionsSection page)
    int m_nSectionFocus = 0;         // TV options row to return to from a submenu
    Page m_subStyleReturn = Page::None; // Subtitle style page: where Back goes (Subtitles or Options)
    int m_nSubStyleReturnFocus = 0;
    int m_nArrowHover = 0;           // mouse over: 0 none, 1 previous, 2 next

    // Pop-up scrollbar (lists / info pages with more than fits): shown for a moment after a
    // scroll, and while the mouse is near the right edge of the window; fades in and out.
    ULONGLONG m_nScrollShownAt = 0;
    bool m_bMouseRightEdge = false;
    float m_fScroll = 0.0f;          // current scrollbar opacity
    ULONGLONG m_nScrollAnimLast = 0;
    void ShowScrollbar() { m_nScrollShownAt = GetTickCount64(); }
    float ScrollbarAmount();         // 0..1 opacity, keeps redrawing while it animates

    // The scrollbar is also a mouse control: click the track to jump, drag the thumb.
    RECT m_hitScrollTrack = {};      // generous hit area (output coordinates), empty = no scrollbar
    int m_hitScrollTop = 0, m_hitScrollH = 0, m_hitScrollThumbH = 0;
    int m_nScrollMax = 0;            // highest scroll position (list offset or info page)
    bool m_bDragScroll = false;
    void ScrollToY(int y, bool follow = false); // maps a mouse y on the track to a scroll position (follow: dragging)
    void ScrollBy(float lines);      // mouse wheel on a list: scrolls the page, the highlight stays in view
    void KeepFocusInView();          // after scrolling: the nearest selectable row of the focus's column in view

    int m_nPendingAudio = kNoPending;    // audio track index chosen with Left/Right, not applied yet
    int m_nPendingSubtitle = kNoPending; // subtitle index (-1 = off) chosen with Left/Right
    UINT_PTR m_nApplyTimer = 0;

    // Control bar state
    int m_nBarRow = 1;                   // 0 = seek row, 1 = buttons
    BarButton m_barButton = BarButton::PlayPause;
    REFERENCE_TIME m_rtPreview = -1;     // seek preview target, -1 = none
    REFERENCE_TIME m_rtDragSought = -1;  // mouse drag: last position actually sought
    ULONGLONG m_nLastDragSeek = 0;
    void DragSeekNow(bool force);        // mouse drag: live seek (throttled, key frames like MPC-HC's bar)
    void EndDragSeek();                  // mouse drag released: exact seek, resume if paused for it
    bool m_bPausedForDrag = false;       // MPC-HC's "pause while dragging the seek bar"
    REFERENCE_TIME m_rtInstantTarget = -1; // last target of an instant seek (positions lag a moment)
    ULONGLONG m_nInstantAt = 0;
    bool InstantSeek() const;            // TV options: jump at once instead of preview + pause
    bool SeekWaitsForOk() const;         // TV options: the seek preview waits for OK
    bool PreviewShowsSelection() const;  // bar preview shows a selected button (button name / icon options)
    ULONGLONG m_nDemoPhase = 0;          // which button that preview shows (changes every 1.6 s)
    ULONGLONG m_nScrubHoldStart = 0;     // when the held Left/Right key went down
    ULONGLONG m_nLastScrub = 0;          // last preview step (held keys are throttled)
    REFERENCE_TIME m_rtDot = -1;         // where the bar's playhead dot is drawn (glides to the position)
    REFERENCE_TIME m_rtDotDur = 0;       // the duration it was drawn for (another file: no glide)
    ULONGLONG m_nDotTick = 0;            // when the dot was last drawn
    ULONGLONG m_nDotWatchUntil = 0;      // after a seek: draw the bar every frame until then
    bool m_bDotMoving = false;           // the dot hasn't reached the position yet
    int m_nGlideDir = 0;                 // held Left / Right gliding the preview: -1 / +1, 0 not
    ULONGLONG m_nGlideTick = 0;          // when the glide last moved
    ULONGLONG m_nGlideKey = 0;           // last key repeat of the glide
    ULONGLONG m_nLastBarRender = 0;

    // Where things were drawn (output coordinates), for mouse hit-testing
    RECT m_outRect = {};           // madVR's full output rect in the video window's client area
    RECT m_elemRect = {};          // the drawn panel / bar
    int m_hitRowsTop = 0, m_hitRowH = 0, m_hitRowsLeft = 0, m_hitRowsRight = 0, m_hitRowsVisible = 0;
    int m_hitFirstRow = 0;           // row index drawn at the top (m_nScroll, or the focus when collapsed)
    // Per row (index into m_rows), output coordinates; empty / -1 when not on screen
    std::vector<RECT> m_hitRowRect;     // the row
    std::vector<int> m_hitValueLeft;    // left arrow of its "< value >" (-1 = no arrows)
    std::vector<int> m_hitValueRightRow; // right end of its value
    int m_hitArrowW = 0;                // width of one arrow click area
    int m_hitSeekX0 = 0, m_hitSeekX1 = 0, m_hitSeekCy = 0, m_hitSeekHalfH = 0;
    struct ButtonHit { BarButton id; POINT center; };
    std::vector<ButtonHit> m_hitButtons;
    int m_hitButtonR = 0;

    // Mouse state
    POINT m_lastMouseScreen = { LONG_MIN, LONG_MIN };
    bool m_bDragSeek = false;
    bool m_bSwallowLUp = false;
    bool m_bSwallowRUp = false;
    REFERENCE_TIME m_rtHover = -1; // seek row hover time, -1 = none
    ULONGLONG m_nLastRender = 0;
    bool m_bDirty = false;

    // Focus crossfade: when the focus moves, the old highlight fades out while the new one
    // fades in (~0.15 s). Nothing travels, so it stays smooth at 24 fps video frame rates.
    // Detected at draw time, so every way of moving the focus (remote, mouse) is covered.
    struct FocusFade {
        int prev = -1, cur = -1;      // row index / button id
        ULONGLONG start = 0;
        bool valid = false;           // false = show the next focus at once
        // Returns how far the new highlight has faded in (0..1); prevOut = the fading-out one or -1.
        float Progress(int target, bool& running, int& prevOut);
    };
    FocusFade m_rowFade;              // list rows
    FocusFade m_buttonFade;           // bar buttons
    FocusFade m_arrowFade;            // info level arrows (mouse hover)
    FocusFade m_rowArrowFade;         // value arrows on list rows (mouse hover)
    int m_nRowArrowHover = 0;         // 0 none, else (row + 1) * 2 + (0 left, 1 right)
    int m_nLastBarRowDrawn = -1;

    float m_fFade = 0.0f;          // current opacity factor 0..1
    float m_fPreviewFade = 0.0f;   // bar preview under TV options: its own fade 0..1
    float m_fPreviewTarget = 0.0f;
    // Show / hide motion: pixels still to travel for the current fade (eased; 0 when fully shown)
    int SlideOffset(int distance) const {
        const float f = m_fFade < 0.0f ? 0.0f : (m_fFade > 1.0f ? 1.0f : m_fFade);
        const float rest = (1.0f - f) * (1.0f - f) * (1.0f - f); // ease-out
        return static_cast<int>(rest * distance + 0.5f);
    }
    float m_fFadeTarget = 0.0f;    // 1 while open, 0 while fading out

    // Presenting (TvMenu.cpp: Present): what each OSD element shows, so fade / morph steps can
    // show it again without drawing (elements: 0 menu, 1 bar preview, 2 old picture, 3-4 focus)
    struct Shown {
        bool valid = false;
        Page page = Page::None;
        int slot = 0, x = 0, y = 0, w = 0, h = 0, radius = 0;
        BYTE alpha = 0;            // full-opacity alpha (before the fade)
        int slideMode = 0;         // 0 none, 1 panel (moves down), 2 bar (rises), 3 info (drops), 4 bar preview
        int slideDist = 0;
        int drawnY = 0, drawnDy = 0;
    };
    Shown m_shown[5];
    void Present(int elem, int slot, int x, int y, int radius, BYTE alpha, int slideMode, int slideDist);
    bool RepresentMenu();          // a fade / morph step without drawing; false = draw instead
    void HideElement(int elem);
    // Focus crossfade: the page is drawn once with the new highlight; the old and new looks of
    // the two rows are blended in small overlays (instead of redrawing the page every tick)
    struct Xfade { bool active = false; int count = 0; RECT rect[3] = {}; ULONGLONG start = 0; }; // two rows + the help / hint lines
    Xfade m_xf;
    void StepXfade();
    void CancelXfade();
    Page m_lastPanelPage = Page::None;
    int m_lastPanelScroll = -1, m_lastPanelSub = -1, m_lastPanelRows = -1;
    bool m_lastPanelCompact = false;
    // Transitions: the previous picture fades out (and drifts) while the new one comes in
    Shown m_oldShown;
    float m_fOld = 0.0f, m_fOldStart = 1.0f;
    int m_oldDy = 0;               // mini view: the strip glides back to its row
    float m_fMorph = 1.0f;         // mini view: the strip glides up from its row (0 -> 1)
    int m_morphDy = 0;
    int m_morphFromRow = INT_MIN;
    bool m_bForceTransition = false; // same page, new content (madVR submenus)
    int MorphOffset() const {
        const float f = m_fMorph < 0.0f ? 0.0f : (m_fMorph > 1.0f ? 1.0f : m_fMorph);
        return static_cast<int>((1.0f - f) * (1.0f - f) * (1.0f - f) * m_morphDy);
    }
    void StartOldFrame();
    void StepOldFrame();
    void PresentOld();
    void SetMadVrCompact(bool on);
    CStringW m_strInfoSig;         // info panel: redraw only when this changes
    CStringW InfoSignature() const;
    ULONGLONG m_nLastInput = 0;    // GetTickCount64 of the last key handled
    SIZE m_lastOutput = {};        // output size used for the last render
    UINT_PTR m_nTickTimer = 0;
};
