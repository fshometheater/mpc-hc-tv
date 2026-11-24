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

class CMainFrame;
class CAppSettings;
class STSStyle;

// Options shown on the TV overlay's "TV options" page. Each has a fixed list of
// choices; Left/Right step through them.
enum class TvOpt {
    ShowOnStart,     // show the control bar when playback starts
    ShowOnPause,     // show the control bar when playback is paused
    ShowOnSeek,      // Left/Right during playback shows: 0 the control bar, 1 a small time box, 2 nothing
    ShowOnMouse,     // moving the mouse over the video shows the control bar
    StatsButton,     // show the madVR statistics button (Ctrl+J) on the bar
    VideoInfoButton, // show the video info button on the bar
    AudioInfoButton, // show the audio info button on the bar
    BarHideSec,      // hide the bar after this long without a key while playing
    MenuHideSec,     // close lists after this long without a key (0 = never)
    SkipBackSec,     // bar's skip-back button
    SkipFwdSec,      // bar's skip-forward button
    SeekStepSec,     // one Left/Right tap on the seek row
    HoldSpeedup,     // holding Left/Right seeks faster: Off, Slowest .. Fastest (Normal = original)
    SubDelayStepMs,  // subtitle delay step
    AudioDelayStepMs,// audio delay step
    MenuTextPct,     // size of the lists
    BarSizePct,      // size of the control bar
    SubOpacity,      // MPC-HC's default subtitle style: text opacity (read and written live)
    AccentColour,    // highlight colour of the TV overlay (named presets, or Custom = colour picker)
    AccentBrightness,// darker / lighter version of the accent colour
    FontFace,        // font of the TV overlay (fonts that ship with Windows)
    BackgroundOpacity, // opacity of panel and bar backgrounds
    AnimSpeed,       // Off / Fast / Normal / Slow for fades and the focus crossfade
    KeyHints,        // key hint line at the bottom of panels
    TimeDisplay,     // right of the seek line: duration or remaining time
    ShowClock,       // current time on the bar
    ShowEndsAt,      // "Ends at" on the bar
    SeekMode,        // Left/Right seeking: 0 preview, jump after a short pause; 1 jump on every press
    ButtonLabels,    // name of the focused / hovered bar button shown under it
    OverlayMode,     // 0 drawn inside madVR (falls back to 1 without madVR), 1 own windows (display-rate, any renderer)
    SmoothPlayhead,  // the bar's playhead dot glides to the position (redraws the bar every frame while it moves)
    SmoothScrub,     // holding Left/Right glides the seek preview every frame (off: steps a few times a second)
    PauseScreen,     // seconds paused (without a key) before the pause screen shows; 0 = never
    HoldOk,          // holding OK on the bar: 0 nothing, 1 opens Info, 2 opens Settings
    ChapterSnap,     // a Left/Right step that passes a chapter start within a few seconds stops on it
    PauseScreenOn,   // the pause screen on / off (PauseScreen = after how long)
    WrapAround,      // Up at the first row goes to the last (and Down at the last to the first)
    WrapWhileHeld,   // ... also while a key is held (off: a held key stops at the end)
    BarOpacity,      // control bar background opacity (separate from the panels' BackgroundOpacity)
    BackgroundColour,// panel / bar background: dark presets, last = Custom (picker)
    BarIconBrightness,// control bar icons that aren't selected: brightness %
    BarIconOpacity,  // ... and opacity % (the selected button stays full)
    BackAction,      // Back / Esc with no menu open: 0 the power menu, 1 leave fullscreen (MPC-HC's), 2 exit
    ResumeLast,      // started without a file: reopen the last one if it was stopped part-way (has a saved position)
    // MPC-HC's own jump distances (Options > Tweaks), used by its small / medium / large jump
    // commands (assignable in Remote keys); read and written live, in ms
    JumpSmall,
    JumpMedium,
    JumpLarge,
    // MPC-HC's own default subtitle style (Options > Subtitles > Default Style), read and
    // written live; not stored in the TvMode section. SubOpacity above is one of them too.
    SubFont,         // font name (short list of installed fonts)
    SubSize,         // Scale x and y together, %
    SubColour,       // text colour presets
    SubBorderStyle,  // Outline / Opaque box
    SubOutlineWidth, // tenths of a pixel (script pixels)
    SubOutlineOpacity,
    SubShadowDepth,  // tenths
    SubShadowOpacity,
    SubMargin,       // distance from the bottom (bottom margin)
    SubRelativeTo,   // Window / Video / Auto
    // MPC-HC's own player settings, read and written live (TV options > Player); not stored in the
    // TvMode section, saved with MPC-HC's settings
    LaunchFullscreen,    // Options > Playback > Fullscreen: open files in fullscreen
    RememberFilePos,     // Options > Player > History: remember file position
    ExitFullscreenAtEnd, // Options > Playback > Fullscreen: leave fullscreen at the end
    AfterPlayback,       // Play > After playback (MPC-HC's AfterPlayback values)
    Count
};

// True for the options above that change MPC-HC's subtitle style.
inline bool IsSubtitleStyleOpt(TvOpt opt)
{
    return opt == TvOpt::SubOpacity || (opt >= TvOpt::SubFont && opt <= TvOpt::SubRelativeTo);
}

// True for options that live in MPC-HC's own settings (not the TvMode section).
inline bool IsMpcSettingOpt(TvOpt opt)
{
    return IsSubtitleStyleOpt(opt) || opt == TvOpt::JumpSmall || opt == TvOpt::JumpMedium || opt == TvOpt::JumpLarge ||
           (opt >= TvOpt::LaunchFullscreen && opt <= TvOpt::AfterPlayback);
}

// Switching between TV Mode and Desktop Mode, and the TV options.
//
// TV Mode = the TV overlay (CTvMenu) takes the remote keys, and MPC-HC's own bars,
// caption/menu and playlist follow TV Mode's own layout (first time: like View >
// Presets > Minimal). Fullscreen stays the user's own choice. Each mode remembers its
// own layout: changes made in View while in TV Mode stay with TV Mode, and leaving TV
// Mode puts back the desktop layout.
//
// The mode is remembered between runs; /tv on the command line forces it on.
// Everything is stored in MPC-HC's normal settings store (registry or .ini) under
// its own "TvMode" section, so AppSettings' load/save code is untouched.
class CTvMode
{
public:
    static void OnMainFrameCreated(CMainFrame* pMainFrame); // end of CMainFrame::OnCreate
    static void OnCommand(CMainFrame* pMainFrame, UINT nID); // ID_TV_MODE, ID_TV_MODE_APPLY*
    static void OnUpdateCommand(CCmdUI* pCmdUI);             // check mark on View > TV Mode

    static bool IsOn();
    static bool RecommendedPending();    // the installer's "Apply recommended TV settings" box was ticked
    static void ApplyRecommendedSettings(CAppSettings& s); // its player settings (while settings load)
    static void ApplyRecommended();      // ... and its layout, once the main window exists
    // View > Presets > TV: caption and menu, no toolbars; TV Minimal: borderless, no toolbars
    static void SetTvPresetLayout(bool minimal = false);
    // The last file of MPC-HC's recent files: OpenLastFile opens it (File > Recent files > the first one),
    // or offers File > Quick Open when there is none. Start-up: StartsWithoutMedia is asked before the
    // command line is handled, ResumeAtStart after it ("Open the last file on start").
    static bool OpenLastFile(CMainFrame* pMainFrame);
    static bool StartsWithoutMedia(const CAppSettings& s);
    static void ResumeAtStart(CMainFrame* pMainFrame);
    static void Enter(CMainFrame* pMainFrame, bool fromDesktop);
    static void Leave(CMainFrame* pMainFrame);

    // TV options
    static int Get(TvOpt opt);
    static bool IsOn(TvOpt opt) { return Get(opt) != 0; }
    static void Step(TvOpt opt, int dir);     // next / previous choice (wraps)
    static CStringW Label(TvOpt opt);
    static bool IsOnOff(TvOpt opt);           // a plain on / off option (drawn as a switch)
    static CStringW ValueText(TvOpt opt);
    static void ResetOptions();      // TV options only; MPC-HC's subtitle style is left alone
    // Subtitle style page: the TV defaults written into a style (also used by MPC-HC's
    // settings loader when no subtitle style was saved yet), or into the live one.
    static void SubtitleStyleDefaults(STSStyle& style);
    static void ResetSubtitleStyle();

    static COLORREF Accent();        // from AccentColour (+ custom colour) and AccentBrightness
    static LPCWSTR Font();           // from FontFace

    // Custom accent colour (colour picker): hue 0-359, saturation and lightness 0-100
    static constexpr int kCustomAccent = 10; // AccentColour value meaning "Custom"
    static int CustomHue();
    static int CustomSat();
    static int CustomLight();
    static void SetCustom(int hue, int sat, int light); // also selects Custom as the accent
    static constexpr int kCustomBackground = 8; // BackgroundColour value meaning "Custom"
    static int BgHue();
    static int BgSat();
    static int BgLight();
    static void SetCustomBackground(int hue, int sat, int light); // also selects Custom as the background
    static COLORREF Background();             // panel / bar background colour
    static COLORREF Hsl(int hue, int sat, int light);

    // Animation speed factor: 0 = off, 0.5 = fast, 1 = normal, 2 = slow
    static float AnimScale();
    // Hold speed-up factor for seeking: 0 = off, 1 = normal (the original curve), > 1 faster
    static float HoldSpeed();

    // Small remembered values that aren't on the options page (e.g. the info panels' level).
    static int GetValue(LPCTSTR key, int def);
    static void SetValue(LPCTSTR key, int value);
    static CString GetText(LPCTSTR key);              // e.g. the bar layout; empty = not set
    static void SetText(LPCTSTR key, LPCTSTR value);

private:
    // Remembered View layouts (toolbars, caption/menu mode, playlist panel); members so
    // they can reach CMainFrame's protected parts through its friend declaration.
    static void SaveLayout(CMainFrame* pMainFrame, bool tvLayout);
    static void ApplyLayout(CMainFrame* pMainFrame, bool tvLayout);
};
