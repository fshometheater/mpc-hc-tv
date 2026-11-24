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

#include <vector>

class CMainFrame;

// One audio or subtitle track as shown in the TV lists.
struct TvTrack {
    int index = -1;         // index for CMainFrame::SetAudioTrackIdx / SetSubtitleTrackIdx
    CStringW title;         // e.g. "Japanese - TrueHD 7.1 - Default"
    CStringW detail;        // raw stream name from the splitter/file, shown dimmed (may be empty)
    bool isCurrent = false;
};

// Read-only view of the player's audio and subtitle tracks, built the same way as
// CMainFrame::SetupAudioSubMenu / SetupSubtitlesSubMenu so the indexes match.
// Selection goes through the player's own setters, so its automatic track
// selection rules (preferred languages) are untouched.
class CTvTracks
{
public:
    // Returns false when audio tracks can't be listed (e.g. DVD, nothing loaded).
    static bool GetAudio(CMainFrame* pMainFrame, std::vector<TvTrack>& tracks);
    static void SelectAudio(CMainFrame* pMainFrame, int index);

    // Returns false when the player's internal subtitle renderer isn't in use
    // (no allocator presenter), because only then can it switch subtitles.
    // subtitlesOn is false when subtitles are turned off (the "Off" row is current).
    static bool GetSubtitles(CMainFrame* pMainFrame, std::vector<TvTrack>& tracks, bool& subtitlesOn);
    static void SelectSubtitle(CMainFrame* pMainFrame, int index); // -1 = off

    // Subtitle delay in ms; false when the renderer doesn't support it.
    static bool GetSubtitleDelay(CMainFrame* pMainFrame, int& delayMs);
    static void ChangeSubtitleDelay(CMainFrame* pMainFrame, int deltaMs);

    // Audio delay in ms (MPC-HC's audio switcher, same as its own audio delay keys).
    static bool GetAudioDelay(CMainFrame* pMainFrame, int& delayMs);
    static void ChangeAudioDelay(CMainFrame* pMainFrame, int deltaMs);

    // Short text for the main list, e.g. "Japanese - TrueHD 7.1" or "Off".
    static CStringW CurrentAudioSummary(CMainFrame* pMainFrame);
    static CStringW CurrentSubtitleSummary(CMainFrame* pMainFrame);
};
