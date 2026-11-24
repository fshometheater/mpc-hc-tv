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

#include "stdafx.h"
#include "TvMode.h"
#include "TvMenu.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "resource.h"
#include <map>
#include <string>
#include <vector>

namespace
{
    // Own section in MPC-HC's settings store (registry key or .ini section).
    LPCTSTR kSection = _T("TvMode");
    LPCTSTR kEnabled = _T("Enabled");                  // last mode: 1 = TV, 0 = Desktop

    // A remembered View layout (toolbars, caption/menu mode, playlist panel).
    struct LayoutKeys {
        LPCTSTR saved, toolbars, caption, playlist;
    };
    const LayoutKeys kDesktopLayout = { _T("DesktopSaved"), _T("DesktopToolbars"), _T("DesktopCaptionMode"), _T("DesktopPlaylist") };
    const LayoutKeys kTvLayout = { _T("TvSaved"), _T("TvToolbars"), _T("TvCaptionMode"), _T("TvPlaylist") };

    // Values are read on every overlay frame, so they're kept in memory after the first read
    // (keyed by name + default, since an absent value reads as the default). Writes go to the
    // settings store and drop that name from the cache.
    std::map<CString, int>& IntCache()
    {
        static std::map<CString, int> cache;
        return cache;
    }
    std::map<CString, CString>& TextCache()
    {
        static std::map<CString, CString> cache;
        return cache;
    }
    int GetInt(LPCTSTR key, int def)
    {
        CString k;
        k.Format(_T("%s|%d"), key, def);
        auto& cache = IntCache();
        auto it = cache.find(k);
        if (it != cache.end()) {
            return it->second;
        }
        const int v = AfxGetApp()->GetProfileInt(kSection, key, def);
        cache[k] = v;
        return v;
    }
    void SetInt(LPCTSTR key, int value)
    {
        AfxGetApp()->WriteProfileInt(kSection, key, value);
        auto& cache = IntCache();
        const CString prefix = CString(key) + _T("|");
        for (auto it = cache.begin(); it != cache.end();) {
            it = (it->first.Left(prefix.GetLength()) == prefix) ? cache.erase(it) : std::next(it);
        }
    }

    enum class Fmt { OnOff, Seconds, SecondsOrNever, Ms, Percent, Named, Tenths, Hundredths, Pixels, MsAsTime };

    // MPC-HC's own jump distance for a Jump* option (Options > Tweaks), nullptr for others
    int* JumpDistance(TvOpt opt)
    {
        auto& s = AfxGetAppSettings();
        switch (opt) {
            case TvOpt::JumpSmall: return &s.nJumpDistS;
            case TvOpt::JumpMedium: return &s.nJumpDistM;
            case TvOpt::JumpLarge: return &s.nJumpDistL;
            default: return nullptr;
        }
    }

    // MPC-HC's own player settings shown under TV options > Player: read into / written from v.
    // False for every other option.
    bool PlayerSetting(TvOpt opt, int* get, const int* set)
    {
        auto& s = AfxGetAppSettings();
        auto flag = [&](bool& b) {
            if (get) {
                *get = b ? 1 : 0;
            }
            if (set) {
                b = *set != 0;
            }
            return true;
        };
        switch (opt) {
            case TvOpt::LaunchFullscreen: return flag(s.fLaunchfullscreen);
            case TvOpt::RememberFilePos: return flag(s.fRememberFilePos);
            case TvOpt::ExitFullscreenAtEnd: return flag(s.fExitFullScreenAtTheEnd);
            case TvOpt::AfterPlayback:
                if (get) {
                    *get = static_cast<int>(s.eAfterPlayback);
                }
                if (set) {
                    s.eAfterPlayback = static_cast<CAppSettings::AfterPlayback>(*set);
                }
                return true;
            default:
                return false;
        }
    }

    struct OptionDef {
        LPCTSTR key;
        LPCWSTR label;
        Fmt fmt;
        int def;
        std::vector<int> choices;
    };

    // Accent colour presets (index = stored value)
    struct NamedColour {
        LPCWSTR name;
        COLORREF colour;
    };
    const NamedColour kAccents[] = {
        { L"Blue", RGB(52, 120, 246) },
        { L"Cyan", RGB(0, 164, 220) },
        { L"Teal", RGB(0, 150, 136) },
        { L"Green", RGB(46, 160, 67) },
        { L"Purple", RGB(126, 87, 194) },
        { L"Pink", RGB(216, 27, 96) },
        { L"Red", RGB(211, 47, 47) },
        { L"Orange", RGB(239, 108, 0) },
        { L"Amber", RGB(230, 160, 0) },
        { L"Grey", RGB(120, 130, 145) },
    };

    // Panel / bar background presets, all dark (index = stored value; only append). The last value,
    // CTvMode::kCustomBackground, is the colour picked in "Custom background colour".
    const NamedColour kBackgrounds[] = {
        { L"Charcoal", RGB(20, 22, 28) }, // the original look
        { L"Black", RGB(4, 4, 6) },
        { L"Graphite", RGB(32, 32, 34) },
        { L"Midnight blue", RGB(12, 18, 36) },
        { L"Slate", RGB(26, 32, 42) },
        { L"Forest", RGB(12, 26, 20) },
        { L"Aubergine", RGB(28, 16, 34) },
        { L"Espresso", RGB(30, 22, 18) },
    };
    static_assert(_countof(kBackgrounds) == CTvMode::kCustomBackground, "Custom comes right after the presets");

    // Fonts that ship with Windows 10 / 11 (index = stored value; only append). The Segoe UI
    // Variable fonts come with Windows 11 and are skipped where they aren't installed.
    const LPCWSTR kFonts[] = {
        L"Segoe UI", L"Segoe UI Light", L"Bahnschrift", L"Calibri", L"Candara", L"Corbel",
        L"Trebuchet MS", L"Verdana", L"Tahoma", L"Arial", L"Georgia",
        L"Segoe UI Variable Display", L"Segoe UI Variable Text",
    };

    int CALLBACK FontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found)
    {
        *reinterpret_cast<bool*>(found) = true;
        return 0; // one match is enough
    }

    bool IsFontInstalled(LPCWSTR face)
    {
        static std::map<std::wstring, bool> cache;
        auto it = cache.find(face);
        if (it != cache.end()) {
            return it->second;
        }
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
        bool found = false;
        HDC hdc = GetDC(nullptr);
        EnumFontFamiliesExW(hdc, &lf, FontFoundProc, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, hdc);
        cache[face] = found;
        return found;
    }

    // Subtitle style choices
    const LPCWSTR kSubFonts[] = {
        L"Arial", L"Calibri", L"Segoe UI", L"Segoe UI Semibold", L"Tahoma", L"Verdana", L"Trebuchet MS",
        L"Bahnschrift", L"Candara", L"Corbel", L"Georgia",
    };
    const NamedColour kSubColours[] = {
        { L"White", RGB(255, 255, 255) },
        { L"Light grey", RGB(210, 210, 210) },
        { L"Yellow", RGB(255, 255, 0) },
        { L"Pale yellow", RGB(255, 240, 150) },
        { L"Cyan", RGB(100, 230, 255) },
        { L"Green", RGB(120, 255, 120) },
    };
    const LPCWSTR kBorderNames[] = { L"Outline", L"Opaque box" };
    const LPCWSTR kRelativeNames[] = { L"Window", L"Video", L"Auto" };

    // stored values 0-3 as before; the new speeds were added after them (choices list = display order)
    const LPCWSTR kAnimNames[] = { L"Off", L"Fast", L"Normal", L"Slow", L"Faster", L"Fastest", L"Slower", L"Slowest" };
    const LPCWSTR kOverlayNames[] = { L"Inside madVR (best for HDR passthrough)", L"Own window (smoothest, any renderer)" };
    const LPCWSTR kTimeNames[] = { L"Duration", L"Remaining" };
    const LPCWSTR kHoldNames[] = { L"Off", L"Slowest", L"Slower", L"Normal", L"Faster", L"Fastest" };
    const LPCWSTR kSeekShowNames[] = { L"Control bar", L"Time only", L"Nothing" };
    const LPCWSTR kSeekModeNames[] = { L"After a short pause", L"Right away", L"After OK" }; // 2: the preview waits for OK (Back cancels)
    const LPCWSTR kHoldOkNames[] = { L"Off", L"Opens Info", L"Opens Settings" };
    const LPCWSTR kBackActionNames[] = { L"Opens the power menu", L"Exits fullscreen", L"Exits the player" };
    // in the order of MPC-HC's AfterPlayback values
    const LPCWSTR kAfterPlaybackNames[] = { L"Do nothing", L"Play the next file in the folder", L"Go back to the start",
                                            L"Turn the display off", L"Close the file", L"Exit the player" };

    CStringW NamedValue(TvOpt opt, int v)
    {
        auto pick = [v](const LPCWSTR* names, int n) { return CStringW(names[std::clamp(v, 0, n - 1)]); };
        switch (opt) {
            case TvOpt::AccentColour:
                return v == CTvMode::kCustomAccent ? CStringW(L"Custom") : CStringW(kAccents[std::clamp(v, 0, static_cast<int>(_countof(kAccents)) - 1)].name);
            case TvOpt::FontFace:
                return pick(kFonts, _countof(kFonts));
            case TvOpt::AnimSpeed:
                return pick(kAnimNames, _countof(kAnimNames));
            case TvOpt::OverlayMode:
                return pick(kOverlayNames, _countof(kOverlayNames));
            case TvOpt::TimeDisplay:
                return pick(kTimeNames, _countof(kTimeNames));
            case TvOpt::HoldSpeedup:
                return pick(kHoldNames, _countof(kHoldNames));
            case TvOpt::ShowOnSeek:
                return pick(kSeekShowNames, _countof(kSeekShowNames));
            case TvOpt::SeekMode:
                return pick(kSeekModeNames, _countof(kSeekModeNames));
            case TvOpt::HoldOk:
                return pick(kHoldOkNames, _countof(kHoldOkNames));
            case TvOpt::BackAction:
                return pick(kBackActionNames, _countof(kBackActionNames));
            case TvOpt::AfterPlayback:
                return pick(kAfterPlaybackNames, _countof(kAfterPlaybackNames));
            case TvOpt::BackgroundColour:
                return v == CTvMode::kCustomBackground ? CStringW(L"Custom")
                                                       : CStringW(kBackgrounds[std::clamp(v, 0, static_cast<int>(_countof(kBackgrounds)) - 1)].name);
            case TvOpt::SubFont:
                return v < 0 ? CStringW(AfxGetAppSettings().subtitlesDefStyle.fontName) : pick(kSubFonts, _countof(kSubFonts));
            case TvOpt::SubColour:
                return v < 0 ? CStringW(L"Custom") : CStringW(kSubColours[std::clamp(v, 0, static_cast<int>(_countof(kSubColours)) - 1)].name);
            case TvOpt::SubBorderStyle:
                return pick(kBorderNames, _countof(kBorderNames));
            case TvOpt::SubRelativeTo:
                return pick(kRelativeNames, _countof(kRelativeNames));
            default:
                return L"";
        }
    }

    LPCTSTR kCustomHueKey = _T("CustomHue");
    LPCTSTR kCustomSatKey = _T("CustomSat");
    LPCTSTR kCustomLightKey = _T("CustomLight");

    // The subtitle style options live in MPC-HC's own default subtitle style
    // (Options > Subtitles > Default Style). Alpha there: 0 = opaque, 255 = invisible.
    int AlphaToPercent(BYTE a)
    {
        const int pct = static_cast<int>((255 - a) * 100.0 / 255.0 + 0.5);
        return std::clamp((pct + 5) / 10 * 10, 0, 100);
    }
    BYTE PercentToAlpha(int pct)
    {
        return static_cast<BYTE>(255 - std::clamp(pct, 0, 100) * 255 / 100);
    }
    int Round(double v) { return static_cast<int>(v < 0 ? v - 0.5 : v + 0.5); }

    // Current value; -1 for a font / colour that isn't on the short lists.
    int SubStyleGet(TvOpt opt)
    {
        const STSStyle& st = AfxGetAppSettings().subtitlesDefStyle;
        switch (opt) {
            case TvOpt::SubOpacity:
                return std::max(10, AlphaToPercent(st.alpha[0]));
            case TvOpt::SubFont:
                for (int i = 0; i < static_cast<int>(_countof(kSubFonts)); i++) {
                    if (st.fontName.CompareNoCase(kSubFonts[i]) == 0) {
                        return i;
                    }
                }
                return -1;
            case TvOpt::SubSize:
                return Round(st.fontScaleY);
            case TvOpt::SubColour:
                for (int i = 0; i < static_cast<int>(_countof(kSubColours)); i++) {
                    if (st.colors[0] == kSubColours[i].colour) {
                        return i;
                    }
                }
                return -1;
            case TvOpt::SubBorderStyle:
                return st.borderStyle == 1 ? 1 : 0;
            case TvOpt::SubOutlineWidth:
                return Round(std::max(st.outlineWidthX, st.outlineWidthY) * 100);
            case TvOpt::SubOutlineOpacity:
                return AlphaToPercent(st.alpha[2]);
            case TvOpt::SubShadowDepth:
                return Round(std::max(st.shadowDepthX, st.shadowDepthY) * 10);
            case TvOpt::SubShadowOpacity:
                return AlphaToPercent(st.alpha[3]);
            case TvOpt::SubMargin:
                return st.marginRect.bottom;
            case TvOpt::SubRelativeTo:
                return std::clamp(static_cast<int>(st.relativeTo), 0, 2);
            default:
                return 0;
        }
    }

    // Writes one subtitle style option into `st`; false for options that aren't subtitle style.
    bool SetStyleField(STSStyle& st, TvOpt opt, int v)
    {
        switch (opt) {
            case TvOpt::SubOpacity:
                st.alpha[0] = st.alpha[1] = PercentToAlpha(v); // text and secondary (karaoke) colour
                break;
            case TvOpt::SubFont:
                st.fontName = kSubFonts[std::clamp(v, 0, static_cast<int>(_countof(kSubFonts)) - 1)];
                break;
            case TvOpt::SubSize:
                st.fontScaleX = st.fontScaleY = v;
                break;
            case TvOpt::SubColour:
                st.colors[0] = kSubColours[std::clamp(v, 0, static_cast<int>(_countof(kSubColours)) - 1)].colour;
                break;
            case TvOpt::SubBorderStyle:
                st.borderStyle = v;
                break;
            case TvOpt::SubOutlineWidth:
                st.outlineWidthX = st.outlineWidthY = v / 100.0;
                break;
            case TvOpt::SubOutlineOpacity:
                st.alpha[2] = PercentToAlpha(v);
                break;
            case TvOpt::SubShadowDepth:
                st.shadowDepthX = st.shadowDepthY = v / 10.0;
                break;
            case TvOpt::SubShadowOpacity:
                st.alpha[3] = PercentToAlpha(v);
                break;
            case TvOpt::SubMargin:
                st.marginRect.bottom = v;
                break;
            case TvOpt::SubRelativeTo:
                st.relativeTo = static_cast<STSStyle::RelativeTo>(std::clamp(v, 0, 2));
                break;
            default:
                return false;
        }
        return true;
    }

    void ApplySubtitleStyleNow()
    {
        // Apply now, exactly like Options > Subtitles > Default Style does.
        if (CMainFrame* pMainFrame = AfxGetMainFrame()) {
            pMainFrame->UpdateSubtitleRenderingParameters();
            pMainFrame->RepaintVideo();
        }
    }

    // Re-applying the style re-initialises MPC-HC's subtitle renderer, which is too heavy to
    // do on every step of a held Left/Right. The sample subtitle updates at once; the real
    // subtitles follow 0.4 s after the last change.
    UINT_PTR g_subApplyTimer = 0;
    void CALLBACK OnSubApplyTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (id == g_subApplyTimer) {
            g_subApplyTimer = 0;
        }
        ApplySubtitleStyleNow();
    }
    void ApplySubtitleStyleSoon()
    {
        if (g_subApplyTimer) {
            KillTimer(nullptr, g_subApplyTimer);
        }
        g_subApplyTimer = SetTimer(nullptr, 0, 400, OnSubApplyTimer);
        if (!g_subApplyTimer) {
            ApplySubtitleStyleNow();
        }
    }

    void SubStyleSet(TvOpt opt, int v)
    {
        if (SetStyleField(AfxGetAppSettings().subtitlesDefStyle, opt, v)) {
            ApplySubtitleStyleSoon();
        }
    }

    std::vector<int> Range(int first, int last, int step)
    {
        std::vector<int> v;
        for (int i = first; i <= last; i += step) {
            v.push_back(i);
        }
        return v;
    }

    const OptionDef& Def(TvOpt opt)
    {
        static const OptionDef defs[] = {
            { _T("ShowControlsOnStart"), L"Show controls when playback starts", Fmt::OnOff, 1, { 0, 1 } },
            { _T("ShowControlsOnPause"), L"Show controls when paused", Fmt::OnOff, 1, { 0, 1 } },
            // Was On/Off ("ShowControlsOnSeek"); now three choices under a new key.
            { _T("SeekDisplay"), L"When seeking, show", Fmt::Named, 0, Range(0, _countof(kSeekShowNames) - 1, 1) },
            { _T("ShowControlsOnMouse"), L"Show controls on mouse move", Fmt::OnOff, 1, { 0, 1 } },
            { _T("StatsButton"), L"madVR stats button (Ctrl+J)", Fmt::OnOff, 0, { 0, 1 } },
            { _T("VideoInfoButton"), L"Video info button", Fmt::OnOff, 1, { 0, 1 } },
            { _T("AudioInfoButton"), L"Audio info button", Fmt::OnOff, 1, { 0, 1 } },
            { _T("BarHideSec"), L"Hide controls after", Fmt::Seconds, 5, { 3, 5, 8, 10, 15, 20 } },
            // Default Never; new key so an earlier pick starts over at Never.
            { _T("MenuHide"), L"Close menus after", Fmt::SecondsOrNever, 0, { 5, 10, 15, 30, 60, 0 } },
            { _T("SkipBackSec"), L"Skip back button", Fmt::Seconds, 10, { 5, 10, 15, 20, 30, 60 } },
            { _T("SkipFwdSec"), L"Skip forward button", Fmt::Seconds, 30, { 10, 15, 20, 30, 60, 120 } },
            { _T("SeekStepSec"), L"Seek step (Left/Right)", Fmt::Seconds, 10, { 5, 10, 15, 30, 60 } },
            // new key: the old "HoldSpeedup" was On/Off and would be misread on the new scale
            { _T("HoldSpeed"), L"Seeking speed-up when holding", Fmt::Named, 3, Range(0, _countof(kHoldNames) - 1, 1) },
            { _T("SubDelayStepMs"), L"Subtitle delay step", Fmt::Ms, 10, Range(5, 100, 5) },
            { _T("AudioDelayStepMs"), L"Audio delay step", Fmt::Ms, 10, Range(5, 100, 5) },
            // Sizes: 100 % = the chosen default look; stored under new names so values
            // from the earlier scale (MenuTextPct / BarSizePct) aren't misread.
            { _T("MenuSize"), L"Menu size", Fmt::Percent, 100, Range(50, 200, 5) },
            { _T("BarSize"), L"Control bar size", Fmt::Percent, 100, Range(50, 200, 5) },
            // Not stored in the TvMode section: shows / changes MPC-HC's own subtitle setting.
            // 60 % on purpose: full white is too bright on projectors and HDR displays. Keep it.
            { nullptr, L"Text opacity", Fmt::Percent, 60, Range(10, 100, 10) },
            { _T("AccentColour"), L"Accent colour", Fmt::Named, 1, Range(0, CTvMode::kCustomAccent, 1) }, // default Cyan; last = Custom
            // New key: 100 % was re-based twice to the chosen default, so older values would be misread.
            { _T("AccentTone"), L"Accent brightness", Fmt::Percent, 100, Range(50, 150, 10) },
            { _T("Font"), L"Font", Fmt::Named, 1, Range(0, _countof(kFonts) - 1, 1) }, // default Segoe UI Light
            { _T("BackgroundOpacity"), L"Menu background opacity", Fmt::Percent, 90, Range(50, 100, 5) },
            // Off, Fastest, Faster, Fast, Normal, Slow, Slower, Slowest (stored values: see kAnimNames)
            { _T("AnimSpeed"), L"Animations", Fmt::Named, 2, { 0, 5, 4, 1, 2, 3, 6, 7 } },
            { _T("KeyHints"), L"Key hints", Fmt::OnOff, 1, { 0, 1 } },
            { _T("TimeDisplay"), L"Time right of the seek line", Fmt::Named, 0, Range(0, _countof(kTimeNames) - 1, 1) },
            { _T("ShowClock"), L"Clock on the bar", Fmt::OnOff, 0, { 0, 1 } },
            { _T("ShowEndsAt"), L"\"Ends at\" on the bar", Fmt::OnOff, 1, { 0, 1 } },
            { _T("SeekMode"), L"Jump when seeking", Fmt::Named, 0, Range(0, _countof(kSeekModeNames) - 1, 1) },
            { _T("ButtonLabels"), L"Button name under the selected button", Fmt::OnOff, 1, { 0, 1 } },
            // Default: own window (smoothest; same look unless HDR passthrough). Inside madVR is the
            // choice for HDR passthrough / exclusive mode; without madVR it's always the window.
            { _T("OverlayMode"), L"Draw the TV overlay", Fmt::Named, 1, { 0, 1 } },
            // Smooth motion that redraws the bar every frame while it moves: can be turned off on slower PCs
            { _T("SmoothPlayhead"), L"Smooth playhead", Fmt::OnOff, 1, { 0, 1 } },
            { _T("SmoothScrub"), L"Smooth seeking when holding", Fmt::OnOff, 1, { 0, 1 } },
            // the delay; on / off is PauseScreenOn (an earlier "Never" = 0 still counts as off)
            { _T("PauseScreen"), L"Pause screen after", Fmt::Seconds, 5, { 3, 5, 10, 20, 30 } },
            { _T("HoldOk"), L"Hold OK on the bar", Fmt::Named, 1, Range(0, _countof(kHoldOkNames) - 1, 1) },
            { _T("ChapterSnap"), L"Seeking stops at chapters", Fmt::OnOff, 1, { 0, 1 } },
            { _T("PauseScreenOn"), L"Pause screen", Fmt::OnOff, 1, { 0, 1 } },
            { _T("WrapAround"), L"Wrap around at the ends", Fmt::OnOff, 1, { 0, 1 } },
            { _T("WrapWhileHeld"), L"Keep going round while held", Fmt::OnOff, 0, { 0, 1 } },
            // Control bar's own background opacity (90 % = the bar's original look; it used to follow the panels')
            { _T("BarOpacity"), L"Control bar opacity", Fmt::Percent, 90, Range(50, 100, 5) },
            { _T("BackgroundColour"), L"Background colour", Fmt::Named, 0, Range(0, CTvMode::kCustomBackground, 1) },
            { _T("BarIconBrightness"), L"Control bar icon brightness", Fmt::Percent, 100, Range(40, 100, 5) },
            { _T("BarIconOpacity"), L"Control bar icon opacity", Fmt::Percent, 100, Range(30, 100, 5) },
            { _T("BackAction"), L"Back with nothing open", Fmt::Named, 0, Range(0, _countof(kBackActionNames) - 1, 1) },
            // off by default; the installer's recommended TV settings turn it on
            { _T("ResumeLast"), L"Resume the last file on start", Fmt::OnOff, 0, { 0, 1 } },
            // MPC-HC's jump distances (no key: Options > Tweaks values, live); defaults = MPC-HC's
            { nullptr, L"Small jump", Fmt::MsAsTime, DEFAULT_JUMPDISTANCE_1, { 500, 1000, 2000, 3000, 5000, 10000 } },
            { nullptr, L"Medium jump", Fmt::MsAsTime, DEFAULT_JUMPDISTANCE_2, { 5000, 10000, 15000, 20000, 30000, 60000 } },
            { nullptr, L"Big jump", Fmt::MsAsTime, DEFAULT_JUMPDISTANCE_3,
              { 20000, 30000, 60000, 120000, 180000, 300000, 600000 } },
            // MPC-HC's default subtitle style (no key: read and written live, see SubStyleGet / SubStyleSet).
            // Defaults = this build's TV look; used for "(default)", "Reset subtitle style" and a fresh setup.
            { nullptr, L"Font", Fmt::Named, 3 /* Segoe UI Semibold */, Range(0, _countof(kSubFonts) - 1, 1) },
            { nullptr, L"Size", Fmt::Percent, 80, Range(50, 250, 10) }, // TV default style: scale 80 %
            { nullptr, L"Text colour", Fmt::Named, 0, Range(0, _countof(kSubColours) - 1, 1) },
            { nullptr, L"Border style", Fmt::Named, 0, { 0, 1 } },
            // Outline width in hundredths: default 0.75, steps of 0.10
            { nullptr, L"Outline width", Fmt::Hundredths, 75, [] {
                  std::vector<int> v;
                  for (int i = 0; i <= 300; i += 10) {
                      v.push_back(i);
                      if (i == 70) {
                          v.push_back(75); // the default sits between 0.70 and 0.80
                      }
                  }
                  for (int i : { 400, 500, 600, 800 }) {
                      v.push_back(i);
                  }
                  return v;
              }() },
            { nullptr, L"Outline / box opacity", Fmt::Percent, 40, Range(0, 100, 10) },
            { nullptr, L"Shadow", Fmt::Tenths, 0, { 0, 5, 10, 15, 20, 25, 30, 40, 50, 60, 80 } },
            { nullptr, L"Shadow opacity", Fmt::Percent, 30, Range(0, 100, 10) },
            // MPC-HC's own default: 20 (in the subtitle's own units, not screen pixels)
            { nullptr, L"Distance from bottom", Fmt::Pixels, 20, Range(0, 150, 5) },
            { nullptr, L"Position relative to", Fmt::Named, 2 /* Auto */, { 0, 1, 2 } },
            // MPC-HC's own player settings (no key: read and written live, see PlayerSetting); defaults = MPC-HC's
            { nullptr, L"Open files in fullscreen", Fmt::OnOff, 0, { 0, 1 } },
            { nullptr, L"Remember file position", Fmt::OnOff, 0, { 0, 1 } },
            { nullptr, L"Leave fullscreen at the end", Fmt::OnOff, 1, { 0, 1 } },
            { nullptr, L"After playback", Fmt::Named, 0, Range(0, _countof(kAfterPlaybackNames) - 1, 1) },
        };
        static_assert(_countof(defs) == static_cast<size_t>(TvOpt::Count), "one definition per TvOpt");
        return defs[static_cast<size_t>(opt)];
    }

}

void CTvMode::SaveLayout(CMainFrame* pMainFrame, bool tvLayout)
{
    const LayoutKeys& keys = tvLayout ? kTvLayout : kDesktopLayout;
    const auto& s = AfxGetAppSettings();
    SetInt(keys.toolbars, static_cast<int>(s.nCS));
    SetInt(keys.caption, static_cast<int>(s.eCaptionMenuMode));
    SetInt(keys.playlist, pMainFrame->m_controls.ControlChecked(CMainFrameControls::Panel::PLAYLIST) ? 1 : 0);
    SetInt(keys.saved, 1);
}

// Applies a remembered layout. When none was saved yet: TV = the TV preset (caption and menu,
// no toolbars), Desktop = like View > Presets > Normal.
void CTvMode::ApplyLayout(CMainFrame* pMainFrame, bool tvLayout)
{
    const LayoutKeys& keys = tvLayout ? kTvLayout : kDesktopLayout;
    const UINT fallbackCS = tvLayout ? CS_NONE : pMainFrame->GetNormalPresetCS();
    const int fallbackCaption = MODE_SHOWCAPTIONMENU;

    auto& controls = pMainFrame->m_controls;
    const bool saved = GetInt(keys.saved, 0) != 0;
    const int caption = saved ? GetInt(keys.caption, fallbackCaption) : fallbackCaption;
    const UINT cs = saved ? static_cast<UINT>(GetInt(keys.toolbars, static_cast<int>(fallbackCS))) : fallbackCS;
    const bool playlist = saved && GetInt(keys.playlist, 0) != 0;

    pMainFrame->SetCaptionState(static_cast<MpcCaptionState>(std::clamp(caption, 0, MpcCaptionState::MODE_COUNT - 1)));
    controls.SetToolbarsSelection(cs, true);
    if (playlist != controls.ControlChecked(CMainFrameControls::Panel::PLAYLIST)) {
        controls.ToggleControl(CMainFrameControls::Panel::PLAYLIST);
    }
}

bool CTvMode::IsOn()
{
    return AfxGetAppSettings().bTvMode;
}

// View > Presets > TV: TV Mode with caption and menu, no toolbars, no playlist
void CTvMode::SetTvPresetLayout(bool minimal)
{
    SetInt(kTvLayout.saved, 1);
    SetInt(kTvLayout.caption, minimal ? MODE_BORDERLESS : MODE_SHOWCAPTIONMENU); // TV Minimal: as Minimal
    SetInt(kTvLayout.toolbars, static_cast<int>(CS_NONE));
    SetInt(kTvLayout.playlist, 0);
}

// "Recommended TV settings": only when the installer's "Apply recommended TV settings" box was
// ticked (it writes TvMode\ApplyRecommended = 1; the request is removed once applied). These
// include remembering file / DVD positions. Existing settings are never changed otherwise (the box is
// ticked by default; where settings exist its label says they will change).
// Two parts: the player settings are set while the settings load (before the theme is chosen, so the
// dark theme and the logo show on that very start), the window layout once the main window exists.
bool CTvMode::RecommendedPending()
{
    return GetInt(_T("ApplyRecommended"), 0) == 1;
}

void CTvMode::ApplyRecommendedSettings(CAppSettings& s)
{
    // Options > Player > General > History
    s.fKeepHistory = true;
    s.bRememberPlaylistItems = true;
    s.fRememberFilePos = true;
    s.fRememberDVDPos = true;
    // Options > Playback > Fullscreen (stay fullscreen at the end; the control window
    // and hiding options keep MPC-HC's own defaults)
    s.fLaunchfullscreen = true;
    s.fExitFullScreenAtTheEnd = false;
    // Options > Player > User Interface: dark modern theme, MPC-HC's own OSD off (TV Mode draws its own)
    s.bMPCTheme = true;
    s.eModernThemeMode = CMPCTheme::ModernThemeMode::DARK;
    s.fShowOSD = false;
    // Options > Player > Logo: this build's TV logo
    s.fLogoExternal = false;
    s.strLogoFileName.Empty();
    s.nLogoId = IDF_LOGO_TV;
    // Options > Subtitles > Default Style: the TV look
    SubtitleStyleDefaults(s.subtitlesDefStyle);
}

void CTvMode::ApplyRecommended()
{
    auto& s = AfxGetAppSettings();
    s.SaveSettings(); // what ApplyRecommendedSettings set while loading
    // TV Mode on, with the TV preset layout (caption and menu)
    SetTvPresetLayout();
    SetInt(_T("ResumeLast"), 1); // started on its own: carry on with an unfinished file
    s.bTvMode = true;
    SetInt(kEnabled, 0); // started from Desktop: its layout is saved, then the TV preset applied
}

bool CTvMode::OpenLastFile(CMainFrame* pMainFrame)
{
    auto& s = AfxGetAppSettings();
    s.MRU.ReadMediaHistory();
    const bool haveRecent = s.fKeepHistory && s.MRU.GetSize() > 0 && !s.MRU[0].fns.IsEmpty() && !s.MRU[0].fns.GetHead().IsEmpty();
    pMainFrame->PostMessage(WM_COMMAND, haveRecent ? ID_RECENT_FILE_START : ID_FILE_OPENQUICK);
    return haveRecent;
}

// Started with nothing to play: no file, disc, device or host program (a media server's player, a
// file opened from Explorer and /slave all bring their own), and no other player window already open.
bool CTvMode::StartsWithoutMedia(const CAppSettings& s)
{
    if (!s.slFiles.IsEmpty() || (s.nCLSwitches & (CLSW_DVD | CLSW_CD | CLSW_DEVICE | CLSW_DVBSCAN | CLSW_SLAVE | CLSW_ADD |
                                                   CLSW_CLOSE | CLSW_THUMBNAILS))) {
        return false;
    }
    int windows = 0;
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, MPC_WND_CLASS_NAME, nullptr)) != nullptr;) {
        windows++; // this player's own window is one of them
    }
    return windows <= 1;
}

// "Resume the last file on start": only when the last file was stopped part-way, i.e. MPC-HC kept a
// position for it (it forgets it when a file plays to the end, and keeps none with "remember file
// position" off). Opening it then carries on from there, as File > Recent files does.
void CTvMode::ResumeAtStart(CMainFrame* pMainFrame)
{
    auto& s = AfxGetAppSettings();
    if (!pMainFrame || !s.bTvMode || !IsOn(TvOpt::ResumeLast) || !s.fKeepHistory || !s.fRememberFilePos) {
        return;
    }
    s.MRU.ReadMediaHistory();
    if (s.MRU.GetSize() > 0 && !s.MRU[0].fns.IsEmpty() && !s.MRU[0].fns.GetHead().IsEmpty() && s.MRU[0].filePosition > 0) {
        pMainFrame->PostMessage(WM_COMMAND, ID_RECENT_FILE_START);
    }
}

void CTvMode::OnMainFrameCreated(CMainFrame* pMainFrame)
{
    auto& s = AfxGetAppSettings();
    if (RecommendedPending()) {
        ApplyRecommended();
        SetInt(_T("ApplyRecommended"), 0); // done
    }
    const bool lastRunWasTv = GetInt(kEnabled, 0) != 0;
    if (s.bTvMode || lastRunWasTv) {
        s.bTvMode = true; // keys work right away
        // Apply TV Mode once the window is fully set up. If the last run ended in TV Mode,
        // MPC-HC has just loaded the TV layout it saved on exit, so only the mode is set;
        // otherwise (a /tv start from Desktop Mode) the desktop layout is saved first.
        pMainFrame->PostMessage(WM_COMMAND, lastRunWasTv ? ID_TV_MODE_APPLY : ID_TV_MODE_APPLY_SAVE);
    }
}

void CTvMode::OnCommand(CMainFrame* pMainFrame, UINT nID)
{
    switch (nID) {
        case ID_TV_MODE:
            if (IsOn()) {
                Leave(pMainFrame);
            } else {
                Enter(pMainFrame, true);
            }
            break;
        case ID_TV_MODE_APPLY:
            Enter(pMainFrame, false);
            break;
        case ID_TV_MODE_APPLY_SAVE:
            Enter(pMainFrame, true);
            break;
        case ID_VIEW_PRESETS_TV:
        case ID_VIEW_PRESETS_TV_MINIMAL: {
            // View > Presets > TV (key 5): TV Mode with caption and menu; TV Minimal (key 6): borderless,
            // like Minimal. The desktop layout is kept for later.
            auto& s = AfxGetAppSettings();
            if (!IsOn()) {
                SaveLayout(pMainFrame, false);
            }
            s.bTvMode = true;
            SetInt(kEnabled, 1);
            SetTvPresetLayout(nID == ID_VIEW_PRESETS_TV_MINIMAL);
            ApplyLayout(pMainFrame, true);
            break;
        }
    }
}

void CTvMode::OnUpdateCommand(CCmdUI* pCmdUI)
{
    pCmdUI->SetCheck(IsOn());
    CMPCThemeMenu::updateItem(pCmdUI);
}

void CTvMode::Enter(CMainFrame* pMainFrame, bool fromDesktop)
{
    auto& s = AfxGetAppSettings();
    s.bTvMode = true;
    SetInt(kEnabled, 1);
    if (fromDesktop) {
        SaveLayout(pMainFrame, false);
        ApplyLayout(pMainFrame, true); // first time: like Minimal
    }
}

void CTvMode::Leave(CMainFrame* pMainFrame)
{
    auto& s = AfxGetAppSettings();
    CTvMenu::Instance().ForceClose();
    SaveLayout(pMainFrame, true); // View changes made in TV Mode stay with TV Mode
    s.bTvMode = false;
    SetInt(kEnabled, 0);
    ApplyLayout(pMainFrame, false);
}

int CTvMode::Get(TvOpt opt)
{
    if (IsSubtitleStyleOpt(opt)) {
        return SubStyleGet(opt); // may be off the choice list (set in MPC-HC's own dialog)
    }
    if (int* jump = JumpDistance(opt)) {
        return *jump; // MPC-HC's own value (ms), may be off the choice list too
    }
    if (int v = 0; PlayerSetting(opt, &v, nullptr)) {
        return v; // MPC-HC's own setting
    }
    const OptionDef& d = Def(opt);
    const int v = GetInt(d.key, d.def);
    // Only values from the list are valid; anything else (hand-edited ini) means default.
    return std::find(d.choices.begin(), d.choices.end(), v) != d.choices.end() ? v : d.def;
}

void CTvMode::Step(TvOpt opt, int dir)
{
    const OptionDef& d = Def(opt);
    const int n = static_cast<int>(d.choices.size());
    const int cur = Get(opt);
    auto it = std::find(d.choices.begin(), d.choices.end(), cur);
    int pos;
    if (it != d.choices.end()) {
        pos = (static_cast<int>(it - d.choices.begin()) + dir + n) % n;
    } else if (IsMpcSettingOpt(opt) && cur >= 0) {
        // A value set elsewhere (MPC-HC's dialog): go to the nearest choice in that direction
        pos = dir > 0 ? 0 : n - 1;
        for (int i = 0; i < n; i++) {
            if (dir > 0 && d.choices[i] > cur) { pos = i; break; }
            if (dir < 0 && d.choices[i] < cur) { pos = i; }
        }
    } else {
        pos = dir > 0 ? 0 : n - 1;
    }
    if (opt == TvOpt::FontFace || opt == TvOpt::SubFont) {
        // skip fonts this PC doesn't have (Segoe UI Variable on Windows 10)
        const LPCWSTR* fonts = opt == TvOpt::FontFace ? kFonts : kSubFonts;
        for (int tries = 0; tries < n && !IsFontInstalled(fonts[d.choices[pos]]); tries++) {
            pos = (pos + (dir >= 0 ? 1 : -1) + n) % n;
        }
    }
    if (IsSubtitleStyleOpt(opt)) {
        SubStyleSet(opt, d.choices[pos]);
        return;
    }
    if (int* jump = JumpDistance(opt)) {
        *jump = d.choices[pos]; // used by MPC-HC's next jump; saved with its settings
        return;
    }
    if (PlayerSetting(opt, nullptr, &d.choices[pos])) {
        return; // MPC-HC's own setting: used from the next file on, saved with its settings
    }
    SetInt(d.key, d.choices[pos]);
}

COLORREF CTvMode::Hsl(int hue, int sat, int light)
{
    // HSL (degrees, percent, percent) -> RGB
    const float h = ((hue % 360) + 360) % 360 / 60.0f;
    const float s = std::clamp(sat, 0, 100) / 100.0f;
    const float l = std::clamp(light, 0, 100) / 100.0f;
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    const float m = l - c / 2.0f;
    auto to8 = [m](float v) { return static_cast<BYTE>(std::clamp((v + m) * 255.0f + 0.5f, 0.0f, 255.0f)); };
    return RGB(to8(r), to8(g), to8(b));
}

int CTvMode::CustomHue() { return std::clamp(GetInt(kCustomHueKey, 217), 0, 359); }  // defaults ~ the Blue preset
int CTvMode::CustomSat() { return std::clamp(GetInt(kCustomSatKey, 91), 0, 100); }
int CTvMode::CustomLight() { return std::clamp(GetInt(kCustomLightKey, 58), 0, 100); }

void CTvMode::SetCustom(int hue, int sat, int light)
{
    SetInt(kCustomHueKey, ((hue % 360) + 360) % 360);
    SetInt(kCustomSatKey, std::clamp(sat, 0, 100));
    SetInt(kCustomLightKey, std::clamp(light, 0, 100));
    SetInt(Def(TvOpt::AccentColour).key, kCustomAccent);
}

// Custom background colour (Hue / Saturation / Lightness, like the custom accent); default a dark blue-grey
int CTvMode::BgHue() { return std::clamp(GetInt(_T("BgHue"), 222), 0, 359); }
int CTvMode::BgSat() { return std::clamp(GetInt(_T("BgSat"), 17), 0, 100); }
int CTvMode::BgLight() { return std::clamp(GetInt(_T("BgLight"), 10), 0, 100); }

void CTvMode::SetCustomBackground(int hue, int sat, int light)
{
    SetInt(_T("BgHue"), ((hue % 360) + 360) % 360);
    SetInt(_T("BgSat"), std::clamp(sat, 0, 100));
    SetInt(_T("BgLight"), std::clamp(light, 0, 100));
    SetInt(Def(TvOpt::BackgroundColour).key, kCustomBackground);
}

COLORREF CTvMode::Background()
{
    const int choice = Get(TvOpt::BackgroundColour);
    return choice == kCustomBackground ? Hsl(BgHue(), BgSat(), BgLight())
                                       : kBackgrounds[std::clamp(choice, 0, static_cast<int>(_countof(kBackgrounds)) - 1)].colour;
}

COLORREF CTvMode::Accent()
{
    const int choice = Get(TvOpt::AccentColour);
    COLORREF c = (choice == kCustomAccent) ? Hsl(CustomHue(), CustomSat(), CustomLight())
                                           : kAccents[std::clamp(choice, 0, static_cast<int>(_countof(kAccents)) - 1)].colour;
    // Brightness: for the named colours 100 % = 70 % of the colour's full strength (the chosen
    // default depth); each 10 % above adds 0.1, so 130 % = the pure colour and 140-150 % mix a
    // little towards white. A custom colour is used as picked at 100 % (its lightness is in the picker).
    // b below 1 mixes towards black, above towards white.
    const float base = (choice == kCustomAccent) ? 1.0f : 0.7f;
    const float pct = Get(TvOpt::AccentBrightness) / 100.0f;
    const float b = pct <= 1.0f ? pct * base : base + (pct - 1.0f);
    auto adjust = [b](int v) {
        const float out = b <= 1.0f ? v * b : v + (255 - v) * (b - 1.0f);
        return static_cast<BYTE>(std::clamp(out + 0.5f, 0.0f, 255.0f));
    };
    return RGB(adjust(GetRValue(c)), adjust(GetGValue(c)), adjust(GetBValue(c)));
}

float CTvMode::AnimScale()
{
    switch (Get(TvOpt::AnimSpeed)) {
        case 0: return 0.0f;  // off
        case 5: return 0.25f; // fastest
        case 4: return 0.35f; // faster
        case 1: return 0.5f;  // fast
        case 3: return 2.0f;  // slow
        case 6: return 3.0f;  // slower
        case 7: return 4.0f;  // slowest
        default: return 1.0f; // normal
    }
}

LPCWSTR CTvMode::Font()
{
    const LPCWSTR face = kFonts[std::clamp(Get(TvOpt::FontFace), 0, static_cast<int>(_countof(kFonts)) - 1)];
    return IsFontInstalled(face) ? face : L"Segoe UI"; // e.g. a Windows 11 font chosen, then used on Windows 10
}

float CTvMode::HoldSpeed()
{
    static const float speeds[] = { 0.0f, 0.5f, 0.7f, 1.0f, 1.5f, 2.0f }; // Off, Slowest .. Fastest
    return speeds[std::clamp(Get(TvOpt::HoldSpeedup), 0, static_cast<int>(_countof(speeds)) - 1)];
}

CStringW CTvMode::Label(TvOpt opt)
{
    return Def(opt).label;
}

bool CTvMode::IsOnOff(TvOpt opt)
{
    return Def(opt).fmt == Fmt::OnOff;
}

CStringW CTvMode::ValueText(TvOpt opt)
{
    const OptionDef& d = Def(opt);
    const int v = Get(opt);
    CStringW s;
    switch (d.fmt) {
        case Fmt::OnOff:
            s = v ? L"On" : L"Off";
            break;
        case Fmt::Seconds:
            s.Format(L"%d s", v);
            break;
        case Fmt::SecondsOrNever:
            if (v == 0) {
                s = L"Never";
            } else {
                s.Format(L"%d s", v);
            }
            break;
        case Fmt::Ms:
            s.Format(L"%d ms", v);
            break;
        case Fmt::Percent:
            s.Format(L"%d %%", v);
            break;
        case Fmt::Named:
            s = NamedValue(opt, v);
            break;
        case Fmt::Tenths:
            s.Format(L"%d.%d", v / 10, std::abs(v % 10));
            break;
        case Fmt::Hundredths: // 0.75, 1.2, 3
            if (v % 100 == 0) {
                s.Format(L"%d", v / 100);
            } else if (v % 10 == 0) {
                s.Format(L"%d.%d", v / 100, std::abs(v / 10 % 10));
            } else {
                s.Format(L"%d.%02d", v / 100, std::abs(v % 100));
            }
            break;
        case Fmt::Pixels:
            s.Format(L"%d", v);
            break;
        case Fmt::MsAsTime:
            if (v % 1000 != 0) {
                s.Format(L"%.1f s", v / 1000.0);
            } else if (v >= 60000 && v % 60000 == 0) {
                s.Format(L"%d min", v / 60000);
            } else {
                s.Format(L"%d s", v / 1000);
            }
            break;
    }
    if (v == d.def) {
        s += L" (default)";
    }
    return s;
}

int CTvMode::GetValue(LPCTSTR key, int def)
{
    return GetInt(key, def);
}

void CTvMode::SetValue(LPCTSTR key, int value)
{
    SetInt(key, value);
}

CString CTvMode::GetText(LPCTSTR key)
{
    auto& cache = TextCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    const CString v = AfxGetApp()->GetProfileString(kSection, key, _T(""));
    cache[key] = v;
    return v;
}

void CTvMode::SetText(LPCTSTR key, LPCTSTR value)
{
    TextCache()[key] = value;
    AfxGetApp()->WriteProfileString(kSection, key, value);
}

void CTvMode::SubtitleStyleDefaults(STSStyle& style)
{
    for (int i = 0; i < static_cast<int>(TvOpt::Count); i++) {
        const TvOpt opt = static_cast<TvOpt>(i);
        if (IsSubtitleStyleOpt(opt)) {
            SetStyleField(style, opt, Def(opt).def);
        }
    }
}

void CTvMode::ResetSubtitleStyle()
{
    SubtitleStyleDefaults(AfxGetAppSettings().subtitlesDefStyle);
    ApplySubtitleStyleNow();
}

void CTvMode::ResetOptions()
{
    for (int i = 0; i < static_cast<int>(TvOpt::Count); i++) {
        const OptionDef& d = Def(static_cast<TvOpt>(i));
        if (d.key) { // not MPC-HC's own subtitle style
            SetInt(d.key, d.def);
        }
    }
    SetText(_T("BarButtons"), _T("")); // bar buttons: default order and choice
    SetText(_T("BarSeparatorsSet"), _T("")); // the default separators between the button groups again
    SetText(_T("MadVrButtonSet"), _T(""));   // the madVR button follows the renderer again
}
