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
#include <deque>

// What the Scopes page shows, measured from madVR while the page is open.
struct TvScopeData {
    // The displayed frame (madVR's render target after it's done, read back small every 2nd frame)
    static constexpr int kCols = 192;    // waveform columns across the picture
    static constexpr int kLevels = 128;  // waveform rows (signal code value 0..1)
    bool haveOutput = false;
    int outputKind = 0;                  // 0 gamma (SDR, tone mapped), 1 PQ (HDR passthrough), 2 linear scRGB (float)
    int outputBits = 8;
    std::vector<int> wave;               // kCols * kLevels sample counts (column major: col * kLevels + level)
    int waveSamplesPerCol = 1;           // samples that went into each column (for scaling the brightness)
    float outPeakCode = 0, outAvgCode = 0; // brightest channel per pixel, as signal code 0..1 (PQ code for kinds 1 and 2)

    // The input (madVR's grab without its HDR -> SDR conversion: the source, PQ), every ~0.7 s
    bool haveInput = false;
    float inPeakNits = 0, inAvgNits = 0; // brightest channel per pixel (like MaxCLL / MaxFALL): frame peak, frame average
    float inMaxSeenNits = 0;             // highest frame peak since the page opened

    // Measured tone curve: input nits (log bins) -> average output code of the same pixels
    static constexpr int kCurveBins = 56; // 0.01 .. 10000 nits, 8 bins per decade
    float curveOut[kCurveBins] = {};     // average output code 0..1 (-1 = no pixels at that input level)
    bool haveCurve = false;              // only while madVR tone maps (output grab differs from input)

    // History, one entry per input grab (last ~60 s)
    struct Hist { ULONGLONG t; float inPeak, inAvg, outPeak, outAvg; }; // nits; out* as code 0..1
    std::deque<Hist> history;

    bool hdrOutput = false;              // madVR sends HDR (PQ) to the display
    int framesDrawn = 0, framesDropped = 0;
    unsigned version = 0;                // changes with every new measurement
    CStringW problem;                    // why something isn't measured (shown on the page)
};

class CTvScopes
{
public:
    static void Start(IUnknown* madVr);  // the madVR filter; starts measuring (no-op if already running)
    static void Stop();                  // stops and releases everything (safe to call any time)
    static bool Running();
    static unsigned Version();
    static void Get(TvScopeData& out);   // a copy of the latest data

    // Signal code <-> nits
    static float PqToNits(float code);   // ST 2084
    static float NitsToPq(float nits);
    static float CurveBinNits(int bin);  // centre of a tone-curve bin
};
