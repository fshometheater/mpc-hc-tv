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

// One line of an info panel: a section header, or a "name: value" pair.
struct TvInfoLine {
    CStringW name;
    CStringW value;
    bool header = false;
};

// A key fact shown as a "chip" at the top of an info panel: small label, big value.
struct TvInfoChip {
    CStringW label;
    CStringW value;
};

// A highlight pill under the info panel's title ("4K", "HDR10", "Atmos", "HEVC"...).
struct TvInfoBadge {
    CStringW text;
    int tone = 0; // 0 plain, 1 accent colour, 2 gold (HDR / object audio / lossless)
};

// Collects what the TV info panels show:
// - the file (MediaInfo, run once per file in the background and cached),
// - the display output (madVR's IMadVRInfo, Windows DXGI output description, the TV's name),
// - the decoders (LAV Video / LAV Audio status).
//
// Levels: 0 = Basic, 1 = Extended, 2 = Nightmare (everything MediaInfo knows about the
// stream, in its "complete" view, plus the display and playback details).
class CTvInfo
{
public:
    enum class Kind { Video, Audio };
    static constexpr int kLevels = 3;
    static LPCWSTR LevelName(int level);

    // Starts reading the current file in the background if that hasn't happened yet.
    static void Prepare(CMainFrame* pMainFrame);
    // False while MediaInfo is still reading (the panel shows a "reading" line meanwhile).
    static bool IsReady(CMainFrame* pMainFrame);

    // Key facts for the chip rows (every level): row 1 = the stream, row 2 = output / decoding.
    static void BuildChips(CMainFrame* pMainFrame, Kind kind, std::vector<TvInfoChip>& row1, std::vector<TvInfoChip>& row2);
    // Highlights for the badge line (also in every level)
    static void BuildBadges(CMainFrame* pMainFrame, Kind kind, std::vector<TvInfoBadge>& badges);
    // "File name . 2 h 12 min" for under the title
    static CStringW FileLine(CMainFrame* pMainFrame);
    // Detail lines below the chips: none for Basic, sections for Extended, everything for Nightmare.
    static void Build(CMainFrame* pMainFrame, Kind kind, int level, std::vector<TvInfoLine>& lines);
    // The file's title tag, else its name (no folders / extension); for the pause screen
    static CStringW MediaTitle(CMainFrame* pMainFrame);
    // The video's HDR metadata as MediaInfo reports it ("" where not known yet / not present)
    static void HdrMetadata(CMainFrame* pMainFrame, CStringW& format, CStringW& maxCll, CStringW& maxFall, CStringW& mastering);

private:
    static void AddDisplay(CMainFrame* pMainFrame, int level, std::vector<TvInfoLine>& lines);
    static void AddVideoPlayback(CMainFrame* pMainFrame, int level, std::vector<TvInfoLine>& lines);
    static void AddAudioPlayback(CMainFrame* pMainFrame, int level, std::vector<TvInfoLine>& lines);
};
