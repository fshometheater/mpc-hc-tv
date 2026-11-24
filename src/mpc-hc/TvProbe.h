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

struct IFilterGraph;

// Measurement probe for a future scopes page: can the TV overlay read video frames from madVR, and
// at what cost? Only active with the environment variable MPCHC_TV_PROBE=1 (does nothing otherwise).
// Two routes, logged to %TEMP%\mpc-hc-tv-probe.log:
//  1. madVR's OSD render callback: after madVR has finished each frame, shrink its render target on
//     the GPU and read the small copy back one frame later (the displayed, tone-mapped output).
//  2. IMadVRFrameGrabber every few seconds on a worker thread, with and without madVR's HDR -> SDR
//     conversion (the input side).
// Also logs madVR's dropped-frame counter and whether madVR sends HDR to the display.
class CTvProbe
{
public:
    static void Start(IFilterGraph* pGraph);   // after a file is loaded (the player's graph)
    static void Stop();                         // before the graph is torn down
};
