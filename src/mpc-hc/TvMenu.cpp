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
#include "TvMenu.h"
#include "TvTracks.h"
#include "TvMode.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "resource.h"
#include "PPageAccelTbl.h" // key names, same as Options > Keys
#include "FGFilterLAV.h"
#include "TvOsdWindow.h"
#include "TvProbe.h"
#include "TvScopes.h"
#include "LAVFilters/src/include/LAVVideoSettings.h"
#include "LAVFilters/src/include/LAVAudioSettings.h"
#include <functional>
#include <memory>
#include <windowsx.h>

// Keep this file ASCII-only: MSVC reads sources without a BOM in the ANSI code page.
// Symbols are built from code points instead of being typed in string literals.

namespace
{
    constexpr char kOsdName[] = "MPC-HC.TvMenu";
    constexpr char kPreviewOsdName[] = "MPC-HC.TvBarPreview";
    constexpr char kSubPreviewOsdName[] = "MPC-HC.TvSubtitlePreview";
    constexpr int kZOrder = 1000;            // above madVR's own OSD elements
    // Background opacity (straight alpha), from the "Background opacity" TV option; set in Render().
    BYTE kPanelAlpha = 235;
    BYTE kBarAlpha = 215;
    // Panel / bar background (TV option "Background colour"); refreshed at the start of every Render().
    COLORREF kPanelColor = RGB(20, 22, 28);
    constexpr int kMaxVisibleRows = 8;
    constexpr UINT kApplyDelayMs = 700;      // pause after Left/Right before a track really switches
    constexpr UINT kTickMs = 20;             // timer while something is visible (also drives the focus glide)
    // Animation timings, scaled by the "Animations" TV option (Off = instant); set in Render().
    ULONGLONG kFocusFadeMs = 150;            // focus highlight crossfade
    float kFadeInStep = 0.34f;               // per tick: ~120 ms to fully visible
    float kFadeOutStep = 0.25f;              // per tick: ~160 ms to gone
    float kMorphStep = 0.12f;                // per tick: mini view glide, ~170 ms
    float kScrollSpring = 20.0f;             // mouse scrolling spring (rad/s): most of the way in ~0.2 s at Normal
    float g_scrollFadeMs = 180.0f;           // scrollbar fade

    void RefreshLookOptions()
    {
        const float scale = CTvMode::AnimScale();
        kFocusFadeMs = static_cast<ULONGLONG>(150 * scale);
        kFadeInStep = scale > 0.0f ? 0.34f / scale : 1.0f;
        kFadeOutStep = scale > 0.0f ? 0.25f / scale : 1.0f;
        kMorphStep = scale > 0.0f ? 0.12f / scale : 1.0f;
        kScrollSpring = scale > 0.0f ? 20.0f / scale : 1000.0f;
        g_scrollFadeMs = 180.0f * scale;
        const int opacity = CTvMode::Get(TvOpt::BackgroundOpacity); // 90 % = the original look
        kPanelAlpha = static_cast<BYTE>(std::min(255, opacity * 255 / 100 + 5));
        // The control bar has its own opacity option; its 90 % = the bar's original look (215, a little
        // more see-through than the panels at 90 %), and 100 % is solid
        const int pb = std::min(255, CTvMode::Get(TvOpt::BarOpacity) * 255 / 100 + 5);
        kBarAlpha = static_cast<BYTE>(std::max(pb * 215 / 235, 255 - (255 - pb) * 2));
    }

    // Per-pixel alpha: "Background opacity" is for the panel background only. A pixel of the panel
    // colour gets bgA; anything drawn on it (text, highlight, icons, lines) is solid (fullA), with
    // anti-aliased edges in between by how far the pixel is from the background colour.
    inline DWORD WithAlpha(DWORD px, BYTE bgA, BYTE fullA)
    {
        if (bgA >= fullA) {
            return (px & 0x00FFFFFF) | (static_cast<DWORD>(bgA) << 24);
        }
        const int b = static_cast<int>(px & 0xFF), g = static_cast<int>((px >> 8) & 0xFF), r = static_cast<int>((px >> 16) & 0xFF);
        const int d = std::max({ std::abs(r - GetRValue(kPanelColor)), std::abs(g - GetGValue(kPanelColor)), std::abs(b - GetBValue(kPanelColor)) });
        const int k = std::min(d * 6, 256);
        const DWORD a = static_cast<DWORD>(bgA + (((fullA - bgA) * k) >> 8));
        return (px & 0x00FFFFFF) | (a << 24);
    }
    constexpr ULONGLONG kSeekApplyMs = 800;      // jump this long after the last Left/Right
    constexpr ULONGLONG kScrubThrottleMs = 140;  // held key: about 7 steps per second
    constexpr ULONGLONG kBarRefreshMs = 500;     // time / play state refresh
    constexpr REFERENCE_TIME kSecond = 10000000;

    // Accent colour and font follow the TV options; refreshed at the start of every Render().
    COLORREF kFocusColor = RGB(52, 120, 246);
    LPCWSTR g_fontFace = L"Segoe UI";
    const COLORREF kTextColor = RGB(240, 240, 240);
    const COLORREF kDimTextColor = RGB(160, 165, 175);
    const COLORREF kFocusDimTextColor = RGB(215, 225, 245);
    const COLORREF kTrackColor = RGB(90, 94, 104);

    const wchar_t kTriLeft[] = { 0x25C0, 0 };   // black left-pointing triangle
    const wchar_t kTriRight[] = { 0x25B6, 0 };  // black right-pointing triangle
    const wchar_t kTriUp[] = { 0x25B2, 0 };
    const wchar_t kTriDown[] = { 0x25BC, 0 };
    const wchar_t kArrowUpDown[] = { 0x2191, 0x2193, 0 };
    const wchar_t kCheck[] = { 0x2713, 0 };
    const wchar_t kChevron[] = { 0x203A, 0 };   // single right-pointing angle quotation mark
    const wchar_t kDotSep[] = { 0x00B7, 0 };    // middle dot
    const wchar_t kDegree[] = { 0x00B0, 0 };    // degree sign

    // Icons from Windows' own "Segoe MDL2 Assets" font
    // Render timing (only while the perf log is on): time since the previous mark, per stage
    bool g_perfOn = false;
    LARGE_INTEGER g_perfLast = {};
    LARGE_INTEGER g_perfKey = {};  // first key since the last render
    double g_perfBuildMs = 0.0;    // row / info building since the last render
    std::vector<std::pair<const char*, double>> g_perfMarks;
    // One line in the perf log for a step that doesn't draw (re-present, crossfade, old picture)
    struct PerfStep {
        const char* name;
        LARGE_INTEGER t0 = {};
        explicit PerfStep(const char* n) : name(n) {
            if (g_perfOn) {
                QueryPerformanceCounter(&t0);
            }
        }
        ~PerfStep() {
            if (!g_perfOn) {
                return;
            }
            LARGE_INTEGER t1, f;
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            wchar_t path[MAX_PATH];
            GetTempPathW(MAX_PATH, path);
            wcscat_s(path, L"mpc-hc-tv-perf.log");
            FILE* fp = nullptr;
            if (_wfopen_s(&fp, path, L"a") == 0 && fp) {
                fprintf(fp, "%llu step=%s ms=%.2f\n", GetTickCount64(), name, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
                fclose(fp);
            }
        }
    };
    void PerfMark(const char* name)
    {
        if (!g_perfOn) {
            return;
        }
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        g_perfMarks.push_back({ name, (t.QuadPart - g_perfLast.QuadPart) * 1000.0 / f.QuadPart });
        g_perfLast = t;
    }

    const int kHitTriDown = 10, kHitTriUp = 11; // madVR edit chip: mouse areas of its triangles
    const wchar_t kIconRemove[] = { 0x2715, 0 }; // multiplication x: remove this tag
    const wchar_t kIconMini[] = { 0xE73F, 0 };   // BackToWindow: shrink the list to the mini view
    const wchar_t kIconExpand[] = { 0xE740, 0 }; // FullScreen: back to the full list
    const wchar_t kIconPrev[] = { 0xE892, 0 };
    const wchar_t kIconNext[] = { 0xE893, 0 };
    const wchar_t kIconPlay[] = { 0xE768, 0 };
    const wchar_t kIconPause[] = { 0xE769, 0 };
    const wchar_t kIconCC[] = { 0xEC42, 0 };     // speech bubble with lines (subtitles)
    const wchar_t kIconAudio[] = { 0xE8D6, 0 };
    const wchar_t kIconVideo[] = { 0xE8B2, 0 }; // "movies" (film frame): the Video options button
    const wchar_t kIconSettings[] = { 0xE713, 0 };
    const wchar_t kIconFullscreen[] = { 0xE740, 0 }; // "full screen" (four corners out)
    const wchar_t kIconWindowed[] = { 0xE73F, 0 };   // "back to window" (four corners in)
    const wchar_t kIconPower[] = { 0xE7E8, 0 };      // "power button": the power menu
    const wchar_t kIconSkipRing[] = { 0xE72C, 0 }; // round clockwise arrow ("refresh"); mirrored for back
    const wchar_t kIconMute[] = { 0xE74F, 0 };   // "mute"
    const wchar_t kIconVol1[] = { 0xE993, 0 };   // "volume 1" .. "volume 3"
    const wchar_t kIconVol2[] = { 0xE994, 0 };
    const wchar_t kIconVol3[] = { 0xE995, 0 };
    const wchar_t kIconStats[] = { 0xE9D9, 0 };  // "diagnostic"
    const wchar_t kIconShaders[] = { 0xE790, 0 }; // palette (MPC-HC pixel shaders)
    const wchar_t kIconVideoInfo[] = { 0xE7F4, 0 }; // monitor, with an "i" badge (see RenderBar)
    const wchar_t kIconAudioInfo[] = { 0xEC4F, 0 }; // single note, with an "i" badge
    const wchar_t kIconInfoBadge[] = { 0xE946, 0 }; // "info" (circled i)
    const wchar_t kIconChapters[] = { 0xE8FD, 0 };  // "bulleted list"
    const wchar_t kMidDot[] = { 0x00B7, 0 };        // separator in one-line texts
    const wchar_t kIconForward[] = { 0xE72A, 0 };   // arrow right (info panels: source -> output)

    // The renderers offered on the Video page (MPC-HC's ids), in this order
    const int kRenderers[] = { VIDRNDT_DS_MADVR, VIDRNDT_DS_MPCVR, VIDRNDT_DS_EVR_CUSTOM, VIDRNDT_DS_SYNC };

    CStringW RendererName(int id)
    {
        switch (id) {
            case VIDRNDT_DS_MADVR: return L"madVR";
            case VIDRNDT_DS_MPCVR: return L"MPC Video Renderer";
            case VIDRNDT_DS_EVR_CUSTOM: return L"EVR (custom presenter)";
            case VIDRNDT_DS_SYNC: return L"Sync Renderer";
            default: return L"Other (Options > Output)";
        }
    }

    // Remembered info panel levels
    LPCTSTR kVideoInfoLevelKey = _T("VideoInfoLevel");
    LPCTSTR kAudioInfoLevelKey = _T("AudioInfoLevel");

    // A 32-bit top-down DIB section with a GDI memory DC selected into it.
    class CDibCanvas
    {
    public:
        CDibCanvas(int w, int h) : m_w(w), m_h(h) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            m_hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&m_bits), nullptr, 0);
            m_hdc = CreateCompatibleDC(nullptr);
            if (m_hbm && m_hdc) {
                m_hbmOld = static_cast<HBITMAP>(SelectObject(m_hdc, m_hbm));
            }
        }
        ~CDibCanvas() {
            if (m_hdc) {
                if (m_hbmOld) {
                    SelectObject(m_hdc, m_hbmOld);
                }
                DeleteDC(m_hdc);
            }
            if (m_hbm) {
                DeleteObject(m_hbm);
            }
        }
        bool IsValid() const { return m_hbm && m_hdc && m_bits; }
        int Width() const { return m_w; }
        int Height() const { return m_h; }
        HDC DC() const { return m_hdc; }
        BYTE* Bits() const { return m_bits; } // B G R A, top-down

        // Anti-aliased filled circle, blended straight into the pixels (GDI's Ellipse has
        // jagged edges). Draw it before any GDI text that should appear on top of it.
        void FillCircleAA(float cx, float cy, float radius, COLORREF c) {
            GdiFlush(); // make pending GDI drawing land before we touch the pixels
            const int x0 = std::max(0, static_cast<int>(cx - radius - 1));
            const int x1 = std::min(m_w - 1, static_cast<int>(cx + radius + 1));
            const int y0 = std::max(0, static_cast<int>(cy - radius - 1));
            const int y1 = std::min(m_h - 1, static_cast<int>(cy + radius + 1));
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                    const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                    if (coverage <= 0.0f) {
                        continue;
                    }
                    BYTE* px = m_bits + (static_cast<size_t>(y) * m_w + x) * 4; // B G R A
                    px[0] = static_cast<BYTE>(px[0] + (GetBValue(c) - px[0]) * coverage + 0.5f);
                    px[1] = static_cast<BYTE>(px[1] + (GetGValue(c) - px[1]) * coverage + 0.5f);
                    px[2] = static_cast<BYTE>(px[2] + (GetRValue(c) - px[2]) * coverage + 0.5f);
                }
            }
        }

        // Anti-aliased convex polygon with rounded corners: the polygon is shrunk by `radius`,
        // then every pixel within `radius` of it is covered (exact distance, so corners come
        // out round). Points in any winding order.
        // `inset` shrinks the whole shape a little more (e.g. half the gap between pieces).
        void FillRoundPolyAA(std::vector<std::pair<float, float>> poly, float radius, COLORREF c, float inset = 0.0f) {
            const size_t n = poly.size();
            if (n < 3) {
                return;
            }
            GdiFlush();
            float gx = 0.0f, gy = 0.0f;
            for (const auto& p : poly) {
                gx += p.first / n;
                gy += p.second / n;
            }
            // inward unit normals and line offsets (inside: nx*x + ny*y >= d)
            auto lines = [&](const std::vector<std::pair<float, float>>& pts, std::vector<float>& nx, std::vector<float>& ny, std::vector<float>& d) {
                nx.assign(n, 0.0f), ny.assign(n, 0.0f), d.assign(n, 0.0f);
                for (size_t i = 0; i < n; i++) {
                    const auto& a = pts[i];
                    const auto& b = pts[(i + 1) % n];
                    float ex = b.first - a.first, ey = b.second - a.second;
                    const float len = std::max(1e-6f, std::sqrt(ex * ex + ey * ey));
                    float x = -ey / len, y = ex / len;
                    if (x * (gx - a.first) + y * (gy - a.second) < 0.0f) {
                        x = -x, y = -y;
                    }
                    nx[i] = x, ny[i] = y, d[i] = x * a.first + y * a.second;
                }
            };
            std::vector<float> nx, ny, d;
            lines(poly, nx, ny, d);
            // shrink: move each edge inwards by `radius`, new corners where neighbouring edges meet
            std::vector<std::pair<float, float>> inner(n);
            for (size_t i = 0; i < n; i++) {
                const size_t j = (i + n - 1) % n; // edge before corner i
                const float d1 = d[j] + radius + inset, d2 = d[i] + radius + inset;
                const float det = nx[j] * ny[i] - ny[j] * nx[i];
                if (std::fabs(det) < 1e-6f) {
                    inner[i] = poly[i];
                } else {
                    inner[i] = { (d1 * ny[i] - ny[j] * d2) / det, (nx[j] * d2 - d1 * nx[i]) / det };
                }
            }
            lines(inner, nx, ny, d);
            float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (const auto& p : poly) {
                minX = std::min(minX, p.first), maxX = std::max(maxX, p.first);
                minY = std::min(minY, p.second), maxY = std::max(maxY, p.second);
            }
            const int x0 = std::max(0, static_cast<int>(minX) - 1), x1 = std::min(m_w - 1, static_cast<int>(maxX) + 1);
            const int y0 = std::max(0, static_cast<int>(minY) - 1), y1 = std::min(m_h - 1, static_cast<int>(maxY) + 1);
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    bool inside = true;
                    for (size_t i = 0; i < n && inside; i++) {
                        inside = nx[i] * px + ny[i] * py >= d[i];
                    }
                    float dist = 0.0f; // distance to the shrunk polygon (0 inside)
                    if (!inside) {
                        dist = 1e9f;
                        for (size_t i = 0; i < n; i++) {
                            const auto& a = inner[i];
                            const auto& b = inner[(i + 1) % n];
                            const float ex = b.first - a.first, ey = b.second - a.second;
                            const float len2 = std::max(1e-9f, ex * ex + ey * ey);
                            const float t = std::clamp(((px - a.first) * ex + (py - a.second) * ey) / len2, 0.0f, 1.0f);
                            const float dx = px - (a.first + t * ex), dy = py - (a.second + t * ey);
                            dist = std::min(dist, std::sqrt(dx * dx + dy * dy));
                        }
                    }
                    const float coverage = std::clamp(radius + 0.5f - dist, 0.0f, 1.0f);
                    if (coverage <= 0.0f) {
                        continue;
                    }
                    BYTE* p = m_bits + (static_cast<size_t>(y) * m_w + x) * 4;
                    p[0] = static_cast<BYTE>(p[0] + (GetBValue(c) - p[0]) * coverage + 0.5f);
                    p[1] = static_cast<BYTE>(p[1] + (GetGValue(c) - p[1]) * coverage + 0.5f);
                    p[2] = static_cast<BYTE>(p[2] + (GetRValue(c) - p[2]) * coverage + 0.5f);
                }
            }
        }

        // GDI output leaves the alpha byte undefined, so alpha is set afterwards for the
        // whole bitmap: `alpha` inside a rounded rectangle (radius 0 = plain), 0 outside.
        // full: alpha of what's drawn on the background (text etc.); 0 = the same as `alpha`
        void ApplyRoundedAlpha(int radius, BYTE alpha, BYTE full = 0) {
            GdiFlush();
            // Only the corner squares need the (anti-aliased) coverage; everything else just gets
            // `alpha`, written in a tight loop (this runs for the whole 4K-wide bar on every frame).
            const int r = std::max(0, std::min(radius, std::min(m_w, m_h) / 2));
            if (full < alpha) {
                full = alpha;
            }
            for (int y = 0; y < m_h; y++) {
                BYTE* row = m_bits + static_cast<size_t>(y) * m_w * 4;
                const bool cornerRow = (r > 0) && (y < r || y >= m_h - r);
                const int x0 = cornerRow ? r : 0;
                const int x1 = cornerRow ? m_w - r : m_w;
                DWORD* px32 = reinterpret_cast<DWORD*>(row);
                for (int x = x0; x < x1; x++) {
                    px32[x] = WithAlpha(px32[x], alpha, full);
                }
                if (alpha == 0) {
                    for (int x = x0; x < x1; x++) {
                        row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
                    }
                }
                if (cornerRow) {
                    for (int x = 0; x < m_w; x = (x == r - 1) ? m_w - r : x + 1) {
                        BYTE* px = row + x * 4;
                        px[3] = static_cast<BYTE>(alpha * RoundedCoverage(x, y, r) + 0.5f);
                        if (px[3] == 0) {
                            px[0] = px[1] = px[2] = 0;
                        }
                    }
                }
            }
        }

        // Moves the finished picture down (dy > 0) or up (dy < 0) inside the bitmap; what moves
        // out is cut off, the uncovered rows become transparent. For the show / hide slide of
        // elements at the screen edges, whose bitmap itself can't move past the edge.
        void ShiftRows(int dy) {
            if (dy == 0 || !m_bits) {
                return;
            }
            GdiFlush();
            const size_t stride = static_cast<size_t>(m_w) * 4;
            const int n = std::min(std::abs(dy), m_h);
            if (dy > 0) {
                memmove(m_bits + n * stride, m_bits, (m_h - n) * stride);
                memset(m_bits, 0, n * stride);
            } else {
                memmove(m_bits, m_bits + n * stride, (m_h - n) * stride);
                memset(m_bits + (m_h - n) * stride, 0, n * stride);
            }
        }

        // Hands the bitmap over to the caller (who must DeleteObject it).
        HBITMAP Detach() {
            if (m_hdc && m_hbmOld) {
                SelectObject(m_hdc, m_hbmOld);
                m_hbmOld = nullptr;
            }
            HBITMAP hbm = m_hbm;
            m_hbm = nullptr;
            return hbm;
        }

    private:
        // 0..1: how much of pixel (x, y) lies inside the rounded rectangle; partial on the
        // curved corner edges so they're anti-aliased.
        float RoundedCoverage(int x, int y, int r) const {
            if (r <= 0) {
                return 1.0f;
            }
            const bool cornerX = x < r || x >= m_w - r;
            const bool cornerY = y < r || y >= m_h - r;
            if (!cornerX || !cornerY) {
                return 1.0f; // straight edges and the inside
            }
            // Distance from the corner circle's centre (pixel centres at +0.5)
            const float cx = x < r ? static_cast<float>(r) : static_cast<float>(m_w - r);
            const float cy = y < r ? static_cast<float>(r) : static_cast<float>(m_h - r);
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            return std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
        }

        int m_w, m_h;
        HBITMAP m_hbm = nullptr;
        HBITMAP m_hbmOld = nullptr;
        HDC m_hdc = nullptr;
        BYTE* m_bits = nullptr;
    };

    // ---- Presenting -------------------------------------------------------------------------
    // Pages draw into content canvases that are kept between frames (no new 4K bitmap per frame;
    // a fade step re-uses the drawing). Presenting copies the content into one of two output
    // bitmaps per OSD element (used in turn, madVR may still read the last one), setting the
    // alpha (fade, rounded corners) on the way, and hands it to madVR.
    enum OsdElem { ElemMenu = 0, ElemPreview, ElemOld, ElemFocus0, ElemFocus1, ElemCount };
    const char* const kElemNames[ElemCount] = { kOsdName, kPreviewOsdName, "MPC-HC.TvMenuOld", "MPC-HC.TvMenuFocus0",
                                                "MPC-HC.TvMenuFocus1" };
    enum ContentSlot { SlotPanel = 0, SlotBar, SlotPreview, SlotInfo, SlotOld, SlotXfade, SlotCount };
    std::unique_ptr<CDibCanvas> g_content[SlotCount];

    CDibCanvas& Content(int slot, int w, int h)
    {
        auto& c = g_content[slot];
        if (!c || !c->IsValid() || c->Width() != w || c->Height() != h) {
            c = std::make_unique<CDibCanvas>(w, h);
        }
        return *c;
    }

    struct OutBuf {
        HBITMAP hbm[2] = {};
        BYTE* bits[2] = {};
        int w = 0, h = 0, next = 0;
    };
    OutBuf g_out[ElemCount];

    void FreeOut(int e)
    {
        for (auto& b : g_out[e].hbm) {
            if (b) {
                DeleteObject(b);
            }
            b = nullptr;
        }
        g_out[e] = OutBuf();
    }

    // The next of the element's two output bitmaps (made / remade at this size)
    HBITMAP NextOut(int e, int w, int h, BYTE*& bits)
    {
        OutBuf& o = g_out[e];
        if (o.w != w || o.h != h) {
            FreeOut(e);
            for (int i = 0; i < 2; i++) {
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h; // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                o.hbm[i] = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&o.bits[i]), nullptr, 0);
            }
            o.w = w;
            o.h = h;
        }
        const int i = o.next;
        o.next ^= 1;
        bits = o.bits[i];
        return o.hbm[i];
    }

    // 0..1 inside a w x h rounded rectangle of radius r (anti-aliased corners)
    float CornerCoverage(int x, int y, int w, int h, int r)
    {
        if (r <= 0 || (x >= r && x < w - r) || (y >= r && y < h - r)) {
            return 1.0f;
        }
        const float cx = x < r ? static_cast<float>(r) : static_cast<float>(w - r);
        const float cy = y < r ? static_cast<float>(r) : static_cast<float>(h - r);
        const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        return std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
    }

    // dst (w x h) = src rect (sx, sy, w, h) with straight alpha `alpha` (0 outside the rounded
    // rectangle of the whole w x h picture), moved down by dy rows (up if negative; uncovered rows
    // are transparent), and `holes` (picture coordinates) left transparent.
    // full: alpha of what's drawn on the panel background (see WithAlpha); 0 = the same as `alpha`
    void Compose(const BYTE* src, int srcW, int sx, int sy, int w, int h, BYTE* dst, int radius, BYTE alpha, int dy,
                 const RECT* holes = nullptr, int holeCount = 0, BYTE full = 0)
    {
        const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
        if (full < alpha) {
            full = alpha;
        }
        for (int y = 0; y < h; y++) {
            DWORD* out = reinterpret_cast<DWORD*>(dst + static_cast<size_t>(y) * w * 4);
            const int py = y - dy; // row of the picture shown here
            if (py < 0 || py >= h || full == 0) {
                memset(out, 0, static_cast<size_t>(w) * 4);
                continue;
            }
            const DWORD* in = reinterpret_cast<const DWORD*>(src + (static_cast<size_t>(sy + py) * srcW + sx) * 4);
            // mostly long runs of the same background pixel: reuse the last result (GDI leaves the
            // alpha byte undefined, so only the colour is compared)
            DWORD lastIn = ~in[0] & 0x00FFFFFF, lastOut = 0;
            for (int x = 0; x < w; x++) {
                const DWORD v = in[x] & 0x00FFFFFF;
                if (v != lastIn) {
                    lastIn = v;
                    lastOut = WithAlpha(v, alpha, full);
                }
                out[x] = lastOut;
            }
            if (r > 0 && (py < r || py >= h - r)) {
                for (int x = 0; x < w; x = (x == r - 1) ? std::max(r, w - r) : x + 1) {
                    const float cov = CornerCoverage(x, py, w, h, r);
                    const BYTE ca = static_cast<BYTE>((out[x] >> 24) * cov + 0.5f);
                    out[x] = ca ? ((in[x] & 0x00FFFFFF) | (static_cast<DWORD>(ca) << 24)) : 0;
                }
            }
            for (int k = 0; k < holeCount; k++) {
                const RECT& hr = holes[k];
                if (py >= hr.top && py < hr.bottom) {
                    const int x0 = std::clamp(static_cast<int>(hr.left), 0, w), x1 = std::clamp(static_cast<int>(hr.right), 0, w);
                    if (x1 > x0) {
                        memset(out + x0, 0, static_cast<size_t>(x1 - x0) * 4);
                    }
                }
            }
        }
    }

    // In a whole w x h output picture: rectangle r = picture a -> picture b by t, alpha `alpha`
    void BlendInto(const BYTE* a, const BYTE* b, int w, int h, const RECT& r, float t, BYTE* dst, BYTE alpha, BYTE full = 0)
    {
        const int x0 = std::clamp(static_cast<int>(r.left), 0, w), x1 = std::clamp(static_cast<int>(r.right), 0, w);
        const int y0 = std::clamp(static_cast<int>(r.top), 0, h), y1 = std::clamp(static_cast<int>(r.bottom), 0, h);
        const int ti = std::clamp(static_cast<int>(t * 256.0f + 0.5f), 0, 256);
        if (full < alpha) {
            full = alpha;
        }
        for (int y = y0; y < y1; y++) {
            const size_t row = static_cast<size_t>(y) * w;
            const BYTE* pa = a + (row + x0) * 4;
            const BYTE* pb = b + (row + x0) * 4;
            DWORD* out = reinterpret_cast<DWORD*>(dst) + row;
            for (int x = x0; x < x1; x++, pa += 4, pb += 4) {
                const int c0 = pa[0] + (((pb[0] - pa[0]) * ti) >> 8);
                const int c1 = pa[1] + (((pb[1] - pa[1]) * ti) >> 8);
                const int c2 = pa[2] + (((pb[2] - pa[2]) * ti) >> 8);
                out[x] = full ? WithAlpha((static_cast<DWORD>(c2) << 16) | (static_cast<DWORD>(c1) << 8) | static_cast<DWORD>(c0), alpha, full) : 0;
            }
        }
    }

    // Copy of a picture (used to keep the old state during a transition)
    void CopyCanvas(const CDibCanvas& from, CDibCanvas& to)
    {
        GdiFlush();
        memcpy(to.Bits(), from.Bits(), static_cast<size_t>(from.Width()) * from.Height() * 4);
    }

    // TV options submenus (display order only; each option keeps its own settings key).
    // TvOpt::Count stands for the "Subtitle style" row. New groups (e.g. remote keys) go here.
    struct OptionSection {
        LPCWSTR heading;
        std::vector<TvOpt> opts;
        bool barButtons = false; // followed by the bar's buttons (show / hide, order)
        bool keys = false;       // followed by the remote key assignments
        LPCWSTR icon = L"";      // Segoe MDL2 glyph for its row on the Settings page
        std::vector<std::pair<size_t, LPCWSTR>> groups = {}; // sub-heading before opts[index]
    };

    // Remote keys submenu: actions whose key can be set by pressing it. Positive ids are
    // MPC-HC commands (stored in MPC-HC's own key table, same as Options > Keys); the
    // negative ones are TV-only extra keys for Back / OK.
    constexpr int kExtraBackKey = -1;
    constexpr int kExtraOkKey = -2;
    constexpr int kBarKey = -3; // TV-only: shows the control bar during playback (default Down)
    // Arrow keys during playback (Button / key mapping > Arrow keys): TV actions; a positive value is
    // an MPC-HC command. kArrowDefault = not set (see CTvMenu::ArrowAction).
    constexpr int kArrowDefault = -1000;
    constexpr int kActNothing = -100, kActPass = -101, kActSeekBack = -102, kActSeekFwd = -103, kActShowBar = -104,
                  kActSubtitles = -105, kActAudio = -106, kActChapters = -107, kActInfo = -108, kActSettings = -109;
    struct KeyAction {
        int id;
        LPCWSTR name;
    };
    // Grouped: an entry with id 0 is a heading. All ids are MPC-HC commands in its key table (or the
    // TV-only extra keys), so a key set here is the same as one set in Options > Keys.
    const KeyAction kKeyActions[] = {
        { 0, L"TV menu" },
        { kBarKey, L"Show the control bar" },
        { ID_TV_AUDIO, L"Audio list" },
        { ID_TV_SUBTITLES, L"Subtitles list" },
        { kExtraBackKey, L"Back (extra key)" },
        { kExtraOkKey, L"OK (extra key)" },
        { ID_TV_MODE, L"TV Mode on / off" },
        { 0, L"Playback" },
        { ID_PLAY_PLAYPAUSE, L"Play / pause" },
        { ID_PLAY_PLAY, L"Play" },
        { ID_PLAY_PAUSE, L"Pause" },
        { ID_PLAY_STOP, L"Stop" },
        { ID_NAVIGATE_SKIPFORWARD, L"Next chapter / file" },
        { ID_NAVIGATE_SKIPBACK, L"Previous chapter / file" },
        { ID_PLAY_FRAMESTEP, L"Next frame" },
        { ID_PLAY_FRAMESTEP_BACK, L"Previous frame" },
        { ID_PLAY_INCRATE, L"Faster" },
        { ID_PLAY_DECRATE, L"Slower" },
        { ID_PLAY_RESETRATE, L"Normal speed" },
        { ID_PLAY_REPEAT_AB, L"A-B repeat" },
        // MPC-HC's jumps; their distances are in TV options > Seeking (= Options > Tweaks)
        { 0, L"Jumps" },
        { ID_PLAY_SEEKFORWARDSMALL, L"Small jump forward" },
        { ID_PLAY_SEEKBACKWARDSMALL, L"Small jump back" },
        { ID_PLAY_SEEKFORWARDMED, L"Medium jump forward" },
        { ID_PLAY_SEEKBACKWARDMED, L"Medium jump back" },
        { ID_PLAY_SEEKFORWARDLARGE, L"Big jump forward" },
        { ID_PLAY_SEEKBACKWARDLARGE, L"Big jump back" },
        { 0, L"Audio" },
        { ID_VOLUME_UP, L"Volume up" },
        { ID_VOLUME_DOWN, L"Volume down" },
        { ID_VOLUME_MUTE, L"Mute" },
        { ID_STREAM_AUDIO_NEXT, L"Next audio track" },
        { ID_STREAM_AUDIO_PREV, L"Previous audio track" },
        { 0, L"Subtitles" },
        { ID_STREAM_SUB_ONOFF, L"Subtitles on / off" },
        { ID_STREAM_SUB_NEXT, L"Next subtitle track" },
        { ID_STREAM_SUB_PREV, L"Previous subtitle track" },
        { ID_SUB_DELAY_UP, L"Subtitle delay +" },
        { ID_SUB_DELAY_DOWN, L"Subtitle delay -" },
        { 0, L"Picture" },
        { ID_VIEW_FULLSCREEN, L"Fullscreen" },
        { ID_ASPECTRATIO_NEXT, L"Next aspect ratio" },
        { ID_VIEW_VF_NORMAL, L"Zoom: normal" },
        { ID_VIEW_VF_FROMINSIDE, L"Zoom: touch window from inside" },
        { ID_VIEW_VF_FROMOUTSIDE, L"Zoom: touch window from outside" },
        { 0, L"Player" },
        { ID_VIEW_OSD_DISPLAY_TIME, L"Show the time (MPC-HC message)" },
        { ID_VIEW_OSD_SHOW_FILENAME, L"Show the file name (MPC-HC message)" },
        { ID_FILE_CLOSE_AND_RESTORE, L"Close the file" },
        { ID_FILE_EXIT, L"Exit the player" },
    };
    LPCTSTR kExtraBackKeyName = _T("ExtraBackKey");
    LPCTSTR kExtraOkKeyName = _T("ExtraOkKey");
    LPCTSTR kBarKeyName = _T("BarKey");

    // Readable name of a plain virtual key (for the TV-only extra keys)
    CStringW VirtualKeyName(UINT vk)
    {
        if (vk == 0) {
            return L"Not set";
        }
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        switch (vk) { // extended keys need the extended bit for the right name
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
            case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
                scan |= 0x100;
                break;
        }
        wchar_t name[64] = {};
        if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, _countof(name)) > 0) {
            return name;
        }
        CStringW s;
        s.Format(L"Key 0x%02X", vk);
        return s;
    }
    const OptionSection kOptionSections[] = {
        // how everything looks (colours, font, sizes, motion, where the overlay is drawn) and how menus behave
        { L"Appearance", { TvOpt::AccentColour, TvOpt::AccentBrightness, TvOpt::FontFace, TvOpt::BackgroundColour,
                           TvOpt::BackgroundOpacity, TvOpt::BarOpacity, TvOpt::BarIconBrightness, TvOpt::BarIconOpacity, TvOpt::MenuTextPct, TvOpt::BarSizePct, TvOpt::KeyHints, TvOpt::MenuHideSec, TvOpt::WrapAround, TvOpt::WrapWhileHeld, TvOpt::AnimSpeed,
                           TvOpt::SmoothPlayhead, TvOpt::OverlayMode },
          false, false, L"\xE790", { { 0, L"Look" }, { 10, L"Menus" }, { 14, L"Motion and drawing" } } },
        // when the bar shows and what's on it, then its buttons (show / hide, order)
        { L"Control bar", { TvOpt::ShowOnStart, TvOpt::ShowOnPause, TvOpt::PauseScreenOn, TvOpt::PauseScreen, TvOpt::ShowOnMouse, TvOpt::BarHideSec,
                            TvOpt::TimeDisplay, TvOpt::ShowClock, TvOpt::ShowEndsAt, TvOpt::ButtonLabels, TvOpt::HoldOk,
                            TvOpt::BackAction, TvOpt::SkipBackSec, TvOpt::SkipFwdSec },
          true, false, L"\xE768", { { 0, L"When it shows" }, { 6, L"What's on it" }, { 10, L"Button actions" } } },
        { L"Seeking", { TvOpt::ShowOnSeek, TvOpt::SeekMode, TvOpt::SeekStepSec, TvOpt::HoldSpeedup, TvOpt::SmoothScrub,
                        TvOpt::ChapterSnap,
                        TvOpt::JumpSmall, TvOpt::JumpMedium, TvOpt::JumpLarge },
          false, false, L"\xE916", { { 0, L"Left / Right" }, { 6, L"Jump distances (keys)" } } },
        { L"Audio and subtitles", { TvOpt::Count, TvOpt::AudioDelayStepMs, TvOpt::SubDelayStepMs }, false, false, L"\xE7F0" },
        { L"Button / key mapping", {}, false, true, L"\xE765" },
    };
    // Pause screen delay in ms; 0 = off (switched off, or an earlier "Never")
    int PauseScreenMs()
    {
        return CTvMode::IsOn(TvOpt::PauseScreenOn) ? std::max(0, CTvMode::Get(TvOpt::PauseScreen)) * 1000 : 0;
    }

    // One line under the TV options list explaining the focused option
    LPCWSTR OptionHelp(TvOpt opt)
    {
        switch (opt) {
            case TvOpt::AccentColour: return L"Colour of the highlight, the seek line and the focus circle";
            case TvOpt::AccentBrightness: return L"Darker or lighter version of the accent colour";
            case TvOpt::FontFace: return L"Font of all TV menus, the bar and the info panels";
            case TvOpt::BackgroundOpacity: return L"How much of the picture shows through the menus (text stays solid)";
            case TvOpt::BarOpacity: return L"How much of the picture shows through the control bar (text stays solid)";
            case TvOpt::BackgroundColour: return L"Colour behind the menus and the bar; Custom uses the colour below";
            case TvOpt::BarIconBrightness: return L"Dims the bar's icons; the selected button stays bright (shown below)";
            case TvOpt::BarIconOpacity: return L"Makes the bar's icons see-through; the selected button stays solid (shown below)";
            case TvOpt::MenuTextPct: return L"Size of the menus and lists";
            case TvOpt::BarSizePct: return L"Size of the control bar (shown below while this is selected)";
            case TvOpt::KeyHints: return L"The line of key hints at the bottom of the menus";
            case TvOpt::MenuHideSec: return L"Close a menu on its own after this long without a key";
            case TvOpt::WrapAround: return L"Up on the first row goes to the last, Down on the last to the first";
            case TvOpt::WrapWhileHeld: return L"Also while a key is held: the list keeps going round instead of stopping at the end";
            case TvOpt::AnimSpeed: return L"Fades, slides and glides; Off makes every change instant";
            case TvOpt::SmoothPlayhead: return L"The playhead dot glides to its new place; off for slower PCs";
            case TvOpt::OverlayMode: return L"Own window works with any renderer; inside madVR for HDR passthrough";
            case TvOpt::ShowOnStart: return L"Show the bar when a file starts playing";
            case TvOpt::ShowOnPause: return L"Show the bar while paused";
            case TvOpt::PauseScreenOn: return L"A card with the title, time and end time after a while paused";
            case TvOpt::PauseScreen: return L"How long paused (without a key) before the pause screen shows";
            case TvOpt::ShowOnMouse: return L"Moving the mouse over the picture shows the bar";
            case TvOpt::BarHideSec: return L"Hide the bar after this long without a key while playing";
            case TvOpt::TimeDisplay: return L"Right of the seek line: the length, or the time left";
            case TvOpt::ShowClock: return L"The current time on the bar";
            case TvOpt::ShowEndsAt: return L"When the file will end, on the bar";
            case TvOpt::ButtonLabels: return L"The name of the selected button, under it";
            case TvOpt::HoldOk: return L"Holding OK on the bar opens a page; a short OK acts when let go";
            case TvOpt::BackAction: return L"What Back / Esc does when no menu or bar is showing";
            case TvOpt::SkipBackSec: return L"How far the skip back button goes";
            case TvOpt::SkipFwdSec: return L"How far the skip forward button goes";
            case TvOpt::ShowOnSeek: return L"What Left / Right show while seeking during playback";
            case TvOpt::SeekMode: return L"Jump once you stop pressing, on every press, or only when you press OK (Back cancels)";
            case TvOpt::SeekStepSec: return L"How far one Left / Right press moves";
            case TvOpt::HoldSpeedup: return L"Holding Left / Right moves faster the longer it's held";
            case TvOpt::SmoothScrub: return L"Holding Left / Right glides the preview; off for slower PCs";
            case TvOpt::ChapterSnap: return L"A press landing within 5 s of the next chapter stops on it";
            case TvOpt::JumpSmall: case TvOpt::JumpMedium: case TvOpt::JumpLarge:
                return L"MPC-HC's own jumps, for keys set in Button / key mapping";
            case TvOpt::AudioDelayStepMs: return L"How much one Left / Right press changes the audio delay";
            case TvOpt::SubDelayStepMs: return L"How much one Left / Right press changes the subtitle delay";
            case TvOpt::SubFont: return L"Font of the subtitles";
            case TvOpt::SubSize: return L"Size of the subtitles";
            case TvOpt::SubColour: return L"Colour of the subtitle text";
            case TvOpt::SubOpacity: return L"How solid the subtitle text is";
            case TvOpt::SubBorderStyle: return L"An outline around the letters, or a box behind them";
            case TvOpt::SubOutlineWidth: return L"Thickness of the outline (or the box's padding)";
            case TvOpt::SubOutlineOpacity: return L"How solid the outline or box is";
            case TvOpt::SubShadowDepth: return L"Distance of the shadow; 0 = no shadow";
            case TvOpt::SubShadowOpacity: return L"How solid the shadow is";
            case TvOpt::SubMargin: return L"Distance of the subtitles from the bottom";
            case TvOpt::SubRelativeTo: return L"Place subtitles in the window or on the picture";
            default: return L"";
        }
    }

    // MPC-HC's own default subtitle style, cut down (Options > Subtitles > Default Style)
    const OptionSection kSubStyleSections[] = {
        { L"Text", { TvOpt::SubFont, TvOpt::SubSize, TvOpt::SubColour, TvOpt::SubOpacity } },
        { L"Border", { TvOpt::SubBorderStyle, TvOpt::SubOutlineWidth, TvOpt::SubOutlineOpacity,
                       TvOpt::SubShadowDepth, TvOpt::SubShadowOpacity } },
        { L"Position", { TvOpt::SubMargin, TvOpt::SubRelativeTo } },
    };

    std::map<CStringW, HFONT>* g_fontCache = nullptr;
    size_t g_fontCacheSize = 0;
    HFONT MakeFont(int height, int weight, LPCWSTR face = nullptr) {
        if (!face) {
            face = g_fontFace; // the "Font" TV option (icons pass their own font)
            // With Segoe UI / Segoe UI Light, bold text uses the real "Segoe UI Semibold" face at
            // normal weight: asking GDI for a heavier weight on top of it (or on the Light face)
            // makes Windows thicken the strokes artificially, which looks smudgy.
            if (weight >= FW_SEMIBOLD && (_wcsicmp(face, L"Segoe UI Light") == 0 || _wcsicmp(face, L"Segoe UI") == 0)) {
                face = L"Segoe UI Semibold";
                weight = FW_NORMAL;
            }
        }
        // Fonts are kept and reused (every frame asks for the same few); ReleaseFont does nothing.
        // The cache is emptied when it grows (sizes change with the output / options), between frames.
        static std::map<CStringW, HFONT> cache;
        CStringW key;
        key.Format(L"%d|%d|%s", height, weight, face);
        if (auto it = cache.find(key); it != cache.end()) {
            return it->second;
        }
        HFONT f = CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
        cache[key] = f;
        g_fontCacheSize = cache.size();
        g_fontCache = &cache;
        return f;
    }
    void ReleaseFont(HFONT) {} // owned by MakeFont's cache
    // Called at the start of a frame (no font is selected into a DC then)
    void TrimFontCache()
    {
        if (g_fontCache && g_fontCache->size() > 96) {
            for (auto& kv : *g_fontCache) {
                DeleteObject(kv.second);
            }
            g_fontCache->clear();
        }
    }

    // a -> b by f (0..1)
    COLORREF Mix(COLORREF a, COLORREF b, float f) {
        auto ch = [f](int x, int y) { return static_cast<BYTE>(x + (y - x) * f + 0.5f); };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    }

    void FillSolid(HDC hdc, const RECT& r, COLORREF c) {
        HBRUSH br = CreateSolidBrush(c);
        FillRect(hdc, &r, br);
        DeleteObject(br);
    }

    void DrawLine(HDC hdc, HFONT font, COLORREF color, LPCWSTR text, RECT r, UINT align) {
        SelectObject(hdc, font);
        SetTextColor(hdc, color);
        DrawTextW(hdc, text, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    int TextWidth(HDC hdc, HFONT font, LPCWSTR text) {
        // Measured the same way DrawLine draws: DrawText substitutes a fallback font for
        // characters the chosen font lacks (e.g. the arrows), GetTextExtentPoint32 doesn't,
        // so it measured too short and DrawText cut the end off with "...".
        SelectObject(hdc, font);
        RECT r = { 0, 0, 0, 0 };
        DrawTextW(hdc, text, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        return r.right - r.left;
    }

    // madVR's full output rect (in the video window's client area); OSD positions are relative to it.
    RECT GetOutputRect(IMadVROsdServices* pOsd) {
        RECT full = {}, video = {};
        if (pOsd && SUCCEEDED(pOsd->OsdGetVideoRects(&full, &video)) && full.right > full.left && full.bottom > full.top) {
            return full;
        }
        return { 0, 0, 1920, 1080 };
    }

    void GetOutputSize(IMadVROsdServices* pOsd, int& w, int& h) {
        const RECT full = GetOutputRect(pOsd);
        w = full.right - full.left;
        h = full.bottom - full.top;
    }

    CStringW FormatTime(REFERENCE_TIME rt) {
        long long s = std::max<long long>(0, rt / kSecond);
        CStringW str;
        if (s >= 3600) {
            str.Format(L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
        } else {
            str.Format(L"%lld:%02lld", s / 60, s % 60);
        }
        return str;
    }

    // "Ends at 21:48" in the user's time format
    CStringW EndsAt(REFERENCE_TIME remaining) {
        FILETIME ftNow;
        GetSystemTimeAsFileTime(&ftNow);
        ULARGE_INTEGER t;
        t.LowPart = ftNow.dwLowDateTime;
        t.HighPart = ftNow.dwHighDateTime;
        t.QuadPart += static_cast<ULONGLONG>(std::max<REFERENCE_TIME>(0, remaining)); // both in 100 ns units
        FILETIME ftEnd = { t.LowPart, t.HighPart }, ftLocal;
        SYSTEMTIME st;
        if (!FileTimeToLocalFileTime(&ftEnd, &ftLocal) || !FileTimeToSystemTime(&ftLocal, &st)) {
            return L"";
        }
        wchar_t buf[64] = {};
        if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, _countof(buf))) {
            return L"";
        }
        return CStringW(L"Ends at ") + buf;
    }

    // Scale factor from a TvOpt percentage option
    float Scale(TvOpt opt) {
        return CTvMode::Get(opt) / 100.0f;
    }

    CStringW DelayText(CMainFrame* pMainFrame, bool audio = false) {
        int delay = 0;
        if (!(audio ? CTvTracks::GetAudioDelay(pMainFrame, delay) : CTvTracks::GetSubtitleDelay(pMainFrame, delay))) {
            return L"Not available";
        }
        CStringW s;
        s.Format(L"%s  %+d ms  %s", kTriLeft, delay, kTriRight);
        return s;
    }

    CStringW AudioTitle(CMainFrame* pMainFrame, int index) {
        std::vector<TvTrack> tracks;
        if (CTvTracks::GetAudio(pMainFrame, tracks)) {
            for (const auto& t : tracks) {
                if (t.index == index) {
                    return t.title;
                }
            }
        }
        return L"";
    }

    CStringW SubtitleTitle(CMainFrame* pMainFrame, int index) {
        if (index < 0) {
            return L"Off";
        }
        std::vector<TvTrack> tracks;
        bool on = false;
        if (CTvTracks::GetSubtitles(pMainFrame, tracks, on)) {
            for (const auto& t : tracks) {
                if (t.index == index) {
                    return t.title;
                }
            }
        }
        return L"";
    }

    // Index of the playing audio track, or -1.
    int CurrentAudioIndex(CMainFrame* pMainFrame) {
        std::vector<TvTrack> tracks;
        if (CTvTracks::GetAudio(pMainFrame, tracks)) {
            for (const auto& t : tracks) {
                if (t.isCurrent) {
                    return t.index;
                }
            }
        }
        return -1;
    }

    // Index of the shown subtitle track, -1 when off.
    int CurrentSubtitleIndex(CMainFrame* pMainFrame) {
        std::vector<TvTrack> tracks;
        bool on = false;
        if (CTvTracks::GetSubtitles(pMainFrame, tracks, on) && on) {
            for (const auto& t : tracks) {
                if (t.isCurrent) {
                    return t.index;
                }
            }
        }
        return -1;
    }
}

float CTvMenu::FocusFade::Progress(int target, bool& running, int& prevOut)
{
    const ULONGLONG now = GetTickCount64();
    prevOut = -1;
    if (!valid) {
        cur = target;
        prev = -1;
        start = 0;
        valid = true;
    } else if (target != cur) {
        prev = cur; // a move during a fade simply restarts it from the last focus
        cur = target;
        start = now;
    }
    if (start == 0) {
        return 1.0f;
    }
    const float t = kFocusFadeMs ? std::min(1.0f, static_cast<float>(now - start) / kFocusFadeMs) : 1.0f;
    if (t >= 1.0f) {
        start = 0;
        prev = -1;
        return 1.0f;
    }
    running = true;
    prevOut = prev;
    return t;
}

LPCWSTR CTvMenu::BarButtonName(BarButton b)
{
    switch (b) {
        case BarButton::Prev: return L"Previous";
        case BarButton::Back10: return L"Skip back";
        case BarButton::PlayPause: return L"Play / pause";
        case BarButton::Fwd30: return L"Skip forward";
        case BarButton::Next: return L"Next";
        case BarButton::Stats: return L"madVR";
        case BarButton::VideoInfo: return L"Info";
        case BarButton::AudioInfo: return L"Audio info";
        case BarButton::Subtitles: return L"Subtitles";
        case BarButton::Audio: return L"Audio";
        case BarButton::Video: return L"Video";
        case BarButton::Settings: return L"Settings";
        case BarButton::Volume: return L"Volume";
        case BarButton::Shaders: return L"Shaders";
        case BarButton::Separator: return L"Separator";
        case BarButton::Chapters: return L"Chapters";
        case BarButton::Fullscreen: return L"Fullscreen";
        case BarButton::Power: return L"Power menu";
        default: return L"";
    }
}

// Stored as "id:on,id:on,..." (BarButton values). Missing buttons are added in their default
// place; until the layout is first changed, the older on/off options decide the info buttons.
std::vector<CTvMenu::BarItem> CTvMenu::BarLayout()
{
    std::vector<BarItem> items;
    bool seen[static_cast<int>(BarButton::Count)] = {};
    const CString stored = CTvMode::GetText(_T("BarButtons"));
    int start = 0;
    while (!stored.IsEmpty()) {
        const CString token = stored.Tokenize(_T(","), start);
        if (start < 0) {
            break;
        }
        int id = -1, on = 1;
        if (_stscanf_s(token, _T("%d:%d"), &id, &on) >= 1 && id >= 0 && id < static_cast<int>(BarButton::Count) &&
            (!seen[id] || id == static_cast<int>(BarButton::Separator))) { // separators: any number
            const BarButton b = static_cast<BarButton>(id);
            seen[id] = true;
            items.push_back({ b, on != 0 || !CanHide(b) });
        }
    }
    const BarItem defaults[] = {
        { BarButton::Prev, true }, { BarButton::Back10, true }, { BarButton::PlayPause, true }, { BarButton::Fwd30, true },
        { BarButton::Next, true },
        // Right side, default order: Volume | Subtitles, Audio | Chapters, Video | Info,
        // (madVR, shaders), Settings | Fullscreen, Power (the power menu, bottom right corner)
        { BarButton::Volume, true }, { BarButton::Subtitles, true }, { BarButton::Audio, true },
        { BarButton::Chapters, true }, // only drawn while the file has chapters
        { BarButton::Video, true },
        { BarButton::VideoInfo, CTvMode::IsOn(TvOpt::VideoInfoButton) },
        { BarButton::AudioInfo, CTvMode::IsOn(TvOpt::AudioInfoButton) },
        { BarButton::Stats, CTvMode::IsOn(TvOpt::StatsButton) },
        { BarButton::Shaders, false },
        { BarButton::Settings, true },
        { BarButton::Fullscreen, true },
        { BarButton::Power, true },
    };
    for (size_t k = 0; k < _countof(defaults); k++) {
        const BarItem& d = defaults[k];
        if (!seen[static_cast<int>(d.id)]) {
            // a new button goes right after its neighbour in the default order (e.g. Video after
            // Audio); without one, after the last item of the same side
            auto pos = items.end();
            if (IsLeftButton(d.id)) {
                pos = std::find_if(items.begin(), items.end(), [](const BarItem& it) { return !IsLeftButton(it.id); });
            }
            if (k > 0) {
                const BarButton before = defaults[k - 1].id;
                auto it = std::find_if(items.begin(), items.end(), [before](const BarItem& x) { return x.id == before; });
                if (it != items.end() && IsLeftButton(before) == IsLeftButton(d.id)) {
                    pos = it + 1;
                }
            }
            items.insert(pos, d);
        }
    }
    // Left (transport) buttons first, each side in its own order
    std::stable_partition(items.begin(), items.end(), [](const BarItem& it) { return IsLeftButton(it.id); });
    // Video info and Audio info are one "Info" button now (OK switches inside): no Audio info button
    items.erase(std::remove_if(items.begin(), items.end(), [](const BarItem& it) { return it.id == BarButton::AudioInfo; }), items.end());
    // Separators: until the buttons are first edited, one between each kind of button on the right
    // (volume | subtitles, audio | chapters, video | info, madVR, shaders, settings | fullscreen, power)
    if (CTvMode::GetText(_T("BarSeparatorsSet")).IsEmpty() &&
        std::none_of(items.begin(), items.end(), [](const BarItem& it) { return it.id == BarButton::Separator; })) {
        auto groupOf = [](BarButton b) {
            switch (b) {
                case BarButton::Stats: case BarButton::Shaders: case BarButton::VideoInfo: case BarButton::AudioInfo:
                case BarButton::Settings: return 1;
                case BarButton::Subtitles: case BarButton::Audio: return 2;
                case BarButton::Video: case BarButton::Chapters: return 5;
                case BarButton::Fullscreen: case BarButton::Power: return 3;
                default: return 4; // volume
            }
        };
        for (size_t i = 1; i < items.size(); i++) {
            const BarItem& a = items[i - 1];
            const BarItem& b = items[i];
            if (!IsLeftButton(a.id) && !IsLeftButton(b.id) && groupOf(a.id) != groupOf(b.id)) {
                items.insert(items.begin() + i, { BarButton::Separator, true });
                i++;
            }
        }
    }
    // The madVR button follows the renderer (shown while madVR is MPC-HC's renderer) until it is
    // switched on or off by hand in Bar buttons; from then on that choice stays.
    if (CTvMode::GetText(_T("MadVrButtonSet")).IsEmpty()) {
        for (auto& it : items) {
            if (it.id == BarButton::Stats) {
                it.on = IsMadVrRenderer();
            }
        }
    }
    return items;
}

void CTvMenu::SaveBarLayout(const std::vector<BarItem>& items)
{
    CString s;
    for (const BarItem& it : items) {
        if (!s.IsEmpty()) {
            s += _T(",");
        }
        s.AppendFormat(_T("%d:%d"), static_cast<int>(it.id), it.on ? 1 : 0);
    }
    CTvMode::SetText(_T("BarButtons"), s);
    CTvMode::SetText(_T("BarSeparatorsSet"), _T("1")); // separators are now the ones stored above
}

// madVR panel ---------------------------------------------------------------
// madVR keeps display-related settings (e.g. the HDR mode) per display *device*. The device to
// change is the one madVR is using right now (shown bold in madVR's settings): the monitor the
// video window is on, matched by its EDID (madVR stores each device's EDID; Windows keeps the
// 128-byte base block, which carries maker, model and serial), falling back to the device name.
// Setting IDs come from madVR's own settings enumeration (IMadVRSettings2) on 0.92.17; madVR
// says the short IDs never change. Left/Right steps through `values` only.
namespace
{
    struct MadVrChoice {
        LPCWSTR value; // madVR's stored text
        LPCWSTR name;  // shown in the panel
    };
    // Choice: text value; List: int value with names; Int: number; Bool: on/off; Heading: section title
    enum class MvKind { Choice, List, Int, Bool, Heading };
    // madVR submenus (MadVrSettingDef::sub)
    enum MvSub { MvRoot = 0, MvHdr, MvCalibration, MvProperties, MvSmoothMotion, MvUpRefine, MvModes, MvTradeQuality, MvSubCount };
    struct MadVrSettingDef {
        MvKind kind = MvKind::Heading;
        int sub = MvRoot;
        bool device = true;               // path below the display's device folder, else a full path
        LPCWSTR id = L"";
        LPCWSTR label = L"";
        std::vector<MadVrChoice> values;  // Choice: Left/Right switch between these
        std::vector<MadVrChoice> others;  // Choice: other valid values, only shown, never set from here
        std::vector<int> listValues;      // List: stored values ...
        std::vector<CStringW> listNames;  // ... and their names
        int minV = 0, maxV = 0;           // Int: range
        int step = 1, bigFrom = INT_MAX, bigStep = 1; // Int: step (bigStep from bigFrom up)
        int div = 1, decimals = 0;        // Int: shown as value / div with this many decimals
        LPCWSTR unit = L"";
        LPCWSTR zeroText = nullptr;       // Int: shown instead of 0 (e.g. "auto")
        bool invert = false;              // Bool: the checkbox means the stored value is false
        LPCWSTR showIfId = nullptr;       // shown only when that setting (same submenu, itself shown) ...
        LPCWSTR showIfValue = nullptr;    // ... has this raw value ("true", "toneMapMath", "5") ...
        bool showIfNot = false;           // ... or, with this, anything else
        MadVrSettingDef If(LPCWSTR id_, LPCWSTR value, bool notEqual = false) const
        {
            MadVrSettingDef d = *this;
            d.showIfId = id_;
            d.showIfValue = value;
            d.showIfNot = notEqual;
            return d;
        }
        int col = 0;                      // two-column submenus: 0 left, 1 right
        bool paired = false;              // shown inside the on/off row just above it (OK on/off,
                                          // Left / Right change this value while it's on)
        MadVrSettingDef Col(int c) const
        {
            MadVrSettingDef d = *this;
            d.col = c;
            return d;
        }
        MadVrSettingDef Paired() const
        {
            MadVrSettingDef d = *this;
            d.paired = true;
            return d;
        }
    };
    MadVrSettingDef MvHead(int sub, LPCWSTR label)
    {
        MadVrSettingDef d;
        d.sub = sub;
        d.label = label;
        return d;
    }
    MadVrSettingDef MvChoice(int sub, LPCWSTR id, LPCWSTR label, std::vector<MadVrChoice> values,
                             std::vector<MadVrChoice> others = {}, bool device = true)
    {
        MadVrSettingDef d;
        d.kind = MvKind::Choice;
        d.sub = sub;
        d.device = device;
        d.id = id;
        d.label = label;
        d.values = std::move(values);
        d.others = std::move(others);
        return d;
    }
    MadVrSettingDef MvList(int sub, LPCWSTR id, LPCWSTR label, std::vector<int> values, std::vector<CStringW> names, bool device = true)
    {
        MadVrSettingDef d;
        d.kind = MvKind::List;
        d.sub = sub;
        d.device = device;
        d.id = id;
        d.label = label;
        d.listValues = std::move(values);
        d.listNames = std::move(names);
        return d;
    }
    // List with values 0..n-1 named by `names`
    MadVrSettingDef MvIndex(int sub, LPCWSTR id, LPCWSTR label, std::vector<CStringW> names, bool device = true)
    {
        std::vector<int> v;
        for (int i = 0; i < static_cast<int>(names.size()); i++) {
            v.push_back(i);
        }
        return MvList(sub, id, label, std::move(v), std::move(names), device);
    }
    MadVrSettingDef MvInt(int sub, LPCWSTR id, LPCWSTR label, int minV, int maxV, int step, LPCWSTR unit = L"", bool device = true)
    {
        MadVrSettingDef d;
        d.kind = MvKind::Int;
        d.sub = sub;
        d.device = device;
        d.id = id;
        d.label = label;
        d.minV = minV;
        d.maxV = maxV;
        d.step = step;
        d.unit = unit;
        return d;
    }
    // Int shown as value / div (e.g. madVR's 10 shown as "1.0")
    MadVrSettingDef MvDec(int sub, LPCWSTR id, LPCWSTR label, int minV, int maxV, int step, int div, int decimals, bool device = true)
    {
        MadVrSettingDef d = MvInt(sub, id, label, minV, maxV, step, L"", device);
        d.div = div;
        d.decimals = decimals;
        return d;
    }
    MadVrSettingDef MvBool(int sub, LPCWSTR id, LPCWSTR label, bool device = true, bool invert = false)
    {
        MadVrSettingDef d;
        d.kind = MvKind::Bool;
        d.sub = sub;
        d.device = device;
        d.id = id;
        d.label = label;
        d.invert = invert;
        return d;
    }
    std::vector<CStringW> Names(std::initializer_list<LPCWSTR> list)
    {
        std::vector<CStringW> v;
        for (LPCWSTR s : list) {
            v.push_back(s);
        }
        return v;
    }

    // The madVR panel, in madVR's own page order (see docs/madvr-settings-map.md for every ID).
    const std::vector<MadVrSettingDef>& MadVrDefs()
    {
        static std::vector<MadVrSettingDef> defs;
        if (!defs.empty()) {
            return defs;
        }
        auto add = [&](MadVrSettingDef d) { defs.push_back(std::move(d)); };
        const wchar_t* tm = L"passthrough"; // tone mapping rows: shown unless HDR is passthrough
        auto pct = [](int from, int to, int step, LPCWSTR suffix0) {
            std::vector<CStringW> v;
            for (int i = from; i <= to; i += step) {
                CStringW s;
                s.Format(L"%d%%", i);
                if (i == 50 && suffix0) {
                    s += suffix0;
                }
                v.push_back(s);
            }
            return v;
        };

        // --- root: frame matching (the rest of the root is built in BuildRows)
        add(MvBool(MvRoot, L"displayModes\\enableDisplayModeChanger", L"Frame matching"));

        // --- HDR, laid out like madVR's "hdr" page: left column = mode, tone mapping, scene
        // detection, HSTM; right column = curve, brightness speed, dynamic. Custom gamut and 3DLUT
        // tone mapping are left out. An on/off with a number is one row (the number is Paired()).
        const auto tmOn = [&](MadVrSettingDef d) { return d.If(L"hdr\\hdrMode", tm, true); };
        add(MvChoice(MvHdr, L"hdr\\hdrMode", L"HDR",
                     { { L"auto", L"Let madVR decide" }, { L"passthrough", L"Passthrough to display" },
                       { L"toneMapMath", L"Tone map (pixel shaders)" } },
                     { { L"toneMap3dlut", L"Tone map (external 3DLUT)" } }));
        add(MvBool(MvHdr, L"hdr\\sendHdrMetadata", L"Send HDR metadata to the display").If(L"hdr\\hdrMode", tm));
        add(tmOn(MvHead(MvHdr, L"Tone mapping")));
        {
            MadVrSettingDef d = MvInt(MvHdr, L"hdr\\hdrNits", L"Display peak luminance", 20, 10000, 10, L" nits");
            d.bigFrom = 1000;
            d.bigStep = 100;
            add(tmOn(d));
        }
        add(tmOn(MvIndex(MvHdr, L"hdr\\bakedInDesat", L"Global desaturation", pct(0, 200, 10, L" (recommended)"))));
        add(tmOn(MvIndex(MvHdr, L"hdr\\highlightSat", L"Highlight saturation",
                         Names({ L"-4", L"-3", L"-2", L"-1", L"balanced", L"+1", L"+2", L"+3", L"+4", L"+5" }))));
        add(tmOn(MvIndex(MvHdr, L"hdr\\hdrHueShift", L"Color tweaks for fire & explosions",
                         Names({ L"high strength", L"balanced", L"disabled" }))));
        add(tmOn(MvIndex(MvHdr, L"hdr\\hdrHighlightRecovery", L"Highlight recovery",
                         Names({ L"disabled", L"low", L"medium", L"high", L"very high", L"are you nuts!?" }))));
        add(tmOn(MvInt(MvHdr, L"hdr\\hdrHighlightRecoveryStrength", L"Highlight recovery strength", 0, 1000, 10)));
        add(tmOn(MvIndex(MvHdr, L"hdr\\hdrContrastRecovery", L"Contrast recovery",
                         Names({ L"disabled", L"Manni's L80", L"Soulnight's Mirror", L"Neo-XP's Equalizer", L"Fer15's old Medium",
                                 L"Neo-XP's 1.0 High", L"Log Very Low", L"Log Very Low - Low", L"Log Low", L"Log Low - Medium",
                                 L"Log Medium", L"Log Medium - High", L"Log High", L"Log High - Very High", L"Log Very High",
                                 L"Log Very Very Low", L"Log Very Very Very Low" }))));
        add(tmOn(MvIndex(MvHdr, L"hdr\\hdrShadowRecovery", L"Shadow recovery",
                         Names({ L"disabled", L"low", L"medium", L"high", L"very high", L"extreme", L"insane" }))));
        add(tmOn(MvBool(MvHdr, L"hdr\\applyDynamicClipping", L"Dynamic clipping")));
        add(MvInt(MvHdr, L"hdr\\dynamicRangeRecoveryStrength", L"Dynamic clipping value", 0, 100, 5).Paired());
        add(tmOn(MvBool(MvHdr, L"hdr\\clip4000", L"Clip at 4000 nits")));
        add(tmOn(MvBool(MvHdr, L"hdr\\useStim", L"Use \"Stim\" lum method")));
        add(tmOn(MvBool(MvHdr, L"hdr\\hdrOutputHdr", L"Output video in HDR format")));
        add(tmOn(MvHead(MvHdr, L"Scene detection")));
        add(tmOn(MvBool(MvHdr, L"hdr\\noSceneChange", L"No scene change if APL within")));
        add(MvInt(MvHdr, L"hdr\\nscPercent", L"percent", 0, 100, 1, L" %").Paired());
        add(MvInt(MvHdr, L"hdr\\nscNits", L"... or nits", 0, 100, 1, L" nits").If(L"hdr\\noSceneChange", L"true"));
        add(tmOn(MvInt(MvHdr, L"hdr\\M2Low2New", L"No scene cut if M2 <", 0, 1000, 1)));
        add(tmOn(MvInt(MvHdr, L"hdr\\M1High", L"Definite cut if M1 >", 0, 10000, 10)));
        add(tmOn(MvInt(MvHdr, L"hdr\\M1Thresh2New", L"M1 threshold", 0, 1000, 1)));
        add(tmOn(MvInt(MvHdr, L"hdr\\M2Thresh2New", L"M2 threshold", 0, 1000, 1)));
        add(tmOn(MvBool(MvHdr, L"hdr\\adjSceneCutApl", L"Adjust scene cut for APL")));

        add(tmOn(MvHead(MvHdr, L"HSTM")));
        add(tmOn(MvBool(MvHdr, L"hdr\\limitContrastRecovery", L"Limit HSTM (min)")));
        add(MvInt(MvHdr, L"hdr\\limitContrastRecoveryMin", L"Limit HSTM min", 0, 1000, 5).Paired());
        add(MvInt(MvHdr, L"hdr\\limitContrastRecoveryMax", L"Limit HSTM max", 0, 1000, 5).If(L"hdr\\limitContrastRecovery", L"true"));
        add(tmOn(MvBool(MvHdr, L"hdr\\hstmNoBrighten", L"No brightening")));
        add(tmOn(MvBool(MvHdr, L"hdr\\hstmNoDarken", L"No darkening")));
        add(tmOn(MvBool(MvHdr, L"hdr\\flickerProtect", L"Flicker protection")));
        add(MvInt(MvHdr, L"hdr\\flickerProtectMs", L"Flicker protection time", 0, 5000, 50, L" ms").Paired());
        add(tmOn(MvBool(MvHdr, L"hdr\\dynShadowBoost", L"Shadow recovery only if % of pixels")));
        add(MvInt(MvHdr, L"hdr\\dynShadowBoostPercent", L"percent", 0, 100, 1, L" %").Paired());
        add(MvInt(MvHdr, L"hdr\\dynShadowBoostNits", L"... are below", 0, 100, 1, L" nits").If(L"hdr\\dynShadowBoost", L"true"));
        // right column
        add(tmOn(MvHead(MvHdr, L"Curve")).Col(1));
        add(tmOn(MvIndex(MvHdr, L"hdr\\tmCurve", L"TM curve",
                         Names({ L"pluto", L"mars", L"saturn", L"mercury", L"phoebe", L"ganymede", L"luna" }))).Col(1));
        {
            std::vector<CStringW> punch;
            for (int i = 1; i <= 20; i++) {
                CStringW s;
                s.Format(L"%d", i);
                punch.push_back(s);
            }
            add(tmOn(MvIndex(MvHdr, L"hdr\\tmCurvePunch", L"Punch", punch)).Col(1));
        }
        add(tmOn(MvBool(MvHdr, L"hdr\\useCustomTmCurves", L"Use custom TM curves")).Col(1));
        add(tmOn(MvHead(MvHdr, L"Brightness adjustment speed")).Col(1));
        const LPCWSTR speedIds[] = { L"hdr\\dif0speed2", L"hdr\\dif20speed2", L"hdr\\dif50speed2", L"hdr\\dif100speed2",
                                     L"hdr\\dif200speed2", L"hdr\\dif400speed2", L"hdr\\dif800speed2" };
        const LPCWSTR speedNames[] = { L"No difference", L"20% difference", L"50% difference", L"100% difference",
                                       L"200% difference", L"400% difference", L"800% difference" };
        for (int i = 0; i < 7; i++) {
            add(tmOn(MvInt(MvHdr, speedIds[i], speedNames[i], 0, 5000, 10)).Col(1));
        }
        add(tmOn(MvInt(MvHdr, L"hdr\\flickerRiskMul", L"Flicker risk multiplier", 1, 100, 1)).Col(1));
        add(tmOn(MvHead(MvHdr, L"Dynamic")).Col(1));
        add(tmOn(MvBool(MvHdr, L"hdr\\applyTargetNitsSelection", L"Dynamic target nits")).Col(1));
        add(MvInt(MvHdr, L"hdr\\dynamicTuning", L"Dynamic target nits value", 0, 500, 5).Paired().Col(1));
        add(tmOn(MvBool(MvHdr, L"hdr\\dontAddPeakNits", L"Don't add peak nits")).Col(1));
        add(tmOn(MvBool(MvHdr, L"hdr\\disableAvgH", L"avgHighlights ceiling", true, true)).Col(1));
        {
            std::vector<int> v;
            std::vector<CStringW> n;
            for (int i = 150; i <= 400; i += 25) {
                v.push_back(i);
                CStringW s;
                s.Format(L"%d.%02dx", i / 100, i % 100);
                n.push_back(s);
            }
            add(MvList(MvHdr, L"hdr\\avgHFactor", L"avgHighlights ceiling value", v, n).Paired().Col(1));
        }
        add(tmOn(MvInt(MvHdr, L"hdr\\noComprLimit", L"No compression limit", 0, 1000, 10)).Col(1));
        {
            std::vector<CStringW> fall = { L"off" };
            for (int i = 90; i >= 0; i -= 10) {
                CStringW s;
                s.Format(L"%d%%", i);
                fall.push_back(s);
            }
            add(tmOn(MvIndex(MvHdr, L"hdr\\fallHighlight", L"FALL highlight mod", fall)).Col(1));
            std::vector<CStringW> detail = { L"off" };
            for (int i = 1; i <= 20; i++) {
                CStringW s;
                s.Format(L"%d", i);
                detail.push_back(s);
            }
            add(tmOn(MvIndex(MvHdr, L"hdr\\fallDetail", L"Detail threshold", detail)).Col(1));
        }
        add(tmOn(MvInt(MvHdr, L"hdr\\skyStrength", L"Sky strength", 0, 100, 1)).Col(1));
        add(tmOn(MvInt(MvHdr, L"hdr\\maxSkyWidth", L"Sky max width", 0, 100, 1)).Col(1));
        add(tmOn(MvInt(MvHdr, L"hdr\\minSky", L"Sky hill height min", 0, 100, 1)).Col(1));
        add(tmOn(MvInt(MvHdr, L"hdr\\maxSky", L"Sky hill height max", 0, 100, 1)).Col(1));
        // --- Calibration
        add(MvChoice(MvCalibration, L"calibration\\calibrate", L"Calibration",
                     { { L"disable calibration controls for this display", L"Disabled" },
                       { L"this display is already calibrated", L"Already calibrated" } },
                     { { L"calibrate this display by using yCMS", L"yCMS" },
                       { L"calibrate this display by using an external 3dlut file", L"External 3DLUT" } }));
        add(MvBool(MvCalibration, L"calibration\\disableGpuGammaRamps", L"Disable GPU gamma ramps"));
        add(MvChoice(MvCalibration, L"calibration\\displayPrimaries", L"Primaries / gamut",
                     { { L"BT.709 (HD)", L"BT.709 (HD)" }, { L"BT.601 (SD)", L"BT.601 (SD)" }, { L"PAL", L"PAL" },
                       { L"BT.2020 (UHD)", L"BT.2020 (UHD)" }, { L"DCI-P3", L"DCI-P3" }, { L"something else", L"something else" },
                       { L"Auto", L"Auto" } }).If(L"calibration\\calibrate", L"this display is already calibrated"));
        add(MvChoice(MvCalibration, L"calibration\\displayGammaCurve", L"Transfer function / gamma",
                     { { L"pure power curve", L"pure power curve" }, { L"BT.709/601 curve", L"BT.709/601 curve" },
                       { L"something else", L"something else" } }).If(L"calibration\\calibrate", L"this display is already calibrated"));
        {
            std::vector<MadVrChoice> gammas;
            static std::vector<CStringW> gammaText; // keeps the strings alive for the LPCWSTR pairs
            gammaText.clear();
            for (int g = 160; g <= 300; g += 5) {
                CStringW s;
                s.Format(L"%d.%02d", g / 100, g % 100);
                gammaText.push_back(s);
            }
            for (const CStringW& s : gammaText) {
                gammas.push_back({ s.GetString(), s.GetString() });
            }
            add(MvChoice(MvCalibration, L"calibration\\displayGammaValue", L"Gamma value", gammas)
                    .If(L"calibration\\calibrate", L"this display is already calibrated"));
        }
        add(MvBool(MvCalibration, L"calibration\\report2020", L"Report BT.2020 to display (Nvidia)"));

        // --- Properties
        add(MvChoice(MvProperties, L"properties\\levels", L"RGB output levels",
                     { { L"PC Levels", L"PC levels (0-255)" }, { L"TV Levels", L"TV levels (16-235)" },
                       { L"Custom", L"Custom" }, { L"Auto", L"Auto" } }));
        add(MvInt(MvProperties, L"properties\\black", L"Custom black", 0, 48, 1).If(L"properties\\levels", L"Custom"));
        add(MvInt(MvProperties, L"properties\\white", L"Custom white", 200, 255, 1).If(L"properties\\levels", L"Custom"));
        add(MvList(MvProperties, L"properties\\displayBitdepth", L"Native display bitdepth", { 0, 3, 4, 5, 6, 7, 8, 9, 10 },
                   Names({ L"auto", L"3 bit", L"4 bit", L"5 bit", L"6 bit", L"7 bit", L"8 bit", L"9 bit", L"10 bit or higher" })));
        add(MvChoice(MvProperties, L"properties\\3dFormat", L"3D format",
                     { { L"auto", L"auto" }, { L"none", L"none" }, { L"side-by-side", L"side-by-side" },
                       { L"top-and-bottom", L"top-and-bottom" }, { L"line alternative", L"line alternative" },
                       { L"column alternative", L"column alternative" } }));
        add(MvBool(MvProperties, L"properties\\swapEyes", L"Swap left / right eye"));

        // --- Smooth motion (global)
        add(MvBool(MvSmoothMotion, L"rendering\\smoothMotion\\smoothMotionEnabled", L"Smooth motion", false));
        add(MvChoice(MvSmoothMotion, L"rendering\\smoothMotion\\smoothMotionMode", L"Use it",
                     { { L"avoidJudder", L"Only if there would be judder" },
                       { L"almostAlways", L"... or if refresh is a multiple of the frame rate" },
                       { L"always", L"Always" } }, {}, false)
                .Paired());

        // --- Upscaling refinement (global, active profile)
        const LPCWSTR up = L"scalingParent\\upRefine\\";
        auto U = [&](LPCWSTR leaf) {
            static std::vector<CStringW> keep; // stable storage for the joined paths (never reallocates)
            if (keep.capacity() < 64) {
                keep.reserve(64);
            }
            keep.push_back(CStringW(up) + leaf);
            return keep.back().GetString();
        };
        add(MvBool(MvUpRefine, U(L"upRefSharpenEdges"), L"Sharpen edges", false));
        add(MvDec(MvUpRefine, U(L"upRefSharpenEdgesStrength"), L"Sharpen edges strength", 1, 40, 1, 10, 1, false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefCrispenEdges"), L"Crispen edges", false));
        add(MvDec(MvUpRefine, U(L"upRefCrispenEdgesStrength"), L"Crispen edges strength", 1, 40, 1, 10, 1, false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefCrispenEdgesLL"), L"Crispen edges in linear light", false).If(U(L"upRefCrispenEdges"), L"true"));
        add(MvBool(MvUpRefine, U(L"upRefThinEdges"), L"Thin edges", false));
        add(MvDec(MvUpRefine, U(L"upRefThinEdgesStrength"), L"Thin edges strength", 1, 80, 1, 10, 1, false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefEnhanceDetail"), L"Enhance detail", false));
        add(MvDec(MvUpRefine, U(L"upRefEnhanceDetailStrength"), L"Enhance detail strength", 1, 40, 1, 10, 1, false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefLumaSharpen"), L"LumaSharpen", false));
        add(MvDec(MvUpRefine, U(L"upRefLumaSharpenStrength"), L"LumaSharpen strength", 10, 1000, 5, 100, 2, false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefAdaptiveSharpen"), L"AdaptiveSharpen", false));
        add(MvDec(MvUpRefine, U(L"upRefAdaptiveSharpenStrength"), L"AdaptiveSharpen strength", 1, 100, 1, 10, 1, false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefAdaptiveSharpenLinear"), L"AdaptiveSharpen in linear light", false).If(U(L"upRefAdaptiveSharpen"), L"true"));
        add(MvBool(MvUpRefine, U(L"upRefSharpenAB"), L"Anti-bloating filter", false));
        add(MvInt(MvUpRefine, U(L"upRefSharpenABStrength"), L"Anti-bloating strength", 25, 150, 25, L" %", false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefSharpenAR"), L"Anti-ringing filter", false));
        add(MvBool(MvUpRefine, U(L"upRefSoftenEdges"), L"Soften edges", false));
        add(MvInt(MvUpRefine, U(L"upRefSoftenEdgesStrength"), L"Soften edges strength", 1, 3, 1, L"", false).Paired());
        add(MvBool(MvUpRefine, U(L"upRefAddGrain"), L"Add grain", false));
        add(MvInt(MvUpRefine, U(L"upRefAddGrainStrength"), L"Add grain strength", 1, 4, 1, L"", false).Paired());
        add(MvBool(MvUpRefine, U(L"superRes"), L"SuperRes", false));
        add(MvInt(MvUpRefine, U(L"superResStrength"), L"SuperRes strength", 1, 4, 1, L"", false).Paired());
        add(MvBool(MvUpRefine, U(L"superResLinear"), L"SuperRes in linear light", false).If(U(L"superRes"), L"true"));
        add(MvBool(MvUpRefine, U(L"superResAB"), L"SuperRes anti-bloating", false).If(U(L"superRes"), L"true"));
        add(MvInt(MvUpRefine, U(L"superResABStrength"), L"SuperRes anti-bloating strength", 25, 150, 25, L" %", false).Paired());
        add(MvBool(MvUpRefine, U(L"superResAR"), L"SuperRes anti-ringing", false).If(U(L"superRes"), L"true"));
        add(MvBool(MvUpRefine, U(L"refineOnce"), L"Refine only once after upscaling", false));

        // --- trade quality for performance (rendering\tradeQuality, global): madVR's own page and order;
        // its hidden copyback / 3dlut options are left out like on madVR's page
        const auto T = [](LPCWSTR id) { return CStringW(L"rendering\\tradeQuality\\") + id; };
        static std::vector<CStringW> tq; // ids must outlive the defs (LPCWSTR)
        const std::pair<LPCWSTR, LPCWSTR> tradeOptions[] = {
            { L"fastSubtitles", L"Optimize subtitles for performance instead of quality" },
            { L"dxvaChromaWhenDecode", L"Use DXVA chroma upscaling when doing native DXVA decoding" },
            { L"dxvaChromaWhenDeint", L"Use DXVA chroma upscaling when doing DXVA deinterlacing" },
            { L"gammaDithering", L"Don't use linear light for dithering" },
            { L"noGradientAngles", L"Don't analyze gradient angles for debanding" },
            { L"dontRerenderFades", L"Don't rerender frames when fade in / out is detected" },
            { L"downscalePath", L"Scale chroma separately, if it saves performance" },
            { L"dumbToneMapping", L"Compromise on HDR tone & gamut mapping accuracy" },
            { L"fastRepair", L"Compromise on HDR luminance channel quality" },
            { L"staticHdr", L"Don't measure HDR frame peak luminance" },
            { L"mayLoseBtb", L"Lose BTB and WTW if it improves performance" },
            { L"gammaBlending", L"Don't use linear light for smooth motion frame blending" },
            { L"10bitChroma", L"Use 10-bit chroma buffer instead of 16-bit" },
            { L"10bitLuma", L"Use 10-bit image buffer instead of 16-bit" },
            { L"customShadersTv", L"Run custom pixel shaders in video levels instead of PC levels" },
            { nullptr, L"Should not be used" },
            { L"halfDxvaDeintFramerate", L"Use half frame rate for DXVA deinterlacing" },
            { L"dxvaTrustMatrix", L"Trust DXVA color & levels conversion" },
        };
        tq.reserve(_countof(tradeOptions));
        for (const auto& o : tradeOptions) {
            if (!o.first) {
                add(MvHead(MvTradeQuality, o.second));
                continue;
            }
            tq.push_back(T(o.first));
            add(MvBool(MvTradeQuality, tq.back().GetString(), o.second, false));
        }
        return defs;
    }

    // How many of madVR's "trade quality for performance" options are on (-1: unknown)
    int MadVrTradeQualityCount(IUnknown* madVr)
    {
        CComQIPtr<IMadVRSettings> s(madVr);
        if (!s) {
            return -1;
        }
        int on = 0;
        for (const auto& d : MadVrDefs()) {
            BOOL b = FALSE;
            if (d.sub == MvTradeQuality && d.kind == MvKind::Bool && s->SettingsGetBoolean(d.id, &b) && b) {
                on++;
            }
        }
        return on;
    }

    LPCWSTR MadVrSubTitle(int sub)
    {
        switch (sub) {
            case MvHdr: return L"madVR  HDR";
            case MvCalibration: return L"madVR  Calibration";
            case MvProperties: return L"madVR  Properties";
            case MvSmoothMotion: return L"madVR  Smooth motion";
            case MvUpRefine: return L"madVR  Upscaling refinement";
            case MvModes: return L"madVR  Display modes";
            case MvTradeQuality: return L"madVR  Trade quality for performance";
            default: return L"madVR settings";
        }
    }
    // EDID (base block) and friendly name of the monitor showing `hWnd`
    bool MonitorEdidForWindow(HWND hWnd, std::vector<BYTE>& edid, CStringW& friendlyName)
    {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            return false;
        }
        UINT32 nPaths = 0, nModes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) {
            return false;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) != ERROR_SUCCESS) {
            return false;
        }
        for (UINT32 i = 0; i < nPaths; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || _wcsicmp(src.viewGdiDeviceName, mi.szDevice) != 0) {
                continue;
            }
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
            tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = sizeof(tgt);
            tgt.header.adapterId = paths[i].targetInfo.adapterId;
            tgt.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) {
                continue;
            }
            friendlyName = tgt.monitorFriendlyDeviceName;
            // "\\?\DISPLAY#ACR0730#5&546ba11&0&UID4352#{guid}" -> Enum\DISPLAY\ACR0730\5&546ba11&0&UID4352
            CStringW devPath = tgt.monitorDevicePath;
            int pos = 0;
            CStringW part, hwid, inst;
            for (int n = 0; !(part = devPath.Tokenize(L"#", pos)).IsEmpty(); n++) {
                if (n == 1) {
                    hwid = part;
                } else if (n == 2) {
                    inst = part;
                }
            }
            if (hwid.IsEmpty() || inst.IsEmpty()) {
                return !friendlyName.IsEmpty();
            }
            CStringW key;
            key.Format(L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s\\%s\\Device Parameters", hwid.GetString(), inst.GetString());
            BYTE buf[1024];
            DWORD size = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EDID", RRF_RT_REG_BINARY, nullptr, buf, &size) == ERROR_SUCCESS && size >= 128) {
                edid.assign(buf, buf + size);
            }
            return true;
        }
        return false;
    }
}

// Debug aid: with MPCHC_TV_MADVR_DUMP=<file>, the madVR panel writes madVR's whole settings tree
// (folders and values: path, id, name, type, current value) to that file once. Read only.
namespace
{
    void DumpMadVrFolder(IMadVRSettings2* s, const CStringW& path, FILE* f, int depth)
    {
        if (depth > 8) {
            return;
        }
        for (int i = 0;; i++) {
            wchar_t id[256], name[512], type[64];
            int a = _countof(id), b = _countof(name), c = _countof(type);
            if (!s->SettingsEnumValues(path, i, id, name, type, &a, &b, &c)) {
                break;
            }
            const CStringW full = path.IsEmpty() ? CStringW(id) : path + L"\\" + id;
            CStringW value;
            BOOL bv = FALSE;
            int iv = 0;
            wchar_t sv[1024] = {};
            int svLen = _countof(sv);
            if (_wcsicmp(type, L"boolean") == 0 && s->SettingsGetBoolean(full, &bv)) {
                value = bv ? L"true" : L"false";
            } else if (_wcsicmp(type, L"integer") == 0 && s->SettingsGetInteger(full, &iv)) {
                value.Format(L"%d", iv);
            } else if (s->SettingsGetString(full, sv, &svLen)) {
                value = sv;
            }
            fwprintf(f, L"V | %s | %s | %s | %s\n", full.GetString(), name, type, value.GetString());
        }
        for (int i = 0;; i++) {
            wchar_t id[256], name[512], type[64];
            int a = _countof(id), b = _countof(name), c = _countof(type);
            if (!s->SettingsEnumFolders(path, i, id, name, type, &a, &b, &c)) {
                break;
            }
            const CStringW full = path.IsEmpty() ? CStringW(id) : path + L"\\" + id;
            fwprintf(f, L"F | %s | %s | %s\n", full.GetString(), name, type);
            DumpMadVrFolder(s, full, f, depth + 1);
        }
    }
}

static void MaybeDumpMadVrSettings(IUnknown* madVr)
{
    static bool done = false;
    wchar_t file[MAX_PATH] = {};
    if (done || !madVr || GetEnvironmentVariableW(L"MPCHC_TV_MADVR_DUMP", file, MAX_PATH) == 0) {
        return;
    }
    CComQIPtr<IMadVRSettings2> s(madVr);
    FILE* f = nullptr;
    if (s && _wfopen_s(&f, file, L"w, ccs=UTF-8") == 0 && f) {
        // the root's name isn't documented: try the usual ways, then the known top folders
        for (LPCWSTR root : { L"", L"\\" }) {
            fwprintf(f, L"# root \"%s\"\n", root);
            DumpMadVrFolder(s, root, f, 0);
        }
        for (LPCWSTR top : { L"rendering", L"processing", L"scalingParent", L"scaling", L"devices", L"settings", L"general" }) {
            fwprintf(f, L"# folder %s\n", top);
            DumpMadVrFolder(s, top, f, 0);
        }
        fclose(f);
        done = true;
    }
}

// madVR's folder for the display device in use ("devices\<id>"), empty if not found.
CStringW CTvMenu::MadVrActiveDevice(CStringW* deviceName)
{
    MaybeDumpMadVrSettings(m_pMadVr);
    CComQIPtr<IMadVRSettings2> s(m_pMadVr); // same madVR filter object
    if (!s || !m_pMainFrame || !m_pMainFrame->m_pVideoWnd) {
        return L"";
    }
    std::vector<BYTE> edid;
    CStringW friendly;
    MonitorEdidForWindow(m_pMainFrame->m_pVideoWnd->m_hWnd, edid, friendly);

    // Only a single, unambiguous match counts (e.g. two identical monitors with no serial in
    // their EDID would both match): otherwise nothing is changed and the reason is shown.
    CStringW byEdid, byEdidLabel, byName, byNameLabel;
    int edidMatches = 0, nameMatches = 0;
    for (int i = 0;; i++) {
        wchar_t id[128], name[256], type[64];
        int idLen = _countof(id), nameLen = _countof(name), typeLen = _countof(type);
        if (!s->SettingsEnumFolders(L"devices", i, id, name, type, &idLen, &nameLen, &typeLen)) {
            break;
        }
        CStringW dev;
        dev.Format(L"devices\\%s", id);
        // the device's identification folder (type "id") holds its EDID
        for (int j = 0; !edid.empty(); j++) {
            wchar_t sid[128], sname[256], stype[64];
            int a = _countof(sid), b = _countof(sname), c = _countof(stype);
            if (!s->SettingsEnumFolders(dev, j, sid, sname, stype, &a, &b, &c)) {
                break;
            }
            if (_wcsicmp(stype, L"id") != 0) {
                continue;
            }
            LPVOID bin = nullptr;
            int size = 0;
            const CStringW path = dev + L"\\" + sid + L"\\edid";
            if (s->SettingsGetBinary(path, &bin, &size) && bin) {
                // as much as both have (at least the 128-byte base block: maker, model, serial)
                const size_t n = std::min(static_cast<size_t>(std::max(size, 0)), edid.size());
                const bool match = n >= 128 && memcmp(bin, edid.data(), n) == 0;
                LocalFree(bin);
                if (match) {
                    if (edidMatches++ == 0) {
                        byEdid = dev;
                        byEdidLabel = name;
                    }
                    break;
                }
            }
        }
        if (!friendly.IsEmpty() && friendly.CompareNoCase(name) == 0) {
            if (nameMatches++ == 0) {
                byName = dev;
                byNameLabel = name;
            }
        }
    }
    m_strMadVrProblem.Empty();
    if (edidMatches == 1) {
        if (deviceName) {
            *deviceName = byEdidLabel;
        }
        return byEdid;
    }
    if (edidMatches > 1) {
        m_strMadVrProblem = L"Several madVR devices match this display";
    } else if (nameMatches == 1) {
        if (deviceName) {
            *deviceName = byNameLabel; // no EDID match (e.g. none readable): the same name
        }
        return byName;
    } else {
        m_strMadVrProblem = edid.empty() ? L"Can't read this display's EDID" : L"This display isn't in madVR's devices yet";
    }
    return L"";
}

// madVR's devices (displays it knows): device folder path ("devices\\<id>") and its name
std::vector<std::pair<CStringW, CStringW>> CTvMenu::MadVrDeviceList()
{
    std::vector<std::pair<CStringW, CStringW>> list;
    CComQIPtr<IMadVRSettings2> s(m_pMadVr);
    if (!s) {
        return list;
    }
    for (int i = 0;; i++) {
        wchar_t id[128], name[256], type[64];
        int idLen = _countof(id), nameLen = _countof(name), typeLen = _countof(type);
        if (!s->SettingsEnumFolders(L"devices", i, id, name, type, &idLen, &nameLen, &typeLen)) {
            break;
        }
        CStringW dev;
        dev.Format(L"devices\\%s", id);
        list.emplace_back(dev, name);
    }
    return list;
}

// Short HDR state for the Video page / madVR panel ("Tone map (pixel shaders)", "Let madVR decide", ...)
CStringW CTvMenu::MadVrHdrSummary()
{
    if (!m_pMadVr) {
        return L"";
    }
    if (m_strMadVrDevice.IsEmpty()) {
        m_strMadVrDevice = MadVrActiveDevice(&m_strMadVrDeviceName);
        m_strMadVrDeviceActive = m_strMadVrDevice;
    }
    if (m_strMadVrDevice.IsEmpty()) {
        return L"";
    }
    const auto& defs = MadVrDefs();
    for (int i = 0; i < static_cast<int>(defs.size()); i++) {
        if (defs[i].kind != MvKind::Heading && defs[i].id && wcscmp(defs[i].id, L"hdr\\hdrMode") == 0) {
            const CStringW v = MadVrValueText(i);
            return v == L"Not available" ? CStringW() : v;
        }
    }
    return L"";
}

// Full madVR path of a setting: below the display's device folder, or as given
CStringW CTvMenu::MadVrPath(int index)
{
    const MadVrSettingDef& d = MadVrDefs()[index];
    return d.device ? m_strMadVrDevice + L"\\" + d.id : CStringW(d.id);
}

// Raw value as text (Choice: madVR's text; List / Int: the number; Bool: "true" / "false")
bool CTvMenu::MadVrGet(int index, CStringW& value)
{
    const auto& defs = MadVrDefs();
    if (index < 0 || index >= static_cast<int>(defs.size())) {
        return false;
    }
    const MadVrSettingDef& d = defs[index];
    CComQIPtr<IMadVRSettings> s(m_pMadVr);
    if (!s || d.kind == MvKind::Heading || (d.device && m_strMadVrDevice.IsEmpty())) {
        return false;
    }
    const CStringW path = MadVrPath(index);
    switch (d.kind) {
        case MvKind::Choice: {
            wchar_t buf[512] = L"";
            int len = _countof(buf);
            if (!s->SettingsGetString(path, buf, &len)) {
                return false;
            }
            value = buf;
            return true;
        }
        case MvKind::List:
        case MvKind::Int: {
            int v = 0;
            if (!s->SettingsGetInteger(path, &v)) {
                return false;
            }
            value.Format(L"%d", v);
            return true;
        }
        case MvKind::Bool: {
            BOOL b = FALSE;
            if (!s->SettingsGetBoolean(path, &b)) {
                return false;
            }
            value = b ? L"true" : L"false";
            return true;
        }
        default:
            return false;
    }
}

// Shown now: always, or when the setting it depends on (same submenu, itself shown) matches
bool CTvMenu::MadVrVisible(int index)
{
    const auto& defs = MadVrDefs();
    if (index < 0 || index >= static_cast<int>(defs.size())) {
        return false;
    }
    const MadVrSettingDef& d = defs[index];
    if (d.device && m_strMadVrDevice.IsEmpty()) {
        return false; // the display in use wasn't identified
    }
    if (!d.showIfId) {
        return d.kind == MvKind::Heading || [&] { CStringW v; return MadVrGet(index, v); }(); // hide what madVR doesn't have
    }
    for (int i = 0; i < static_cast<int>(defs.size()); i++) {
        if (i != index && defs[i].sub == d.sub && defs[i].kind != MvKind::Heading && wcscmp(defs[i].id, d.showIfId) == 0) {
            CStringW v;
            if (!MadVrVisible(i) || !MadVrGet(i, v)) {
                return false;
            }
            const bool equal = v.CompareNoCase(d.showIfValue) == 0;
            return d.showIfNot ? !equal : equal;
        }
    }
    return false;
}

// 0 hidden, 1 shown, 2 shown faded: like MadVrVisible, but a setting that only applies while an
// on / off setting is on (or off) stays in the list, faded, instead of disappearing. Settings that
// depend on a choice (e.g. tone mapping values while HDR is passthrough) are still hidden.
int CTvMenu::MadVrShowState(int index)
{
    const auto& defs = MadVrDefs();
    if (index < 0 || index >= static_cast<int>(defs.size())) {
        return 0;
    }
    const MadVrSettingDef& d = defs[index];
    if (!d.showIfId) {
        return MadVrVisible(index) ? 1 : 0;
    }
    if (d.device && m_strMadVrDevice.IsEmpty()) {
        return 0;
    }
    for (int i = 0; i < static_cast<int>(defs.size()); i++) {
        if (i != index && defs[i].sub == d.sub && defs[i].kind != MvKind::Heading && wcscmp(defs[i].id, d.showIfId) == 0) {
            const int parent = MadVrShowState(i);
            CStringW v, own;
            if (parent == 0 || !MadVrGet(i, v) || (d.kind != MvKind::Heading && !MadVrGet(index, own))) {
                return 0; // madVR doesn't have it
            }
            const bool equal = v.CompareNoCase(d.showIfValue) == 0;
            if (d.showIfNot ? !equal : equal) {
                return parent;
            }
            return defs[i].kind == MvKind::Bool ? 2 : 0;
        }
    }
    return 0;
}

int CTvMenu::MadVrSettingCount()
{
    return static_cast<int>(MadVrDefs().size());
}

CStringW CTvMenu::MadVrLabel(int index)
{
    return MadVrDefs()[std::clamp(index, 0, MadVrSettingCount() - 1)].label;
}

CStringW CTvMenu::MadVrValueText(int index)
{
    const MadVrSettingDef& d = MadVrDefs()[std::clamp(index, 0, MadVrSettingCount() - 1)];
    CStringW cur;
    if (!MadVrGet(index, cur)) {
        return L"Not available";
    }
    switch (d.kind) {
        case MvKind::Bool:
            return (cur == L"true") != d.invert ? L"On" : L"Off";
        case MvKind::Int: {
            const int v = _wtoi(cur);
            if (v == 0 && d.zeroText) {
                return d.zeroText;
            }
            CStringW s;
            if (d.div > 1) {
                s.Format(L"%.*f", d.decimals, static_cast<double>(v) / d.div);
            } else {
                s.Format(L"%d", v);
            }
            return s + d.unit;
        }
        case MvKind::List: {
            const int v = _wtoi(cur);
            for (size_t i = 0; i < d.listValues.size() && i < d.listNames.size(); i++) {
                if (d.listValues[i] == v) {
                    return d.listNames[i];
                }
            }
            return cur;
        }
        case MvKind::Choice:
            for (const auto* list : { &d.values, &d.others }) {
                for (const MadVrChoice& c : *list) {
                    if (cur.CompareNoCase(c.value) == 0) {
                        return c.name;
                    }
                }
            }
            return cur;
        default:
            return L"";
    }
}

void CTvMenu::StepMadVrSetting(int index, int dir)
{
    const MadVrSettingDef& d = MadVrDefs()[std::clamp(index, 0, MadVrSettingCount() - 1)];
    CComQIPtr<IMadVRSettings> s(m_pMadVr);
    CStringW cur;
    if (!s || !MadVrGet(index, cur)) {
        return; // only when the setting (and for device settings the display in use) was found
    }
    // madVR applies changes right away (and saves them, like its own settings window)
    const CStringW path = MadVrPath(index);
    switch (d.kind) {
        case MvKind::Choice: {
            const int n = static_cast<int>(d.values.size());
            if (n == 0) {
                return;
            }
            int pos = -1;
            for (int i = 0; i < n; i++) {
                if (cur.CompareNoCase(d.values[i].value) == 0) {
                    pos = i;
                }
            }
            pos = (pos < 0) ? (dir > 0 ? 0 : n - 1) : (pos + dir + n) % n;
            s->SettingsSetString(path, d.values[pos].value);
            break;
        }
        case MvKind::List: {
            const int n = static_cast<int>(d.listValues.size());
            if (n == 0) {
                return;
            }
            const int v = _wtoi(cur);
            int pos = -1;
            for (int i = 0; i < n; i++) {
                if (d.listValues[i] == v) {
                    pos = i;
                }
            }
            if (pos < 0) { // a value madVR's window set that isn't on our list: nearest in that direction
                pos = dir > 0 ? 0 : n - 1;
                for (int i = 0; i < n; i++) {
                    if (dir > 0 && d.listValues[i] > v) { pos = i; break; }
                    if (dir < 0 && d.listValues[i] < v) { pos = i; }
                }
            } else {
                pos = std::clamp(pos + dir, 0, n - 1); // lists stop at the ends
            }
            s->SettingsSetInteger(path, d.listValues[pos]);
            break;
        }
        case MvKind::Int: {
            const int v = _wtoi(cur);
            const int step = (dir > 0 ? v >= d.bigFrom : v > d.bigFrom) ? d.bigStep : d.step;
            int next = v + dir * step;
            if (dir < 0 && v > d.bigFrom && next < d.bigFrom) {
                next = d.bigFrom; // land on the boundary when coming down
            }
            // a value outside our range (set in madVR's window) is kept until moved towards the range
            s->SettingsSetInteger(path, std::clamp(next, std::min(d.minV, v), std::max(d.maxV, v)));
            break;
        }
        case MvKind::Bool:
            s->SettingsSetBoolean(path, cur == L"true" ? FALSE : TRUE);
            break;
        default:
            break;
    }
}
namespace
{
    // madVR's name for a mode: "2160p23" for 16:9 sizes, else "1366x768p59" (its own examples)
    CStringW MadVrModeName(int w, int h, int hz, bool interlaced)
    {
        CStringW s;
        if (w * 9 == h * 16) {
            s.Format(L"%d%c%d", h, interlaced ? L'i' : L'p', hz);
        } else {
            s.Format(L"%dx%d%c%d", w, h, interlaced ? L'i' : L'p', hz);
        }
        return s;
    }
}

CStringW CTvMenu::MadVrModesInMadVr()
{
    CComQIPtr<IMadVRSettings> s(m_pMadVr);
    if (!s || m_strMadVrDevice.IsEmpty()) {
        return L"";
    }
    LPVOID bin = nullptr;
    int size = 0;
    CStringW text;
    if (s->SettingsGetBinary(m_strMadVrDevice + L"\\displayModes\\displayModesData", &bin, &size) && bin) {
        const INT32* v = static_cast<const INT32*>(bin);
        for (int i = 0; i + 3 < size / 4; i += 4) {
            if (!text.IsEmpty()) {
                text += L", ";
            }
            text += MadVrModeName(v[i], v[i + 1], v[i + 2], v[i + 3] != 0);
        }
        LocalFree(bin);
    }
    return text;
}

// The same list in madVR's stored form: 16 bytes per mode (int32 width, height, refresh, interlaced)
std::vector<BYTE> CTvMenu::MadVrModesAvailableData()
{
    std::vector<BYTE> data;
    if (!m_pMainFrame || !m_pMainFrame->m_pVideoWnd) {
        return data;
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    DEVMODEW cur = {};
    cur.dmSize = sizeof(cur);
    if (!GetMonitorInfoW(MonitorFromWindow(m_pMainFrame->m_pVideoWnd->m_hWnd, MONITOR_DEFAULTTONEAREST), &mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) {
        return data;
    }
    for (int hz : MadVrRatesAt(mi.szDevice, cur)) {
        const INT32 rec[4] = { static_cast<INT32>(cur.dmPelsWidth), static_cast<INT32>(cur.dmPelsHeight), hz, 0 };
        data.insert(data.end(), reinterpret_cast<const BYTE*>(rec), reinterpret_cast<const BYTE*>(rec) + sizeof(rec));
    }
    return data;
}

// Writes madVR's display mode list for the active device. madVR has no binary setter and ignores
// text for this value, so: export madVR's settings record, replace just this one value in it (and
// fix the sizes of the folders around it), import it, and check the result. If anything other than
// this value differs afterwards, the original record is imported again.
// Record layout (checked against madVR 0.92.17's settings.bin): every item is int32 size + body;
// body = type, id, name, help (each int32 length + bytes); folders/devices contain their children.
// A binary item continues with int32 0, int32 (21 + data size), int32 0, int32 1, int32 data size,
// the data, then 9 more bytes.
bool CTvMenu::MadVrWriteDisplayModes(IMadVRSettings* s, const std::vector<BYTE>& data)
{
    if (!s || m_strMadVrDevice.IsEmpty()) { // empty data = no modes (madVR then never switches)
        return false;
    }
    const CStringA device(m_strMadVrDevice.Mid(m_strMadVrDevice.ReverseFind(L'\\') + 1));
    LPVOID buf = nullptr;
    int size = 0;
    if (!s->SettingsExport(&buf, &size) || !buf) {
        return false;
    }
    std::vector<BYTE> orig(static_cast<BYTE*>(buf), static_cast<BYTE*>(buf) + size);
    LocalFree(buf);

    auto i32 = [&](size_t o) -> INT32 { return o + 4 <= orig.size() ? *reinterpret_cast<const INT32*>(&orig[o]) : -1; };
    auto str = [&](size_t o) -> CStringA { // int32 length + ASCII at o
        const INT32 n = i32(o);
        return (n > 0 && n < 64 && o + 4 + n <= orig.size()) ? CStringA(reinterpret_cast<const char*>(&orig[o + 4]), n) : CStringA();
    };
    struct Item { size_t o, end; CStringA type, id; };
    std::vector<Item> items;
    for (size_t o = 16; o + 12 < orig.size(); o++) {
        const CStringA type = str(o + 4);
        if (type != "string" && type != "boolean" && type != "integer" && type != "folder" && type != "binary" && type != "device") {
            continue;
        }
        const INT32 len = i32(o);
        if (len <= 0 || o + 4 + len > orig.size()) {
            continue;
        }
        items.push_back({ o, o + 4 + len, type, str(o + 8 + type.GetLength()) });
    }
    // the device folder, then its displayModesData item
    const Item* dev = nullptr;
    for (const auto& it : items) {
        if (it.type == "device" && it.id == device) {
            dev = dev ? nullptr : &it; // must be unique
            if (!dev) {
                return false;
            }
        }
    }
    const Item* bin = nullptr;
    for (const auto& it : items) {
        if (dev && it.type == "binary" && it.id == "displayModesData" && it.o > dev->o && it.end <= dev->end) {
            if (bin) {
                return false;
            }
            bin = &it;
        }
    }
    if (!bin) {
        return false;
    }
    size_t p = bin->o + 4;
    for (int f = 0; f < 4; f++) { // type, id, name, help
        const INT32 n = i32(p);
        if (n < 0 || p + 4 + n > bin->end) {
            return false;
        }
        p += 4 + n;
    }
    const size_t posBlock = p + 4, posSize = p + 16, posData = p + 20;
    const INT32 oldSize = i32(posSize);
    if (i32(p) != 0 || oldSize < 0 || i32(posBlock) != 21 + oldSize || i32(p + 8) != 0 || i32(p + 12) != 1 ||
        posData + oldSize + 9 != bin->end) {
        return false; // not the layout this was written for: leave madVR alone
    }
    const INT32 delta = static_cast<INT32>(data.size()) - oldSize;
    std::vector<BYTE> next(orig);
    auto put = [&](size_t o, INT32 v) { *reinterpret_cast<INT32*>(&next[o]) = v; };
    for (const auto& it : items) { // the item itself and every folder around it
        if (it.o <= bin->o && it.end >= bin->end) {
            put(it.o, i32(it.o) + delta);
        }
    }
    put(posBlock, 21 + static_cast<INT32>(data.size()));
    put(posSize, static_cast<INT32>(data.size()));
    next.erase(next.begin() + posData, next.begin() + posData + oldSize);
    next.insert(next.begin() + posData, data.begin(), data.end());

    if (!s->SettingsImport(next.data(), static_cast<int>(next.size()))) {
        return false;
    }
    // Check: the record madVR now has must equal what was handed in (the header's revision aside)
    bool same = false;
    if (s->SettingsExport(&buf, &size) && buf) {
        same = size == static_cast<int>(next.size()) && size > 16 &&
               memcmp(static_cast<BYTE*>(buf) + 16, next.data() + 16, size - 16) == 0;
        LocalFree(buf);
    }
    if (!same) {
        s->SettingsImport(orig.data(), static_cast<int>(orig.size()));
    }
    return same;
}

// ---------------------------------------------------------------------------
// LAV Audio / LAV Video pages. Every setting goes through LAV's settings interface; an option
// the running LAV doesn't have is left out (its Get fails), so other LAV versions are safe.
namespace
{
    enum class LvKind { Heading, Bool, Choice, Int };
    struct LavDef {
        int page = 0;          // 0 audio, 1 video
        LvKind kind = LvKind::Heading;
        LPCWSTR label = L"";
        int col = 0;
        std::vector<LPCWSTR> names; // Choice
        int minV = 0, maxV = 0, step = 1;
        LPCWSTR unit = L"";
        int fadeUnless = -1;   // faded while that setting is 0 (off / "None")
        std::function<bool(IUnknown*, int&)> get;
        std::function<void(IUnknown*, int)> set;
    };

    template <class I>
    LavDef LvDef(int page, LvKind kind, LPCWSTR label, int col, std::function<bool(I*, int&)> g, std::function<void(I*, int)> s)
    {
        LavDef d;
        d.page = page;
        d.kind = kind;
        d.label = label;
        d.col = col;
        d.get = [g](IUnknown* u, int& v) { CComQIPtr<I> p(u); return p && g(p, v); };
        d.set = [s](IUnknown* u, int v) { CComQIPtr<I> p(u); if (p) { s(p, v); } };
        return d;
    }

    const std::vector<LavDef>& LavDefs()
    {
        static std::vector<LavDef> defs;
        if (!defs.empty()) {
            return defs;
        }
        using A = ILAVAudioSettings;
        using V = ILAVVideoSettings;
        auto head = [&](int page, LPCWSTR label, int col) {
            LavDef d;
            d.page = page;
            d.label = label;
            d.col = col;
            defs.push_back(d);
            return static_cast<int>(defs.size()) - 1;
        };
        auto add = [&](LavDef d) {
            defs.push_back(d);
            return static_cast<int>(defs.size()) - 1;
        };
        // Audio: like LAV Audio's settings page (left: bitstreaming, DRC, options; right: delay, output formats)
        auto aBit = [&](LPCWSTR label, LAVBitstreamCodec c) {
            return add(LvDef<A>(0, LvKind::Bool, label, 0, [c](A* p, int& v) { v = p->GetBitstreamConfig(c) ? 1 : 0; return true; },
                                [c](A* p, int v) { p->SetBitstreamConfig(c, v != 0); }));
        };
        auto aBool = [&](LPCWSTR label, int col, BOOL(STDMETHODCALLTYPE A::*g)(), HRESULT(STDMETHODCALLTYPE A::*s)(BOOL)) {
            return add(LvDef<A>(0, LvKind::Bool, label, col, [g](A* p, int& v) { v = (p->*g)() ? 1 : 0; return true; },
                                [s](A* p, int v) { (p->*s)(v != 0); }));
        };
        auto aFmt = [&](LPCWSTR label, LAVAudioSampleFormat f) {
            return add(LvDef<A>(0, LvKind::Bool, label, 1, [f](A* p, int& v) { v = p->GetSampleFormat(f) ? 1 : 0; return true; },
                                [f](A* p, int v) { p->SetSampleFormat(f, v != 0); }));
        };
        head(0, L"Bitstreaming (S/PDIF, HDMI)", 0);
        aBit(L"Dolby Digital (AC-3)", Bitstream_AC3);
        aBit(L"Dolby Digital Plus (E-AC3)", Bitstream_EAC3);
        aBit(L"Dolby TrueHD", Bitstream_TRUEHD);
        const int dts = aBit(L"DTS", Bitstream_DTS);
        defs[aBit(L"DTS-HD", Bitstream_DTSHD)].fadeUnless = dts;
        aBool(L"Fallback to PCM if not supported", 0, &A::GetBitstreamingFallback, &A::SetBitstreamingFallback);
        head(0, L"Dynamic range compression", 0);
        const int drc = add(LvDef<A>(0, LvKind::Bool, L"DRC (AC-3, E-AC3)", 0,
                                     [](A* p, int& v) { BOOL on = FALSE; int lvl = 0; v = SUCCEEDED(p->GetDRC(&on, &lvl)) && on ? 1 : 0; return true; },
                                     [](A* p, int v) { BOOL on = FALSE; int lvl = 100; p->GetDRC(&on, &lvl); p->SetDRC(v != 0, lvl); }));
        {
            LavDef d = LvDef<A>(0, LvKind::Int, L"DRC level", 0,
                                [](A* p, int& v) { BOOL on = FALSE; return SUCCEEDED(p->GetDRC(&on, &v)); },
                                [](A* p, int v) { BOOL on = FALSE; int lvl = 0; p->GetDRC(&on, &lvl); p->SetDRC(on, v); });
            d.minV = 0, d.maxV = 100, d.step = 5, d.unit = L" %", d.fadeUnless = drc;
            add(d);
        }
        head(0, L"Options", 0);
        aBool(L"Auto A/V sync correction", 0, &A::GetAutoAVSync, &A::SetAutoAVSync);
        aBool(L"Standard channel layouts", 0, &A::GetOutputStandardLayout, &A::SetOutputStandardLayout);
        aBool(L"Expand mono to stereo", 0, &A::GetExpandMono, &A::SetExpandMono);
        aBool(L"Expand 6.1 to 7.1", 0, &A::GetExpand61, &A::SetExpand61);
        aBool(L"Legacy 5.1 layout", 0, &A::GetOutput51LegacyLayout, &A::SetOutput51LegacyLayout);
        head(0, L"Audio delay", 1);
        const int delay = add(LvDef<A>(0, LvKind::Bool, L"Audio delay", 1,
                                       [](A* p, int& v) { BOOL on = FALSE; int ms = 0; v = SUCCEEDED(p->GetAudioDelay(&on, &ms)) && on ? 1 : 0; return true; },
                                       [](A* p, int v) { BOOL on = FALSE; int ms = 0; p->GetAudioDelay(&on, &ms); p->SetAudioDelay(v != 0, ms); }));
        {
            LavDef d = LvDef<A>(0, LvKind::Int, L"Delay", 1,
                                [](A* p, int& v) { BOOL on = FALSE; return SUCCEEDED(p->GetAudioDelay(&on, &v)); },
                                [](A* p, int v) { BOOL on = FALSE; int ms = 0; p->GetAudioDelay(&on, &ms); p->SetAudioDelay(on, v); });
            d.minV = -2000, d.maxV = 2000, d.step = 10, d.unit = L" ms", d.fadeUnless = delay;
            add(d);
        }
        head(0, L"Output formats", 1);
        aFmt(L"16-bit integer", SampleFormat_16);
        aFmt(L"24-bit integer", SampleFormat_24);
        aFmt(L"32-bit integer", SampleFormat_32);
        aFmt(L"8-bit integer", SampleFormat_U8);
        aFmt(L"32-bit floating point", SampleFormat_FP32);
        aBool(L"Dithering for 16-bit output", 1, &A::GetSampleConvertDithering, &A::SetSampleConvertDithering);

        // Video: the options that matter from the couch (left: hardware decoding; right: output, deinterlacing)
        head(1, L"Hardware decoding", 0);
        int hw = 0;
        {
            LavDef d = LvDef<V>(1, LvKind::Choice, L"Hardware decoder", 0,
                                [](V* p, int& v) { v = static_cast<int>(p->GetHWAccel()); return true; },
                                [](V* p, int v) { p->SetHWAccel(static_cast<LAVHWAccel>(v)); });
            d.names = { L"None", L"NVIDIA CUVID", L"Intel QuickSync", L"DXVA2 (copy-back)", L"DXVA2 (native)", L"D3D11" };
            hw = add(d);
        }
        auto vRes = [&](LPCWSTR label, DWORD flag) {
            defs[add(LvDef<V>(1, LvKind::Bool, label, 0, [flag](V* p, int& v) { v = (p->GetHWAccelResolutionFlags() & flag) ? 1 : 0; return true; },
                              [flag](V* p, int v) {
                                  const DWORD f = p->GetHWAccelResolutionFlags();
                                  p->SetHWAccelResolutionFlags(v ? (f | flag) : (f & ~flag));
                              }))].fadeUnless = hw;
        };
        auto vCodec = [&](LPCWSTR label, LAVVideoHWCodec c) {
            defs[add(LvDef<V>(1, LvKind::Bool, label, 0, [c](V* p, int& v) { v = p->GetHWAccelCodec(c) ? 1 : 0; return true; },
                              [c](V* p, int v) { p->SetHWAccelCodec(c, v != 0); }))].fadeUnless = hw;
        };
        head(1, L"Use hardware for", 0);
        vRes(L"SD", LAVHWResFlag_SD);
        vRes(L"HD (up to 1080p)", LAVHWResFlag_HD);
        vRes(L"UHD (4K)", LAVHWResFlag_UHD);
        vCodec(L"H.264", HWCodec_H264);
        vCodec(L"HEVC", HWCodec_HEVC);
        vCodec(L"AV1", HWCodec_AV1);
        vCodec(L"VP9", HWCodec_VP9);
        vCodec(L"VC-1", HWCodec_VC1);
        vCodec(L"MPEG-2", HWCodec_MPEG2);
        vCodec(L"MPEG-4", HWCodec_MPEG4);
        head(1, L"Output", 1);
        {
            LavDef d = LvDef<V>(1, LvKind::Choice, L"RGB output levels", 1,
                                [](V* p, int& v) { v = static_cast<int>(p->GetRGBOutputRange()); return true; },
                                [](V* p, int v) { p->SetRGBOutputRange(static_cast<DWORD>(v)); });
            d.names = { L"Auto (same as input)", L"Limited (16-235)", L"Full (0-255)" };
            add(d);
        }
        {
            LavDef d = LvDef<V>(1, LvKind::Choice, L"Dithering", 1,
                                [](V* p, int& v) { v = static_cast<int>(p->GetDitherMode()); return true; },
                                [](V* p, int v) { p->SetDitherMode(static_cast<LAVDitherMode>(v)); });
            d.names = { L"Ordered", L"Random" };
            add(d);
        }
        head(1, L"Deinterlacing", 1);
        {
            LavDef d = LvDef<V>(1, LvKind::Choice, L"Deinterlacing mode", 1,
                                [](V* p, int& v) { v = static_cast<int>(p->GetDeinterlacingMode()); return true; },
                                [](V* p, int v) { p->SetDeinterlacingMode(static_cast<LAVDeintMode>(v)); });
            d.names = { L"Auto", L"Aggressive", L"Force", L"Disabled" };
            add(d);
        }
        return defs;
    }
}

CComPtr<IBaseFilter> CTvMenu::LavFilter(bool video)
{
    CComPtr<IBaseFilter> found;
    if (!m_pMainFrame || !m_pMainFrame->m_pGB) {
        return found;
    }
    CComPtr<IEnumFilters> pEnum;
    if (FAILED(m_pMainFrame->m_pGB->EnumFilters(&pEnum))) {
        return found;
    }
    for (CComPtr<IBaseFilter> pBF; pEnum->Next(1, &pBF, nullptr) == S_OK; pBF.Release()) {
        if (video ? !!CComQIPtr<ILAVVideoSettings>(pBF) : !!CComQIPtr<ILAVAudioSettings>(pBF)) {
            found = pBF;
            break;
        }
    }
    return found;
}

// MPC-HC's own LAV keeps its settings in MPC-HC's settings (it runs LAV in "runtime config"
// mode), so save them the way Options > Internal Filters does. A separately installed LAV
// saves its own settings when they're set.
void CTvMenu::LavSave(bool video)
{
    CComPtr<IBaseFilter> pBF = LavFilter(video);
    if (!pBF || !CFGFilterLAV::IsInternalInstance(pBF)) {
        return;
    }
    if (video) {
        CFGFilterLAVVideo::Settings s;
        if (s.GetSettings(CComQIPtr<ILAVVideoSettings>(pBF))) {
            s.SaveSettings();
        }
    } else {
        CFGFilterLAVAudio::Settings s;
        if (s.GetSettings(CComQIPtr<ILAVAudioSettings>(pBF))) {
            s.SaveSettings();
        }
    }
}

void CTvMenu::LavStep(int defIndex, int dir)
{
    const auto& defs = LavDefs();
    if (defIndex < 0 || defIndex >= static_cast<int>(defs.size())) {
        return;
    }
    const LavDef& d = defs[defIndex];
    CComPtr<IBaseFilter> pBF = LavFilter(d.page == 1);
    int v = 0;
    if (!pBF || !d.get(pBF, v)) {
        return;
    }
    if (d.kind == LvKind::Bool) {
        v = v ? 0 : 1;
    } else if (d.kind == LvKind::Choice) {
        const int n = static_cast<int>(d.names.size());
        v = ((v + dir) % n + n) % n;
    } else if (d.kind == LvKind::Int) {
        v = std::clamp(v + dir * d.step, d.minV, d.maxV);
    }
    d.set(pBF, v);
    LavSave(d.page == 1);
    BuildRows();
    for (int i = 0; i < static_cast<int>(m_rows.size()); i++) {
        if (m_rows[i].kind == RowKind::LavSetting && m_rows[i].data == defIndex) {
            m_nFocus = i;
        }
    }
    m_rowFade.valid = false;
    Render();
}

// Short value for the Settings page rows
CStringW CTvMenu::LavSummary(bool video)
{
    CComPtr<IBaseFilter> pBF = LavFilter(video);
    if (!pBF) {
        return L"not in use";
    }
    if (video) {
        CComQIPtr<ILAVVideoSettings> p(pBF);
        const int hw = static_cast<int>(p->GetHWAccel());
        const LPCWSTR names[] = { L"Software", L"NVIDIA CUVID", L"QuickSync", L"DXVA2 copy-back", L"DXVA2 native", L"D3D11" };
        const DWORD r = p->GetRGBOutputRange();
        CStringW s = (hw >= 0 && hw < static_cast<int>(_countof(names))) ? names[hw] : L"?";
        if (r == 1 || r == 2) {
            s += CStringW(L"  ") + kDotSep + L"  " + (r == 1 ? L"Limited" : L"Full range");
        }
        return s;
    }
    CComQIPtr<ILAVAudioSettings> p(pBF);
    const struct { LAVBitstreamCodec c; LPCWSTR n; } bs[] = {
        { Bitstream_TRUEHD, L"TrueHD" }, { Bitstream_DTSHD, L"DTS-HD" }, { Bitstream_DTS, L"DTS" }, { Bitstream_EAC3, L"E-AC3" }, { Bitstream_AC3, L"AC-3" },
    };
    CStringW s;
    for (const auto& b : bs) {
        if (p->GetBitstreamConfig(b.c)) {
            s += s.IsEmpty() ? L"Bitstream " : L", ";
            s += b.n;
        }
    }
    return s.IsEmpty() ? CStringW(L"Decoding everything") : s;
}

// LAV's own settings window (mouse): the running filter's pages, saved as MPC-HC does; without
// playback, MPC-HC's stand-alone LAV settings window.
void CTvMenu::LavWindow(bool video)
{
    CComPtr<IBaseFilter> pBF = LavFilter(video);
    CMainFrame* pMF = m_pMainFrame;
    ForceClose();
    if (pBF) {
        pMF->FilterSettings(CComPtr<IUnknown>(pBF), pMF);
    } else if (video) {
        CFGFilterLAVVideo::ShowPropertyPages(pMF);
    } else {
        CFGFilterLAVAudio::ShowPropertyPages(pMF);
    }
}

// madVR's list for the active display
std::vector<std::array<INT32, 4>> CTvMenu::MadVrModesList()
{
    std::vector<std::array<INT32, 4>> list;
    CComQIPtr<IMadVRSettings> s(m_pMadVr);
    LPVOID bin = nullptr;
    int size = 0;
    if (s && !m_strMadVrDevice.IsEmpty() &&
        s->SettingsGetBinary(m_strMadVrDevice + L"\\displayModes\\displayModesData", &bin, &size) && bin) {
        const INT32* v = static_cast<const INT32*>(bin);
        for (int i = 0; i + 3 < size / 4; i += 4) {
            list.push_back({ v[i], v[i + 1], v[i + 2], v[i + 3] });
        }
        LocalFree(bin);
    }
    return list;
}

// Writes the list (sorted: biggest resolution first, then by refresh rate)
bool CTvMenu::MadVrSetModes(std::vector<std::array<INT32, 4>> list)
{
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a[1] != b[1] ? a[1] > b[1] : a[0] != b[0] ? a[0] > b[0] : a[2] != b[2] ? a[2] < b[2] : a[3] < b[3];
    });
    std::vector<BYTE> data;
    for (const auto& m : list) {
        data.insert(data.end(), reinterpret_cast<const BYTE*>(m.data()), reinterpret_cast<const BYTE*>(m.data()) + 16);
    }
    CComQIPtr<IMadVRSettings> s(m_pMadVr);
    return MadVrWriteDisplayModes(s, data);
}

// What an "Add ..." button adds: the refresh rates Windows offers for this display at that resolution
std::vector<std::array<INT32, 4>> CTvMenu::MadVrModesFor(int action)
{
    std::vector<std::array<INT32, 4>> list;
    if (!m_pMainFrame || !m_pMainFrame->m_pVideoWnd) {
        return list;
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    DEVMODEW cur = {};
    cur.dmSize = sizeof(cur);
    if (!GetMonitorInfoW(MonitorFromWindow(m_pMainFrame->m_pVideoWnd->m_hWnd, MONITOR_DEFAULTTONEAREST), &mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) {
        return list;
    }
    if (action == MvActionAddCurrentMode) {
        list.push_back({ static_cast<INT32>(cur.dmPelsWidth), static_cast<INT32>(cur.dmPelsHeight), static_cast<INT32>(cur.dmDisplayFrequency),
                         (cur.dmDisplayFlags & DM_INTERLACED) ? 1 : 0 });
        return list;
    }
    DEVMODEW at = cur;
    switch (action) {
        case MvActionAdd2160: at.dmPelsWidth = 3840; at.dmPelsHeight = 2160; break;
        case MvActionAdd1440: at.dmPelsWidth = 2560; at.dmPelsHeight = 1440; break;
        case MvActionAdd1080: at.dmPelsWidth = 1920; at.dmPelsHeight = 1080; break;
        case MvActionAddCurrentRes: break;
        default: return list;
    }
    for (int hz : MadVrRatesAt(mi.szDevice, at)) {
        list.push_back({ static_cast<INT32>(at.dmPelsWidth), static_cast<INT32>(at.dmPelsHeight), hz, 0 });
    }
    return list;
}

CStringW CTvMenu::MadVrModesAvailable()
{
    if (!m_pMainFrame || !m_pMainFrame->m_pVideoWnd) {
        return L"";
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(m_pMainFrame->m_pVideoWnd->m_hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        return L"";
    }
    DEVMODEW cur = {};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) {
        return L"";
    }
    CStringW text;
    for (int hz : MadVrRatesAt(mi.szDevice, cur)) {
        if (!text.IsEmpty()) {
            text += L", ";
        }
        text += MadVrModeName(cur.dmPelsWidth, cur.dmPelsHeight, hz, false);
    }
    return text;
}

// Every refresh rate the driver offers (from the display's EDID) at the current resolution
std::vector<int> CTvMenu::MadVrRatesAt(LPCWSTR device, const DEVMODEW& cur)
{
    // Every refresh rate the driver offers (from the display's EDID) at the current resolution
    std::vector<int> rates;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(device, i, &dm); i++) {
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight && !(dm.dmDisplayFlags & DM_INTERLACED) &&
            dm.dmDisplayFrequency > 1 && std::find(rates.begin(), rates.end(), static_cast<int>(dm.dmDisplayFrequency)) == rates.end()) {
            rates.push_back(static_cast<int>(dm.dmDisplayFrequency));
        }
    }
    std::sort(rates.begin(), rates.end());
    return rates;
}

// After a madVR change: rebuild (rows can appear or go, e.g. tone mapping options) and keep the
// focus on the same setting, scrolled into view.
void CTvMenu::RebuildKeepingMadVrFocus(int defIndex)
{
    BuildRows();
    for (int i = 0; i < static_cast<int>(m_rows.size()); i++) {
        if (m_rows[i].kind == RowKind::MadVrSetting && m_rows[i].data == defIndex) {
            m_nFocus = i;
            break;
        }
    }
    m_nFocus = std::clamp(m_nFocus, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
    if (!m_rows.empty()) {
        const int line = m_rows[m_nFocus].line;
        if (line < m_nScroll) {
            m_nScroll = line;
        } else if (line >= m_nScroll + MaxRows()) {
            m_nScroll = line - MaxRows() + 1;
        }
        m_nScroll = std::clamp(m_nScroll, 0, std::max(0, LineCount() - MaxRows()));
    }
    m_rowFade.valid = false; // same row, no crossfade
    Render();
}

// madVR rows: arrows only navigate (Up / Down within a column, Left / Right to the other column).
// OK starts editing a row; while editing, Left / Right pick a part of the row (on / off, the
// value, the mini-view icon) and Up / Down change it. OK on the icon shrinks the list to a small
// strip at the top (just this row) so the picture can be seen changing; OK again brings the list
// back. OK elsewhere or Back ends editing.
namespace
{
    enum MvPart { MvPartToggle = 0, MvPartValue = 1, MvPartMini = 2 };
}

std::vector<int> CTvMenu::MadVrParts(int row) const
{
    std::vector<int> parts;
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || m_rows[row].kind != RowKind::MadVrSetting) {
        return parts;
    }
    const Row& r = m_rows[row];
    if (r.faded) {
        return parts; // only applies while an on / off above is on: can't be edited
    }
    if (!r.prefix.IsEmpty()) {
        parts.push_back(MvPartToggle); // "On" / "Off"
    }
    if (!r.value.IsEmpty() && !(r.data2 >= 0 && r.prefix != L"On")) {
        parts.push_back(MvPartValue); // the number next to an on / off only while it's on
    }
    if (m_nMadVrSub != MvRoot) {
        parts.push_back(MvPartMini);
    }
    return parts;
}

void CTvMenu::StartMadVrEdit()
{
    const auto parts = MadVrParts(m_nFocus);
    if (parts.empty()) {
        return;
    }
    m_nMadVrPart = std::find(parts.begin(), parts.end(), MvPartValue) != parts.end() ? MvPartValue : parts[0];
    m_rowFade.valid = false;
    Render();
}

void CTvMenu::EndMadVrEdit()
{
    m_nMadVrPart = -1;
    m_rowFade.valid = false;
    SetMadVrCompact(false); // draws (with the glide back when the strip was up)
}

// Up / Down (or the wheel) on the picked part: +1 up, -1 down
void CTvMenu::MadVrEditStep(int dir)
{
    if (m_nFocus < 0 || m_nFocus >= static_cast<int>(m_rows.size())) {
        return;
    }
    const Row r = m_rows[m_nFocus]; // copy: rebuilt below
    if (m_nMadVrPart == MvPartToggle) {
        StepMadVrSetting(r.data, +1); // on <-> off
    } else if (m_nMadVrPart == MvPartValue) {
        StepMadVrSetting(r.data2 >= 0 ? r.data2 : r.data, dir);
    } else {
        // the mini / full icon: Up = mini view (the strip at the top), Down = full list
        if (m_nMadVrPart == MvPartMini && m_bMadVrCompact != (dir > 0)) {
            SetMadVrCompact(dir > 0);
        }
        return;
    }
    RebuildKeepingMadVrFocus(r.data); // shows what madVR now reports; rows may appear / go
    const auto parts = MadVrParts(m_nFocus);
    if (!parts.empty() && std::find(parts.begin(), parts.end(), m_nMadVrPart) == parts.end()) {
        m_nMadVrPart = parts[0];
        Render();
    }
}

// Left / Right in a two-column page: the nearest row of the other column
void CTvMenu::MoveColumn(int dir)
{
    if (!m_bTwoColumns || m_nFocus < 0 || m_nFocus >= static_cast<int>(m_rows.size())) {
        return;
    }
    const int col = m_rows[m_nFocus].col + dir;
    const int line = m_rows[m_nFocus].line;
    int best = -1;
    int bestD = INT_MAX;
    for (int i = 0; i < static_cast<int>(m_rows.size()); i++) {
        if (m_rows[i].col == col && !IsSkipped(m_rows[i]) && std::abs(m_rows[i].line - line) < bestD) {
            bestD = std::abs(m_rows[i].line - line);
            best = i;
        }
    }
    if (best < 0) {
        return;
    }
    m_nFocus = best;
    const int l = m_rows[best].line;
    const int oldScroll = m_nScroll;
    if (l < m_nScroll) {
        m_nScroll = l;
    } else if (l >= m_nScroll + MaxRows()) {
        m_nScroll = l - MaxRows() + 1;
    }
    if (m_nScroll != oldScroll) {
        ShowScrollbar();
    }
    Render();
}

// Mouse on a madVR row: a click on another row starts editing it; on the edited row, a click on
// a part picks it, the small down / up triangles of the picked part change it, the icon switches
// the mini view, anywhere else ends editing.
bool CTvMenu::MadVrClick(int row, POINT pt)
{
    if (m_page == Page::MadVR && row >= 0 && row < static_cast<int>(m_rows.size()) && m_rows[row].kind == RowKind::MadVrModeTags) {
        // Tags: click picks one, a second click shows the x, a click on a tag with the x removes it
        for (const auto& ht : m_hitTags) {
            if (PtInRect(&ht.first, pt)) {
                if (m_nFocus == row && ht.second == m_nModeTag) {
                    if (m_bModeTagArmed) {
                        HandleModeTagKey(VK_RETURN, false); // removes it
                    } else {
                        m_bModeTagArmed = true;
                        Render();
                    }
                } else {
                    m_nFocus = row;
                    m_nModeTag = ht.second;
                    m_bModeTagArmed = false;
                    m_rowFade.valid = false;
                    Render();
                }
                return true;
            }
        }
        return true; // between tags: nothing
    }
    if (m_page != Page::MadVR || row < 0 || row >= static_cast<int>(m_rows.size()) || m_rows[row].kind != RowKind::MadVrSetting) {
        return false;
    }
    if (m_rows[row].toggle >= 0) {
        // a plain on / off (a switch): a click anywhere on the row flips it, as OK does
        if (!m_rows[row].faded) {
            m_nFocus = row;
            Activate();
        }
        return true;
    }
    if (row != m_nFocus || m_nMadVrPart < 0) {
        m_nFocus = row;
        StartMadVrEdit();
        return true;
    }
    for (const auto& hp : m_hitMadVrParts) {
        if (PtInRect(&hp.first, pt)) {
            if (hp.second == kHitTriDown || hp.second == kHitTriUp) {
                MadVrEditStep(hp.second == kHitTriUp ? 1 : -1); // the triangles (listed first)
            } else if (hp.second == MvPartMini) {
                m_nMadVrPart = hp.second;
                SetMadVrCompact(!m_bMadVrCompact);
            } else if (hp.second != m_nMadVrPart) {
                const auto parts = MadVrParts(row);
                if (std::find(parts.begin(), parts.end(), hp.second) == parts.end()) {
                    return true; // faded (needs the on / off on): can't be picked
                }
                m_nMadVrPart = hp.second; // the value itself: just picks it
                Render();
            }
            return true;
        }
    }
    EndMadVrEdit();
    return true;
}

void CTvMenu::ScrollFocusIntoView()
{
    if (m_nFocus < 0 || m_nFocus >= static_cast<int>(m_rows.size())) {
        return;
    }
    const int l = m_rows[m_nFocus].line;
    const int oldScroll = m_nScroll;
    if (l < m_nScroll) {
        m_nScroll = l;
    } else if (l >= m_nScroll + MaxRows()) {
        m_nScroll = l - MaxRows() + 1;
    }
    if (m_nScroll != oldScroll) {
        ShowScrollbar();
    }
}

// Display modes tags: Left / Right pick a mode (across lines), Up / Down move between lines (and
// out to the buttons), OK shows an x on the tag, OK again removes it, Back hides the x again.
bool CTvMenu::HandleModeTagKey(UINT vk, bool isRepeat)
{
    const Row r = m_rows[m_nFocus];
    const int total = static_cast<int>(m_madVrModes.size());
    auto focusTag = [&](int tag) {
        m_nModeTag = std::clamp(tag, 0, std::max(0, total - 1));
        for (int i = 0; i < static_cast<int>(m_rows.size()); i++) {
            if (m_rows[i].kind == RowKind::MadVrModeTags && m_nModeTag >= m_rows[i].data && m_nModeTag < m_rows[i].data + m_rows[i].data2) {
                m_nFocus = i;
            }
        }
        ScrollFocusIntoView();
    };
    switch (vk) {
        case VK_LEFT:
        case VK_RIGHT: {
            const int t = std::clamp(m_nModeTag + (vk == VK_RIGHT ? 1 : -1), 0, std::max(0, total - 1));
            if (t != m_nModeTag || m_bModeTagArmed) {
                m_bModeTagArmed = false;
                focusTag(t);
                m_rowFade.valid = false;
                Render();
            }
            return true;
        }
        case VK_UP:
        case VK_DOWN: {
            const int col = std::clamp(m_nModeTag - r.data, 0, std::max(0, r.data2 - 1));
            m_bModeTagArmed = false;
            Move(vk == VK_DOWN ? 1 : -1);
            if (m_rows[m_nFocus].kind == RowKind::MadVrModeTags) {
                m_nModeTag = std::min(m_rows[m_nFocus].data + col, m_rows[m_nFocus].data + m_rows[m_nFocus].data2 - 1);
                Render();
            }
            return true;
        }
        case VK_RETURN:
            if (!isRepeat && m_nModeTag < total) {
                if (!m_bModeTagArmed) {
                    m_bModeTagArmed = true; // the x shows; OK again removes
                    Render();
                } else {
                    auto list = m_madVrModes;
                    list.erase(list.begin() + m_nModeTag);
                    m_bModeTagArmed = false;
                    if (!MadVrSetModes(list)) {
                        m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, L"madVR didn't take the change - nothing was removed", 3000);
                    }
                    BuildRows();
                    if (m_madVrModes.empty()) {
                        m_nFocus = 0;
                        ScrollFocusIntoView();
                    } else {
                        focusTag(std::min(m_nModeTag, static_cast<int>(m_madVrModes.size()) - 1));
                    }
                    m_rowFade.valid = false;
                    Render();
                }
            }
            return true;
        case VK_ESCAPE:
        case VK_BACK:
        case VK_BROWSER_BACK:
            if (m_bModeTagArmed) {
                if (!isRepeat) {
                    m_bModeTagArmed = false;
                    Render();
                }
                return true;
            }
            return false; // back out of the submenu as usual
        default:
            return false;
    }
}

bool CTvMenu::HandleMadVrKey(UINT vk, bool isRepeat)
{
    if (m_rows.empty()) {
        return false;
    }
    if (m_rows[m_nFocus].kind == RowKind::MadVrModeTags) {
        return HandleModeTagKey(vk, isRepeat);
    }
    if (m_nMadVrSub == MvModes && m_nMadVrPart < 0 && (vk == VK_DOWN || vk == VK_UP)) {
        // from the buttons into the tags: start on the line's first tag
        Move(vk == VK_DOWN ? 1 : -1);
        if (m_rows[m_nFocus].kind == RowKind::MadVrModeTags) {
            m_nModeTag = m_rows[m_nFocus].data;
            m_bModeTagArmed = false;
            Render();
        }
        return true;
    }
    const bool editing = m_nMadVrPart >= 0 && m_rows[m_nFocus].kind == RowKind::MadVrSetting;
    if (!editing) {
        m_nMadVrPart = -1;
        if (vk == VK_LEFT || vk == VK_RIGHT) {
            MoveColumn(vk == VK_RIGHT ? 1 : -1);
            return true;
        }
        return false; // Up / Down move, OK opens / starts editing, Back as usual
    }
    const auto parts = MadVrParts(m_nFocus);
    const int idx = static_cast<int>(std::find(parts.begin(), parts.end(), m_nMadVrPart) - parts.begin());
    switch (vk) {
        case VK_UP:
        case VK_DOWN:
            if (!(isRepeat && m_nMadVrPart == MvPartToggle)) {
                MadVrEditStep(vk == VK_UP ? 1 : -1);
            }
            return true;
        case VK_LEFT:
        case VK_RIGHT: {
            const int next = std::clamp(idx + (vk == VK_RIGHT ? 1 : -1), 0, static_cast<int>(parts.size()) - 1);
            if (next < static_cast<int>(parts.size()) && parts[next] != m_nMadVrPart) {
                m_nMadVrPart = parts[next];
                Render();
            }
            return true;
        }
        case VK_RETURN:
            if (!isRepeat) {
                if (m_nMadVrPart == MvPartMini) {
                    SetMadVrCompact(!m_bMadVrCompact); // mini view on / off
                } else if (m_bMadVrCompact) {
                    m_nMadVrPart = -1; // mini view: OK just stops editing, the strip stays
                    m_rowFade.valid = false;
                    Render();
                } else {
                    EndMadVrEdit();
                }
            }
            return true;
        case VK_ESCAPE:
        case VK_BACK:
        case VK_BROWSER_BACK:
            if (!isRepeat) {
                if (m_bMadVrCompact) {
                    // mini view: first Back stops editing but stays small, so Up / Down roll
                    // through the settings (OK edits again); the next Back returns to the list
                    m_nMadVrPart = -1;
                    m_rowFade.valid = false;
                    Render();
                } else {
                    EndMadVrEdit();
                }
            }
            return true;
        default:
            return false;
    }
}

// madVR panel buttons
void CTvMenu::MadVrAction(int action)
{
    if (action >= MvActionAdd2160 && action <= MvActionClearModes) {
        // Display modes submenu: add a set of modes (no duplicates) or clear the list
        auto list = MadVrModesList();
        const size_t before = list.size();
        if (action == MvActionClearModes) {
            list.clear();
        } else {
            const auto add = MadVrModesFor(action);
            if (add.empty()) {
                return; // nothing this display offers there
            }
            for (const auto& m : add) {
                if (std::find(list.begin(), list.end(), m) == list.end()) {
                    list.push_back(m);
                }
            }
        }
        if (list.size() == before && action != MvActionClearModes) {
            m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, L"Already in madVR's list", 2000);
            return;
        }
        const bool ok = MadVrSetModes(list);
        CStringW msg;
        if (ok) {
            msg.Format(L"madVR display modes: %d", static_cast<int>(list.size()));
        } else {
            msg = L"madVR didn't take the list - nothing was changed";
        }
        m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, msg, 3000);
        const int focus = m_nFocus;
        BuildRows();
        m_nFocus = std::clamp(focus, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
        Render();
        return;
    }
    if (action == MvActionFillModes) {
        // Hand madVR the list as text, the way its own window does, then read back what it stored.
        // Nothing else is touched; if madVR doesn't accept text for this value, the list stays as it was.
        CComQIPtr<IMadVRSettings> s(m_pMadVr);
        const CStringW want = MadVrModesAvailable();
        if (!s || m_strMadVrDevice.IsEmpty() || want.IsEmpty()) {
            return;
        }
        const bool ok = MadVrWriteDisplayModes(s, MadVrModesAvailableData());
        const CStringW got = MadVrModesInMadVr();
        const CStringW msg = (ok && got == want) ? L"madVR display modes set: " + got
                                                 : CStringW(L"madVR didn't take the list - set it in madVR's window");
        m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, msg, 4000);
        BuildRows();
        Render();
        return;
    }
    if (action == MvActionSettingsWindow) {
        // madVR's own settings window (madHcCtrl.exe next to the madVR filter this player loaded),
        // for the mouse; the overlay steps aside.
        HMODULE mod = GetModuleHandleW(L"madVR64.ax");
        if (!mod) {
            mod = GetModuleHandleW(L"madVR.ax");
        }
        wchar_t path[MAX_PATH] = L"";
        if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH)) {
            m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, L"madVR's folder wasn't found", 3000);
            return;
        }
        CStringW dir(path);
        dir = dir.Left(dir.ReverseFind(L'\\'));
        const CStringW exe = dir + L"\\madHcCtrl.exe";
        ForceClose();
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe, L"editLocalSettings", dir, SW_SHOWNORMAL)) <= 32) {
            m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, L"madVR's settings window couldn't be opened", 3000);
        }
    }
}

void CTvMenu::ToggleMadVrStats()
{
    // Exactly like pressing Ctrl+J on the keyboard: madVR toggles its statistics.
    // (The TV key hook ignores Ctrl combinations, so this reaches MPC-HC / madVR as usual.)
    INPUT in[4] = {};
    for (auto& i : in) {
        i.type = INPUT_KEYBOARD;
    }
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = 'J';
    in[2].ki.wVk = 'J';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(_countof(in), in, sizeof(INPUT));
}

// MPC-HC's pixel shaders are in use (pre- or post-resize switch on, and the preset has some)
bool CTvMenu::ShadersOn(CMainFrame* pMainFrame)
{
    if (!pMainFrame) {
        return false;
    }
    const auto& preset = AfxGetAppSettings().m_Shaders.GetCurrentPreset();
    return (pMainFrame->m_bToggleShader && !preset.GetPreResize().empty()) ||
           (pMainFrame->m_bToggleShaderScreenSpace && !preset.GetPostResize().empty());
}

std::vector<CTvMenu::BarButton> CTvMenu::VisibleButtons()
{
    std::vector<BarButton> v;
    const bool hasChapters = !Instance().GetChapters().empty();
    for (const BarItem& it : BarLayout()) {
        if (it.on && it.id != BarButton::Separator && // separators aren't buttons
            (it.id != BarButton::Chapters || hasChapters)) { // Chapters: only while the file has some
            v.push_back(it.id);
        }
    }
    return v;
}

std::vector<CTvMenu::Chapter> CTvMenu::GetChapters() const
{
    std::vector<Chapter> chapters;
    if (!m_pMainFrame || !m_pMainFrame->m_pCB) {
        return chapters;
    }
    const DWORD n = m_pMainFrame->m_pCB->ChapGetCount();
    for (DWORD i = 0; i < n; i++) {
        REFERENCE_TIME rt = 0;
        CComBSTR name;
        if (SUCCEEDED(m_pMainFrame->m_pCB->ChapGet(i, &rt, &name))) {
            chapters.push_back({ rt, CStringW(name) });
        }
    }
    std::sort(chapters.begin(), chapters.end(), [](const Chapter& a, const Chapter& b) { return a.rt < b.rt; });
    return chapters;
}

UINT CTvMenu::ExtraKey(bool back)
{
    return static_cast<UINT>(CTvMode::GetValue(back ? kExtraBackKeyName : kExtraOkKeyName, 0));
}

// The key that shows the control bar during playback (TV-only, a single key; 0 = none)
UINT CTvMenu::BarKey()
{
    return static_cast<UINT>(CTvMode::GetValue(kBarKeyName, VK_DOWN));
}

// No way to bring up the bar from the keyboard or on its own: then MPC-HC's right-click menu
// gets a "Show control bar" item (see PrepareContextMenu)
bool CTvMenu::NeedsBarFallback()
{
    return BarKey() == 0 && CPPageAccelTbl::MakeAccelShortcutLabel(ID_TV_MENU).IsEmpty() &&
           !CTvMode::IsOn(TvOpt::ShowOnStart) && !CTvMode::IsOn(TvOpt::ShowOnPause) && CTvMode::Get(TvOpt::ShowOnSeek) != 0;
}

void CTvMenu::PrepareContextMenu(CMainFrame* pMainFrame, CMenu* pMenu)
{
    if (!pMenu) {
        return;
    }
    CMPCThemeMenu* themed = DYNAMIC_DOWNCAST(CMPCThemeMenu, pMenu);
    auto del = [&](UINT item, UINT flags) { themed ? themed->DeleteMenu(item, flags) : pMenu->DeleteMenu(item, flags); };
    // Take off what was added last time (the item, then its separator, now at the top)
    if (pMenu->GetMenuState(ID_TV_MENU, MF_BYCOMMAND) != static_cast<UINT>(-1)) {
        del(ID_TV_MENU, MF_BYCOMMAND);
        if (pMenu->GetMenuItemCount() > 0 && (pMenu->GetMenuState(0, MF_BYPOSITION) & MF_SEPARATOR)) {
            del(0, MF_BYPOSITION);
        }
    }
    // Always offered in TV Mode with a file loaded (a mouse way to the bar, also
    // when no key brings it up)
    if (!AfxGetAppSettings().bTvMode || pMainFrame->GetLoadState() != MLS::LOADED) {
        return;
    }
    pMenu->InsertMenu(0, MF_BYPOSITION | MF_STRING, ID_TV_MENU, _T("Show control bar"));
    pMenu->InsertMenu(1, MF_BYPOSITION | MF_SEPARATOR);
    if (themed) {
        themed->fulfillThemeReqsItem(0);
        themed->fulfillThemeReqsItem(1);
    }
}

CStringW CTvMenu::KeyText(int actionId)
{
    if (actionId == kBarKey) {
        return VirtualKeyName(BarKey());
    }
    if (actionId == kExtraBackKey || actionId == kExtraOkKey) {
        return VirtualKeyName(ExtraKey(actionId == kExtraBackKey));
    }
    // its MPC-HC key, the arrows set for it, and the keys held for it ("Space, Right, hold Red")
    CStringW keys = CStringW(CPPageAccelTbl::MakeAccelShortcutLabel(static_cast<UINT>(actionId)));
    auto add = [&keys](const CStringW& k) { keys += keys.IsEmpty() ? k : (CStringW(L", ") + k); };
    static const LPCWSTR kArrowNames[4] = { L"Left", L"Right", L"Up", L"Down" };
    for (int a = 0; a < 4; a++) {
        if (ArrowAction(a * 2) == actionId) {
            add(kArrowNames[a]);
        }
    }
    for (const auto& kv : HoldMap()) {
        if (kv.second == actionId) {
            add(L"hold " + VirtualKeyName(kv.first));
        }
    }
    return keys.IsEmpty() ? CStringW(L"Not set") : keys;
}

// Sets (vk != 0) or clears the key of an action. MPC-HC commands go into MPC-HC's own key
// table, exactly as Options > Keys does it (the same key is taken off any other command),
// and are saved right away. One key, one action across both: a TV-only key (bar, extra Back /
// OK) takes that plain key off any MPC-HC command (except the arrows, which TV Mode handles
// first anyway and Desktop Mode keeps), and an MPC-HC command's plain key clears a TV-only key.
void CTvMenu::AssignKey(int actionId, UINT vk, BYTE fVirt)
{
    const int tvIds[] = { kBarKey, kExtraBackKey, kExtraOkKey };
    const LPCTSTR tvNames[] = { kBarKeyName, kExtraBackKeyName, kExtraOkKeyName };
    const bool tvKey = (actionId == kExtraBackKey || actionId == kExtraOkKey || actionId == kBarKey);
    const bool plain = (fVirt & (FCONTROL | FSHIFT | FALT)) == 0;
    if (vk && plain) {
        for (int k = 0; k < 3; k++) { // the other TV-only keys
            if (tvIds[k] != actionId && static_cast<UINT>(CTvMode::GetValue(tvNames[k], tvIds[k] == kBarKey ? VK_DOWN : 0)) == vk) {
                CTvMode::SetValue(tvNames[k], 0);
            }
        }
    }
    if (tvKey) {
        CTvMode::SetValue(actionId == kBarKey ? kBarKeyName : actionId == kExtraBackKey ? kExtraBackKeyName : kExtraOkKeyName,
                          static_cast<int>(vk));
        if (!vk || vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) {
            return; // nothing to take off MPC-HC's commands
        }
    }
    CAppSettings& s = AfxGetAppSettings();
    for (POSITION pos = s.wmcmds.GetHeadPosition(); pos;) {
        wmcmd& wc = s.wmcmds.GetNext(pos);
        if (!tvKey && wc.cmd == static_cast<WORD>(actionId)) {
            wc.key = static_cast<WORD>(vk);
            wc.fVirt = vk ? fVirt : static_cast<BYTE>(FVIRTKEY | FNOINVERT);
        } else if (vk && wc.key == vk && (tvKey ? (wc.fVirt & (FCONTROL | FSHIFT | FALT)) == 0
                                                : (wc.fVirt & ~FNOINVERT) == (fVirt & ~FNOINVERT))) {
            wc.key = 0; // one key, one command
        }
    }
    // Rebuild the accelerator table (as CPPageAccelTbl::OnApply)
    if (s.hAccel) {
        DestroyAcceleratorTable(s.hAccel);
    }
    CAtlArray<ACCEL> accel;
    accel.SetCount(ACCEL_LIST_SIZE);
    int count = 0;
    for (POSITION pos = s.wmcmds.GetHeadPosition(); pos && count < ACCEL_LIST_SIZE;) {
        const ACCEL a = s.wmcmds.GetNext(pos);
        if (a.key > 0) {
            accel[count++] = a;
        }
    }
    s.hAccel = CreateAcceleratorTable(accel.GetData(), count);
    if (CMainFrame* pMainFrame = AfxGetMainFrame()) {
        pMainFrame->m_hAccelTable = s.hAccel;
    }
    s.SaveSettings();
}

// Remote keys submenu, waiting for a key: the next key press is assigned to the action.
// Back cancels, Delete clears; arrows and OK stay with the menus.
bool CTvMenu::HandleKeyCapture(const MSG* pMsg)
{
    if (m_nListenKey < 0 || (pMsg->message != WM_KEYDOWN && pMsg->message != WM_SYSKEYDOWN)) {
        return false;
    }
    const UINT vk = static_cast<UINT>(pMsg->wParam);
    switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
        case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
            return true; // modifiers alone: wait for the real key
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool plain = !ctrl && !shift && !alt;
    const int action = kKeyActions[std::clamp(m_nListenKey, 0, static_cast<int>(_countof(kKeyActions)) - 1)].id;
    const bool extra = (action == kExtraBackKey || action == kExtraOkKey || action == kBarKey);

    const bool arrow = (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT);
    if (plain && (vk == VK_ESCAPE || vk == VK_BACK || vk == VK_BROWSER_BACK)) {
        m_nListenKey = -1; // cancel
        m_bListenHold = false;
    } else if (plain && vk == VK_DELETE) {
        ClearAction(action); // a keyboard's shortcut for "Clear"
        m_nListenKey = -1;
        m_bListenHold = false;
    } else if (plain && vk == VK_RETURN) {
        m_strKeyMessage = L"OK is used by the menus";
        BuildRows();
        Render();
        return true;
    } else if (m_bListenHold) {
        // A hold: any single key, arrows too (acts only while the bar and menus are hidden)
        if (!plain) {
            m_strKeyMessage = L"Use a single key (no Ctrl / Shift / Alt)";
            BuildRows();
            Render();
            return true;
        }
        // Only holds change here: this action's old hold goes, and another action holding this key
        // loses it (one hold per key, one per action). Short presses (keys, arrows) stay as they are.
        auto m = HoldMap();
        for (auto it = m.begin(); it != m.end();) {
            it = (it->second == action) ? m.erase(it) : std::next(it);
        }
        m[vk] = action;
        SaveHoldMap(m);
        m_nListenKey = -1;
        m_bListenHold = false;
    } else if (plain && arrow && action > 0) {
        // An arrow for this action: used while no bar or menu is shown (the bar key, if it was this
        // arrow, gives way: one key, one action)
        SetArrowAction(ArrowIndex(vk) * 2, action);
        if (BarKey() == vk) {
            CTvMode::SetValue(kBarKeyName, 0);
        }
        m_nListenKey = -1;
    } else if (plain && action == kBarKey && (vk == VK_UP || vk == VK_DOWN)) {
        AssignKey(action, vk, 0); // the bar key may be Up or Down (Left / Right seek, OK plays / pauses)
        m_nListenKey = -1;
    } else if (plain && arrow) {
        m_strKeyMessage = L"Arrows are used by the menus here";
        BuildRows();
        Render();
        return true;
    } else if (extra && !plain) {
        m_strKeyMessage = L"Use a single key (no Ctrl / Shift / Alt)";
        BuildRows();
        Render();
        return true;
    } else {
        BYTE fVirt = FVIRTKEY | FNOINVERT;
        fVirt |= ctrl ? FCONTROL : 0;
        fVirt |= shift ? FSHIFT : 0;
        fVirt |= alt ? FALT : 0;
        AssignKey(action, vk, fVirt);
        m_nListenKey = -1;
    }
    m_strKeyMessage.Empty();
    m_nLastInput = GetTickCount64();
    BuildRows();
    Render();
    return true;
}

void CTvMenu::SetVolumeFromX(int x)
{
    if (!m_pMainFrame || m_hitVolumeX1 <= m_hitVolumeX0) {
        return;
    }
    const int v = std::clamp((x - m_hitVolumeX0) * 100 / (m_hitVolumeX1 - m_hitVolumeX0), 0, 100);
    if (m_pMainFrame->IsMuted() && v > 0) {
        m_pMainFrame->SendMessage(WM_COMMAND, ID_VOLUME_MUTE); // dragging the level unmutes
    }
    m_pMainFrame->m_wndToolBar.m_volctrl.SetPosInternal(v); // MPC-HC's own volume (saved, API, OSD)
    m_nLastBarRender = 0; // redraw with the new level on the next tick
    m_bDirty = true;
}

CTvMenu& CTvMenu::Instance()
{
    static CTvMenu instance;
    return instance;
}

CTvMenu::~CTvMenu()
{
    // m_pOsd is normally released in OnCloseMedia; at static destruction time
    // madVR may already be gone, so only free our own GDI objects here.
    m_pOsd.Detach();
    if (m_hbmPreview) {
        DeleteObject(m_hbmPreview);
    }
    if (m_hbmMenu) {
        DeleteObject(m_hbmMenu);
    }
}

// ---------------------------------------------------------------------------
// Input

bool CTvMenu::PreTranslateMessage(CMainFrame* pMainFrame, const MSG* pMsg)
{
    if (!AfxGetAppSettings().bTvMode) {
        return false;
    }
    if (pMsg->message >= WM_MOUSEFIRST && pMsg->message <= WM_MOUSELAST) {
        return IsTargetWindow(pMainFrame, pMsg->hwnd) && HandleMouse(pMainFrame, pMsg);
    }
    if (m_bOpen && m_nListenKey >= 0 && IsTargetWindow(pMainFrame, pMsg->hwnd) && HandleKeyCapture(pMsg)) {
        return true; // Remote keys submenu: this key press is being assigned
    }
    // After Back held down closed the menu: the rest of that key's repeats (until it's released)
    // must not reach MPC-HC, where Esc would leave fullscreen
    if (m_bSwallowBack) {
        const UINT k = static_cast<UINT>(pMsg->wParam);
        const bool backKey = k == VK_ESCAPE || k == VK_BACK || k == VK_BROWSER_BACK || (ExtraKey(true) && k == ExtraKey(true));
        if (backKey && pMsg->message == WM_KEYUP) {
            m_bSwallowBack = false;
            return true;
        }
        if (backKey && pMsg->message == WM_KEYDOWN && (pMsg->lParam & (1 << 30))) {
            return true;
        }
        if (pMsg->message == WM_KEYDOWN) {
            m_bSwallowBack = false; // another key: back to normal
        }
    }
    if (pMsg->message == WM_KEYUP && m_nGlideDir != 0) {
        m_nGlideDir = 0; // the held seek key was let go: the preview stops right there
    }
    if (pMsg->message == WM_KEYUP && m_bOkPending) {
        const UINT k = static_cast<UINT>(pMsg->wParam);
        if (k == VK_RETURN || (ExtraKey(false) && k == ExtraKey(false))) {
            ReleaseOk(); // a short OK on the bar: its normal action now
        }
    }
    if (pMsg->message == WM_KEYUP && m_nArrowPendingVk && static_cast<UINT>(pMsg->wParam) == m_nArrowPendingVk) {
        ReleaseArrow(); // an arrow with a hold action let go: its tap action now (if it wasn't held)
        return true;
    }
    if (pMsg->message == WM_KEYDOWN && m_bOpen && m_nArrowPendingVk && static_cast<UINT>(pMsg->wParam) == m_nArrowPendingVk &&
        (pMsg->lParam & (1 << 30))) {
        m_nArrowLastKey = GetTickCount64();
        return true; // the rest of a hold that opened a menu: not moving around in it
    }
    if (pMsg->message != WM_KEYDOWN) {
        return false;
    }
    // Leave shortcuts with modifiers (Ctrl+..., Shift+...) to MPC-HC; Alt arrives as WM_SYSKEYDOWN.
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000)) {
        return false;
    }
    if (!IsTargetWindow(pMainFrame, pMsg->hwnd)) {
        return false;
    }
    UINT vk = static_cast<UINT>(pMsg->wParam);
    const bool isRepeat = (pMsg->lParam & (1 << 30)) != 0; // key was already down: auto-repeat
    // Remote keys: extra keys for Back / OK act like Esc / Enter
    if (const UINT extraBack = ExtraKey(true); extraBack && vk == extraBack) {
        vk = VK_ESCAPE;
    } else if (const UINT extraOk = ExtraKey(false); extraOk && vk == extraOk) {
        vk = VK_RETURN;
    }

    // Back with nothing open: instead of MPC-HC leaving fullscreen, a small menu with Exit, Enter / Exit
    // fullscreen and Restart player (with or without a file). The rest of that key press is swallowed.
    // TV option "Back with nothing open": 0 the power menu, 1 leave fullscreen, 2 exit the player.
    if (!m_bOpen && (vk == VK_ESCAPE || vk == VK_BACK || vk == VK_BROWSER_BACK)) {
        const int backAction = CTvMode::Get(TvOpt::BackAction);
        if (backAction == 1) {
            if (!pMainFrame->IsFullScreenMode()) {
                return false; // windowed: the key keeps its MPC-HC meaning
            }
            if (!isRepeat) {
                pMainFrame->PostMessage(WM_COMMAND, ID_VIEW_FULLSCREEN);
            }
            return true;
        }
        if (backAction == 2) {
            if (!isRepeat) {
                pMainFrame->PostMessage(WM_COMMAND, ID_FILE_EXIT);
            }
            return true;
        }
        if (!isRepeat && Open(pMainFrame, Page::Quit, Page::None)) {
            m_bSwallowBack = true;
        }
        return true;
    }

    if (!m_bOpen) {
        if (pMainFrame->GetLoadState() != MLS::LOADED) {
            return false;
        }
        // A key with a hold action (Button / key mapping): a press waits; let go = what the key does
        // normally (ReleaseArrow), held 0.6 s = the hold action (the rest of that hold does nothing)
        if (vk != VK_ESCAPE && vk != VK_BACK && vk != VK_BROWSER_BACK) {
            const UINT raw = static_cast<UINT>(pMsg->wParam); // the key itself (its key-up ends the wait)
            if (const int hold = HoldFor(raw)) {
                m_pMainFrame = pMainFrame;
                const ULONGLONG now = GetTickCount64();
                if (!isRepeat || m_nArrowPendingVk != raw) {
                    m_nArrowPendingVk = raw;
                    m_nArrowDownAt = now;
                    m_bArrowHoldDone = false;
                } else if (!m_bArrowHoldDone && now - m_nArrowDownAt >= 600) {
                    m_bArrowHoldDone = true;
                    pMainFrame->PostMessage(WM_COMMAND, static_cast<UINT>(hold));
                }
                m_nArrowLastKey = now;
                ArmArrowTimer();
                return true;
            }
        }
        // The control bar key (Button / key mapping; default Down). Another key set there gives
        // Down back its normal MPC-HC meaning.
        if (const UINT barKey = BarKey(); barKey && vk == barKey) {
            return Open(pMainFrame, Page::Bar, Page::None); // false: the key keeps its normal meaning
        }
        switch (vk) {
            case VK_RETURN:
                // OK toggles play/pause. Pausing brings up the bar through OnPaused, like
                // every other way of pausing (mouse click, media key, toolbar).
                if (!isRepeat) {
                    pMainFrame->PostMessage(WM_COMMAND, ID_PLAY_PLAYPAUSE);
                }
                return true;
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN: {
                // Arrows with no menu open: a command set for the arrow in Button / key mapping, else
                // Left / Right seek and Up / Down do what MPC-HC has on them (holds: see above)
                m_pMainFrame = pMainFrame;
                return RunArrowAction(ArrowAction(ArrowIndex(vk) * 2), vk, isRepeat, false);
            }
            default:
                return false;
        }
    }
    return HandleKeyDown(vk, isRepeat);
}

// Arrow keys during playback --------------------------------------------------
// Eight actions (Left, Right, Up, Down; each tap and hold), set in Button / key mapping. Stored as
// TvMode "ArrowAction0".."7"; not stored = the default (Left / Right seek, Up MPC-HC's own key,
// Down the same unless it's the control bar key, which is checked first; holds do nothing).

int CTvMenu::ArrowIndex(UINT vk)
{
    return vk == VK_LEFT ? 0 : vk == VK_RIGHT ? 1 : vk == VK_UP ? 2 : 3;
}

// MPC-HC's six jump commands: the only actions an arrow key can be given (Button / key mapping > Jumps)
bool CTvMenu::IsJumpCommand(int id)
{
    return id == ID_PLAY_SEEKFORWARDSMALL || id == ID_PLAY_SEEKBACKWARDSMALL || id == ID_PLAY_SEEKFORWARDMED ||
           id == ID_PLAY_SEEKBACKWARDMED || id == ID_PLAY_SEEKFORWARDLARGE || id == ID_PLAY_SEEKBACKWARDLARGE;
}

int CTvMenu::ArrowAction(int slot)
{
    // Taps only (holds are in HoldKeys), and only a command set from a Button / key mapping row; the
    // older TV-only actions of the removed "Arrows during playback" rows fall back to the default
    CString name;
    name.Format(_T("ArrowAction%d"), slot);
    const int v = CTvMode::GetValue(name, kArrowDefault);
    if (slot % 2 == 0 && v > 0) {
        return v;
    }
    switch (slot) {
        case 0: return kActSeekBack;
        case 2: return kActSeekFwd;
        case 4: case 6: return kActPass;
        default: return kActNothing;
    }
}

void CTvMenu::SetArrowAction(int slot, int action)
{
    CString name;
    name.Format(_T("ArrowAction%d"), slot);
    CTvMode::SetValue(name, action);
}

// What an arrow can do: TV pages and seeking first, then every command in the key list
std::vector<int> CTvMenu::ArrowChoices(bool hold)
{
    std::vector<int> v = { kActNothing };
    if (!hold) {
        v.push_back(kActPass);
    }
    for (int a : { kActSeekBack, kActSeekFwd, kActShowBar, kActSubtitles, kActAudio, kActChapters, kActInfo, kActSettings }) {
        v.push_back(a);
    }
    for (const KeyAction& k : kKeyActions) {
        if (k.id > 0 && k.id != ID_TV_MENU && k.id != ID_TV_AUDIO && k.id != ID_TV_SUBTITLES) {
            v.push_back(k.id);
        }
    }
    return v;
}

CStringW CTvMenu::ArrowActionName(int action)
{
    switch (action) {
        case kActNothing: return L"Nothing";
        case kActPass: return L"MPC-HC's own key";
        case kActSeekBack: return L"Seek back";
        case kActSeekFwd: return L"Seek forward";
        case kActShowBar: return L"Show the control bar";
        case kActSubtitles: return L"Subtitles list";
        case kActAudio: return L"Audio list";
        case kActChapters: return L"Chapters list";
        case kActInfo: return L"Info";
        case kActSettings: return L"Settings";
        default:
            for (const KeyAction& k : kKeyActions) {
                if (k.id == action) {
                    return k.name;
                }
            }
            return L"?";
    }
}

// Runs an arrow's action. deferred: the key press itself already went (a tap / hold decided later),
// so "MPC-HC's own key" runs MPC-HC's command for that key here. False = let MPC-HC have the key.
bool CTvMenu::RunArrowAction(int action, UINT vk, bool isRepeat, bool deferred)
{
    CMainFrame* pMF = m_pMainFrame;
    if (!pMF) {
        return false;
    }
    switch (action) {
        case kActNothing:
            return true;
        case kActPass:
            if (!deferred) {
                return false;
            }
            for (POSITION pos = AfxGetAppSettings().wmcmds.GetHeadPosition(); pos;) {
                const wmcmd& wc = AfxGetAppSettings().wmcmds.GetNext(pos);
                if (wc.key == vk && (wc.fVirt & (FCONTROL | FSHIFT | FALT)) == 0) {
                    pMF->PostMessage(WM_COMMAND, wc.cmd);
                    break;
                }
            }
            return true;
        case kActSeekBack:
        case kActSeekFwd: {
            // shown on the bar's seek row, in a small time box, or (Nothing) without anything on
            // screen, jumping right away
            const int dir = action == kActSeekFwd ? 1 : -1;
            const int display = CTvMode::Get(TvOpt::ShowOnSeek); // 0 bar, 1 time box, 2 nothing
            if (display == 2) {
                Scrub(dir, isRepeat); // instant (see InstantSeek)
                return true;
            }
            if (m_bOpen || Open(pMF, display == 0 ? Page::Bar : Page::MiniSeek, Page::None)) {
                m_nBarRow = 0;
                Scrub(dir, isRepeat);
                return true;
            }
            return false;
        }
        case kActShowBar:
            if (!isRepeat && !m_bOpen) {
                Open(pMF, Page::Bar, Page::None);
            }
            return true;
        case kActSubtitles:
        case kActAudio:
            if (!isRepeat) {
                OnCommand(pMF, action == kActAudio ? ID_TV_AUDIO : ID_TV_SUBTITLES);
            }
            return true;
        case kActChapters:
        case kActInfo:
        case kActSettings:
            // opened from the bar, so Back returns to it
            if (!isRepeat && (m_bOpen || Open(pMF, Page::Bar, Page::None))) {
                PressBarButton(action == kActChapters ? BarButton::Chapters : action == kActInfo ? BarButton::VideoInfo : BarButton::Settings);
            }
            return true;
        default:
            if (action > 0) {
                pMF->PostMessage(WM_COMMAND, static_cast<UINT>(action)); // an MPC-HC command
            }
            return true;
    }
}

// The key-up of a waiting arrow: its tap action (unless the hold action already ran)
void CTvMenu::ReleaseArrow()
{
    if (!m_nArrowPendingVk) {
        return;
    }
    UINT vk = m_nArrowPendingVk;
    if (ExtraKey(false) && vk == ExtraKey(false)) {
        vk = VK_RETURN; // the extra OK key acts as OK
    }
    const bool held = m_bArrowHoldDone;
    m_nArrowPendingVk = 0;
    m_bArrowHoldDone = false;
    if (m_nArrowTimer) {
        KillTimer(nullptr, m_nArrowTimer);
        m_nArrowTimer = 0;
    }
    if (!held && m_pMainFrame && m_pMainFrame->GetLoadState() == MLS::LOADED) {
        // let go before it counted as held: what a plain press of this key does
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
            RunArrowAction(ArrowAction(ArrowIndex(vk) * 2), vk, false, true);
        } else if (BarKey() == vk) {
            Open(m_pMainFrame, Page::Bar, Page::None);
        } else if (vk == VK_RETURN) {
            m_pMainFrame->PostMessage(WM_COMMAND, ID_PLAY_PLAYPAUSE);
        } else {
            RunArrowAction(kActPass, vk, false, true); // MPC-HC's own command on that key
        }
    }
}

// Button / key mapping, a row's choices are showing: Left / Right pick Set / Set as hold / Clear
// (TV-only keys: Set / Clear), OK does it, Back puts the choices away, Up / Down move on
bool CTvMenu::HandleKeyEditKey(UINT vk, bool isRepeat)
{
    const int row = m_nKeyEditRow;
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || m_rows[row].kind != RowKind::KeyRow) {
        m_nKeyEditRow = -1;
        return false;
    }
    const int ki = m_rows[row].data;
    const int action = kKeyActions[std::clamp(ki, 0, static_cast<int>(_countof(kKeyActions)) - 1)].id;
    const int parts = action < 0 ? 2 : 3;
    switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
            m_nKeyEditPart = (m_nKeyEditPart + (vk == VK_RIGHT ? 1 : -1) + parts) % parts;
            Render();
            return true;
        case VK_RETURN:
            if (!isRepeat) {
                const int choice = (action < 0 && m_nKeyEditPart == 1) ? 2 : m_nKeyEditPart;
                m_nKeyEditRow = -1;
                if (choice == 2) {
                    ClearAction(action);
                } else {
                    m_nListenKey = ki; // the next key pressed is taken (HandleKeyCapture)
                    m_bListenHold = (choice == 1);
                }
                m_strKeyMessage.Empty();
                BuildRows();
                Render();
            }
            return true;
        case VK_ESCAPE:
        case VK_BACK:
        case VK_BROWSER_BACK:
            if (!isRepeat) {
                m_nKeyEditRow = -1;
                Render();
            }
            return true;
        case VK_UP:
        case VK_DOWN:
            m_nKeyEditRow = -1;
            return false; // and move on as usual
        default:
            return true;
    }
}

// Holds: "set as a hold" in Button / key mapping. Any plain key; stored as TvMode "HoldKeys" =
// "vk:command,vk:command" (one action per key). They act only while no bar or menu is shown.
std::map<UINT, int> CTvMenu::HoldMap()
{
    std::map<UINT, int> m;
    const CString s = CTvMode::GetText(_T("HoldKeys"));
    int pos = 0;
    for (CString tok = s.Tokenize(_T(","), pos); pos >= 0; tok = s.Tokenize(_T(","), pos)) {
        UINT vk = 0;
        int action = 0;
        if (_stscanf_s(tok, _T("%u:%d"), &vk, &action) == 2 && vk && action > 0) {
            m[vk] = action;
        }
    }
    return m;
}

void CTvMenu::SaveHoldMap(const std::map<UINT, int>& m)
{
    CString s;
    for (const auto& kv : m) {
        s.AppendFormat(_T("%s%u:%d"), s.IsEmpty() ? _T("") : _T(","), kv.first, kv.second);
    }
    CTvMode::SetText(_T("HoldKeys"), s);
}

int CTvMenu::HoldFor(UINT vk)
{
    const auto m = HoldMap();
    const auto it = m.find(vk);
    return it == m.end() ? 0 : it->second;
}

// Takes every key off an action: its MPC-HC key (or the TV-only key), arrows and holds
void CTvMenu::ClearAction(int action)
{
    AssignKey(action, 0, 0);
    if (action > 0) {
        for (int a = 0; a < 4; a++) {
            if (ArrowAction(a * 2) == action) {
                SetArrowAction(a * 2, kArrowDefault); // the arrow goes back to its normal job
            }
        }
        auto m = HoldMap();
        for (auto it = m.begin(); it != m.end();) {
            it = (it->second == action) ? m.erase(it) : std::next(it);
        }
        SaveHoldMap(m);
    }
}

// No repeat or key-up for a second (the key-up went elsewhere): count it as let go
void CTvMenu::ArmArrowTimer()
{
    if (m_nArrowTimer) {
        KillTimer(nullptr, m_nArrowTimer);
    }
    m_nArrowTimer = SetTimer(nullptr, 0, 1100, &CTvMenu::OnArrowTimer);
}

void CALLBACK CTvMenu::OnArrowTimer(HWND /*hWnd*/, UINT /*msg*/, UINT_PTR /*id*/, DWORD /*time*/)
{
    CTvMenu& m = Instance();
    if (m.m_nArrowTimer) {
        KillTimer(nullptr, m.m_nArrowTimer);
        m.m_nArrowTimer = 0;
    }
    if (m.m_nArrowPendingVk && GetTickCount64() - m.m_nArrowLastKey >= 1000) {
        m.ReleaseArrow();
    }
}

void CTvMenu::OnCommand(CMainFrame* pMainFrame, UINT nID)
{
    if (!AfxGetAppSettings().bTvMode || pMainFrame->GetLoadState() != MLS::LOADED) {
        return;
    }
    m_nLastInput = GetTickCount64();
    switch (nID) {
        case ID_TV_MENU:
            if (m_bOpen) {
                Close();
            } else {
                Open(pMainFrame, Page::Bar, Page::None);
            }
            break;
        case ID_TV_AUDIO:
        case ID_TV_SUBTITLES: {
            const Page page = (nID == ID_TV_AUDIO) ? Page::Audio : Page::Subtitles;
            if (m_bOpen && m_page == page) {
                Close(); // same key again closes, like a toggle
            } else if (m_bOpen) {
                ApplyPending();
                OpenChild(page, -1);
            } else {
                Open(pMainFrame, page, Page::None);
            }
            break;
        }
    }
}

void CTvMenu::OnPaused(CMainFrame* pMainFrame)
{
    if (!AfxGetAppSettings().bTvMode) {
        return;
    }
    m_pMainFrame = pMainFrame;
    if (!m_bOpen && CTvMode::IsOn(TvOpt::ShowOnPause)) {
        Open(pMainFrame, Page::Bar, Page::None); // stays up while paused
    }
    m_nLastInput = GetTickCount64(); // the pause screen counts from here
    ArmPauseTimer(PauseScreenMs());
}

// Pause screen ---------------------------------------------------------------
// After "Pause screen after" seconds paused without a key (TV options > Control bar), a card with
// the title, the time and when it would end replaces the bar. Not over other menus (it waits).

void CTvMenu::ArmPauseTimer(int ms)
{
    if (m_nPauseTimer) {
        KillTimer(nullptr, m_nPauseTimer);
        m_nPauseTimer = 0;
    }
    if (ms > 0) {
        m_nPauseTimer = SetTimer(nullptr, 0, static_cast<UINT>(ms), &CTvMenu::OnPauseTimer);
    }
}

void CALLBACK CTvMenu::OnPauseTimer(HWND /*hWnd*/, UINT /*msg*/, UINT_PTR /*id*/, DWORD /*time*/)
{
    Instance().CheckPauseScreen();
}

void CTvMenu::CheckPauseScreen()
{
    ArmPauseTimer(0); // one shot; armed again below if it has to wait
    CMainFrame* pMF = m_pMainFrame;
    const int sec = PauseScreenMs() / 1000;
    if (!AfxGetAppSettings().bTvMode || !pMF || sec <= 0 || pMF->GetLoadState() != MLS::LOADED ||
        pMF->GetMediaState() != State_Paused || (m_bOpen && m_page == Page::Pause)) {
        return;
    }
    const ULONGLONG need = static_cast<ULONGLONG>(sec) * 1000;
    const ULONGLONG idle = GetTickCount64() - m_nLastInput;
    if (idle + 50 < need) {
        ArmPauseTimer(static_cast<int>(need - idle)); // a key in the meantime: wait the rest
        return;
    }
    if ((m_bOpen && m_page != Page::Bar) || m_bDragSeek || m_rtPreview >= 0 || m_nMovingItem >= 0) {
        ArmPauseTimer(static_cast<int>(need)); // another menu is open: don't cover it
        return;
    }
    GetCursorPos(&m_ptPauseMouse); // the mouse has to move from here (screen) to bring the bar back
    CTvInfo::Prepare(pMF);         // the file's title tag (read in the background; the card updates)
    if (m_bOpen) {
        ShowPage(Page::Pause, 0);
    } else {
        Open(pMF, Page::Pause, Page::None);
    }
}

void CTvMenu::OnMediaLoaded(CMainFrame* pMainFrame)
{
    CTvProbe::Start(pMainFrame->m_pGB); // scopes measurement probe: only with MPCHC_TV_PROBE=1
    m_nLoadedRenderer = AfxGetAppSettings().iDSVideoRendererType; // what this file plays with (Video page)
    if (AfxGetAppSettings().bTvMode) {
        pMainFrame->m_wndView.HideCursorNow(); // no arrow left in the middle of the picture at the start
    }
    if (AfxGetAppSettings().bTvMode && !m_bOpen && CTvMode::IsOn(TvOpt::ShowOnStart)) {
        Open(pMainFrame, Page::Bar, Page::None); // hides on its own once playing
    }
}

void CTvMenu::OnCloseMedia()
{
    // Must run before the graph (and with it madVR) is torn down.
    // A track chosen with Left/Right but not applied yet belongs to the old file: drop it,
    // and so is a seek preview.
    CTvProbe::Stop();
    CTvScopes::Stop();
    m_nLoadedRenderer = -1;
    CancelApplyTimer();
    ArmPauseTimer(0);
    m_bOkPending = false;
    m_nPendingAudio = kNoPending;
    m_nPendingSubtitle = kNoPending;
    m_rtPreview = -1;
    RemoveNow();
    m_pOsd.Release();
    m_pMadVr.Release();
}

bool CTvMenu::HandleKeyDown(UINT vk, bool isRepeat)
{
    m_nLastInput = GetTickCount64();
    if (g_perfOn) {
        if (g_perfKey.QuadPart == 0) {
            QueryPerformanceCounter(&g_perfKey); // perf log: key -> next finished render
        }
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"mpc-hc-tv-perf.log");
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path, L"a") == 0 && fp) {
            fprintf(fp, "%llu key vk=%u page=%d repeat=%d barRow=%d button=%d\n", GetTickCount64(), vk, static_cast<int>(m_page),
                    isRepeat ? 1 : 0, m_nBarRow, static_cast<int>(m_barButton));
            fclose(fp);
        }
    }
    // Pause screen: OK plays, Back hides it, the arrows bring up the bar; other keys go to MPC-HC
    // (a play key resumes, which closes it). It comes back after the same quiet time while paused.
    if (m_page == Page::Pause) {
        if (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_BACK || vk == VK_BROWSER_BACK) {
            if (!isRepeat) {
                Close();
                if (vk == VK_RETURN) {
                    m_pMainFrame->PostMessage(WM_COMMAND, ID_PLAY_PLAYPAUSE);
                }
                ArmPauseTimer(PauseScreenMs());
            }
            return true;
        }
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) {
            m_nBarRow = 1;
            m_barButton = BarButton::PlayPause;
            ShowPage(Page::Bar, 0);
            ArmPauseTimer(PauseScreenMs());
            return true;
        }
        return false;
    }
    // The control bar key (when it isn't an arrow or OK) never reaches MPC-HC while a menu is open
    // (its repeats after opening the bar would run MPC-HC's command on that key); pressed again on
    // the bar it hides the bar
    if (const UINT barKey = BarKey(); barKey && vk == barKey && vk != VK_UP && vk != VK_DOWN && vk != VK_LEFT &&
                                      vk != VK_RIGHT && vk != VK_RETURN && vk != VK_ESCAPE && vk != VK_BACK) {
        if (!isRepeat && m_page == Page::Bar) {
            Close();
        }
        return true;
    }
    // Back held down (the key repeats): close everything, from any depth
    if (vk == VK_ESCAPE || vk == VK_BACK || vk == VK_BROWSER_BACK) {
        const ULONGLONG now = GetTickCount64();
        if (!isRepeat) {
            m_nBackDownAt = now;
        } else if (m_nBackDownAt && now - m_nBackDownAt >= 600) {
            m_nBackDownAt = 0;
            m_crumbs.clear();
            m_bSwallowBack = true; // the key is still down: keep its repeats away from MPC-HC
            Close();
            return true;
        }
    }
    if (m_page == Page::Bar) {
        return HandleBarKey(vk, isRepeat);
    }
    if (m_page == Page::MiniSeek) {
        return HandleMiniSeekKey(vk, isRepeat);
    }
    if (m_page == Page::ColourPicker) {
        return HandlePickerKey(vk, isRepeat);
    }
    if (m_page == Page::Scopes) {
        // Scopes: OK goes on to the next tab (Video), Back as from the info panels
        if (vk == VK_RETURN) {
            if (!isRepeat) {
                SwitchInfoKind();
            }
            return true;
        }
        if (vk == VK_ESCAPE || vk == VK_BACK || vk == VK_BROWSER_BACK) {
            if (!isRepeat) {
                Back();
            }
            return true;
        }
        return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT; // nothing to move
    }
    if (IsInfoPage(m_page)) {
        switch (vk) {
            case VK_UP:
            case VK_DOWN:
                Move(vk == VK_DOWN ? 1 : -1); // scrolls
                return true;
            case VK_LEFT:
            case VK_RIGHT:
                // Basic -> Extended -> Nightmare (wraps)
                SetInfoLevel((InfoLevel() + (vk == VK_LEFT ? -1 : 1) + CTvInfo::kLevels) % CTvInfo::kLevels);
                m_nInfoScroll = 0;
                BuildInfo();
                Render();
                return true;
            case VK_RETURN:
                if (!isRepeat) {
                    SwitchInfoKind(); // OK: video <-> audio
                }
                return true;
            case VK_ESCAPE:
            case VK_BACK:
            case VK_BROWSER_BACK:
                if (!isRepeat) {
                    Back();
                }
                return true;
            default:
                return false;
        }
    }
    if (m_nKeyEditRow >= 0 && m_page == Page::OptionsSection && HandleKeyEditKey(vk, isRepeat)) {
        return true;
    }
    if (m_page == Page::MadVR && HandleMadVrKey(vk, isRepeat)) {
        return true;
    }
    const bool moving = (m_nMovingItem >= 0); // Bar buttons submenu: a button is picked up
    switch (vk) {
        case VK_UP:
            if (moving) {
                MoveBarItem(-1);
            } else {
                Move(-1, !isRepeat || CTvMode::IsOn(TvOpt::WrapWhileHeld)); // the top may wrap to the bottom (options)
            }
            return true;
        case VK_DOWN:
            if (moving) {
                MoveBarItem(+1);
            } else {
                Move(+1, !isRepeat || CTvMode::IsOn(TvOpt::WrapWhileHeld));
            }
            return true;
        case VK_LEFT:
        case VK_RIGHT:
            // a value to change, else (two columns, e.g. Button / key mapping) the other column
            if (!AdjustRow(vk == VK_RIGHT ? 1 : -1) && m_bTwoColumns) {
                MoveColumn(vk == VK_RIGHT ? 1 : -1);
            }
            return true; // never seek while a list is open
        case VK_RETURN:
            if (!isRepeat) {
                Activate();
            }
            return true;
        case VK_ESCAPE:
        case VK_BACK:
        case VK_BROWSER_BACK:
            if (!isRepeat) {
                if (moving) {
                    m_nMovingItem = -1; // put the button down where it is
                    BuildRows();
                    Render();
                } else {
                    Back();
                }
            }
            return true;
        default:
            return false; // play/pause, media keys etc. keep working
    }
}

bool CTvMenu::HandleBarKey(UINT vk, bool isRepeat)
{
    switch (vk) {
        case VK_UP:
            if (m_nBarRow == 1 && m_barButton == BarButton::Volume) {
                m_pMainFrame->SendMessage(WM_COMMAND, ID_VOLUME_UP); // on the volume control: louder
                m_nLastBarRender = 0;
                Render();
            } else if (m_nBarRow == 1) {
                m_nBarRow = 0;
                Render();
            } else if (!isRepeat) {
                Close(); // Up from the seek row hides the bar
            }
            return true;
        case VK_DOWN:
            if (m_nBarRow == 1 && m_barButton == BarButton::Volume) {
                m_pMainFrame->SendMessage(WM_COMMAND, ID_VOLUME_DOWN); // quieter
                m_nLastBarRender = 0;
                Render();
                return true;
            }
            if (m_nBarRow == 0) {
                m_nBarRow = 1;
                Render();
            }
            return true;
        case VK_LEFT:
        case VK_RIGHT: {
            const int dir = (vk == VK_RIGHT) ? 1 : -1;
            if (m_nBarRow == 0) {
                Scrub(dir, isRepeat);
            } else {
                // Step through the buttons actually shown (the stats button is optional).
                const auto visible = VisibleButtons();
                auto it = std::find(visible.begin(), visible.end(), m_barButton);
                int i = (it == visible.end()) ? 2 : static_cast<int>(it - visible.begin());
                i = std::clamp(i + dir, 0, static_cast<int>(visible.size()) - 1);
                m_barButton = visible[i];
                Render();
            }
            return true;
        }
        case VK_RETURN:
            if (CTvMode::Get(TvOpt::HoldOk) == 0) {
                if (!isRepeat) {
                    BarOk();
                }
                return true;
            }
            // "Hold OK on the bar": a short press acts when OK is let go (ReleaseOk); held for 0.6 s
            // it opens Info or Settings instead, and the rest of that hold does nothing
            m_nOkLastKey = GetTickCount64();
            if (!isRepeat) {
                m_bOkPending = true;
                m_nOkDownAt = GetTickCount64();
            } else if (m_bOkPending && GetTickCount64() - m_nOkDownAt >= 600) {
                m_bOkPending = false;
                PressBarButton(CTvMode::Get(TvOpt::HoldOk) == 2 ? BarButton::Settings : BarButton::VideoInfo);
            }
            return true;
        case VK_ESCAPE:
        case VK_BACK:
        case VK_BROWSER_BACK:
            if (!isRepeat) {
                if (m_rtPreview >= 0) {
                    CancelSeek();
                } else {
                    Close();
                }
            }
            return true;
        default:
            return false;
    }
}

bool CTvMenu::HandleMiniSeekKey(UINT vk, bool isRepeat)
{
    switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
            Scrub(vk == VK_RIGHT ? 1 : -1, isRepeat);
            return true;
        case VK_RETURN:
            if (!isRepeat) {
                ApplySeek(); // jump now
                Close();
            }
            return true;
        case VK_ESCAPE:
        case VK_BACK:
        case VK_BROWSER_BACK:
            if (!isRepeat) {
                CancelSeek();
                Close();
            }
            return true;
        case VK_DOWN:
            // Down brings up the full bar, keeping the preview on its seek row.
            m_page = Page::Bar;
            m_nBarRow = 0;
            Render();
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Mouse

bool CTvMenu::ToOutput(const MSG* pMsg, POINT& pt) const
{
    if (!m_pMainFrame || !m_pMainFrame->m_pVideoWnd || !::IsWindow(m_pMainFrame->m_pVideoWnd->m_hWnd)) {
        return false;
    }
    pt = { GET_X_LPARAM(pMsg->lParam), GET_Y_LPARAM(pMsg->lParam) };
    if (pMsg->message != WM_MOUSEWHEEL && pMsg->message != WM_MOUSEHWHEEL) {
        ::ClientToScreen(pMsg->hwnd, &pt); // wheel messages already carry screen coordinates
    }
    ::ScreenToClient(m_pMainFrame->m_pVideoWnd->m_hWnd, &pt);
    pt.x -= m_outRect.left; // OSD positions are relative to madVR's output rect
    pt.y -= m_outRect.top;
    return true;
}

int CTvMenu::HitRow(POINT pt) const
{
    if (m_hitRowsVisible <= 0) {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(m_hitRowRect.size()) && i < static_cast<int>(m_rows.size()); i++) {
        if (!IsSkipped(m_rows[i]) && PtInRect(&m_hitRowRect[i], pt)) {
            return i;
        }
    }
    return -1;
}

// Which value arrow of `row` the mouse is on: 0 none, else (row + 1) * 2 + (0 left, 1 right).
int CTvMenu::RowArrowAt(int row, POINT pt) const
{
    if (row < 0 || row >= static_cast<int>(m_hitValueLeft.size()) || row >= static_cast<int>(m_hitValueRightRow.size()) ||
        m_hitArrowW <= 0) {
        return 0;
    }
    const int valueLeft = m_hitValueLeft[row];
    const int valueRight = m_hitValueRightRow[row];
    if (valueLeft < 0) {
        return 0;
    }
    if (pt.x >= valueLeft - m_hitArrowW / 2 && pt.x < valueLeft + m_hitArrowW) {
        return (row + 1) * 2;
    }
    if (pt.x >= valueRight - m_hitArrowW && pt.x < valueRight + m_hitArrowW / 2) {
        return (row + 1) * 2 + 1;
    }
    return 0;
}

bool CTvMenu::HitButton(POINT pt, BarButton& button) const
{
    for (const auto& b : m_hitButtons) {
        const int dx = pt.x - b.center.x, dy = pt.y - b.center.y;
        if (dx * dx + dy * dy <= m_hitButtonR * m_hitButtonR) {
            button = b.id;
            return true;
        }
    }
    return false;
}

bool CTvMenu::HitSeek(POINT pt) const
{
    return m_hitSeekX1 > m_hitSeekX0 && pt.x >= m_hitSeekX0 - m_hitSeekHalfH && pt.x <= m_hitSeekX1 + m_hitSeekHalfH &&
           std::abs(pt.y - m_hitSeekCy) <= m_hitSeekHalfH;
}

REFERENCE_TIME CTvMenu::TimeAtX(int x) const
{
    const REFERENCE_TIME dur = m_pMainFrame ? m_pMainFrame->GetDur() : 0;
    if (dur <= 0 || m_hitSeekX1 <= m_hitSeekX0) {
        return -1;
    }
    const double f = std::clamp((x - m_hitSeekX0) / static_cast<double>(m_hitSeekX1 - m_hitSeekX0), 0.0, 1.0);
    return static_cast<REFERENCE_TIME>(dur * f);
}

void CTvMenu::RequestRender()
{
    // Mouse moves arrive far faster than redraws are useful; the tick picks up the rest.
    if (GetTickCount64() - m_nLastRender >= 33) {
        Render();
    } else {
        m_bDirty = true;
    }
}

bool CTvMenu::HandleMouse(CMainFrame* pMainFrame, const MSG* pMsg)
{
    const UINT msg = pMsg->message;

    // Pause screen: the mouse brings up the bar instead (a click also shows it, nothing else)
    if (m_bOpen && m_page == Page::Pause && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN)) {
        POINT pt = {};
        GetCursorPos(&pt); // screen coordinates, like m_ptPauseMouse
        if (msg == WM_LBUTTONDOWN || abs(pt.x - m_ptPauseMouse.x) + abs(pt.y - m_ptPauseMouse.y) > 8) {
            m_nLastInput = GetTickCount64();
            m_nBarRow = 1;
            m_barButton = BarButton::PlayPause;
            ShowPage(Page::Bar, 0);
            ArmPauseTimer(PauseScreenMs());
            if (msg == WM_LBUTTONDOWN) {
                m_bSwallowLUp = true;
                return true;
            }
        }
    }

    // No file loaded: a click on the empty player (the logo) opens a file, as File > Quick Open
    if (!m_bOpen && msg == WM_LBUTTONDOWN && pMainFrame->GetLoadState() == MLS::CLOSED) {
        HWND hView = pMainFrame->m_wndView.m_hWnd;
        if (hView && (pMsg->hwnd == hView || ::IsChild(hView, pMsg->hwnd))) {
            m_bSwallowLUp = true;
            pMainFrame->PostMessage(WM_COMMAND, ID_FILE_OPENQUICK);
            return true;
        }
    }

    // The button-up that belongs to a press we handled must not reach MPC-HC either
    // (its click-to-pause and context menu act on button-up).
    if (msg == WM_LBUTTONUP) {
        if (m_nDragPicker >= 0) {
            m_nDragPicker = -1;
            m_bSwallowLUp = false;
            return true;
        }
        if (m_bDragScroll) {
            m_bDragScroll = false;
            m_bSwallowLUp = false;
            return true;
        }
        if (m_bDragVolume) {
            m_bDragVolume = false;
            m_bSwallowLUp = false;
            return true;
        }
        if (m_bDragSeek) {
            m_bDragSeek = false;
            if (::GetCapture() == pMsg->hwnd) {
                ::ReleaseCapture();
            }
            EndDragSeek(); // final, exact position (and playing again if the drag paused it)
            m_bSwallowLUp = false;
            return true;
        }
        if (m_bSwallowLUp) {
            m_bSwallowLUp = false;
            return true;
        }
        return false;
    }
    if (msg == WM_RBUTTONUP) {
        if (m_bSwallowRUp) {
            m_bSwallowRUp = false;
            return true;
        }
        return false;
    }

    if (msg == WM_MOUSEMOVE) {
        POINT screen = { GET_X_LPARAM(pMsg->lParam), GET_Y_LPARAM(pMsg->lParam) };
        ::ClientToScreen(pMsg->hwnd, &screen);
        const bool firstMove = (m_lastMouseScreen.x == LONG_MIN);
        const bool moved = !firstMove && (std::abs(screen.x - m_lastMouseScreen.x) > 8 || std::abs(screen.y - m_lastMouseScreen.y) > 8);
        if (firstMove || moved) {
            m_lastMouseScreen = screen;
        }
        if (!m_bOpen) {
            // A real move (not jitter, not the cursor re-appearing) shows the bar, like Jellyfin.
            if (moved && pMainFrame->GetLoadState() == MLS::LOADED && CTvMode::IsOn(TvOpt::ShowOnMouse)) {
                Open(pMainFrame, Page::Bar, Page::None);
            }
            return false;
        }
        if (!moved && !m_bDragSeek && !m_bDragScroll && m_nDragPicker < 0) {
            return false;
        }
        m_nLastInput = GetTickCount64();

        POINT pt;
        if (!ToOutput(pMsg, pt)) {
            return false;
        }
        if (m_bDragScroll) {
            ScrollToY(pt.y, true); // dragging the scrollbar thumb
            return false;
        }
        if (m_bDragVolume) {
            SetVolumeFromX(pt.x); // dragging the volume level
            return false;
        }
        if (m_nDragPicker >= 0) {
            PickerSetFromX(m_nDragPicker, pt.x); // dragging along a colour bar
            return false;
        }
        if (m_page == Page::ColourPicker) {
            for (int i = 0; i < 3; i++) {
                if (PtInRect(&m_hitPickerBar[i], pt) && m_nFocus != i) {
                    m_nFocus = i;
                    RequestRender();
                }
            }
            return false;
        }
        if (m_page == Page::Bar) {
            BarButton b;
            const REFERENCE_TIME oldHover = m_rtHover;
            m_rtHover = -1;
            if (m_bDragSeek) {
                // Dragging: the video follows (live seeks, throttled), anywhere on screen
                const REFERENCE_TIME t = TimeAtX(std::clamp<int>(pt.x, m_hitSeekX0, m_hitSeekX1));
                if (t >= 0) {
                    m_rtPreview = t;
                    DragSeekNow(false);
                }
                RequestRender();
            } else if (HitButton(pt, b)) {
                if (m_nBarRow != 1 || m_barButton != b) {
                    m_nBarRow = 1;
                    m_barButton = b;
                    RequestRender();
                }
            } else if (HitSeek(pt)) {
                m_nBarRow = 0;
                m_rtHover = TimeAtX(pt.x);
                RequestRender();
            }
            if (oldHover >= 0 && m_rtHover < 0) {
                RequestRender(); // hover bubble gone
            }
        } else if (m_page != Page::MiniSeek) {
            // Near the right edge of the window: show the scrollbar (lists / info pages).
            const int outW = m_outRect.right - m_outRect.left;
            // ...or near the right edge of the panel itself (the scrollbar sits there).
            const int trackW = m_hitScrollTrack.right - m_hitScrollTrack.left;
            const bool nearBar = !IsRectEmpty(&m_hitScrollTrack) && pt.x >= m_hitScrollTrack.left - trackW * 2 &&
                                 pt.y >= m_elemRect.top && pt.y < m_elemRect.bottom;
            const bool edge = (outW > 0 && pt.x >= outW * 95 / 100) || nearBar;
            bool redraw = (edge != m_bMouseRightEdge);
            m_bMouseRightEdge = edge;
            if (IsInfoPage(m_page)) {
                int hover = 0; // level pill under the mouse (1-based)
                for (int l = 0; l < CTvInfo::kLevels; l++) {
                    if (PtInRect(&m_hitLevelPill[l], pt)) {
                        hover = l + 1;
                    }
                }
                redraw = redraw || (hover != m_nArrowHover);
                m_nArrowHover = hover;
            } else {
                const int row = HitRow(pt);
                if (row >= 0 && row != m_nFocus && m_nMovingItem < 0 && m_nMadVrPart < 0) { // not while a bar button is picked up / a madVR row is edited
                    m_nFocus = row;
                    redraw = true;
                }
                const int hover = RowArrowAt(row, pt);
                if (hover != m_nRowArrowHover) {
                    m_nRowArrowHover = hover;
                    redraw = true;
                }
            }
            if (redraw) {
                RequestRender();
            }
        }
        return false; // MPC-HC still sees moves (cursor show / hide)
    }

    if (!m_bOpen) {
        return false; // closed: clicks, right-click menu, wheel volume all stay MPC-HC's
    }

    POINT pt;
    if (!ToOutput(pMsg, pt)) {
        return false;
    }
    const bool inside = PtInRect(&m_elemRect, pt) != FALSE;
    m_nLastInput = GetTickCount64();

    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            // On the (visible) scrollbar: jump to that spot and keep following the mouse.
            if (!IsRectEmpty(&m_hitScrollTrack) && m_fScroll > 0.3f && PtInRect(&m_hitScrollTrack, pt)) {
                m_bDragScroll = true;
                ScrollToY(pt.y);
                m_bSwallowLUp = true;
                return true;
            }
            if (m_page == Page::Bar) {
                if (!inside) {
                    return false; // clicking the video still pauses / double-click fullscreen
                }
                BarButton b;
                if (!IsRectEmpty(&m_hitVolume) && PtInRect(&m_hitVolume, pt)) {
                    // Volume slider: click / drag sets the level
                    m_nBarRow = 1;
                    m_barButton = BarButton::Volume;
                    m_bDragVolume = true;
                    SetVolumeFromX(pt.x);
                } else if (HitButton(pt, b)) {
                    m_nBarRow = 1;
                    m_barButton = b;
                    PressBarButton(b);
                } else if (HitSeek(pt)) {
                    // Click or drag on the seek row: jumps right away and follows the drag
                    // (the mouse is captured, so the drag can leave the bar or the window).
                    m_nBarRow = 0;
                    m_bDragSeek = true;
                    m_rtDragSought = -1;
                    ::SetCapture(pMsg->hwnd);
                    const REFERENCE_TIME t = TimeAtX(pt.x);
                    if (t >= 0) {
                        m_rtPreview = t;
                        DragSeekNow(true);
                    }
                    Render();
                }
                m_bSwallowLUp = true;
                return true; // the bar's background doesn't pause either
            }
            if (m_page == Page::MiniSeek) {
                if (!inside) {
                    return false;
                }
                m_bSwallowLUp = true;
                return true;
            }
            // Colour picker: click / drag on a bar sets that value; outside the panel = Back.
            if (m_page == Page::ColourPicker) {
                if (inside) {
                    for (int i = 0; i < 3; i++) {
                        if (PtInRect(&m_hitPickerBar[i], pt)) {
                            m_nFocus = i;
                            m_nDragPicker = i;
                            PickerSetFromX(i, pt.x);
                        }
                    }
                } else {
                    Back();
                }
                m_bSwallowLUp = true;
                return true;
            }
            // Scopes: a click on a tab switches, a click outside the panel = Back
            if (m_page == Page::Scopes) {
                if (inside) {
                    for (int k = 0; k < 2; k++) {
                        if (PtInRect(&m_hitInfoTab[k], pt)) {
                            ShowInfoTab(k);
                        }
                    }
                } else {
                    Back();
                }
                m_bSwallowLUp = true;
                return true;
            }
            // Info panels: the level arrows switch level; a click below the panel = Back.
            if (IsInfoPage(m_page)) {
                if (inside) {
                    const int curTab = (m_page == Page::AudioInfo) ? 1 : 0;
                    bool switched = false;
                    for (int k = 0; k < InfoTabCount() && !switched; k++) {
                        if (k != curTab && PtInRect(&m_hitInfoTab[k], pt)) {
                            ShowInfoTab(k); // another tab: video / audio / scopes
                            switched = true;
                        }
                    }
                    if (switched) {
                        m_bSwallowLUp = true;
                        return true;
                    }
                    int level = -1;
                    for (int l = 0; l < CTvInfo::kLevels; l++) {
                        if (PtInRect(&m_hitLevelPill[l], pt)) {
                            level = l;
                        }
                    }
                    if (level >= 0 && level != InfoLevel()) {
                        SetInfoLevel(level);
                        m_nInfoScroll = 0;
                        BuildInfo();
                        Render();
                    }
                } else {
                    Back();
                }
                m_bSwallowLUp = true;
                return true;
            }
            // Lists: click a row = select it, click outside = Back. On a row with "< value >",
            // the left / right arrow areas step the value back / forward like Left / Right.
            if (inside) {
                const int row = HitRow(pt);
                if (row >= 0 && MadVrClick(row, pt)) {
                    // madVR row: handled (start editing / pick a part / mini view)
                } else if (row >= 0) {
                    m_nFocus = row;
                    const int hit = RowArrowAt(row, pt);
                    const int dir = hit == 0 ? 0 : (hit % 2 == 0 ? -1 : +1);
                    if (dir == 0 || !AdjustRow(dir)) {
                        Activate();
                    }
                }
            } else {
                Back();
            }
            m_bSwallowLUp = true;
            return true;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            // Right-click on the video next to the bar: MPC-HC's context menu, as usual.
            if ((m_page == Page::Bar || m_page == Page::MiniSeek) && !inside) {
                return false;
            }
            // On the bar or anywhere while a list is open: Back.
            if (m_page == Page::Bar && m_rtPreview >= 0) {
                CancelSeek();
            } else {
                Back();
            }
            m_bSwallowRUp = true;
            return true;
        case WM_MOUSEWHEEL:
            if (m_page == Page::MadVR && m_nMadVrPart >= 0) {
                MadVrEditStep(GET_WHEEL_DELTA_WPARAM(pMsg->wParam) > 0 ? 1 : -1); // editing: the wheel changes the part
                return true;
            }
            if (m_page != Page::Bar && m_page != Page::MiniSeek) {
                const int dir = GET_WHEEL_DELTA_WPARAM(pMsg->wParam) > 0 ? -1 : 1;
                if (IsInfoPage(m_page) || m_bMadVrCompact) {
                    Move(dir * (IsInfoPage(m_page) ? 3 : 1)); // info panels scroll by line; the mini view steps rows
                } else {
                    // lists: the wheel scrolls the page, 2 lines per notch (finer wheels in proportion), gliding
                    ScrollBy(-GET_WHEEL_DELTA_WPARAM(pMsg->wParam) / static_cast<float>(WHEEL_DELTA) * 2.0f);
                }
                return true;
            }
            return false; // over the bar the wheel keeps its MPC-HC meaning (volume)
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Pages

bool CTvMenu::Open(CMainFrame* pMainFrame, Page page, Page parent)
{
    m_pMainFrame = pMainFrame;
    if (!ChooseOverlay()) {
        TRACE(_T("TV menu: no video window to draw on\n"));
        return false;
    }
    m_bOpen = true;
    m_parent = parent;
    m_mainParent = parent;
    m_nMainFocus = 0;
    m_rtPreview = -1;
    m_nLastInput = GetTickCount64();
    m_fFadeTarget = 1.0f; // m_fFade keeps its value if a fade-out was still running
    if (page == Page::Bar) {
        m_nBarRow = 1;
        m_barButton = BarButton::PlayPause;
    }
    ShowPage(page, page == Page::Main ? 0 : -1);
    StartTick();
    return true;
}

void CTvMenu::Close()
{
    if (!m_bOpen) {
        return;
    }
    m_bOpen = false;
    CTvScopes::Stop(); // nothing to measure for once the menu is closing
    m_bMadVrCompact = false;
    m_nMadVrPart = -1;
    m_crumbs.clear();
    ApplyPending(); // a track picked with Left/Right counts as chosen
    m_rtPreview = -1;
    // Keys go back to the player at once; the panel fades out (state kept for drawing).
    m_fFadeTarget = 0.0f;
    if (m_fFade <= 0.0f) {
        RemoveNow();
    } else {
        StartTick();
    }
}

void CTvMenu::ForceClose()
{
    if (m_bOpen) {
        Close(); // applies a pending Left/Right track choice
    }
    RemoveNow();
}

void CTvMenu::RemoveNow()
{
    StopTick();
    m_bOpen = false;
    if (m_bDragSeek && m_pMainFrame && m_pMainFrame->m_pVideoWnd && ::GetCapture() == m_pMainFrame->m_pVideoWnd->m_hWnd) {
        ::ReleaseCapture(); // closed in the middle of a seek drag
    }
    m_bDragSeek = false;
    m_bPausedForDrag = false;
    m_rtHover = -1;
    m_nArrowHover = 0;
    m_bDragScroll = false;
    m_bDragVolume = false;
    m_fPreviewFade = m_fPreviewTarget = 0.0f;
    m_nMovingItem = -1;
    m_nListenKey = -1;
    m_bListenHold = false;
    m_nKeyEditRow = -1;
    m_bScrollGlide = false;
    m_nDragPicker = -1;
    m_bMouseRightEdge = false;
    m_fScroll = 0.0f;
    m_nScrollShownAt = 0;
    m_info.clear();
    m_hitButtons.clear();
    m_hitRowsVisible = 0;
    m_hitSeekX1 = m_hitSeekX0 = 0;
    m_elemRect = {};
    m_page = Page::None;
    m_rows.clear();
    m_fFade = 0.0f;
    m_fFadeTarget = 0.0f;
    RemoveOsd();
}

void CTvMenu::ShowPage(Page page, int focusRow)
{
    m_bResetArmed = false; // a Reset that was asking doesn't carry over to another page
    if (m_bOpen && (page != m_page || m_bForceTransition) && CTvMode::AnimScale() > 0.0f) {
        StartOldFrame();                   // the page on screen fades out and drifts away...
        m_fFade = std::min(m_fFade, 0.2f); // ...while the new one fades / slides in
    }
    m_bForceTransition = false;
    CancelXfade();
    m_fMorph = 1.0f;
    m_bMadVrCompact = false;
    m_nMadVrPart = -1;
    m_bModeTagArmed = false;
    m_page = page;
    if (page != Page::Scopes) {
        CTvScopes::Stop(); // the scopes only measure while their page is open
    }
    // A new page starts with the highlight in place; only moves within a page crossfade.
    m_rowFade.valid = false;
    m_buttonFade.valid = false;
    m_rowArrowFade.valid = false;
    m_nRowArrowHover = 0;
    m_nMovingItem = -1;
    m_nListenKey = -1;
    m_bListenHold = false;
    m_nKeyEditRow = -1;
    m_bScrollGlide = false;
    if (page == Page::Bar || page == Page::MiniSeek) {
        m_rows.clear();
        Render();
        return;
    }
    if (page == Page::ColourPicker) {
        m_rows.clear();
        m_nFocus = std::clamp(focusRow, 0, 2);
        Render();
        return;
    }
    // Scopes measure only while their page is open
    if (page == Page::Scopes) {
        m_rows.clear();
        CTvInfo::Prepare(m_pMainFrame); // the file's HDR metadata
        CTvScopes::Start(m_pMadVr);
        m_nScopesVersion = 0;
        Render();
        return;
    }
    if (IsInfoPage(page)) {
        m_rows.clear();
        m_nInfoScroll = 0;
        CTvInfo::Prepare(m_pMainFrame); // MediaInfo reads the file in the background
        BuildInfo();
        Render();
        return;
    }
    BuildRows();
    if (focusRow < 0) {
        // Track lists open on the current track, like Emby / Jellyfin.
        focusRow = 0;
        for (size_t i = 0; i < m_rows.size(); i++) {
            if (m_rows[i].checked) {
                focusRow = static_cast<int>(i);
                break;
            }
        }
    }
    m_nFocus = std::clamp(focusRow, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
    // never start on a section heading or a faded setting: the next usable row, else the one before
    {
        int f = m_nFocus;
        while (f < static_cast<int>(m_rows.size()) && IsSkipped(m_rows[f])) {
            f++;
        }
        if (f >= static_cast<int>(m_rows.size())) {
            f = m_nFocus;
            while (f > 0 && IsSkipped(m_rows[f])) {
                f--;
            }
        }
        m_nFocus = std::clamp(f, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
    }
    m_nScroll = std::max(0, (m_rows.empty() ? 0 : m_rows[m_nFocus].line) - MaxRows() + 1);
    Render();
}

void CTvMenu::OpenChild(Page page, int focusRow)
{
    if (page == Page::Main) {
        m_mainParent = m_page;
    } else {
        if (m_page == Page::Main) {
            m_nMainFocus = m_nFocus;
        }
        if ((page == Page::Lav || page == Page::MadVR) && (m_page == Page::Audio || m_page == Page::Video)) {
            m_crumbs.push_back({ m_page, m_nFocus, m_parent }); // Back returns into Audio / Video
        }
        m_parent = m_page;
    }
    m_nLastInput = GetTickCount64();
    ShowPage(page, focusRow);
}

void CTvMenu::Back()
{
    if (m_bMadVrCompact) {
        // the small live-adjust strip: back to the full madVR list, same row
        SetMadVrCompact(false);
        return;
    }
    if (!m_crumbs.empty() && (m_page == Page::Lav || (m_page == Page::MadVR && m_nMadVrSub == MvRoot))) {
        const Crumb c = m_crumbs.back();
        m_crumbs.pop_back();
        ShowPage(c.page, c.focus);
        m_parent = c.parent;
        return;
    }
    switch (m_page) {
        case Page::ColourPicker:
            ShowPage(Page::OptionsSection, m_nOptionsFocus); // back into the Look submenu
            break;
        case Page::OptionsSection:
            ShowPage(Page::Main, m_nSectionFocus); // the sections are on the Settings page
            break;
        case Page::SubStyle:
            ShowPage(m_subStyleReturn == Page::None ? Page::Main : m_subStyleReturn, m_nSubStyleReturnFocus);
            break;
        case Page::Main:
            if (m_mainParent == Page::Bar) {
                ShowPage(Page::Bar, 0);
            } else {
                Close();
            }
            break;
        case Page::MadVR:
            if (m_nMadVrSub != MvRoot) {
                m_nMadVrSub = MvRoot; // a madVR submenu: back to the panel, on the row it came from
                m_bForceTransition = true; // same page type, new content: transition anyway
                ShowPage(Page::MadVR, m_nMadVrSubFocus);
                break;
            }
            [[fallthrough]];
        case Page::Audio:
        case Page::Subtitles:
        case Page::Options:
        case Page::Lav:
        case Page::Video:
        case Page::Advanced:
        case Page::Chapters:
        case Page::VideoInfo:
        case Page::AudioInfo:
        case Page::Scopes:
            if (m_parent == Page::Main) {
                ShowPage(Page::Main, m_nMainFocus);
            } else if (m_parent == Page::Bar) {
                ShowPage(Page::Bar, 0);
            } else {
                Close(); // a list opened straight from its own key closes again
            }
            break;
        default:
            Close();
            break;
    }
}

void CTvMenu::BuildRows()
{
    LARGE_INTEGER b0 = {};
    if (g_perfOn) {
        QueryPerformanceCounter(&b0);
    }
    BuildRowsContent();
    if (g_perfOn) {
        LARGE_INTEGER b1, f;
        QueryPerformanceCounter(&b1);
        QueryPerformanceFrequency(&f);
        g_perfBuildMs += (b1.QuadPart - b0.QuadPart) * 1000.0 / f.QuadPart;
    }
    // Layout lines: one column = the row index; two columns = the line within its column
    // (Up / Down still walk the rows in order: down the left column, then the right one).
    m_bTwoColumns = std::any_of(m_rows.begin(), m_rows.end(), [](const Row& r) { return r.col == 1; });
    int lines[2] = { 0, 0 };
    for (Row& r : m_rows) {
        const int c = m_bTwoColumns ? std::clamp(r.col, 0, 1) : 0;
        r.col = c;
        r.line = lines[c]++;
    }
}

int CTvMenu::LineCount() const
{
    int n = 0;
    for (const Row& r : m_rows) {
        n = std::max(n, r.line + 1);
    }
    return n;
}

void CTvMenu::BuildRowsContent()
{
    m_rows.clear();
    CMainFrame* pMF = m_pMainFrame;

    switch (m_page) {
        case Page::Main: {
            // Setup only: what's playing now (tracks, delays, info) lives on the bar's own pages
            Row video;
            video.kind = RowKind::OpenVideo; // renderer, madVR, LAV Video, shaders
            video.text = L"Video";
            video.icon = L"\xE8B2"; // film frame, like the bar's Video button
            video.value = RendererName(AfxGetAppSettings().iDSVideoRendererType);
            m_rows.push_back(video);

            Row audio;
            audio.kind = RowKind::OpenLav; // LAV Audio: bitstreaming, DRC, delay, output formats
            audio.data = 0;
            audio.text = L"Audio";
            audio.icon = L"\xE767";
            audio.value = LavSummary(false);
            m_rows.push_back(audio);

            // The TV options' sections right here (no separate TV options page), then Reset
            Row hTv;
            hTv.kind = RowKind::Heading;
            hTv.text = L"TV options";
            m_rows.push_back(hTv);
            for (size_t si = 0; si < _countof(kOptionSections); si++) {
                Row r;
                r.kind = RowKind::OpenSection;
                r.text = kOptionSections[si].heading;
                r.icon = kOptionSections[si].icon;
                r.data = static_cast<int>(si);
                m_rows.push_back(r);
            }
            Row reset;
            reset.icon = L"\xE72C";
            reset.kind = RowKind::ResetOptions;
            reset.text = m_bResetArmed ? L"Press OK again to reset all TV options" : L"Reset TV options to defaults";
            m_rows.push_back(reset);

            Row hMore;
            hMore.kind = RowKind::Heading;
            hMore.text = L"Other options";
            m_rows.push_back(hMore);
            Row advanced;
            advanced.kind = RowKind::OpenAdvanced;
            advanced.text = L"Advanced (mouse)";
            advanced.icon = L"\xE713";
            m_rows.push_back(advanced);

            Row desktop;
            desktop.kind = RowKind::DesktopMode;
            desktop.text = L"Switch to Desktop Mode";
            desktop.icon = L"\xE7F4";
            m_rows.push_back(desktop);

            Row exitRow;
            exitRow.kind = RowKind::ExitPlayer;
            exitRow.text = L"Exit";
            exitRow.icon = L"\xE7E8"; // power button
            m_rows.push_back(exitRow);
            break;
        }
        case Page::Quit: {
            // Back with nothing open (instead of leaving fullscreen): exit, fullscreen, restart
            Row exitRow;
            exitRow.kind = RowKind::ExitPlayer;
            exitRow.text = L"Exit";
            exitRow.icon = L"\xE7E8"; // power button
            m_rows.push_back(exitRow);
            const bool fs = pMF && pMF->IsFullScreenMode();
            Row fsRow;
            fsRow.kind = RowKind::ToggleFullscreen;
            fsRow.text = fs ? L"Exit fullscreen" : L"Enter fullscreen";
            fsRow.icon = fs ? L"\xE73F" : L"\xE740";
            m_rows.push_back(fsRow);
            Row restart;
            restart.kind = RowKind::RestartPlayer;
            restart.text = L"Restart player";
            restart.icon = L"\xE72C"; // refresh
            m_rows.push_back(restart);
            break;
        }
        case Page::Chapters: {
            // One row per chapter: its name and start time; the one playing now is marked (and the
            // list opens on it)
            const auto chapters = GetChapters();
            const REFERENCE_TIME pos = pMF->GetPos();
            int current = -1;
            for (int i = 0; i < static_cast<int>(chapters.size()); i++) {
                if (chapters[i].rt <= pos) {
                    current = i;
                }
            }
            for (int i = 0; i < static_cast<int>(chapters.size()); i++) {
                Row r;
                r.kind = RowKind::ChapterRow;
                r.data = i;
                r.text = chapters[i].name;
                if (r.text.IsEmpty()) {
                    r.text.Format(L"Chapter %d", i + 1);
                }
                r.value = FormatTime(chapters[i].rt);
                r.checked = (i == current);
                m_rows.push_back(r);
            }
            if (chapters.empty()) {
                Row none;
                none.text = L"This file has no chapters";
                m_rows.push_back(none);
            }
            break;
        }
        case Page::Advanced: {
            // The full dialogs, for the mouse; the overlay steps aside while they're open
            Row options;
            options.kind = RowKind::MpcOptions;
            options.text = L"MPC-HC options";
            m_rows.push_back(options);
            if (m_pMadVr) {
                Row mv;
                mv.kind = RowKind::MadVrAction;
                mv.data = MvActionSettingsWindow;
                mv.text = L"madVR settings window";
                m_rows.push_back(mv);
            }
            for (int v = 0; v < 2; v++) {
                Row lav;
                lav.kind = RowKind::LavAction;
                lav.data = v;
                lav.text = v ? L"LAV Video settings window" : L"LAV Audio settings window";
                m_rows.push_back(lav);
            }
            break;
        }
        case Page::Audio: {
            // Decoder and delay first: Up from the first track reaches them (Left/Right adjust the delay)
            Row lav;
            lav.kind = RowKind::OpenLav;
            lav.data = 0;
            lav.text = L"Audio decoder (LAV)";
            lav.value = LavSummary(false);
            m_rows.push_back(lav);

            Row delay;
            delay.kind = RowKind::AudioDelay;
            delay.text = L"Audio delay";
            delay.value = DelayText(pMF, true);
            m_rows.push_back(delay);

            std::vector<TvTrack> tracks;
            if (CTvTracks::GetAudio(pMF, tracks)) {
                for (const auto& t : tracks) {
                    Row r;
                    r.kind = RowKind::AudioTrack;
                    r.text = t.title;
                    r.detail = t.detail;
                    r.checked = t.isCurrent;
                    r.data = t.index;
                    m_rows.push_back(r);
                }
            } else {
                Row r;
                r.text = L"No audio tracks to choose from";
                m_rows.push_back(r);
            }
            break;
        }
        case Page::Subtitles: {
            std::vector<TvTrack> tracks;
            bool on = false;
            if (CTvTracks::GetSubtitles(pMF, tracks, on)) {
                Row style;
                style.kind = RowKind::OpenSubStyle;
                style.text = L"Subtitle style";
                m_rows.push_back(style);

                // Delay above "Off": Up from Off reaches it (Left/Right adjust)
                Row delay;
                delay.kind = RowKind::SubtitleDelay;
                delay.text = L"Subtitle delay";
                delay.value = DelayText(pMF);
                m_rows.push_back(delay);

                Row off;
                off.kind = RowKind::SubtitleOff;
                off.text = L"Off";
                off.checked = !on || std::none_of(tracks.begin(), tracks.end(), [](const TvTrack& t) { return t.isCurrent; });
                m_rows.push_back(off);
                for (const auto& t : tracks) {
                    Row r;
                    r.kind = RowKind::SubtitleTrack;
                    r.text = t.title;
                    r.detail = t.detail;
                    r.checked = t.isCurrent;
                    r.data = t.index;
                    m_rows.push_back(r);
                }
            } else {
                Row r;
                r.text = L"Subtitles can't be switched here";
                r.detail = L"Needs MPC-HC's own subtitle renderer (Output = madVR)";
                m_rows.push_back(r);
            }
            break;
        }
        case Page::MadVR: {
            // The display madVR is using right now (its bold device); settings only when found
            if (m_nMadVrSub == MvRoot || m_strMadVrDevice.IsEmpty()) {
                m_strMadVrDevice = MadVrActiveDevice(&m_strMadVrDeviceName);
                m_strMadVrDeviceActive = m_strMadVrDevice;
                // another display picked on the panel (several connected): its settings instead
                if (!m_strMadVrDeviceOverride.IsEmpty()) {
                    for (const auto& d : MadVrDeviceList()) {
                        if (d.first == m_strMadVrDeviceOverride) {
                            m_strMadVrDevice = d.first;
                            m_strMadVrDeviceName = d.second;
                        }
                    }
                }
            }
            auto addSettings = [&](int sub) {
                const auto& defs = MadVrDefs();
                for (int i = 0; i < static_cast<int>(defs.size()); i++) {
                    const int state = (defs[i].sub == sub && !defs[i].paired) ? MadVrShowState(i) : 0;
                    if (state == 0) {
                        continue; // e.g. tone mapping settings while HDR is passthrough
                    }
                    Row r;
                    r.faded = (state == 2); // only applies while an on / off above is on
                    r.col = defs[i].col;
                    if (defs[i].kind == MvKind::Heading) {
                        r.kind = RowKind::Heading;
                        r.text = defs[i].label;
                    } else {
                        r.kind = RowKind::MadVrSetting;
                        r.text = MadVrLabel(i);
                        r.data = i;
                        const bool pair = (i + 1 < static_cast<int>(defs.size()) && defs[i + 1].paired && defs[i].kind == MvKind::Bool);
                        // prefix = the on / off part, value = the value part (empty while off)
                        if (pair) {
                            r.data2 = i + 1;
                            r.prefix = MadVrValueText(i) == L"On" ? L"On" : L"Off";
                            r.value = MadVrValueText(i + 1); // shown faded while off
                        } else if (defs[i].kind == MvKind::Bool) {
                            r.prefix = MadVrValueText(i);
                            r.toggle = r.prefix == L"On" ? 1 : 0; // a plain on / off: drawn as a switch, OK flips it
                        } else {
                            r.value = MadVrValueText(i);
                        }
                    }
                    m_rows.push_back(r);
                }
            };
            if (m_nMadVrSub == MvModes) {
                // Add buttons (each shows what it would add), then madVR's list as tags
                struct AddDef { int action; LPCWSTR label; };
                const AddDef adds[] = {
                    { MvActionAdd2160, L"Add 2160p (4K) modes" }, { MvActionAdd1440, L"Add 1440p modes" },
                    { MvActionAdd1080, L"Add 1080p modes" }, { MvActionAddCurrentRes, L"Add current resolution" },
                    { MvActionAddCurrentMode, L"Add current mode only" },
                };
                for (const auto& a : adds) {
                    Row r;
                    r.kind = RowKind::MadVrAction;
                    r.text = a.label;
                    r.data = a.action;
                    for (const auto& m : MadVrModesFor(a.action)) {
                        if (!r.value.IsEmpty()) {
                            r.value += L", ";
                        }
                        r.value += MadVrModeName(m[0], m[1], m[2], m[3] != 0);
                    }
                    if (r.value.IsEmpty()) {
                        r.value = L"not offered by this display";
                        r.faded = true;
                    }
                    m_rows.push_back(r);
                }
                Row clear;
                clear.kind = RowKind::MadVrAction;
                clear.text = L"Clear list";
                clear.data = MvActionClearModes;
                m_rows.push_back(clear);
                m_madVrModes = MadVrModesList();
                Row head;
                head.kind = RowKind::Heading;
                head.text.Format(L"In madVR: %d mode%s%s", static_cast<int>(m_madVrModes.size()), m_madVrModes.size() == 1 ? L"" : L"s",
                                 m_madVrModes.empty() ? L" (no switching)" : L"");
                m_rows.push_back(head);
                const int perLine = 6;
                for (int k = 0; k < static_cast<int>(m_madVrModes.size()); k += perLine) {
                    Row t;
                    t.kind = RowKind::MadVrModeTags;
                    t.data = k;
                    t.data2 = std::min(perLine, static_cast<int>(m_madVrModes.size()) - k);
                    m_rows.push_back(t);
                }
                m_nModeTag = std::clamp(m_nModeTag, 0, std::max(0, static_cast<int>(m_madVrModes.size()) - 1));
                break;
            }
            if (m_nMadVrSub != MvRoot) {
                addSettings(m_nMadVrSub);
                break;
            }
            // The panel: Picture (how the video is processed), Display (the display in use = madVR's
            // bold device, and its own settings), Tools
            auto addSub = [&](int sub, LPCWSTR text) {
                Row r;
                r.kind = RowKind::MadVrOpenSub;
                r.text = text;
                r.data = sub;
                m_rows.push_back(r);
            };
            auto heading = [&](LPCWSTR text) {
                Row h;
                h.kind = RowKind::Heading;
                h.text = text;
                m_rows.push_back(h);
            };
            if (CComQIPtr<IMadVRSettings>(m_pMadVr)) {
                heading(L"Picture");
                if (!m_strMadVrDevice.IsEmpty()) {
                    addSub(MvHdr, L"HDR");
                    m_rows.back().value = MadVrHdrSummary();
                }
                addSub(MvUpRefine, L"Upscaling refinement");
                addSub(MvSmoothMotion, L"Smooth motion");
                addSub(MvTradeQuality, L"Trade quality for performance");
                const int tradeOn = MadVrTradeQualityCount(m_pMadVr);
                m_rows.back().value = tradeOn < 0 ? CStringW() : tradeOn == 0 ? CStringW(L"None") : [tradeOn] {
                    CStringW v;
                    v.Format(L"%d on", tradeOn);
                    return v;
                }();
            }
            heading(L"Display");
            // The display in use; with more than one display connected, Left / Right pick which
            // madVR device the settings below are for. With one it's just shown (not selectable).
            Row display;
            display.kind = RowKind::Info;
            display.text = L"Display in use";
            display.value = !m_strMadVrDevice.IsEmpty() ? m_strMadVrDeviceName
                            : !m_strMadVrProblem.IsEmpty() ? m_strMadVrProblem : CStringW(L"madVR settings not available");
            if (GetSystemMetrics(SM_CMONITORS) > 1 && MadVrDeviceList().size() > 1) {
                display.kind = RowKind::MadVrDevice;
                display.text = L"Display";
                CStringW name = display.value;
                if (!m_strMadVrDeviceOverride.IsEmpty() && m_strMadVrDevice != m_strMadVrDeviceActive) {
                    name += L" (not in use)";
                }
                display.value.Format(L"%s  %s  %s", kTriLeft, name.GetString(), kTriRight);
            } else {
                display.skip = true;
            }
            m_rows.push_back(display);
            if (!m_strMadVrDevice.IsEmpty()) {
                addSub(MvProperties, L"Properties");
                addSub(MvCalibration, L"Calibration");
                addSettings(MvRoot); // Frame matching
                addSub(MvModes, L"Display modes");
                const CStringW modes = MadVrModesInMadVr();
                m_rows.back().value = modes.IsEmpty() ? CStringW(L"none (no switching)") : modes;
            }
            heading(L"Tools");
            Row stats;
            stats.kind = RowKind::MadVrStats;
            stats.text = L"Statistics (Ctrl+J)";
            m_rows.push_back(stats);
            Row window;
            window.kind = RowKind::MadVrAction;
            window.text = L"madVR settings window (mouse)";
            window.data = MvActionSettingsWindow;
            m_rows.push_back(window);
            break;
        }
        case Page::Video: {
            // Quick video options: renderer, madVR, LAV Video, shaders
            const auto& s = AfxGetAppSettings();
            Row renderer;
            renderer.kind = RowKind::VideoRenderer;
            renderer.text = L"Video renderer";
            renderer.value.Format(L"%s  %s  %s", kTriLeft, RendererName(s.iDSVideoRendererType).GetString(), kTriRight);
            m_rows.push_back(renderer);
            // Another renderer than the one this file plays with: say so, and offer to reopen the file
            // (same place, same tracks) so it takes effect now
            const bool rendererPending = m_nLoadedRenderer >= 0 && s.iDSVideoRendererType != m_nLoadedRenderer;
            if (rendererPending) {
                Row reopen;
                reopen.kind = RowKind::ReopenToApply;
                reopen.text = L"Restart playback to apply";
                reopen.icon = L"\xE72C"; // refresh
                reopen.help = L"Reopens this file at the same place with the new renderer";
                m_rows.push_back(reopen);
                Row note;
                note.kind = RowKind::Info;
                note.text.Format(L"Now playing with %s", RendererName(m_nLoadedRenderer).GetString());
                m_rows.push_back(note);
            } else if (!IsMadVrRenderer()) {
                Row note;
                note.kind = RowKind::Info;
                note.text = L"Renderer changes apply from the next file";
                m_rows.push_back(note);
            } else {
                Row madvr;
                madvr.kind = RowKind::OpenMadVr;
                madvr.text = L"madVR settings";
                madvr.value = MadVrHdrSummary(); // e.g. "Tone map (pixel shaders)"
                m_rows.push_back(madvr);
            }
            Row lav;
            lav.kind = RowKind::OpenLav;
            lav.data = 1;
            lav.text = L"Video decoder (LAV)";
            lav.value = LavSummary(true);
            m_rows.push_back(lav);
            Row shaders;
            shaders.kind = RowKind::ShadersRow;
            shaders.text = L"Pixel shaders";
            shaders.value.Format(L"%s  %s  %s", kTriLeft, ShadersOn(pMF) ? L"On" : L"Off", kTriRight);
            m_rows.push_back(shaders);
            break;
        }
        case Page::Lav: {
            // LAV Audio / Video: two columns like LAV's own page; options this LAV doesn't have
            // are left out, and headings only appear above something
            const bool video = (m_nLavPage == 1);
            CComPtr<IBaseFilter> pBF = LavFilter(video);
            if (!pBF) {
                Row info;
                info.kind = RowKind::Info;
                info.text = video ? L"LAV Video isn't playing anything" : L"LAV Audio isn't playing anything";
                info.value = L"start a file to change these";
                m_rows.push_back(info);
            } else {
                const auto& defs = LavDefs();
                Row pendingHead;
                bool havePending = false;
                for (int i = 0; i < static_cast<int>(defs.size()); i++) {
                    const LavDef& d = defs[i];
                    if (d.page != m_nLavPage) {
                        continue;
                    }
                    if (d.kind == LvKind::Heading) {
                        pendingHead = Row();
                        pendingHead.kind = RowKind::Heading;
                        pendingHead.text = d.label;
                        pendingHead.col = d.col;
                        havePending = true;
                        continue;
                    }
                    int v = 0;
                    if (!d.get(pBF, v)) {
                        continue; // this LAV doesn't have it
                    }
                    if (havePending) {
                        m_rows.push_back(pendingHead);
                        havePending = false;
                    }
                    Row r;
                    r.kind = RowKind::LavSetting;
                    r.data = i;
                    r.col = d.col;
                    r.text = d.label;
                    CStringW text;
                    if (d.kind == LvKind::Bool) {
                        text = v ? L"On" : L"Off";
                    } else if (d.kind == LvKind::Choice) {
                        text = (v >= 0 && v < static_cast<int>(d.names.size())) ? CStringW(d.names[v]) : CStringW(L"?");
                        if (video && wcscmp(d.label, L"Hardware decoder") == 0 && v > 0) {
                            const DWORD support = CComQIPtr<ILAVVideoSettings>(pBF)->CheckHWAccelSupport(static_cast<LAVHWAccel>(v));
                            text += support == 0 ? L" (not available)" : support == 2 ? L" (in use)" : L"";
                        }
                    } else {
                        text.Format(L"%d%s", v, d.unit);
                    }
                    int dep = 0;
                    r.faded = d.fadeUnless >= 0 && defs[d.fadeUnless].get(pBF, dep) && dep == 0;
                    if (d.kind == LvKind::Bool) {
                        r.toggle = v ? 1 : 0; // drawn as a switch
                    }
                    r.value.Format(L"%s  %s  %s", kTriLeft, text.GetString(), kTriRight); // arrows hidden while faded
                    m_rows.push_back(r);
                }
                Row note;
                note.kind = RowKind::Info;
                note.col = 1;
                note.text = L"Some changes apply from the next file";
                m_rows.push_back(note);
            }
            Row window;
            window.kind = RowKind::LavAction;
            window.col = pBF ? 1 : 0;
            window.data = m_nLavPage;
            window.text = video ? L"LAV Video settings window (mouse)" : L"LAV Audio settings window (mouse)";
            m_rows.push_back(window);
            break;
        }
        case Page::Options: {
            // TV options: one row per section (submenu), then Reset
            for (size_t si = 0; si < _countof(kOptionSections); si++) {
                Row r;
                r.kind = RowKind::OpenSection;
                r.text = kOptionSections[si].heading;
                r.data = static_cast<int>(si);
                m_rows.push_back(r);
            }
            Row hReset;
            hReset.kind = RowKind::Heading;
            m_rows.push_back(hReset); // empty heading = a little space before Reset
            Row reset;
            reset.kind = RowKind::ResetOptions;
            reset.text = L"Reset TV options to defaults";
            m_rows.push_back(reset);
            break;
        }
        case Page::OptionsSection:
        case Page::SubStyle: {
            // An options submenu (no headings), or the subtitle style page (sections with headings).
            // TvOpt::Count = the "Subtitle style" row.
            const bool subStyle = (m_page == Page::SubStyle);
            const OptionSection* sections = subStyle ? kSubStyleSections : &kOptionSections[std::clamp(m_nSection, 0, static_cast<int>(_countof(kOptionSections)) - 1)];
            const size_t sectionCount = subStyle ? _countof(kSubStyleSections) : 1;
            for (size_t si = 0; si < sectionCount; si++) {
                const OptionSection& s = sections[si];
                if (subStyle) {
                    Row h;
                    h.kind = RowKind::Heading;
                    h.text = s.heading;
                    m_rows.push_back(h);
                }
                for (size_t oi = 0; oi < s.opts.size(); oi++) {
                    const TvOpt opt = s.opts[oi];
                    for (const auto& g : s.groups) { // sub-headings inside a long section
                        if (g.first == oi) {
                            Row h;
                            h.kind = RowKind::Heading;
                            h.text = g.second;
                            m_rows.push_back(h);
                        }
                    }
                    if (opt == TvOpt::Count) {
                        Row style;
                        style.kind = RowKind::OpenSubStyle;
                        style.text = L"Subtitle style";
                        style.help = L"Font, size, colour, border and position of the subtitles";
                        m_rows.push_back(style);
                        continue;
                    }
                    Row r;
                    r.kind = RowKind::Option;
                    r.text = CTvMode::Label(opt);
                    r.value.Format(L"%s  %s  %s", kTriLeft, CTvMode::ValueText(opt).GetString(), kTriRight);
                    r.data = static_cast<int>(opt);
                    r.help = OptionHelp(opt);
                    if (CTvMode::IsOnOff(opt)) {
                        r.toggle = CTvMode::Get(opt) != 0 ? 1 : 0; // drawn as a switch
                    }
                    if (opt == TvOpt::SeekMode && CTvMode::Get(TvOpt::ShowOnSeek) == 2) {
                        // With nothing shown while seeking it always jumps right away (InstantSeek):
                        // nothing to choose, so a fixed, dimmed line without arrows.
                        r.kind = RowKind::Info;
                        r.value = L"Right away (nothing is shown)";
                    }
                    // Only matters while another option is on: faded, no arrows, can't be changed
                    if ((opt == TvOpt::SubShadowOpacity && CTvMode::Get(TvOpt::SubShadowDepth) == 0) ||
                        (opt == TvOpt::SubOutlineOpacity && CTvMode::Get(TvOpt::SubOutlineWidth) == 0 &&
                         CTvMode::Get(TvOpt::SubBorderStyle) == 0) ||
                        (opt == TvOpt::SmoothPlayhead && CTvMode::AnimScale() <= 0.0f) || // Animations Off: no glide
                        (opt == TvOpt::SmoothScrub && InstantSeek()) ||                   // jumps at once: no preview
                        (opt == TvOpt::PauseScreen && !CTvMode::IsOn(TvOpt::PauseScreenOn)) || // pause screen off
                        (opt == TvOpt::WrapWhileHeld && !CTvMode::IsOn(TvOpt::WrapAround))) {   // no wrapping at all
                        r.faded = true;
                    }
                    m_rows.push_back(r);
                    if (opt == TvOpt::AccentColour || opt == TvOpt::BackgroundColour) {
                        Row picker;
                        picker.kind = RowKind::OpenPicker;
                        picker.data = opt == TvOpt::BackgroundColour ? 1 : 0; // what the picker edits
                        picker.text = opt == TvOpt::BackgroundColour ? L"Custom background colour" : L"Custom colour";
                        m_rows.push_back(picker);
                    }
                }
                if (s.barButtons) {
                    // The bar's buttons in their order: Left/Right show / hide, OK picks one up
                    // to move it with Up/Down (within its side of the bar), OK again puts it down.
                    Row h;
                    h.kind = RowKind::Heading;
                    h.text = L"Buttons";
                    m_rows.push_back(h);
                    const auto layout = BarLayout();
                    for (size_t bi = 0; bi < layout.size(); bi++) {
                        const BarItem& it = layout[bi];
                        Row r;
                        r.kind = RowKind::BarItemRow;
                        r.data = static_cast<int>(bi);
                        r.text = BarButtonName(it.id);
                        r.data2 = it.id == BarButton::Separator ? 1 : 0; // drawn as a line (RenderPanel)
                        if (m_nMovingItem == static_cast<int>(bi)) {
                            r.text = CStringW(kArrowUpDown) + L"  " + r.text;
                            r.value = L"Moving: Up / Down, OK to drop";
                        } else if (it.id == BarButton::Separator) {
                            r.value.Format(L"%s  remove  %s", kTriLeft, kTriRight);
                        } else if (!CanHide(it.id)) {
                            r.value = L"Always shown";
                        } else {
                            r.value.Format(L"%s  %s  %s", kTriLeft, it.on ? L"Shown" : L"Hidden", kTriRight);
                        }
                        r.checked = false;
                        m_rows.push_back(r);
                    }
                    Row add;
                    add.kind = RowKind::AddSeparator;
                    add.text = L"Add a separator";
                    m_rows.push_back(add);
                }
                if (s.keys) {
                    // Two columns: the first three groups (TV menu, Playback, Jumps) on the left, the rest
                    // on the right; Up / Down stay in a column, Left / Right change it
                    int group = -1;
                    for (size_t ki = 0; ki < _countof(kKeyActions); ki++) {
                        if (kKeyActions[ki].id == 0) { // group heading
                            group++;
                            Row h;
                            h.kind = RowKind::Heading;
                            h.text = kKeyActions[ki].name;
                            h.col = group >= 3 ? 1 : 0;
                            m_rows.push_back(h);
                            continue;
                        }
                        Row r;
                        r.col = group >= 3 ? 1 : 0;
                        r.kind = RowKind::KeyRow;
                        r.data = static_cast<int>(ki);
                        r.text = kKeyActions[ki].name;
                        if (m_nListenKey == static_cast<int>(ki)) {
                            r.value = !m_strKeyMessage.IsEmpty() ? m_strKeyMessage
                                      : m_bListenHold ? CStringW(L"Press the key to hold...") : CStringW(L"Press a key...");
                        } else {
                            r.value = KeyText(kKeyActions[ki].id);
                        }
                        r.help = kKeyActions[ki].id < 0
                                 ? L"OK: set a key or clear it"
                                 : L"OK: set a key, set a hold (0.6 s) or clear. Arrows and holds work only while the bar and menus are hidden";
                        m_rows.push_back(r);
                    }
                }
            }
            if (subStyle) {
                Row hReset;
                hReset.kind = RowKind::Heading;
                m_rows.push_back(hReset); // a little space before Reset
                Row reset;
                reset.kind = RowKind::ResetSubStyle;
                reset.text = m_bResetArmed ? L"Press OK again to reset the subtitle style" : L"Reset subtitle style to defaults";
                m_rows.push_back(reset);
            }
            break;
        }
        default:
            break;
    }
}

void CTvMenu::Move(int delta, bool mayWrap)
{
    if (IsInfoPage(m_page)) {
        // Info panels have no selection: Up/Down (and the wheel) turn pages of the detail columns.
        const int page = std::clamp(m_nInfoScroll + delta, 0, std::max(0, m_nInfoPages - 1));
        if (page != m_nInfoScroll) {
            m_nInfoScroll = page;
            ShowScrollbar();
            Render();
        }
        return;
    }
    if (m_rows.empty()) {
        return;
    }
    if (m_bResetArmed) {
        m_bResetArmed = false; // moved away from a Reset row that was asking: nothing is reset
        BuildRows();
    }
    // Clamp at the ends instead of wrapping, like TV apps do. Section headings are skipped.
    const int last = static_cast<int>(m_rows.size()) - 1;
    const int step = delta > 0 ? 1 : -1;
    int focus = m_nFocus;
    for (int n = 0; n < std::abs(delta); n++) {
        int next = focus + step;
        while (next >= 0 && next <= last && IsSkipped(m_rows[next])) { // headings, faded settings
            next += step;
        }
        if (next < 0 || next > last) {
            break;
        }
        if (m_bTwoColumns && (m_page == Page::MadVR || m_page == Page::OptionsSection) && m_rows[next].col != m_rows[focus].col) {
            break; // two columns: Up / Down stay in the column, Left / Right change it
        }
        focus = next;
    }
    const int oldScroll = m_nScroll;
    if (focus == m_nFocus) {
        // At the top: still scroll up to show a heading above the first row
        if (step < 0 && m_nScroll > 0) {
            m_nScroll = 0;
            ShowScrollbar();
            Render();
            return;
        }
        // "Wrap around at the ends": a fresh press at the last row goes to the first, and back.
        // Two columns read as one list: the end of the left column goes on at the top of the right
        // one, the end of the right one back to the top of the left (and the same upwards).
        if (!mayWrap || std::abs(delta) != 1 || !CTvMode::IsOn(TvOpt::WrapAround)) {
            return;
        }
        int target = -1;
        if (m_bTwoColumns && (m_page == Page::MadVR || m_page == Page::OptionsSection)) {
            std::vector<int> order; // selectable rows, column by column, top to bottom
            for (int i = 0; i <= last; i++) {
                if (!IsSkipped(m_rows[i])) {
                    order.push_back(i);
                }
            }
            std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
                return m_rows[a].col != m_rows[b].col ? m_rows[a].col < m_rows[b].col : m_rows[a].line < m_rows[b].line;
            });
            const int n = static_cast<int>(order.size());
            const int p = static_cast<int>(std::find(order.begin(), order.end(), m_nFocus) - order.begin());
            if (n > 0 && p < n) {
                target = order[((p + step) % n + n) % n];
            }
        } else {
            for (int k = 0; k <= last; k++) {
                const int i = step > 0 ? k : last - k;
                if (!IsSkipped(m_rows[i])) {
                    target = i;
                    break;
                }
            }
        }
        if (target < 0 || target == m_nFocus) {
            return;
        }
        focus = target;
    }
    m_nFocus = focus;
    // Keep the focused line (and a heading directly above it) in view; scrolling is by line,
    // which is the row index for one column and the line within its column for two
    const int line = m_rows[m_nFocus].line;
    const bool headingAbove = m_nFocus > 0 && m_rows[m_nFocus - 1].kind == RowKind::Heading && m_rows[m_nFocus - 1].col == m_rows[m_nFocus].col;
    const int topWanted = headingAbove ? m_rows[m_nFocus - 1].line : line;
    if (topWanted < m_nScroll) {
        m_nScroll = topWanted;
    } else if (line >= m_nScroll + MaxRows()) {
        m_nScroll = line - MaxRows() + 1;
    }
    if (m_nScroll != oldScroll) {
        ShowScrollbar(); // the list scrolled
    }
    Render();
}

bool CTvMenu::AdjustRow(int dir)
{
    if (m_rows.empty()) {
        return false;
    }
    const RowKind kind = m_rows[m_nFocus].kind;
    if (m_rows[m_nFocus].faded && IsSettingRow(kind)) {
        return true; // faded: only applies while another setting is on, so it can't be changed
    }
    if (m_rows[m_nFocus].toggle >= 0) {
        m_nToggleAnimKey = ToggleKey(m_rows[m_nFocus]); // the switch slides to its new side
        m_nToggleAnimStart = GetTickCount64();
    }
    if (kind == RowKind::SubtitleDelay) {
        CTvTracks::ChangeSubtitleDelay(m_pMainFrame, dir * CTvMode::Get(TvOpt::SubDelayStepMs));
        m_rows[m_nFocus].value = DelayText(m_pMainFrame);
        Render();
    } else if (kind == RowKind::AudioDelay) {
        CTvTracks::ChangeAudioDelay(m_pMainFrame, dir * CTvMode::Get(TvOpt::AudioDelayStepMs));
        m_rows[m_nFocus].value = DelayText(m_pMainFrame, true);
        Render();
    } else if (kind == RowKind::Option) {
        CTvMode::Step(static_cast<TvOpt>(m_rows[m_nFocus].data), dir);
        if (m_rows[m_nFocus].data == static_cast<int>(TvOpt::OverlayMode)) {
            SwitchOverlay(); // takes effect right away
        }
        BuildRows();
        Render();
    } else if (kind == RowKind::OpenAudio || kind == RowKind::OpenSubtitles) {
        CycleTrack(kind, dir);
    } else if (kind == RowKind::LavSetting) {
        LavStep(m_rows[m_nFocus].data, dir);
    } else if (kind == RowKind::VideoRenderer) {
        // MPC-HC's renderer setting (Options > Playback > Output); used from the next file
        auto& s = AfxGetAppSettings();
        const int n = static_cast<int>(_countof(kRenderers));
        int i = static_cast<int>(std::find(kRenderers, kRenderers + n, s.iDSVideoRendererType) - kRenderers);
        i = (i >= n) ? 0 : ((i + dir) % n + n) % n;
        s.iDSVideoRendererType = kRenderers[i];
        s.SaveSettings();
        BuildRows();
        Render();
    } else if (kind == RowKind::ShadersRow) {
        ToggleShaders(m_pMainFrame);
        BuildRows();
        Render();
    } else if (kind == RowKind::ArrowRow) {
        // the next / previous action in the list
        const int slot = m_rows[m_nFocus].data;
        const auto choices = ArrowChoices(slot % 2 == 1);
        const int n = static_cast<int>(choices.size());
        auto it = std::find(choices.begin(), choices.end(), ArrowAction(slot));
        int i = it == choices.end() ? 0 : static_cast<int>(it - choices.begin());
        i = ((i + dir) % n + n) % n;
        SetArrowAction(slot, choices[i]);
        BuildRows();
        Render();
    } else if (kind == RowKind::MadVrDevice) {
        // the next / previous madVR device; back at the one in use = no override
        const auto list = MadVrDeviceList();
        if (list.size() > 1) {
            int i = 0;
            for (int k = 0; k < static_cast<int>(list.size()); k++) {
                if (list[k].first == m_strMadVrDevice) {
                    i = k;
                }
            }
            const int n = static_cast<int>(list.size());
            i = ((i + dir) % n + n) % n;
            m_strMadVrDeviceOverride = (list[i].first == m_strMadVrDeviceActive) ? CStringW() : list[i].first;
            const int focus = m_nFocus;
            BuildRows();
            m_nFocus = std::clamp(focus, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
            m_rowFade.valid = false;
            Render();
        }
    } else if (kind == RowKind::MadVrSetting) {
        const Row r = m_rows[m_nFocus];
        if (r.data2 >= 0) {
            // on/off with its number: Left / Right change the number, only while it's on
            if (r.prefix == L"On") {
                StepMadVrSetting(r.data2, dir);
            }
        } else {
            StepMadVrSetting(r.data, dir);
        }
        RebuildKeepingMadVrFocus(r.data); // shows what madVR now reports; rows may appear / go
    } else if (kind == RowKind::BarItemRow) {
        auto layout = BarLayout();
        const int i = m_rows[m_nFocus].data;
        if (m_nMovingItem >= 0 || i < 0 || i >= static_cast<int>(layout.size()) || !CanHide(layout[i].id)) {
            return true; // nothing to switch (or a button is being moved)
        }
        if (layout[i].id == BarButton::Separator) {
            // Left / Right on a separator removes it
            layout.erase(layout.begin() + i);
            SaveBarLayout(layout);
            BuildRows();
            m_nFocus = std::clamp(m_nFocus, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
            m_rowFade.valid = false;
            Render();
            return true;
        }
        layout[i].on = !layout[i].on;
        if (layout[i].id == BarButton::Stats) {
            CTvMode::SetText(_T("MadVrButtonSet"), _T("1")); // chosen by hand: no longer follows the renderer
        }
        SaveBarLayout(layout);
        BuildRows();
        Render();
    } else {
        return false;
    }
    return true;
}

// Bar buttons submenu: moves the picked-up button one place up / down within its side.
void CTvMenu::MoveBarItem(int dir)
{
    auto layout = BarLayout();
    const int i = m_nMovingItem;
    const int j = i + dir;
    if (i < 0 || j < 0 || i >= static_cast<int>(layout.size()) || j >= static_cast<int>(layout.size()) ||
        IsLeftButton(layout[i].id) != IsLeftButton(layout[j].id)) {
        return; // end of its side of the bar
    }
    std::swap(layout[i], layout[j]);
    SaveBarLayout(layout);
    m_nMovingItem = j;
    m_nFocus += dir; // rows follow the layout order
    BuildRows();
    const int oldScroll = m_nScroll;
    if (m_nFocus < m_nScroll) {
        m_nScroll = m_nFocus;
    } else if (m_nFocus >= m_nScroll + MaxRows()) {
        m_nScroll = m_nFocus - MaxRows() + 1;
    }
    if (m_nScroll != oldScroll) {
        ShowScrollbar();
    }
    m_rowFade.valid = false; // the highlight moves with the button, no crossfade
    Render();
}

void CTvMenu::Activate()
{
    if (m_rows.empty()) {
        return;
    }
    const Row row = m_rows[m_nFocus]; // copy: the handlers below rebuild m_rows
    if (row.faded && IsSettingRow(row.kind)) {
        return; // faded: only applies while another setting is on, so it can't be changed
    }
    if (row.toggle >= 0) {
        m_nToggleAnimKey = ToggleKey(row); // the switch slides to its new side
        m_nToggleAnimStart = GetTickCount64();
    }
    switch (row.kind) {
        case RowKind::OpenAudio:
            ApplyPending(); // the full list then shows the track picked with Left/Right as current
            OpenChild(Page::Audio, -1);
            break;
        case RowKind::OpenSubtitles:
            ApplyPending();
            OpenChild(Page::Subtitles, -1);
            break;
        case RowKind::SubtitleDelay:
        case RowKind::AudioDelay: {
            // Left/Right change it; OK puts it back to 0
            const bool audio = (row.kind == RowKind::AudioDelay);
            int ms = 0;
            if ((audio ? CTvTracks::GetAudioDelay(m_pMainFrame, ms) : CTvTracks::GetSubtitleDelay(m_pMainFrame, ms)) && ms != 0) {
                audio ? CTvTracks::ChangeAudioDelay(m_pMainFrame, -ms) : CTvTracks::ChangeSubtitleDelay(m_pMainFrame, -ms);
                m_rows[m_nFocus].value = DelayText(m_pMainFrame, audio);
                Render();
            }
            break;
        }
        case RowKind::OpenOptions:
            OpenChild(Page::Options, 0);
            break;
        case RowKind::OpenLav:
            m_nLavPage = row.data;
            OpenChild(Page::Lav, 1); // row 0 is a heading
            break;
        case RowKind::OpenMadVr:
            m_nMadVrSub = MvRoot;
            OpenChild(Page::MadVR, 1); // row 0 = which display
            break;
        case RowKind::VideoRenderer:
            AdjustRow(+1);
            break;
        case RowKind::ShadersRow:
        case RowKind::MadVrDevice:
        case RowKind::ArrowRow:
            AdjustRow(+1);
            break;
        case RowKind::LavSetting:
            LavStep(row.data, +1); // OK: on / off, or the next choice
            break;
        case RowKind::LavAction:
            LavWindow(row.data == 1);
            break;
        case RowKind::MadVrSetting:
            if (row.toggle >= 0) {
                StepMadVrSetting(row.data, +1); // a plain on / off: OK flips it (the switch slides)
                RebuildKeepingMadVrFocus(row.data);
            } else {
                StartMadVrEdit(); // OK starts editing; nothing changes until Up / Down
            }
            break;
        case RowKind::MadVrStats:
            ToggleMadVrStats(); // the menu stays open
            break;
        case RowKind::MadVrOpenSub:
            m_nMadVrSubFocus = m_nFocus;
            m_nMadVrSub = row.data;
            m_bForceTransition = true; // same page type, new content: transition anyway
            ShowPage(Page::MadVR, 0);
            break;
        case RowKind::MadVrAction:
            MadVrAction(row.data);
            break;
        case RowKind::KeyRow:
            // OK shows the row's choices: Set / Set as hold / Clear (Left / Right pick, OK does it;
            // see HandleKeyEditKey). No Delete key needed on a remote.
            m_nKeyEditRow = m_nFocus;
            m_nKeyEditPart = 0;
            m_strKeyMessage.Empty();
            m_rowFade.valid = false;
            BuildRows();
            Render();
            break;
        case RowKind::BarItemRow:
            // OK picks the button up (then Up / Down move it), OK again puts it down
            m_nMovingItem = (m_nMovingItem >= 0) ? -1 : row.data;
            BuildRows();
            Render();
            break;
        case RowKind::OpenSection:
            m_nSectionFocus = m_nFocus;
            m_nSection = row.data;
            ShowPage(Page::OptionsSection, 0);
            break;
        case RowKind::OpenSubStyle:
            m_subStyleReturn = m_page;
            m_nSubStyleReturnFocus = m_nFocus;
            ShowPage(Page::SubStyle, 0);
            break;
        case RowKind::OpenPicker:
            m_nOptionsFocus = m_nFocus;
            // Opening the picker makes the custom colour the accent / background (from the current values)
            m_nPickerTarget = row.data == 1 ? 1 : 0;
            if (m_nPickerTarget == 1) {
                CTvMode::SetCustomBackground(CTvMode::BgHue(), CTvMode::BgSat(), CTvMode::BgLight());
            } else {
                CTvMode::SetCustom(CTvMode::CustomHue(), CTvMode::CustomSat(), CTvMode::CustomLight());
            }
            ShowPage(Page::ColourPicker, 0);
            break;
        case RowKind::Option:
            CTvMode::Step(static_cast<TvOpt>(row.data), +1); // OK also steps forward
            if (row.data == static_cast<int>(TvOpt::OverlayMode)) {
                SwitchOverlay();
            }
            BuildRows();
            Render();
            break;
        case RowKind::ResetSubStyle:
        case RowKind::ResetOptions:
            // Two presses: the first asks (the row says so), the second resets; moving away cancels
            if (!m_bResetArmed) {
                m_bResetArmed = true;
            } else {
                m_bResetArmed = false;
                if (row.kind == RowKind::ResetOptions) {
                    CTvMode::ResetOptions();
                } else {
                    CTvMode::ResetSubtitleStyle();
                }
            }
            BuildRows();
            Render();
            break;
        case RowKind::OpenVideo:
            OpenChild(Page::Video, 0);
            break;
        case RowKind::AddSeparator: {
            // A new separator at the end of the bar's right side, picked up right away so Up / Down
            // place it and OK drops it
            auto layout = BarLayout();
            layout.push_back({ BarButton::Separator, true });
            SaveBarLayout(layout);
            const int added = static_cast<int>(layout.size()) - 1;
            m_nMovingItem = added;
            BuildRows();
            for (int i = 0; i < static_cast<int>(m_rows.size()); i++) {
                if (m_rows[i].kind == RowKind::BarItemRow && m_rows[i].data == added) {
                    m_nFocus = i;
                }
            }
            ScrollFocusIntoView();
            m_rowFade.valid = false;
            Render();
            break;
        }
        case RowKind::OpenAdvanced:
            OpenChild(Page::Advanced, 0);
            break;
        case RowKind::MpcOptions:
            // The overlay steps aside; MPC-HC's own dialog is used with the mouse.
            ForceClose();
            m_pMainFrame->PostMessage(WM_COMMAND, ID_VIEW_OPTIONS);
            break;
        case RowKind::DesktopMode:
            CTvMode::Leave(m_pMainFrame); // closes the overlay and restores the desktop layout
            break;
        case RowKind::ExitPlayer: {
            CMainFrame* pMF = m_pMainFrame;
            ForceClose();
            if (pMF) {
                pMF->PostMessage(WM_COMMAND, ID_FILE_EXIT); // as File > Exit
            }
            break;
        }
        case RowKind::ToggleFullscreen: {
            // The menu stays open: switch now (as View > Full Screen), then the row says the new state
            if (m_pMainFrame) {
                m_pMainFrame->SendMessage(WM_COMMAND, ID_VIEW_FULLSCREEN);
            }
            const int keep = m_nFocus;
            BuildRows();
            m_nFocus = std::clamp(keep, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
            m_rowFade.valid = false; // same row, no crossfade
            m_lastOutput = {};       // the picture changed size: lay the panel out again
            Render();
            break;
        }
        case RowKind::RestartPlayer:
            RestartPlayer();
            break;
        case RowKind::ReopenToApply: {
            // The new renderer takes effect when the file is opened again: reopen it at the same place,
            // with the same audio / subtitle tracks and A-B repeat (as MPC-HC's own reload after a long pause)
            CMainFrame* pMF = m_pMainFrame;
            if (!pMF || pMF->GetLoadState() != MLS::LOADED) {
                break;
            }
            pMF->m_reloadFilename = pMF->lastOpenFile;
            pMF->m_rtReloadPos = pMF->m_wndSeekBar.HasDuration() ? pMF->m_wndSeekBar.GetPos() : 0;
            pMF->reloadABRepeat = pMF->abRepeat;
            pMF->m_iReloadAudioIdx = pMF->GetCurrentAudioTrackIdx();
            pMF->m_iReloadSubIdx = pMF->GetCurrentSubtitleTrackIdx();
            ForceClose();
            pMF->OnFileReopen();
            break;
        }
        case RowKind::Close:
            Close();
            break;
        case RowKind::AudioTrack:
            Close();
            CTvTracks::SelectAudio(m_pMainFrame, row.data);
            break;
        case RowKind::SubtitleOff:
            Close();
            CTvTracks::SelectSubtitle(m_pMainFrame, -1);
            break;
        case RowKind::SubtitleTrack:
            Close();
            CTvTracks::SelectSubtitle(m_pMainFrame, row.data);
            break;
        case RowKind::ChapterRow: {
            const auto chapters = GetChapters();
            if (row.data >= 0 && row.data < static_cast<int>(chapters.size())) {
                Close();
                m_pMainFrame->SeekTo(chapters[row.data].rt);
            }
            break;
        }
        case RowKind::Info:
            Back();
            break;
    }
}

// ---------------------------------------------------------------------------
// Control bar actions

bool CTvMenu::IsPlaying()
{
    return m_pMainFrame && m_pMainFrame->GetMediaState() == State_Running;
}

void CTvMenu::PressBarButton(BarButton button)
{
    CMainFrame* pMF = m_pMainFrame;
    switch (button) {
        case BarButton::Prev:
            pMF->PostMessage(WM_COMMAND, ID_NAVIGATE_SKIPBACK);  // previous chapter / file
            break;
        case BarButton::Next:
            pMF->PostMessage(WM_COMMAND, ID_NAVIGATE_SKIPFORWARD); // next chapter / file
            break;
        case BarButton::Back10:
            pMF->SeekTo(std::max<REFERENCE_TIME>(0, pMF->GetPos() - CTvMode::Get(TvOpt::SkipBackSec) * kSecond));
            break;
        case BarButton::Fwd30: {
            const REFERENCE_TIME dur = pMF->GetDur();
            REFERENCE_TIME target = pMF->GetPos() + CTvMode::Get(TvOpt::SkipFwdSec) * kSecond;
            if (dur > 0) {
                target = std::min(target, dur);
            }
            pMF->SeekTo(target);
            break;
        }
        case BarButton::PlayPause:
            pMF->PostMessage(WM_COMMAND, ID_PLAY_PLAYPAUSE);
            break;
        case BarButton::Volume:
            pMF->SendMessage(WM_COMMAND, ID_VOLUME_MUTE); // OK on the volume control: mute / unmute
            break;
        case BarButton::Shaders:
            ToggleShaders(pMF);
            break;
        case BarButton::Video:
            OpenChild(Page::Video, 0);
            return;
        case BarButton::Stats:
            m_nMadVrSub = MvRoot;
            OpenChild(Page::MadVR, 1); // madVR panel (row 0 = which display)
            return;
        case BarButton::VideoInfo:
            OpenChild(Page::VideoInfo, 0);
            return;
        case BarButton::AudioInfo:
            OpenChild(Page::AudioInfo, 0);
            return;
        case BarButton::Subtitles:
            OpenChild(Page::Subtitles, -1);
            return;
        case BarButton::Audio:
            OpenChild(Page::Audio, -1);
            return;
        case BarButton::Chapters:
            OpenChild(Page::Chapters, -1); // opens on the current chapter
            return;
        case BarButton::Settings:
            OpenChild(Page::Main, 0);
            return;
        case BarButton::Fullscreen:
            pMF->PostMessage(WM_COMMAND, ID_VIEW_FULLSCREEN); // the bar stays; its icon follows the state
            break;
        case BarButton::Power:
            OpenChild(Page::Quit, 0); // the power menu (exit, fullscreen, restart); Back returns to the bar
            return;
        default:
            return;
    }
    m_nLastBarRender = 0; // redraw on the next tick (play state, position)
    Render();
}

bool CTvMenu::IsMadVrRenderer()
{
    return AfxGetAppSettings().iDSVideoRendererType == VIDRNDT_DS_MADVR;
}

// MPC-HC's own two shader switches (Play > Shaders): any on -> both off; all off -> on for the
// kinds the current shader preset has. MPC-HC shows its usual message for each.
void CTvMenu::ToggleShaders(CMainFrame* pMF)
{
    const auto& preset = AfxGetAppSettings().m_Shaders.GetCurrentPreset();
    const bool hasPre = !preset.GetPreResize().empty();
    const bool hasPost = !preset.GetPostResize().empty();
    if (ShadersOn(pMF)) {
        if (pMF->m_bToggleShader) {
            pMF->SendMessage(WM_COMMAND, ID_PRESIZE_SHADERS_TOGGLE);
        }
        if (pMF->m_bToggleShaderScreenSpace) {
            pMF->SendMessage(WM_COMMAND, ID_POSTSIZE_SHADERS_TOGGLE);
        }
    } else if (!hasPre && !hasPost) {
        pMF->m_OSD.DisplayMessage(OSD_TOPLEFT, L"No shaders in the current shader preset (Play > Shaders)", 3000);
    } else {
        if (hasPre && !pMF->m_bToggleShader) {
            pMF->SendMessage(WM_COMMAND, ID_PRESIZE_SHADERS_TOGGLE);
        }
        if (hasPost && !pMF->m_bToggleShaderScreenSpace) {
            pMF->SendMessage(WM_COMMAND, ID_POSTSIZE_SHADERS_TOGGLE);
        }
    }
}

void CTvMenu::Scrub(int dir, bool isRepeat)
{
    CMainFrame* pMF = m_pMainFrame;
    const REFERENCE_TIME dur = pMF->GetDur();
    if (dur <= 0) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG sinceLast = now - m_nLastScrub;
    // A new press, or a held key we haven't seen start (e.g. it opened the bar), starts the hold clock.
    const bool newHold = !isRepeat || now - m_nLastScrub > 1000;
    if (newHold) {
        m_nScrubHoldStart = now;
    }
    if (isRepeat && !InstantSeek() && CTvMode::IsOn(TvOpt::SmoothScrub)) {
        // Held key with the preview: it glides (StepGlide moves it a little every frame, speeding
        // up smoothly) instead of jumping a few times a second. The repeats only keep it going.
        if (m_rtPreview < 0) {
            m_rtPreview = pMF->GetPos();
        }
        if (m_nGlideDir != dir) {
            m_nGlideDir = dir;
            m_nGlideTick = now;
        }
        m_nGlideKey = now;
        m_nLastScrub = now;
        return;
    }
    m_nGlideDir = 0;
    if (!newHold && now - m_nLastScrub < kScrubThrottleMs) {
        return; // the remote's key repeat is much faster than we want to step
    }
    m_nLastScrub = now;

    // Tap = the seek step (default 10 s). Holding speeds up, if enabled: x3 after 0.5 s,
    // x6 after 2 s, x18 after 4 s (10 s -> 30 s -> 1 min -> 3 min).
    const int tapSec = CTvMode::Get(TvOpt::SeekStepSec);
    const ULONGLONG held = isRepeat ? now - m_nScrubHoldStart : 0;
    // "Seeking speed-up when holding": Normal = the original curve; faster levels reach the
    // big steps sooner (times divided), slower levels later. Off = always the tap step.
    const float speed = CTvMode::HoldSpeed();
    const float t = static_cast<float>(held) * speed;
    const int factor = speed <= 0.0f ? 1 : t < 500 ? 1 : t < 2000 ? 3 : t < 4000 ? 6 : 18;
    const int stepSec = tapSec * factor;

    if (InstantSeek()) {
        // "Jump when seeking: Right away": every step seeks at once. Quick presses build on the
        // last target, since the reported position can lag just after a jump.
        const REFERENCE_TIME base = (m_rtInstantTarget >= 0 && sinceLast < 1500) ? m_rtInstantTarget : pMF->GetPos();
        m_rtInstantTarget = SnapToChapter(base, std::clamp<REFERENCE_TIME>(base + dir * stepSec * kSecond, 0, dur), dir);
        m_nInstantAt = now;
        m_rtPreview = -1;
        pMF->SeekTo(m_rtInstantTarget);
        m_nDotWatchUntil = now + 1500;
        m_nLastBarRender = 0;
        if (m_bOpen) {
            Render();
        }
        return;
    }
    const REFERENCE_TIME base = (m_rtPreview >= 0) ? m_rtPreview : pMF->GetPos();
    m_rtPreview = SnapToChapter(base, std::clamp<REFERENCE_TIME>(base + dir * stepSec * kSecond, 0, dur), dir);
    Render();
}

// "Seeking stops at chapters": a step that lands within a few seconds of a chapter start (one
// ahead of where it started, in its direction) lands on it instead. Not for held-key glides.
REFERENCE_TIME CTvMenu::SnapToChapter(REFERENCE_TIME from, REFERENCE_TIME target, int dir) const
{
    if (!CTvMode::IsOn(TvOpt::ChapterSnap)) {
        return target;
    }
    constexpr REFERENCE_TIME kWindow = 5 * kSecond;
    REFERENCE_TIME best = target, bestDist = kWindow + 1;
    for (const Chapter& c : GetChapters()) {
        const REFERENCE_TIME d = c.rt > target ? c.rt - target : target - c.rt;
        if (c.rt > 0 && dir * (c.rt - from) > 0 && d <= kWindow && d < bestDist) {
            best = c.rt;
            bestDist = d;
        }
    }
    return best;
}

// Held Left / Right with the preview: moves it by the time since the last frame. The speed is the
// old stepping's average (one seek step per kScrubThrottleMs), and the hold speed-up (x3, x6, x18)
// ramps up around the old switch points instead of jumping (times scaled by the speed-up option). Stops when the key is let go (WM_KEYUP), or if its repeats stop.
bool CTvMenu::StepGlide()
{
    if (m_nGlideDir == 0) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    const REFERENCE_TIME dur = m_pMainFrame->GetDur();
    if (m_rtPreview < 0 || dur <= 0 || now - m_nGlideKey > 600) {
        m_nGlideDir = 0;
        return false;
    }
    const ULONGLONG ms = std::min<ULONGLONG>(now - m_nGlideTick, 100);
    m_nGlideTick = now;
    const float speed = CTvMode::HoldSpeed();
    float factor = 1.0f;
    if (speed > 0.0f) {
        const float t = static_cast<float>(now - m_nScrubHoldStart) * speed;
        // (time held in ms, speed): short ramps around where the old steps switched (0.5, 2, 4 s)
        static const float kCurve[][2] = { { 300, 1 }, { 700, 3 }, { 1800, 3 }, { 2400, 6 }, { 3600, 6 }, { 4800, 18 } };
        factor = kCurve[_countof(kCurve) - 1][1];
        for (size_t k = 0; k < _countof(kCurve); k++) {
            if (t < kCurve[k][0]) {
                factor = k == 0 ? kCurve[0][1]
                                : kCurve[k - 1][1] + (kCurve[k][1] - kCurve[k - 1][1]) * (t - kCurve[k - 1][0]) / (kCurve[k][0] - kCurve[k - 1][0]);
                break;
            }
        }
    }
    const double perMs = CTvMode::Get(TvOpt::SeekStepSec) * static_cast<double>(kSecond) * factor / kScrubThrottleMs;
    m_rtPreview = std::clamp<REFERENCE_TIME>(m_rtPreview + static_cast<REFERENCE_TIME>(m_nGlideDir * perMs * ms), 0, dur);
    m_nLastInput = now; // not idle while gliding: the jump waits until it stops
    Render();
    return true;
}

// OK on the bar: apply a seek preview, play / pause on the seek row, or press the focused button
void CTvMenu::BarOk()
{
    if (m_nBarRow == 0) {
        if (m_rtPreview >= 0) {
            ApplySeek(); // jump now instead of waiting
        } else {
            PressBarButton(BarButton::PlayPause);
        }
    } else {
        PressBarButton(m_barButton);
    }
}

// OK let go before it counted as held: the normal OK (if the bar is still what it was pressed on)
void CTvMenu::ReleaseOk()
{
    if (!m_bOkPending) {
        return;
    }
    m_bOkPending = false;
    if (m_bOpen && m_page == Page::Bar) {
        m_nLastInput = GetTickCount64();
        BarOk();
    }
}

bool CTvMenu::InstantSeek() const
{
    // With nothing shown while seeking there's no preview to look at, so it always jumps at once.
    return CTvMode::Get(TvOpt::SeekMode) == 1 || CTvMode::Get(TvOpt::ShowOnSeek) == 2;
}

// "Jump when seeking: After OK": the preview stays until OK (Back cancels). Not with nothing shown
// while seeking (there is no preview then).
bool CTvMenu::SeekWaitsForOk() const
{
    return CTvMode::Get(TvOpt::SeekMode) == 2 && CTvMode::Get(TvOpt::ShowOnSeek) != 2;
}

void CTvMenu::ApplySeek()
{
    if (m_rtPreview < 0) {
        return;
    }
    const REFERENCE_TIME target = m_rtPreview;
    m_rtPreview = -1;
    m_pMainFrame->SeekTo(target);
    m_nDotWatchUntil = GetTickCount64() + 1500; // the playhead glides there once it's reported
    m_nLastBarRender = 0;
    Render();
}

// Mouse drag on the seek row: seek to the dragged position now, at most every kDragSeekMs
// (each seek makes the decoder start again, so faster would only make it stutter).
void CTvMenu::DragSeekNow(bool force)
{
    // Like MPC-HC's own seek bar (CPlayerSeekBar): with its "fast seek" option, files of a minute
    // or more seek to the nearest key frame while dragging (Shift = exact), which shows a picture
    // almost at once; with "pause while dragging the seek bar", playback pauses during the drag.
    // The exact position follows on release (EndDragSeek).
    constexpr ULONGLONG kDragSeekMs = 100;
    const ULONGLONG now = GetTickCount64();
    if (m_rtPreview < 0 || (!force && now - m_nLastDragSeek < kDragSeekMs)) {
        return;
    }
    const CAppSettings& s = AfxGetAppSettings();
    REFERENCE_TIME target = m_rtPreview;
    const REFERENCE_TIME dur = m_pMainFrame->GetDur();
    if (dur >= 600000000LL && s.bFastSeek && GetKeyState(VK_SHIFT) >= 0) {
        const REFERENCE_TIME maxDiff = s.bAllowInaccurateFastseek ? 200000000LL : std::min<REFERENCE_TIME>(100000000LL, dur / 30);
        target = m_pMainFrame->GetClosestKeyFrame(target, maxDiff, maxDiff);
    }
    if (target == m_rtDragSought) {
        return; // same key frame as last time
    }
    if (m_rtDragSought < 0 && s.bPauseWhileDraggingSeekbar && m_pMainFrame->GetMediaState() == State_Running) {
        m_pMainFrame->MediaControlPause();
        m_bPausedForDrag = true;
    }
    m_nLastDragSeek = now;
    m_rtDragSought = target;
    m_pMainFrame->SeekTo(target, false); // no MPC-HC OSD message for every step
    m_nDotWatchUntil = now + 1500;        // the playhead glides after each live seek
}

// End of a mouse drag on the seek row: exact position, then playing again if the drag paused it
void CTvMenu::EndDragSeek()
{
    m_bDragSeek = false;
    ApplySeek();
    if (m_bPausedForDrag) {
        m_bPausedForDrag = false;
        m_pMainFrame->MediaControlRun();
    }
}

void CTvMenu::CancelSeek()
{
    m_rtPreview = -1;
    Render();
}

// ---------------------------------------------------------------------------
// Left/Right on the Playback list

void CTvMenu::CycleTrack(RowKind kind, int dir)
{
    // The sequence to step through: audio tracks, or Off followed by the subtitle tracks.
    std::vector<int> seq;
    int base = -1;
    if (kind == RowKind::OpenAudio) {
        std::vector<TvTrack> tracks;
        if (!CTvTracks::GetAudio(m_pMainFrame, tracks)) {
            return;
        }
        for (const auto& t : tracks) {
            seq.push_back(t.index);
        }
        base = (m_nPendingAudio != kNoPending) ? m_nPendingAudio : CurrentAudioIndex(m_pMainFrame);
    } else {
        std::vector<TvTrack> tracks;
        bool on = false;
        if (!CTvTracks::GetSubtitles(m_pMainFrame, tracks, on)) {
            return;
        }
        seq.push_back(-1);
        for (const auto& t : tracks) {
            seq.push_back(t.index);
        }
        base = (m_nPendingSubtitle != kNoPending) ? m_nPendingSubtitle : CurrentSubtitleIndex(m_pMainFrame);
    }
    if (seq.size() < 2) {
        return; // nothing to switch between
    }

    auto it = std::find(seq.begin(), seq.end(), base);
    const int n = static_cast<int>(seq.size());
    int pos = (it == seq.end()) ? 0 : static_cast<int>(it - seq.begin());
    pos = (pos + dir + n) % n; // wraps around, so one direction reaches every track

    if (kind == RowKind::OpenAudio) {
        m_nPendingAudio = seq[pos];
    } else {
        m_nPendingSubtitle = seq[pos];
    }

    BuildRows(); // main list: values now show the pending choice
    Render();

    // Switch only once the user stops pressing, so stepping past tracks doesn't make audio stutter.
    CancelApplyTimer();
    m_nApplyTimer = SetTimer(nullptr, 0, kApplyDelayMs, &CTvMenu::OnApplyTimer);
}

void CTvMenu::ApplyPending()
{
    CancelApplyTimer();
    if (!m_pMainFrame) {
        return;
    }
    if (m_nPendingAudio != kNoPending) {
        const int index = m_nPendingAudio;
        m_nPendingAudio = kNoPending;
        if (index != CurrentAudioIndex(m_pMainFrame)) {
            CTvTracks::SelectAudio(m_pMainFrame, index);
        }
    }
    if (m_nPendingSubtitle != kNoPending) {
        const int index = m_nPendingSubtitle;
        m_nPendingSubtitle = kNoPending;
        if (index != CurrentSubtitleIndex(m_pMainFrame)) {
            CTvTracks::SelectSubtitle(m_pMainFrame, index);
        }
    }
    if (m_bOpen && m_page == Page::Main) {
        BuildRows();
        Render();
    }
}

void CTvMenu::CancelApplyTimer()
{
    if (m_nApplyTimer) {
        KillTimer(nullptr, m_nApplyTimer);
        m_nApplyTimer = 0;
    }
}

void CALLBACK CTvMenu::OnApplyTimer(HWND /*hWnd*/, UINT /*msg*/, UINT_PTR /*id*/, DWORD /*time*/)
{
    // Thread timer: runs on the UI thread from the main message loop.
    Instance().ApplyPending();
}

// ---------------------------------------------------------------------------
// Timer

void CTvMenu::StartTick()
{
    if (!m_nTickTimer) {
        // Own-window overlay: Windows shows every update at the display rate, so tick faster
        // (animation steps scale with the real time between ticks, see OnTick)
        m_nTickTimer = SetTimer(nullptr, 0, m_bWindowOsd ? USER_TIMER_MINIMUM : kTickMs, &CTvMenu::OnTickTimer);
        m_nLastTick = GetTickCount64();
    }
}

void CTvMenu::StopTick()
{
    if (m_nTickTimer) {
        KillTimer(nullptr, m_nTickTimer);
        m_nTickTimer = 0;
    }
}

void CALLBACK CTvMenu::OnTickTimer(HWND /*hWnd*/, UINT /*msg*/, UINT_PTR /*id*/, DWORD /*time*/)
{
    Instance().OnTick();
}

void CTvMenu::OnTick()
{
    // Steps are defined per 20 ms; scale them by the real time since the last tick, so animations
    // take the same time whatever the tick rate (faster with the own-window overlay).
    const ULONGLONG tickNow = GetTickCount64();
    const float dt = std::clamp(static_cast<float>(tickNow - m_nLastTick) / kTickMs, 0.2f, 3.0f);
    m_nLastTick = tickNow;
    const float fadeIn = std::min(1.0f, kFadeInStep * dt), fadeOut = std::min(1.0f, kFadeOutStep * dt);
    const float morphStep = std::min(1.0f, kMorphStep * dt);
    // Own-window overlay: follow the video window when it moves (the windows are placed on screen)
    if (m_bWindowOsd && m_bOpen && m_pMainFrame && m_pMainFrame->m_pVideoWnd) {
        POINT o = { 0, 0 };
        ClientToScreen(m_pMainFrame->m_pVideoWnd->m_hWnd, &o);
        if (o.x != m_lastOrigin.x || o.y != m_lastOrigin.y) {
            const bool first = m_lastOrigin.x == LONG_MIN;
            m_lastOrigin = o;
            if (!first) {
                m_bDirty = true;
            }
        }
    }
    // Bar preview under TV options: its own fade in / out. Stepped first, so whichever presenting
    // happens this tick shows its current state (it used to stop half-way when its last steps fell on
    // ticks where the page's own fade was presenting).
    bool previewChanged = false;
    if (m_fPreviewFade != m_fPreviewTarget && m_pOsd) {
        m_fPreviewFade = (m_fPreviewTarget > m_fPreviewFade) ? std::min(m_fPreviewTarget, m_fPreviewFade + fadeIn)
                                                             : std::max(m_fPreviewTarget, m_fPreviewFade - fadeOut);
        previewChanged = true;
    }
    // Fade towards the target opacity.
    bool rendered = false;
    if (m_fFade != m_fFadeTarget) {
        if (m_fFadeTarget > m_fFade) {
            m_fFade = std::min(m_fFadeTarget, m_fFade + fadeIn);
        } else {
            m_fFade = std::max(m_fFadeTarget, m_fFade - fadeOut);
        }
        if (m_fFade <= 0.0f && !m_bOpen) {
            RemoveNow(); // fade-out finished
            return;
        }
        if (!RepresentMenu()) { // only the fade changed: no need to draw again
            Render();
        }
        rendered = true;
    }
    // Mini view glide (applied at the next presenting; the fade step above may already have shown it)
    if (m_fMorph < 1.0f) {
        m_fMorph = std::min(1.0f, m_fMorph + morphStep);
        if (!rendered) {
            if (!RepresentMenu()) {
                Render();
            }
            rendered = true;
        }
    }
    // Wheel scroll glide: eases towards the new scroll position
    if (m_bScrollGlide) {
        // Once the wheel / scrollbar has been left alone a moment, settle on a whole line
        if (!m_bDragScroll && tickNow - m_nScrollInputAt > 120) {
            m_fScrollTarget = static_cast<float>(m_nScroll);
        }
        // Critically damped spring: eases in and out like a browser's smooth scrolling, and
        // notches that come quickly carry the motion on instead of restarting it
        const float w = kScrollSpring;
        const float maxScroll = static_cast<float>(std::max(0, LineCount() - MaxRows()));
        const float secs = dt * kTickMs / 1000.0f;
        for (int k = 0; k < 8; k++) { // small steps: stable at the fastest Animations setting too
            const float h = secs / 8.0f;
            const float a = w * w * (m_fScrollTarget - m_fScrollShown) - 2.0f * w * m_fScrollVel;
            m_fScrollVel += a * h;
            m_fScrollShown = std::clamp(m_fScrollShown + m_fScrollVel * h, 0.0f, maxScroll);
        }
        if (!m_bDragScroll && m_fScrollTarget == static_cast<float>(m_nScroll) &&
            std::abs(m_fScrollTarget - m_fScrollShown) < 0.01f && std::abs(m_fScrollVel) < 0.1f) {
            m_fScrollShown = m_fScrollTarget;
            m_fScrollVel = 0.0f;
            m_bScrollGlide = false;
        }
        if (!rendered) {
            Render();
            rendered = true;
        }
    }
    // The previous page / state fading out under the new one
    if (m_fOld > 0.0f) {
        StepOldFrame();
    }

    if (previewChanged && m_pOsd) {
        const Shown p = m_shown[ElemPreview];
        if (!p.valid || m_fPreviewFade <= 0.0f || !g_content[p.slot]) {
            UpdateBarPreview(); // first appearance (draws it), or faded out (removes it)
        } else if (!rendered) {
            Present(ElemPreview, p.slot, p.x, p.y, p.radius, p.alpha, p.slideMode, p.slideDist); // same drawing, new fade
        } // else: already presented with the new value by the page's fade step above
    }

    if (!m_bOpen) {
        return;
    }
    if (m_bDirty && !rendered) {
        Render(); // a throttled mouse redraw
        rendered = true;
    }
    if (m_xf.active && !rendered) {
        StepXfade(); // focus crossfade: only the two rows' overlays
        rendered = true;
    }
    // Bar preview showing a selected button: Play and Settings take turns
    if (PreviewShowsSelection() && tickNow / 1600 != m_nDemoPhase) {
        m_nDemoPhase = tickNow / 1600;
        UpdateBarPreview();
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG idle = now - m_nLastInput;

    // OK on the bar waiting to see if it's held, but neither a repeat nor its key-up has come for a
    // second (e.g. the key-up went to another window): act as a short press
    if (m_bOkPending && now - m_nOkLastKey > 1000) {
        ReleaseOk();
        rendered = true;
    }
    if ((m_page == Page::Bar || m_page == Page::MiniSeek) && !rendered && StepGlide()) {
        rendered = true; // held Left / Right: the preview moved this frame
    }
    if (m_page == Page::Bar) {
        // Mouse drag held still: catch up with the last position the throttle skipped
        if (m_bDragSeek) {
            DragSeekNow(false);
        }
        // Jump once the user stops pressing Left/Right (a mouse drag jumps on button-up), unless
        // "Jump when seeking" is "After OK" (then OK jumps, Back cancels)
        if (m_rtPreview >= 0 && idle >= kSeekApplyMs && !m_bDragSeek && !SeekWaitsForOk()) {
            ApplySeek();
            rendered = true;
        }
        // Hide while playing after a while; a paused bar stays up.
        const ULONGLONG barHideMs = static_cast<ULONGLONG>(CTvMode::Get(TvOpt::BarHideSec)) * 1000;
        if (m_rtPreview < 0 && idle > barHideMs && IsPlaying()) {
            Close();
            return;
        }
        // Every frame while the playhead dot glides, and for a moment after a key / seek (the new
        // position can take a little while to be reported); otherwise twice a second.
        const bool dotWatch = CTvMode::IsOn(TvOpt::SmoothPlayhead) && CTvMode::AnimScale() > 0.0f &&
                              (now - m_nLastInput < 1000 || now < m_nDotWatchUntil);
        if (!rendered && (m_bDotMoving || dotWatch || now - m_nLastBarRender >= kBarRefreshMs)) {
            Render(); // time, play/pause icon, playhead
            rendered = true;
        }
    } else if (m_page == Page::Pause) {
        if (IsPlaying()) {
            Close(); // playing again (any way): the pause screen goes
            return;
        }
        if (!rendered && now - m_nLastRender >= 5000) {
            Render(); // "Ends at" moves on while paused
        }
    } else if (m_page == Page::MiniSeek) {
        // The small indicator only lives as long as the seek preview (with instant seeking:
        // until the keys have been quiet for a moment).
        if (m_rtPreview >= 0 && idle >= kSeekApplyMs && !SeekWaitsForOk()) {
            ApplySeek();
        }
        if (m_rtPreview < 0 && idle >= kSeekApplyMs) {
            Close();
            return;
        }
    } else if (m_page == Page::Scopes) {
        // New measurements arrive every couple of frames: redraw up to ~6 times a second
        if (!rendered && CTvScopes::Version() != m_nScopesVersion && now - m_nLastRender >= 160) {
            Render();
            rendered = true;
        }
    } else if (IsInfoPage(m_page)) {
        // Info panels stay until Back (they're for reading). Refresh once a second: the
        // MediaInfo result arrives, and display / decoder details are live.
        if (now - m_nLastInfoBuild >= 1000) {
            BuildInfo();
            if (InfoSignature() != m_strInfoSig) { // only when something changed
                Render();
                rendered = true;
            }
        }
    } else {
        const int menuHideSec = CTvMode::Get(TvOpt::MenuHideSec); // 0 = never
        // TV options and the colour picker never close by themselves (you're busy adjusting).
        const bool settingsPage = (IsBandPage(m_page) && m_page != Page::Main) || m_page == Page::ColourPicker || m_page == Page::SubStyle;
        if (!settingsPage && menuHideSec > 0 && idle > static_cast<ULONGLONG>(menuHideSec) * 1000) {
            Close();
            return;
        }
    }

    // Follow window / fullscreen size changes, so the panel stays placed and scaled.
    if (!rendered) {
        int w = 0, h = 0;
        GetOutputSize(m_pOsd, w, h);
        if (w != m_lastOutput.cx || h != m_lastOutput.cy) {
            Render();
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing

void CTvMenu::RemoveOsd()
{
    if (m_pOsd) {
        m_pOsd->OsdSetBitmap(kOsdName, nullptr);
        m_pOsd->OsdSetBitmap(kPreviewOsdName, nullptr);
        m_pOsd->OsdSetBitmap(kSubPreviewOsdName, nullptr);
    }
    if (m_hbmPreview) {
        DeleteObject(m_hbmPreview);
        m_hbmPreview = nullptr;
    }
    if (m_hbmSubPreview) {
        DeleteObject(m_hbmSubPreview);
        m_hbmSubPreview = nullptr;
    }
    if (m_hbmMenu) {
        DeleteObject(m_hbmMenu);
        m_hbmMenu = nullptr;
    }
    // Presented pictures, overlays and kept drawings
    for (int e = 0; e < ElemCount; e++) {
        if (m_pOsd && e != ElemMenu && e != ElemPreview) {
            m_pOsd->OsdSetBitmap(kElemNames[e], nullptr);
        }
        FreeOut(e);
        m_shown[e].valid = false;
    }
    for (auto& c : g_content) {
        c.reset();
    }
    m_xf = Xfade();
    m_fOld = 0.0f;
    m_fMorph = 1.0f;
    m_morphFromRow = INT_MIN;
    m_lastPanelPage = Page::None;
}

// Hands a kept drawing to madVR: alpha for the current fade (rounded corners), the show / hide
// slide, the mini-view glide, and transparent holes where focus overlays sit.
void CTvMenu::Present(int elem, int slot, int x, int y, int radius, BYTE alpha, int slideMode, int slideDist)
{
    CDibCanvas* c = g_content[slot].get();
    if (!c || !c->IsValid() || !m_pOsd) {
        return;
    }
    GdiFlush();
    const float fade = std::clamp(m_fFade, 0.0f, 1.0f) * (elem == ElemPreview ? std::clamp(m_fPreviewFade, 0.0f, 1.0f) : 1.0f);
    int yPos = y, dy = 0;
    switch (slideMode) {
        case 1: yPos += SlideOffset(slideDist) + MorphOffset(); break;
        case 2: dy = SlideOffset(slideDist); break;
        case 3: dy = -SlideOffset(slideDist); break;
        case 4: {
            const float pf = std::clamp(m_fPreviewFade, 0.0f, 1.0f);
            dy = static_cast<int>((1.0f - pf) * (1.0f - pf) * (1.0f - pf) * slideDist + 0.5f); // rises from the edge
            break;
        }
        default: break;
    }
    const int w = c->Width(), h = c->Height();
    BYTE* bits = nullptr;
    HBITMAP hbm = NextOut(elem, w, h, bits);
    if (!hbm || !bits) {
        return;
    }
    const BYTE outAlpha = static_cast<BYTE>(alpha * fade + 0.5f);   // the background
    const BYTE outFull = static_cast<BYTE>(255 * fade + 0.5f);      // text etc. on it (solid)
    Compose(c->Bits(), w, 0, 0, w, h, bits, radius, outAlpha, dy, nullptr, 0, outFull);
    // Focus crossfade: the two rows blend from the kept "before" picture to the new drawing, inside
    // this same picture (one element, so madVR always shows a complete page).
    CDibCanvas* was = g_content[SlotXfade].get();
    if (elem == ElemMenu && slot == SlotPanel && m_xf.active && was && was->Width() == w && was->Height() == h && dy == 0) {
        const ULONGLONG ms = GetTickCount64() - m_xf.start;
        const float t = kFocusFadeMs ? std::min(1.0f, static_cast<float>(ms) / kFocusFadeMs) : 1.0f;
        const float e = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
        for (int i = 0; i < m_xf.count; i++) {
            const RECT& r = m_xf.rect[i];
            BlendInto(was->Bits(), c->Bits(), w, h, r, e, bits, outAlpha, outFull);
        }
    }
    // Debug only: MPCHC_TV_DUMP=<folder> saves each element's latest picture as <folder>\elem<N>.bmp
    // (as drawn, before opacity), since the overlay can't be screenshotted
    wchar_t dumpDir[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"MPCHC_TV_DUMP", dumpDir, MAX_PATH) > 0) {
        CStringW file;
        file.Format(L"%s\\elem%d.bmp", dumpDir, elem);
        BITMAPINFOHEADER bih = { sizeof(bih), w, -h, 1, 32, BI_RGB };
        const DWORD size = static_cast<DWORD>(w) * h * 4;
        BITMAPFILEHEADER bfh = { 0x4D42, sizeof(bfh) + sizeof(bih) + size, 0, 0, sizeof(bfh) + sizeof(bih) };
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, file, L"wb") == 0 && fp) {
            fwrite(&bfh, sizeof(bfh), 1, fp);
            fwrite(&bih, sizeof(bih), 1, fp);
            fwrite(c->Bits(), size, 1, fp);
            fclose(fp);
        }
    }
    PerfMark("alpha");
    const int z = elem == ElemPreview ? kZOrder - 1 : elem == ElemOld ? kZOrder - 2 : elem >= ElemFocus0 ? kZOrder + 1 : kZOrder;
    m_pOsd->OsdSetBitmap(kElemNames[elem], hbm, nullptr, 0, x, yPos, false, z, 0, BITMAP_USER_INTERFACE, nullptr, nullptr, nullptr);
    PerfMark("osd");
    Shown& s = m_shown[elem];
    s.valid = true;
    s.page = m_page;
    s.slot = slot;
    s.x = x, s.y = y, s.w = w, s.h = h, s.radius = radius, s.alpha = alpha;
    s.slideMode = slideMode, s.slideDist = slideDist, s.drawnY = yPos, s.drawnDy = dy;
    // a bitmap from the older drawing path (colour picker, mini seek) isn't shown any more
    HBITMAP& legacy = elem == ElemPreview ? m_hbmPreview : m_hbmMenu;
    if ((elem == ElemMenu || elem == ElemPreview) && legacy) {
        DeleteObject(legacy);
        legacy = nullptr;
    }
}

void CTvMenu::HideElement(int elem)
{
    if (m_pOsd) {
        m_pOsd->OsdSetBitmap(kElemNames[elem], nullptr);
    }
    m_shown[elem].valid = false;
}

// A fade / glide step: the same drawing, presented again. False when it has to be drawn.
bool CTvMenu::RepresentMenu()
{
    const Shown s = m_shown[ElemMenu];
    if (!m_pOsd || !s.valid || s.page != m_page || m_bDirty || !g_content[s.slot]) {
        return false;
    }
    int w = 0, h = 0;
    GetOutputSize(m_pOsd, w, h);
    if (w != m_lastOutput.cx || h != m_lastOutput.cy) {
        return false;
    }
    PerfStep perf("represent");
    Present(ElemMenu, s.slot, s.x, s.y, s.radius, s.alpha, s.slideMode, s.slideDist);
    const Shown p = m_shown[ElemPreview];
    if (p.valid && g_content[p.slot]) {
        Present(ElemPreview, p.slot, p.x, p.y, p.radius, p.alpha, p.slideMode, p.slideDist);
    }
    return true;
}

void CTvMenu::CancelXfade()
{
    m_xf = Xfade();
}

// Focus crossfade step: the rows blend from the old look (kept copy) to the new drawing.
void CTvMenu::StepXfade()
{
    if (!m_xf.active) {
        return;
    }
    PerfStep perf("xfade");
    const Shown s = m_shown[ElemMenu];
    const ULONGLONG ms = GetTickCount64() - m_xf.start;
    const float t = kFocusFadeMs ? std::min(1.0f, static_cast<float>(ms) / kFocusFadeMs) : 1.0f;
    if (t >= 1.0f || !s.valid || s.slot != SlotPanel) {
        m_xf.active = false; // done: the plain new picture
    }
    if (s.valid && s.slot == SlotPanel) {
        Present(ElemMenu, s.slot, s.x, s.y, s.radius, s.alpha, s.slideMode, s.slideDist); // blends while active
    }
}

// The picture on screen becomes the "old" one: it fades out (and drifts a little) under the new.
void CTvMenu::StartOldFrame()
{
    const Shown s = m_shown[ElemMenu];
    if (CTvMode::AnimScale() <= 0.0f || !m_pOsd || !s.valid || !g_content[s.slot] || m_fFade <= 0.05f) {
        return;
    }
    CDibCanvas* c = g_content[s.slot].get();
    CDibCanvas& old = Content(SlotOld, c->Width(), c->Height());
    if (!old.IsValid()) {
        return;
    }
    CopyCanvas(*c, old);
    CancelXfade();
    m_oldShown = s;
    m_oldShown.slot = SlotOld;
    m_fOldStart = m_fOld = std::clamp(m_fFade, 0.0f, 1.0f);
    m_oldDy = 0;
    PresentOld(); // in place, as it was, until the new page starts fading in
}

// The old picture's opacity follows the new page's fade so that, where they overlap, together
// they always make exactly the normal panel opacity (no see-through dip halfway): with the new
// page at alpha A*f on top, the old one needs A*(1-f)/(1-A*f). It's gone when the new one is in.
void CTvMenu::StepOldFrame()
{
    if (m_fOld <= 0.0f) {
        return;
    }
    const float f = std::clamp(m_fFade, 0.0f, 1.0f);
    const float a = m_oldShown.alpha / 255.0f;
    const float k = (f >= 1.0f || m_fFadeTarget <= 0.0f) ? 0.0f : (1.0f - f) / std::max(0.05f, 1.0f - a * f);
    m_fOld = std::min(m_fOld, k * m_fOldStart); // never comes back up
    if (m_fOld <= 0.01f) {
        m_fOld = 0.0f;
        HideElement(ElemOld);
        return;
    }
    PresentOld();
}

void CTvMenu::PresentOld()
{
    PerfStep perf("old");
    CDibCanvas* c = g_content[SlotOld].get();
    if (!c || !m_pOsd) {
        m_fOld = 0.0f;
        return;
    }
    GdiFlush();
    const Shown& s = m_oldShown;
    const float p = 1.0f - m_fOld / std::max(0.01f, m_fOldStart); // 0 -> 1
    const float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
    // Drifts the way it came in (bar down, info up, panels down a little), or to its row (mini view)
    const int drift = m_oldDy != 0 ? static_cast<int>(e * m_oldDy)
                                   : static_cast<int>(e * s.slideDist * (s.slideMode == 3 ? -1.0f : 1.0f));
    int y = s.drawnY, dy = s.drawnDy;
    if (s.slideMode == 2 || s.slideMode == 3) {
        dy += drift; // pinned to a screen edge: moves inside its bitmap
    } else {
        y += drift;
    }
    BYTE* bits = nullptr;
    HBITMAP hbm = NextOut(ElemOld, c->Width(), c->Height(), bits);
    if (!hbm || !bits) {
        return;
    }
    Compose(c->Bits(), c->Width(), 0, 0, c->Width(), c->Height(), bits, s.radius, static_cast<BYTE>(s.alpha * m_fOld + 0.5f), dy,
            nullptr, 0, static_cast<BYTE>((s.alpha ? 255 : 0) * m_fOld + 0.5f)); // text solid, background see-through
    m_pOsd->OsdSetBitmap(kElemNames[ElemOld], hbm, nullptr, 0, s.x, y, false, kZOrder - 2, 0, BITMAP_USER_INTERFACE, nullptr,
                         nullptr, nullptr);
    m_shown[ElemOld].valid = true;
}

// madVR mini view on / off: the full list fades out while the focused row glides up into the
// strip at the top; back to the list, the strip glides down to its row while the list fades in.
void CTvMenu::SetMadVrCompact(bool on)
{
    if (m_bMadVrCompact == on) {
        Render();
        return;
    }
    const bool anim = CTvMode::AnimScale() > 0.0f && m_shown[ElemMenu].valid && m_fFade > 0.05f;
    const int rowTop = (m_nFocus >= 0 && m_nFocus < static_cast<int>(m_hitRowRect.size())) ? m_hitRowRect[m_nFocus].top : INT_MIN;
    if (anim) {
        StartOldFrame();
        m_fFade = std::min(m_fFade, 0.3f);
    }
    m_bMadVrCompact = on;
    m_rowFade.valid = false;
    if (anim && on && rowTop != INT_MIN) {
        m_morphFromRow = rowTop; // RenderPanel turns this into the glide distance
    }
    Render();
    if (anim && !on && m_fOld > 0.0f && m_nFocus >= 0 && m_nFocus < static_cast<int>(m_hitRowRect.size())) {
        // the strip (old picture) glides down to where its row now is in the list
        m_oldDy = m_hitRowRect[m_nFocus].top - (m_oldShown.y + m_oldShown.radius);
    }
}

CStringW CTvMenu::InfoSignature() const
{
    CStringW s = m_strInfoFile;
    for (const auto& l : m_info) {
        s += L'\x1f' + l.name + L'\x1e' + l.value;
    }
    for (const auto& c : m_chips1) {
        s += L'\x1d' + c.label + L'\x1e' + c.value;
    }
    for (const auto& c : m_chips2) {
        s += L'\x1c' + c.label + L'\x1e' + c.value;
    }
    for (const auto& b : m_badges) {
        s += L'\x1b' + b.text;
    }
    return s;
}

void CTvMenu::Render()
{
    if (!m_pOsd) {
        return;
    }
    // More keys already waiting (a held / fast remote key): handle them first and draw once after,
    // instead of drawing every in-between state (which made navigation lag behind the keys).
    MSG pending;
    if (m_bOpen && PeekMessageW(&pending, nullptr, WM_KEYDOWN, WM_KEYDOWN, PM_NOREMOVE)) {
        m_bDirty = true; // the tick draws it once the keys are handled
        return;
    }
    // Optional timing log (TvMode registry value "PerfLog" = 1, or environment MPCHC_TV_PERF=1):
    // %TEMP%\mpc-hc-tv-perf.log, one line per render with the stages marked by PerfMark
    static const bool perfLog = CTvMode::GetValue(_T("PerfLog"), 0) != 0 || GetEnvironmentVariableW(L"MPCHC_TV_PERF", nullptr, 0) > 0;
    LARGE_INTEGER t0 = {};
    g_perfOn = perfLog;
    g_perfMarks.clear();
    if (perfLog) {
        QueryPerformanceCounter(&t0);
        g_perfLast = t0;
    }
    struct PerfLogLine {
        bool on;
        LARGE_INTEGER t0;
        Page page;
        float fade;
        ~PerfLogLine() {
            if (!on) {
                return;
            }
            LARGE_INTEGER t1, f;
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            wchar_t path[MAX_PATH];
            GetTempPathW(MAX_PATH, path);
            wcscat_s(path, L"mpc-hc-tv-perf.log");
            FILE* fp = nullptr;
            if (_wfopen_s(&fp, path, L"a") == 0 && fp) {
                fprintf(fp, "%llu page=%d fade=%.2f scroll=%.3f ms=%.2f", GetTickCount64(), static_cast<int>(page), fade,
                        Instance().m_fScrollShown, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
                for (const auto& m : g_perfMarks) {
                    fprintf(fp, " %s=%.2f", m.first, m.second);
                }
                fprintf(fp, " build=%.2f", g_perfBuildMs);
                if (g_perfKey.QuadPart) {
                    fprintf(fp, " sinceKey=%.2f", (t1.QuadPart - g_perfKey.QuadPart) * 1000.0 / f.QuadPart);
                }
                fprintf(fp, "\n");
                fclose(fp);
            }
            g_perfKey.QuadPart = 0;
            g_perfBuildMs = 0.0;
        }
    } perfLine { perfLog, t0, m_page, m_fFade };
    m_nLastRender = GetTickCount64();
    m_bDirty = false;
    TrimFontCache();
    kFocusColor = CTvMode::Accent();
    kPanelColor = CTvMode::Background();
    g_fontFace = CTvMode::Font();
    RefreshLookOptions();
    m_hitScrollTrack = {}; // set again by whichever page draws a scrollbar
    m_nScrollMax = 0;
    if (m_page == Page::Bar) {
        RenderBar();
    } else if (m_page == Page::MiniSeek) {
        RenderMiniSeek();
    } else if (m_page == Page::Pause) {
        RenderPauseScreen();
    } else if (IsInfoPage(m_page)) {
        RenderInfo();
    } else if (m_page == Page::Scopes) {
        RenderScopes();
    } else if (m_page == Page::ColourPicker) {
        RenderPicker();
    } else if (!m_rows.empty()) {
        RenderPanel();
    }
    UpdateBarPreview();
}

int CTvMenu::MaxRows() const
{
    if (!IsBandPage(m_page) && m_page != Page::SubStyle) {
        return kMaxVisibleRows;
    }
    // Settings pages (TV options, madVR, LAV): as tall as the band from 5 % below the top to 2 % above
    // the control bar (same row size as RenderPanel uses). Subtitle style: at most the top ~62 %, so
    // the sample subtitle near the bottom stays clear.
    const RECT r = GetOutputRect(m_pOsd);
    const int outH = r.bottom - r.top;
    const int unit = std::max(16, static_cast<int>(outH / 13.0f * 0.5f * Scale(TvOpt::MenuTextPct)));
    const int pad = unit / 3;
    const int hintH = CTvMode::IsOn(TvOpt::KeyHints) ? unit * 7 / 10 : 0;
    const int helpH = (m_page == Page::OptionsSection || m_page == Page::SubStyle) ? unit * 8 / 10 : 0; // as RenderPanel
    int bandTop = 0, bandBottom = 0;
    SettingsBand(outH, bandTop, bandBottom);
    const int avail = (m_page == Page::SubStyle) ? outH * 62 / 100 : bandBottom - bandTop;
    return std::max(4, (avail - 2 * pad - unit - hintH - helpH) / unit);
}

// Control bar height at this output height (same formula as RenderBar)
int CTvMenu::BarHeightAt(int outH) const
{
    const int unit = std::max(14, static_cast<int>(outH / 22.0f * 0.7f * Scale(TvOpt::BarSizePct)));
    const bool hasChapters = !GetChapters().empty() && m_pMainFrame && m_pMainFrame->GetDur() > 0;
    const int chapterRowH = hasChapters ? unit * 8 / 10 : 0;
    const int labelRowH = CTvMode::IsOn(TvOpt::ButtonLabels) ? unit * 55 / 100 : 0;
    return unit * 4 + chapterRowH + labelRowH;
}

// Where settings pages may sit: from 5 % below the top to 2 % above the control bar (the bar preview
// under TV options appears there)
void CTvMenu::SettingsBand(int outH, int& top, int& bottom) const
{
    top = outH * 5 / 100;
    bottom = std::max(top + outH / 3, outH - BarHeightAt(outH) - outH * 2 / 100);
}

void CTvMenu::RenderPanel()
{
    m_outRect = GetOutputRect(m_pOsd);
    const int outW = m_outRect.right - m_outRect.left;
    const int outH = m_outRect.bottom - m_outRect.top;
    m_lastOutput = { outW, outH };

    // Settings and TV options: label + value rows, no check column
    const bool isMain = (m_page == Page::Main || m_page == Page::Options || m_page == Page::OptionsSection || m_page == Page::SubStyle ||
                         m_page == Page::MadVR || m_page == Page::Lav || m_page == Page::Video || m_page == Page::Advanced ||
                         m_page == Page::Quit);
    const int count = static_cast<int>(m_rows.size());
    // madVR live-adjust strip: just the focused row, small, at the top of the picture
    const bool compact = m_bMadVrCompact && m_page == Page::MadVR && m_nFocus >= 0 && m_nFocus < count;
    const int lineCount = LineCount();      // = count for one column
    // A wheel scroll glides: the list is drawn between lines (first line + offPx pixels up)
    if (!m_bScrollGlide || compact) {
        m_bScrollGlide = false;
        m_fScrollShown = m_fScrollTarget = static_cast<float>(m_nScroll);
        m_fScrollVel = 0.0f;
    } else if (!m_bDragScroll && m_nScroll != static_cast<int>(std::lround(m_fScrollTarget))) {
        m_fScrollTarget = static_cast<float>(m_nScroll); // the keys moved the list meanwhile: glide there
    }
    const int first = compact ? m_rows[m_nFocus].line : static_cast<int>(std::floor(m_fScrollShown)); // first line shown
    const int visible = compact ? 1 : std::min(lineCount, MaxRows());
    const int cols = (m_bTwoColumns && !compact) ? 2 : 1;
    m_hitFirstRow = first;
    const bool anyDetail = std::any_of(m_rows.begin(), m_rows.end(), [](const Row& r) { return !r.detail.IsEmpty(); });

    // Sizes scale with the output height so the lists read from the couch at 1080p and 2160p,
    // times the "Menu size" option (100 % = half the original size, the chosen default look).
    const int unit = std::max(16, static_cast<int>(outH / 13.0f * 0.5f * Scale(TvOpt::MenuTextPct)));
    const int rowH = anyDetail ? unit * 13 / 10 : unit;
    const int fontH = unit * 52 / 100;
    const int detailH = unit * 34 / 100;
    const int pad = unit / 3;
    const int offPx = compact ? 0 : static_cast<int>(std::lround((m_fScrollShown - first) * rowH));
    const int titleH = compact ? 0 : unit;
    const bool showHints = CTvMode::IsOn(TvOpt::KeyHints);
    const int hintH = showHints ? unit * 7 / 10 : 0;
    const int checkW = isMain ? 0 : unit * 7 / 10;
    // TV options: wider, so values like "Seeking speed-up when holding ... Normal (default)" fit
    // (two columns, like madVR's HDR page: nearly the full width)
    const int widthPct = compact ? 50 : cols == 2 ? 112 : m_page == Page::Quit ? 30
                         : (m_page == Page::OptionsSection || m_page == Page::MadVR) ? 58 : (m_page == Page::Options) ? 44 : (m_page == Page::SubStyle ? 42 : (isMain ? 44 : 50));
    const int panelW = std::min(outW - unit, std::max(unit * 12, outW * widthPct / 100 * CTvMode::Get(TvOpt::MenuTextPct) / 120));
    // TV options pages: one line explaining the focused option, between the list and the key hints
    const bool hasHelp = (m_page == Page::OptionsSection || m_page == Page::SubStyle) && !compact;
    const int helpH = hasHelp ? unit * 8 / 10 : 0;
    const int panelH = pad + titleH + visible * rowH + helpH + hintH + pad;
    // the help and hint lines change with the focus: they crossfade along with the two rows
    // (kept inside the rounded corners: blending the full width would paint them square)
    const RECT rFooter = { pad, pad + titleH + visible * rowH, panelW - pad, panelH - pad };

    // Hit areas for the mouse (output coordinates)
    const int panelX = (outW - panelW) / 2;
    // Subtitle style sits near the top, out of the way of the sample subtitle at the bottom
    // Settings pages: centred in the band between 5 % from the top and 2 % above the control bar
    const bool settingsBand = IsBandPage(m_page);
    int bandTop = 0, bandBottom = outH;
    if (settingsBand) {
        SettingsBand(outH, bandTop, bandBottom);
    }
    const int panelY = (m_page == Page::SubStyle) ? outH * 4 / 100
                       : compact                  ? outH * 3 / 100
                       : settingsBand             ? bandTop + std::max(0, (bandBottom - bandTop - panelH) / 2)
                                                  : (outH - panelH) / 2;
    m_elemRect = { panelX, panelY, panelX + panelW, panelY + panelH };
    m_hitRowsTop = panelY + pad + titleH;
    m_hitRowH = rowH;
    m_hitRowsLeft = panelX + pad;
    m_hitRowsRight = panelX + panelW - pad;
    m_hitRowsVisible = visible;
    m_hitRowRect.assign(count, RECT {});
    m_hitValueLeft.assign(count, -1);
    m_hitValueRightRow.assign(count, -1);
    m_hitArrowW = 0;
    // Where a row sits (panel coordinates); empty when its line is scrolled out of view
    const int colGap = pad * 2;
    const int colW = (panelW - pad * 2 - colGap * (cols - 1)) / cols;
    auto rowRect = [&](int i) -> RECT {
        const Row& rr = m_rows[i];
        const int v = rr.line - first;
        if (v < 0 || v >= visible + (offPx > 0 ? 1 : 0) || (compact && i != m_nFocus)) {
            return RECT {};
        }
        const int x = pad + (compact ? 0 : rr.col) * (colW + colGap);
        const int y = pad + titleH + v * rowH - offPx;
        return { x, y, x + colW, y + rowH };
    };
    const int listTop = pad + titleH, listBottom = pad + titleH + visible * rowH;
    for (int i = 0; i < count; i++) {
        const RECT rr = rowRect(i);
        if (!IsRectEmpty(&rr)) {
            // mouse: only the part inside the list area (a gliding row may stick out)
            m_hitRowRect[i] = { panelX + rr.left, panelY + std::max<int>(rr.top, listTop), panelX + rr.right,
                                panelY + std::min<int>(rr.bottom, listBottom) };
            if (m_hitRowRect[i].bottom <= m_hitRowRect[i].top) {
                m_hitRowRect[i] = RECT {};
            }
        }
    }
    m_hitButtons.clear();
    m_hitSeekX1 = m_hitSeekX0 = 0;

    PerfMark("layout");
    // Focus crossfade: when the focus moved on the same picture (same page, scroll, size), keep
    // a copy of the picture before the move; the page is then drawn once with the new highlight
    // and the two rows blend in small overlays (StepXfade) instead of redrawing the whole page.
    bool fadeRunning = false;
    int fadePrev = -1;
    m_rowFade.Progress(m_nFocus, fadeRunning, fadePrev);
    const bool samePicture = g_content[SlotPanel] && g_content[SlotPanel]->Width() == panelW &&
                             g_content[SlotPanel]->Height() == panelH && m_lastPanelPage == m_page && m_lastPanelScroll == m_nScroll && offPx == 0 && !m_bScrollGlide &&
                             m_lastPanelSub == m_nMadVrSub && m_lastPanelRows == count && m_lastPanelCompact == compact;
    if (fadeRunning && fadePrev >= 0 && m_rowFade.start != m_xf.start) {
        CancelXfade();
        const RECT ra = rowRect(fadePrev), rb = rowRect(m_nFocus);
        if (samePicture && !IsRectEmpty(&ra) && !IsRectEmpty(&rb) && m_fFade >= 1.0f) {
            CDibCanvas& keep = Content(SlotXfade, panelW, panelH);
            if (keep.IsValid()) {
                CopyCanvas(*g_content[SlotPanel], keep);
                m_xf.active = true;
                m_xf.start = m_rowFade.start;
                // one overlay when the rows touch in the same column, else one each
                if (ra.left == rb.left && (ra.bottom == rb.top || rb.bottom == ra.top)) {
                    m_xf.count = 1;
                    m_xf.rect[0] = { ra.left, std::min(ra.top, rb.top), ra.right, std::max(ra.bottom, rb.bottom) };
                } else {
                    m_xf.count = 2;
                    m_xf.rect[0] = ra;
                    m_xf.rect[1] = rb;
                }
                if (helpH > 0 || hintH > 0) {
                    m_xf.rect[m_xf.count++] = rFooter;
                }
            }
        }
    } else if (m_xf.active && !samePicture) {
        CancelXfade();
    }
    m_lastPanelPage = m_page;
    m_lastPanelScroll = m_nScroll;
    m_lastPanelSub = m_nMadVrSub;
    m_lastPanelRows = count;
    m_lastPanelCompact = compact;
    // Mini view on: the strip glides up from where its row was in the list
    if (m_morphFromRow != INT_MIN) {
        m_morphDy = compact ? m_morphFromRow - (panelY + pad) : 0;
        m_fMorph = m_morphDy != 0 ? 0.0f : 1.0f;
        m_morphFromRow = INT_MIN;
    }

    CDibCanvas& canvas = Content(SlotPanel, panelW, panelH);
    if (!canvas.IsValid()) {
        return;
    }
    HDC hdc = canvas.DC();
    SetBkMode(hdc, TRANSPARENT);

    RECT all = { 0, 0, panelW, panelH };
    FillSolid(hdc, all, kPanelColor);

    HFONT fontTitle = MakeFont(fontH, FW_SEMIBOLD);
    HFONT fontRow = MakeFont(fontH, FW_NORMAL);
    HFONT fontDetail = MakeFont(detailH, FW_NORMAL);
    HFONT fontHint = MakeFont(fontH * 6 / 10, FW_NORMAL);
    HFONT fontMdl = MakeFont(fontH, FW_NORMAL, L"Segoe MDL2 Assets");
    HGDIOBJ oldFont = SelectObject(hdc, fontTitle);
    m_hitMadVrParts.clear();
    m_hitTags.clear();

    // Title, with a marker when rows are hidden above
    LPCWSTR title = m_page == Page::Main ? L"Settings"
                    : m_page == Page::Options ? L"TV options"
                    : m_page == Page::OptionsSection ? kOptionSections[std::clamp(m_nSection, 0, static_cast<int>(_countof(kOptionSections)) - 1)].heading
                    : m_page == Page::SubStyle ? L"Subtitle style"
                    : m_page == Page::MadVR ? MadVrSubTitle(m_nMadVrSub)
                    : m_page == Page::Lav ? (m_nLavPage == 1 ? L"LAV Video decoder" : L"LAV Audio decoder")
                    : m_page == Page::Video ? L"Video"
                    : m_page == Page::Advanced ? L"Advanced (mouse)"
                    : m_page == Page::Chapters ? L"Chapters"
                    : m_page == Page::Quit ? L"Power menu"
                    : m_page == Page::Audio ? L"Audio" : L"Subtitles";
    RECT rTitle = { pad * 2, pad, panelW - pad * 2, pad + titleH };
    if (!compact) {
        DrawLine(hdc, fontTitle, kDimTextColor, title, rTitle, DT_LEFT);
    }
    if (m_nScroll > 0 && !compact) {
        DrawLine(hdc, fontHint, kDimTextColor, kTriUp, rTitle, DT_RIGHT);
    }

    // Focus highlight (the move itself crossfades in overlays, see above)
    {
        auto highlight = [&](int row, float amount) {
            if (row >= 0 && row < count && m_rows[row].kind != RowKind::MadVrModeTags) { // tags light up on their own
                const RECT rHi = rowRect(row);
                if (!IsRectEmpty(&rHi)) {
                    // rounded corners (anti-aliased)
                    const float l = static_cast<float>(rHi.left), t = static_cast<float>(rHi.top);
                    const float rr = static_cast<float>(rHi.right), b = static_cast<float>(rHi.bottom);
                    canvas.FillRoundPolyAA({ { l, t }, { rr, t }, { rr, b }, { l, b } }, std::min(rowH * 0.22f, pad * 0.9f),
                                           Mix(kPanelColor, kFocusColor, amount));
                }
            }
        };
        highlight(m_nFocus, 1.0f);
    }

    // Value arrows under the mouse: crossfade like the focus highlight
    int arrowPrev = 0;
    float arrowT = 1.0f;
    {
        bool running = false;
        arrowT = m_rowArrowFade.Progress(m_nRowArrowHover, running, arrowPrev);
        if (running) {
            m_bDirty = true;
        }
    }

    // Rows
    for (int i = 0; i < count; i++) {
        const RECT r = rowRect(i);
        if (IsRectEmpty(&r)) {
            continue; // scrolled out of view
        }
        const Row& row = m_rows[i];
        const bool focused = (i == m_nFocus);

        if (row.kind == RowKind::Heading) {
            // Section heading: accent-coloured, sitting low in its row, with a thin rule after it
            if (!row.text.IsEmpty()) {
                RECT rHead = { r.left + pad / 2, r.top + rowH / 4, r.right - pad, r.bottom };
                DrawLine(hdc, fontHint, Mix(kDimTextColor, kFocusColor, 0.6f), row.text, rHead, DT_LEFT);
                const int lineX = rHead.left + TextWidth(hdc, fontHint, row.text) + pad;
                const int lineY = (rHead.top + rHead.bottom) / 2;
                if (lineX < rHead.right) {
                    RECT rLine = { lineX, lineY, rHead.right, lineY + std::max(1, unit / 40) };
                    FillSolid(hdc, rLine, Mix(kPanelColor, kDimTextColor, 0.3f));
                }
            }
            continue;
        }

        if (row.kind == RowKind::BarItemRow && row.data2 == 1) {
            // A bar separator in the buttons list: drawn as what it is, a line, between the name
            // ("Separator", dim) and its value ("< remove >", or "Moving ...")
            const COLORREF dim = focused ? kFocusDimTextColor : kDimTextColor;
            const int textLeft = r.left + pad;
            RECT rName = { textLeft, r.top, r.right - pad, r.bottom };
            DrawLine(hdc, fontRow, dim, row.text, rName, DT_LEFT);
            const int nameW = TextWidth(hdc, fontRow, row.text);
            const int valueW = TextWidth(hdc, fontRow, row.value);
            RECT rValue = { r.right - pad - valueW, r.top, r.right - pad, r.bottom };
            DrawLine(hdc, fontRow, dim, row.value, rValue, DT_RIGHT);
            const int x0 = textLeft + nameW + pad, x1 = rValue.left - pad;
            if (x1 > x0) {
                const int lineH = std::max(2, unit / 24);
                const int cy = (r.top + r.bottom) / 2;
                RECT rLine = { x0, cy - lineH / 2, x1, cy - lineH / 2 + lineH };
                FillSolid(hdc, rLine, focused ? Mix(kFocusColor, kTextColor, 0.6f) : Mix(kPanelColor, kTextColor, 0.35f));
            }
            continue;
        }

        if (row.kind == RowKind::MadVrModeTags) {
            // One line of display-mode tags; the picked one in the accent colour, with an x when
            // OK would remove it
            int x = r.left + pad / 2;
            const int tagH = rowH * 3 / 4;
            const int top = r.top + (rowH - tagH) / 2;
            for (int t = row.data; t < row.data + row.data2 && t < static_cast<int>(m_madVrModes.size()); t++) {
                const auto& m = m_madVrModes[t];
                const CStringW s = MadVrModeName(m[0], m[1], m[2], m[3] != 0);
                const bool picked = focused && t == m_nModeTag;
                const bool armed = picked && m_bModeTagArmed;
                const int textW = TextWidth(hdc, fontRow, s);
                const int xW = armed ? TextWidth(hdc, fontHint, kIconRemove) + pad / 2 : 0;
                RECT rc = { x, top, x + textW + pad * 2 + xW, top + tagH };
                if (rc.right > r.right) {
                    break;
                }
                const COLORREF bg = armed ? RGB(176, 58, 58) : picked ? kFocusColor : Mix(kPanelColor, kTextColor, 0.12f);
                HBRUSH br = CreateSolidBrush(bg);
                HGDIOBJ oldBr = SelectObject(hdc, br);
                HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
                RoundRect(hdc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, tagH * 7 / 10, tagH * 7 / 10);
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBr);
                DeleteObject(br);
                RECT rText = { rc.left + pad, rc.top, rc.left + pad + textW, rc.bottom };
                DrawLine(hdc, fontRow, picked ? kTextColor : kDimTextColor, s, rText, DT_LEFT);
                if (armed) {
                    RECT rX = { rText.right + pad / 2, rc.top, rc.right - pad, rc.bottom };
                    DrawLine(hdc, fontHint, kTextColor, kIconRemove, rX, DT_CENTER);
                }
                m_hitTags.push_back({ RECT { panelX + rc.left, panelY + rc.top, panelX + rc.right, panelY + rc.bottom }, t });
                x = rc.right + pad / 2;
            }
            continue;
        }

        if (m_page == Page::MadVR && row.kind == RowKind::MadVrSetting && row.toggle < 0) { // plain on / off: the switch below
            // Label on the left; on the right the row's parts: on / off, value, and while editing
            // the mini-view icon. The picked part (editing) sits on a see-through rounded chip with
            // small down / up triangles either side. Rows (or values) that only apply while some
            // option is on are shown faded back while it's off.
            const bool editing = focused && m_nMadVrPart >= 0;
            const auto parts = MadVrParts(i);
            const COLORREF rowBg = focused ? kFocusColor : kPanelColor;
            auto fadeBack = [&](COLORREF c, bool faded) { return faded ? Mix(c, rowBg, 0.75f) : c; };
            int x = r.right - pad;
            const int chipPad = pad / 2;
            const int triGap = pad / 3;
            for (int k = static_cast<int>(parts.size()) - 1; k >= 0; k--) {
                const int part = parts[k];
                if (part == 2 && !editing) {
                    continue; // the mini-view icon only while editing
                }
                const bool icon = (part == 2);
                HFONT f = icon ? fontMdl : fontRow;
                const CStringW s = icon ? CStringW(m_bMadVrCompact ? kIconExpand : kIconMini)
                                        : part == 0 ? row.prefix : row.value;
                const bool picked = editing && part == m_nMadVrPart;
                const bool arrows = picked && !icon; // Up / Down change it
                const bool faded = row.faded || (part == 1 && row.data2 >= 0 && row.prefix != L"On");
                const int triW = arrows ? TextWidth(hdc, fontHint, kTriUp) : 0;
                const int textW = TextWidth(hdc, f, s);
                const int w = textW + (arrows ? 2 * (triW + triGap) : 0);
                RECT rc = { x - w - chipPad * 2, r.top + rowH / 8, x, r.bottom - rowH / 8 };
                if (picked) {
                    // 70 % see-through panel colour over the highlight, rounded
                    HBRUSH br = CreateSolidBrush(Mix(rowBg, kPanelColor, 0.3f));
                    HGDIOBJ oldBr = SelectObject(hdc, br);
                    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
                    const int rad = (rc.bottom - rc.top) * 7 / 10;
                    RoundRect(hdc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, rad, rad);
                    SelectObject(hdc, oldPen);
                    SelectObject(hdc, oldBr);
                    DeleteObject(br);
                }
                const COLORREF c = fadeBack(picked ? kTextColor : focused ? kFocusDimTextColor : kDimTextColor, faded);
                if (arrows) {
                    RECT rDown = { rc.left + chipPad, rc.top, rc.left + chipPad + triW, rc.bottom };
                    RECT rUp = { rc.right - chipPad - triW, rc.top, rc.right - chipPad, rc.bottom };
                    RECT rText = { rDown.right + triGap, rc.top, rUp.left - triGap, rc.bottom };
                    DrawLine(hdc, fontHint, c, kTriDown, rDown, DT_CENTER);
                    DrawLine(hdc, f, c, s, rText, DT_CENTER);
                    DrawLine(hdc, fontHint, c, kTriUp, rUp, DT_CENTER);
                    // mouse: the triangles change the value (a bit wider than drawn), tested first
                    m_hitMadVrParts.insert(m_hitMadVrParts.begin(),
                                           { { RECT { panelX + rc.left, panelY + rc.top, panelX + rText.left, panelY + rc.bottom }, kHitTriDown },
                                             { RECT { panelX + rText.right, panelY + rc.top, panelX + rc.right, panelY + rc.bottom }, kHitTriUp } });
                } else {
                    DrawLine(hdc, f, c, s, rc, DT_CENTER);
                }
                if (editing) {
                    m_hitMadVrParts.push_back({ RECT { panelX + rc.left, panelY + rc.top, panelX + rc.right, panelY + rc.bottom }, part });
                }
                x = rc.left - pad;
            }
            RECT rLabel = { r.left + pad, r.top, std::max<int>(r.left + pad, x - pad), r.bottom };
            DrawLine(hdc, fontRow, fadeBack(kTextColor, row.faded), row.text, rLabel, DT_LEFT);
            continue;
        }

        RECT rCheck = { r.left + pad / 2, r.top, r.left + pad / 2 + checkW, r.bottom };
        if (row.checked) {
            DrawLine(hdc, fontRow, kTextColor, kCheck, rCheck, DT_CENTER);
        }

        int textLeft = r.left + pad + checkW;
        COLORREF textColor = (row.kind == RowKind::Info && !focused) ? kDimTextColor : kTextColor;
        if (row.faded) {
            textColor = Mix(textColor, focused ? kFocusColor : kPanelColor, 0.6f); // only matters while another option is on
        }
        if (!row.icon.IsEmpty()) {
            // Settings page: an icon in front of the text
            const int iconW = unit * 9 / 10;
            RECT rIcon = { textLeft, r.top, textLeft + iconW, r.bottom };
            DrawLine(hdc, fontMdl, textColor, row.icon, rIcon, DT_LEFT);
            textLeft += iconW;
        }
        if (row.kind == RowKind::KeyRow && i == m_nKeyEditRow) {
            // Button / key mapping: the row's choices as pills at the right, the picked one solid
            const int action = kKeyActions[std::clamp(row.data, 0, static_cast<int>(_countof(kKeyActions)) - 1)].id;
            const std::vector<LPCWSTR> names = action < 0 ? std::vector<LPCWSTR> { L"Set", L"Clear" }
                                                          : std::vector<LPCWSTR> { L"Set", L"Set hold", L"Clear" };
            const int pillH = fontH * 13 / 10, gapX = pad / 2, padX = pad;
            const float cy = (r.top + r.bottom) / 2.0f;
            int x = r.right - pad;
            int pillLeft = x;
            for (int k = static_cast<int>(names.size()) - 1; k >= 0; k--) {
                const int w = TextWidth(hdc, fontRow, names[k]) + padX * 2;
                const RECT rp = { x - w, static_cast<LONG>(cy - pillH / 2.0f), x, static_cast<LONG>(cy + pillH / 2.0f) };
                const bool picked = (k == m_nKeyEditPart);
                const COLORREF fill = picked ? kTextColor : Mix(kFocusColor, kTextColor, 0.2f);
                canvas.FillRoundPolyAA({ { static_cast<float>(rp.left), static_cast<float>(rp.top) }, { static_cast<float>(rp.right), static_cast<float>(rp.top) },
                                         { static_cast<float>(rp.right), static_cast<float>(rp.bottom) }, { static_cast<float>(rp.left), static_cast<float>(rp.bottom) } },
                                       pillH / 2.0f, fill);
                DrawLine(hdc, fontRow, picked ? kFocusColor : kTextColor, names[k], rp, DT_CENTER);
                pillLeft = rp.left;
                x = rp.left - gapX;
            }
            RECT rLabel = { textLeft, r.top, pillLeft - pad, r.bottom };
            DrawLine(hdc, fontRow, textColor, row.text, rLabel, DT_LEFT);
            continue;
        }
        if (row.toggle >= 0 && row.detail.IsEmpty()) {
            // On / off as a switch at the right: a pill with a knob (right + filled = on). OK, Left or
            // Right flips it; the mouse anywhere on the row too.
            const float h = fontH * 0.95f, w = h * 1.8f;
            // its right end lines up with the other values' text: TV options keep room for their right
            // arrow; madVR rows have no arrows, their values end at the row's padding
            const int arrowRoom = (m_page == Page::MadVR) ? 0 : TextWidth(hdc, fontRow, kTriRight) + TextWidth(hdc, fontRow, L"  ");
            const float cy = (r.top + r.bottom) / 2.0f, xr = static_cast<float>(r.right - pad - arrowRoom), xl = xr - w;
            const bool on = row.toggle == 1;
            const COLORREF rowBg = focused ? kFocusColor : kPanelColor;
            // Just flipped: the knob slides over and the colours blend (Animations speed)
            float amount = on ? 1.0f : 0.0f; // 0 = off look, 1 = on look
            if (m_nToggleAnimKey == ToggleKey(row) && CTvMode::AnimScale() > 0.0f) {
                const float t = std::clamp(static_cast<float>(GetTickCount64() - m_nToggleAnimStart) / (160.0f * CTvMode::AnimScale()), 0.0f, 1.0f);
                const float e = 1.0f - (1.0f - t) * (1.0f - t); // ease out
                amount = on ? e : 1.0f - e;
                if (t < 1.0f) {
                    m_bDirty = true; // next frame
                } else {
                    m_nToggleAnimKey = -1;
                }
            }
            const COLORREF trackOn = focused ? kTextColor : kFocusColor, trackOff = Mix(rowBg, kTextColor, focused ? 0.3f : 0.22f);
            const COLORREF knobOn = focused ? kFocusColor : kTextColor, knobOff = Mix(kTextColor, rowBg, 0.1f);
            COLORREF track = Mix(trackOff, trackOn, amount);
            COLORREF knob = Mix(knobOff, knobOn, amount);
            if (row.faded) {
                track = Mix(track, rowBg, 0.6f);
                knob = Mix(knob, rowBg, 0.6f);
            }
            canvas.FillCircleAA(xl + h / 2, cy, h / 2, track);
            canvas.FillCircleAA(xr - h / 2, cy, h / 2, track);
            RECT rMid = { static_cast<LONG>(xl + h / 2), static_cast<LONG>(cy - h / 2 + 0.5f), static_cast<LONG>(xr - h / 2),
                          static_cast<LONG>(cy + h / 2 + 0.5f) };
            FillSolid(hdc, rMid, track);
            canvas.FillCircleAA(xl + h / 2 + (w - h) * amount, cy, h / 2 - std::max(2.0f, h * 0.12f), knob);
            RECT rLabel = { textLeft, r.top, static_cast<LONG>(xl) - pad * 2, r.bottom };
            DrawLine(hdc, fontRow, textColor, row.text, rLabel, DT_LEFT);
            continue;
        }
        if (row.detail.IsEmpty()) {
            RECT rText = { textLeft, r.top, r.right - pad, r.bottom };
            CStringW text = row.text;
            if (row.kind == RowKind::OpenAudio || row.kind == RowKind::OpenSubtitles || row.kind == RowKind::OpenOptions ||
                row.kind == RowKind::OpenPicker || row.kind == RowKind::OpenSubStyle || row.kind == RowKind::OpenSection ||
                row.kind == RowKind::MadVrOpenSub || row.kind == RowKind::OpenLav || row.kind == RowKind::OpenMadVr ||
                row.kind == RowKind::OpenVideo || row.kind == RowKind::OpenAdvanced) {
                text += L"  ";
                text += kChevron;
            }
            if (!row.value.IsEmpty()) {
                // Value right-aligned, label on the left. The value is placed first and the label
                // gets all the remaining room, so "..." only appears when something really doesn't fit.
                CStringW value = row.value;
                if (row.kind == RowKind::OpenAudio || row.kind == RowKind::OpenSubtitles) {
                    // Left/Right change this value in place (arrows shown on the focused row only)
                    value = CStringW(kTriLeft) + L"  " + value + L"  " + kTriRight;
                }
                const int avail = rText.right - textLeft;
                const int prefixW = row.prefix.IsEmpty() ? 0 : TextWidth(hdc, fontRow, row.prefix) + pad * 2;
                const int valueW = TextWidth(hdc, fontRow, value) + pad / 2 + prefixW;
                const int labelW = TextWidth(hdc, fontRow, text) + pad;
                // Both fit: the value keeps its width. Otherwise (e.g. a long track name as the value)
                // the label keeps up to 55 % and the value is shortened instead.
                const int valueLeft = (labelW + pad * 2 + valueW <= avail)
                                      ? static_cast<int>(rText.right) - valueW
                                      : textLeft + std::min(labelW, avail * 55 / 100) + pad * 2;
                RECT rLabel = { textLeft, r.top, valueLeft - pad * 2, r.bottom };
                RECT rValue = { valueLeft, r.top, rText.right, r.bottom };
                m_hitArrowW = TextWidth(hdc, fontRow, CStringW(kTriLeft) + L"  ") + pad;
                DrawLine(hdc, fontRow, textColor, text, rLabel, DT_LEFT);
                // Faded (needs another setting on): the value greyed like the label
                const COLORREF valueColor = row.faded ? Mix(kDimTextColor, kPanelColor, 0.6f)
                                            : focused ? kFocusDimTextColor : kDimTextColor;
                const int len = value.GetLength();
                if (len >= 2 && value[0] == kTriLeft[0] && value[len - 1] == kTriRight[0]) {
                    // "<  value  >": the arrows are drawn on their own so they can light up under
                    // the mouse (fading to the accent colour, like the info panels' arrows; on the
                    // highlighted row, where the accent is the background, to the panel colour).
                    const COLORREF hoverColor = focused ? kPanelColor : kFocusColor;
                    auto amountFor = [&](int id) {
                        return id == m_nRowArrowHover ? arrowT : (id == arrowPrev ? 1.0f - arrowT : 0.0f);
                    };
                    const CStringW mid = value.Mid(1, len - 2); // keeps the spaces next to the arrows
                    const int triW = TextWidth(hdc, fontRow, kTriRight);
                    RECT rRight = { rValue.right - triW, r.top, rValue.right, r.bottom };
                    RECT rMid = { rValue.left + triW, r.top, rRight.left, r.bottom };
                    const int midW = std::min<int>(TextWidth(hdc, fontRow, mid), rMid.right - rMid.left);
                    RECT rLeft = { rMid.right - midW - triW, r.top, rMid.right - midW, r.bottom };
                    // Mouse: the arrows at both ends of the value are click areas (not while faded)
                    if (!row.faded) {
                        m_hitValueLeft[i] = panelX + rLeft.left;
                        m_hitValueRightRow[i] = panelX + rRight.right;
                    }
                    if (!row.prefix.IsEmpty()) { // e.g. "On" of an on/off with its number
                        RECT rPrefix = { rLabel.right, r.top, rLeft.left - pad, r.bottom };
                        DrawLine(hdc, fontRow, valueColor, row.prefix, rPrefix, DT_RIGHT);
                    }
                    // The arrows only on the focused row (it crossfades with the highlight); elsewhere,
                    // and while faded, they keep their room so values never shift
                    const bool arrows = focused && !row.faded;
                    if (arrows) {
                        DrawLine(hdc, fontRow, Mix(valueColor, hoverColor, amountFor((i + 1) * 2)), kTriLeft, rLeft, DT_LEFT);
                    }
                    DrawLine(hdc, fontRow, valueColor, mid, rMid, DT_RIGHT);
                    if (arrows) {
                        DrawLine(hdc, fontRow, Mix(valueColor, hoverColor, amountFor((i + 1) * 2 + 1)), kTriRight, rRight, DT_LEFT);
                    }
                } else {
                    DrawLine(hdc, fontRow, valueColor, value, rValue, DT_RIGHT);
                }
            } else {
                DrawLine(hdc, fontRow, textColor, text, rText, DT_LEFT);
            }
        } else {
            // Two lines: track title, then the original stream name.
            const int mid = r.top + rowH * 56 / 100;
            RECT rText = { textLeft, r.top + pad / 4, r.right - pad, mid };
            RECT rDetail = { textLeft, mid, r.right - pad, r.bottom - pad / 4 };
            DrawLine(hdc, fontRow, textColor, row.text, rText, DT_LEFT);
            DrawLine(hdc, fontDetail, focused ? kFocusDimTextColor : kDimTextColor, row.detail, rDetail, DT_LEFT);
        }
    }

    // Gliding: rows half out of the list area are covered again by the title and footer bands
    if (offPx > 0) {
        RECT rTop = { 0, 0, panelW, listTop };
        RECT rBottom = { 0, listBottom, panelW, panelH };
        FillSolid(hdc, rTop, kPanelColor);
        FillSolid(hdc, rBottom, kPanelColor);
        DrawLine(hdc, fontTitle, kDimTextColor, title, rTitle, DT_LEFT);
        if (m_nScroll > 0) {
            DrawLine(hdc, fontHint, kDimTextColor, kTriUp, rTitle, DT_RIGHT);
        }
    }

    // Pop-up scrollbar for long lists (inside the panel's right padding)
    if (lineCount > visible && !compact) {
        const int w = std::max(4, pad / 2);
        const int x = panelW - pad / 2 - w / 2 - 1;
        const int top = pad + titleH;
        const int h = visible * rowH;
        const int thumbH = std::max(w * 3, h * visible / lineCount);
        // Mouse control (output coordinates), wider than the bar so it's easy to grab
        m_hitScrollTrack = { panelX + x - pad, panelY + top, panelX + panelW, panelY + top + h };
        m_hitScrollTop = panelY + top;
        m_hitScrollH = h;
        m_hitScrollThumbH = thumbH;
        m_nScrollMax = lineCount - visible;
        const float amount = ScrollbarAmount();
        if (amount > 0.0f) {
            const int thumbY = top + static_cast<int>((h - thumbH) * m_fScrollShown / std::max(1, lineCount - visible));
            RECT rTrack = { x, top, x + w, top + h };
            RECT rThumb = { x, thumbY, x + w, thumbY + thumbH };
            FillSolid(hdc, rTrack, Mix(kPanelColor, kTextColor, 0.12f * amount));
            FillSolid(hdc, rThumb, Mix(kPanelColor, kFocusColor, amount));
        }
    }

    // Key hint, with a marker when rows are hidden below
    CStringW hint;
    const bool madVrRow = m_page == Page::MadVR && m_nFocus < count && m_rows[m_nFocus].kind == RowKind::MadVrSetting;
    const bool tagRow = m_page == Page::MadVR && m_nFocus < count && m_rows[m_nFocus].kind == RowKind::MadVrModeTags;
    if (tagRow && m_bModeTagArmed) {
        hint = L"OK remove this mode   Back keep it";
    } else if (tagRow) {
        hint.Format(L"%s%s pick   %s move   OK remove...   Back return", kTriLeft, kTriRight, kArrowUpDown);
    } else if (madVrRow && m_nMadVrPart == 2) {
        hint.Format(L"%s mini view   %s full list   %s%s switch   Back done", kTriUp, kTriDown, kTriLeft, kTriRight);
    } else if (madVrRow && m_nMadVrPart >= 0) {
        hint.Format(L"%s change   %s%s switch   OK done", kArrowUpDown, kTriLeft, kTriRight);
    } else if (compact) {
        hint.Format(L"%s next setting   %sOK edit   Back full list", kArrowUpDown,
                    m_bTwoColumns ? (CStringW(kTriLeft) + kTriRight + L" column   ").GetString() : L"");
    } else if (m_page == Page::MadVR && m_nFocus < count) {
        hint.Format(L"%s move   %sOK %s   Back return", kArrowUpDown,
                    m_bTwoColumns ? (CStringW(kTriLeft) + kTriRight + L" column   ").GetString() : L"",
                    madVrRow ? (m_rows[m_nFocus].toggle >= 0 ? L"on / off" : L"edit") : L"select");
    } else if (m_page == Page::Main) {
        hint.Format(L"%s move   %s%s change   OK open   Back return", kArrowUpDown, kTriLeft, kTriRight);
    } else if (m_nListenKey >= 0) {
        hint = L"Press the key to use   Back cancel";
    } else if (m_nKeyEditRow >= 0) {
        hint.Format(L"%s%s choose   OK do it   Back cancel", kTriLeft, kTriRight);
    } else if (m_nFocus < count && m_rows[m_nFocus].kind == RowKind::KeyRow) {
        hint.Format(L"%s move   OK choices   Back return", kArrowUpDown);
    } else if (m_nMovingItem >= 0) {
        hint.Format(L"%s move button   OK drop", kArrowUpDown);
    } else if (m_nFocus < count && m_rows[m_nFocus].kind == RowKind::BarItemRow) {
        hint.Format(L"%s move   %s%s show / hide   OK pick up to reorder   Back return", kArrowUpDown, kTriLeft, kTriRight);
    } else if (m_nFocus < count && m_rows[m_nFocus].kind == RowKind::MadVrSetting && m_rows[m_nFocus].data2 >= 0) {
        hint.Format(L"%s move   OK on / off   %s%s value   Back return", kArrowUpDown, kTriLeft, kTriRight);
    } else if (m_page == Page::OptionsSection || m_page == Page::SubStyle || (m_page == Page::MadVR && m_nMadVrSub != MvRoot) ||
               (m_page == Page::Lav && m_nFocus < count && m_rows[m_nFocus].kind == RowKind::LavSetting)) {
        hint.Format(L"%s move   %s%s change   Back return", kArrowUpDown, kTriLeft, kTriRight);
    } else {
        hint.Format(L"%s move   OK select   Back return", kArrowUpDown);
    }
    if (helpH > 0 && m_nFocus >= 0 && m_nFocus < count) {
        // What the focused option does (TV options pages)
        const Row& fr = m_rows[m_nFocus];
        CStringW help = fr.help;
        if (help.IsEmpty()) {
            switch (fr.kind) {
                case RowKind::OpenPicker: help = fr.data == 1 ? L"Pick your own background colour" : L"Pick your own accent colour"; break;
                case RowKind::BarItemRow: help = L"Left / Right show or hide it; OK picks it up to move it"; break;
                case RowKind::AddSeparator: help = L"Adds a divider line to the bar; place it with Up / Down"; break;
                case RowKind::KeyRow: help = L"OK, then press the key to use; Delete clears it"; break;
                case RowKind::ResetSubStyle: help = L"Back to this build's subtitle look"; break;
                default: break;
            }
        }
        HFONT fontHelp = MakeFont(fontH * 72 / 100, FW_NORMAL);
        RECT rHelp = { pad * 2, pad + titleH + visible * rowH, panelW - pad * 2, pad + titleH + visible * rowH + helpH };
        DrawLine(hdc, fontHelp, Mix(kTextColor, kPanelColor, 0.25f), help, rHelp, DT_LEFT);
        ReleaseFont(fontHelp);
    }
    RECT rHint = { pad * 2, pad + titleH + visible * rowH + helpH, panelW - pad * 2, panelH - pad };
    if (showHints) {
        DrawLine(hdc, fontHint, kDimTextColor, hint, rHint, DT_LEFT);
        if (m_nScroll + visible < lineCount && !compact) {
            DrawLine(hdc, fontHint, kDimTextColor, kTriDown, rHint, DT_RIGHT);
        }
    }

    SelectObject(hdc, oldFont);
    ReleaseFont(fontTitle);
    ReleaseFont(fontRow);
    ReleaseFont(fontDetail);
    ReleaseFont(fontHint);
    ReleaseFont(fontMdl);

    // The whole panel (text included) shares one alpha, so fading just scales it. Show / hide:
    // rises a little into place while it fades in (sinks while fading out).
    PerfMark("draw");
    Present(ElemMenu, SlotPanel, panelX, panelY, pad, kPanelAlpha, 1, outH * 2 / 100); // with the crossfade's rows blended
}

void CTvMenu::RenderBar(bool preview)
{
    // preview: the bar shown under the TV options list while "Control bar size" is focused.
    // Drawn into its own OSD element, without focus, and without touching the mouse hit
    // areas or focus-fade state that belong to the visible page.
    CMainFrame* pMF = m_pMainFrame;
    const RECT outRect = GetOutputRect(m_pOsd);
    const int outW = outRect.right - outRect.left;
    const int outH = outRect.bottom - outRect.top;
    if (!preview) {
        m_outRect = outRect;
        m_lastOutput = { outW, outH };
        m_nLastBarRender = GetTickCount64();
    }

    const REFERENCE_TIME pos = pMF->GetPos();
    const REFERENCE_TIME dur = pMF->GetDur();
    const bool playing = IsPlaying();
    // The preview under "Button name under the selected button" and the icon options shows a selected
    // button, Play and Settings in turn (both are always on the bar), with its name if that's on
    const bool demoFocus = preview && PreviewShowsSelection();
    const int barRow = preview ? (demoFocus ? 1 : -1) : m_nBarRow; // -1: nothing focused
    const BarButton focusedButton = demoFocus ? ((GetTickCount64() / 1600) % 2 ? BarButton::Settings : BarButton::PlayPause)
                                              : m_barButton;

    // Layout: bubble space, seek row, button row. Everything scales with the output height,
    // times the "Control bar size" option (100 % = 70 % of the original size, the chosen default look).
    const int unit = std::max(14, static_cast<int>(outH / 22.0f * 0.7f * Scale(TvOpt::BarSizePct)));
    const int pad = unit / 2;
    // Chapters: marks on the seek line, and the chapter name in a small line under it (the bar
    // only grows by that line when the file has chapters).
    const std::vector<Chapter> chapters = GetChapters();
    const bool hasChapters = !chapters.empty() && dur > 0;
    const int chapterRowH = hasChapters ? unit * 8 / 10 : 0; // starts below the (focused) playhead dot
    // Bottom padding for the selected button's name (TV option), shown under the button
    const int labelRowH = CTvMode::IsOn(TvOpt::ButtonLabels) ? unit * 55 / 100 : 0;
    const int barH = unit * 4 + chapterRowH + labelRowH; // was 5: less empty space above the seek row
    const int bubbleCy = unit * 6 / 10;        // time bubble, only shown while seeking / hovering
    const int seekCy = unit * 3 / 2;
    const int buttonCy = barH - labelRowH - unit * 6 / 5;
    const int margin = std::max(pad * 2, outW / 40);
    const int barY = outH - barH;
    std::vector<ButtonHit> hitButtons;
    if (!preview) {
        m_elemRect = { 0, barY, outW, outH };
        m_hitRowsVisible = 0;
    }

    PerfMark("layout");
    CDibCanvas& canvas = Content(preview ? SlotPreview : SlotBar, outW, barH);
    if (!canvas.IsValid()) {
        return;
    }
    HDC hdc = canvas.DC();
    SetBkMode(hdc, TRANSPARENT);
    RECT all = { 0, 0, outW, barH };
    FillSolid(hdc, all, kPanelColor);

    HFONT fontTime = MakeFont(unit * 6 / 10, FW_NORMAL);
    HFONT fontIcon = MakeFont(unit * 9 / 10, FW_NORMAL, L"Segoe MDL2 Assets");
    HFONT fontBubble = MakeFont(unit * 6 / 10, FW_SEMIBOLD);
    HGDIOBJ oldFont = SelectObject(hdc, fontTime);

    // --- Seek row: current time, track, duration
    const bool seekFocused = (barRow == 0);
    const CStringW posText = FormatTime(pos);
    // Right of the seek line: duration, or the time left ("-0:42:10"), per the TV option
    const bool remaining = CTvMode::Get(TvOpt::TimeDisplay) == 1;
    const CStringW durText = dur <= 0 ? CStringW(L"--:--") : remaining ? L"-" + FormatTime(dur - pos) : FormatTime(dur);
    // Both labels get a fixed width: the longest time this file can show, with every digit as the
    // widest digit of the font. Otherwise the seek line's ends moved whenever the text got wider or
    // narrower (e.g. 9:59 -> 10:00), which made dragging feel jumpy.
    int digitW = 0;
    for (wchar_t c = L'0'; c <= L'9'; c++) {
        const wchar_t s[2] = { c, 0 };
        digitW = std::max(digitW, TextWidth(hdc, fontTime, s));
    }
    auto widest = [&](const CStringW& sample) {
        int w = 0;
        for (int i = 0; i < sample.GetLength(); i++) {
            const wchar_t c = sample[i];
            const wchar_t s[2] = { c, 0 };
            w += (c >= L'0' && c <= L'9') ? digitW : TextWidth(hdc, fontTime, s);
        }
        return w;
    };
    const CStringW longest = dur > 0 ? FormatTime(dur) : CStringW(L"00:00");
    const int posW = std::max(widest(longest), TextWidth(hdc, fontTime, posText));
    const int durW = std::max(widest(remaining && dur > 0 ? L"-" + longest : longest), TextWidth(hdc, fontTime, durText));
    RECT rPos = { margin, seekCy - unit / 2, margin + posW, seekCy + unit / 2 };
    RECT rDur = { outW - margin - durW, seekCy - unit / 2, outW - margin, seekCy + unit / 2 };
    DrawLine(hdc, fontTime, kTextColor, posText, rPos, DT_LEFT);
    DrawLine(hdc, fontTime, kTextColor, durText, rDur, DT_RIGHT);

    const int x0 = rPos.right + unit;
    const int x1 = rDur.left - unit;
    const int thick = std::max(2, seekFocused ? unit / 6 : unit / 10);
    if (!preview) {
        m_hitSeekX0 = x0;
        m_hitSeekX1 = x1;
        m_hitSeekCy = barY + seekCy;
        m_hitSeekHalfH = unit / 2;
    }
    auto xFor = [&](REFERENCE_TIME rt) {
        if (dur <= 0 || x1 <= x0) {
            return x0;
        }
        return x0 + static_cast<int>((x1 - x0) * (static_cast<double>(std::clamp<REFERENCE_TIME>(rt, 0, dur)) / dur));
    };
    RECT rTrack = { x0, seekCy - thick / 2, x1, seekCy - thick / 2 + thick };
    FillSolid(hdc, rTrack, kTrackColor);
    // The playhead dot (and the filled line) glide to the playing position instead of jumping,
    // e.g. when a seek lands: eased by the time since the last drawing, snapped when within half a
    // pixel. While it's still moving, OnTick draws the bar every frame (m_bDotMoving).
    REFERENCE_TIME dotPos = pos;
    if (!preview) {
        const ULONGLONG nowDot = GetTickCount64();
        const float anim = CTvMode::AnimScale();
        const double pxPerRt = (dur > 0 && x1 > x0) ? static_cast<double>(x1 - x0) / dur : 0.0;
        if (m_rtDot < 0 || anim <= 0.0f || !CTvMode::IsOn(TvOpt::SmoothPlayhead) || pxPerRt <= 0.0 || dur != m_rtDotDur || nowDot - m_nDotTick > 1000) {
            m_rtDot = pos; // first drawing, no animations, other file, or the bar was away
        } else {
            const double dtMs = static_cast<double>(std::min<ULONGLONG>(nowDot - m_nDotTick, 50));
            const double k = 1.0 - std::exp(-dtMs / (90.0 * anim));
            m_rtDot += static_cast<REFERENCE_TIME>((pos - m_rtDot) * k);
            if (std::abs(static_cast<double>(pos - m_rtDot)) * pxPerRt < 0.5) {
                m_rtDot = pos;
            }
        }
        m_nDotTick = nowDot;
        m_rtDotDur = dur;
        m_bDotMoving = (m_rtDot != pos);
        dotPos = m_rtDot;
    }
    const int xPos = xFor(dotPos);
    RECT rFill = { x0, rTrack.top, xPos, rTrack.bottom };
    FillSolid(hdc, rFill, kFocusColor);
    if (hasChapters) {
        // Chapter starts: small gaps cut into the line, so it reads as segments
        const int gap = std::max(2, unit / 12);
        for (const Chapter& c : chapters) {
            if (c.rt > 0 && c.rt < dur) {
                const int xc = xFor(c.rt);
                RECT rGap = { xc - gap / 2, rTrack.top - 1, xc - gap / 2 + gap, rTrack.bottom + 1 };
                FillSolid(hdc, rGap, kPanelColor);
            }
        }
    }
    // the dot at its exact (sub-pixel) place, so a slow glide doesn't step pixel by pixel
    const float xDot = (dur > 0 && x1 > x0)
                       ? x0 + static_cast<float>((x1 - x0) * (static_cast<double>(std::clamp<REFERENCE_TIME>(dotPos, 0, dur)) / dur))
                       : static_cast<float>(xPos);
    canvas.FillCircleAA(xDot, static_cast<float>(seekCy),
                        seekFocused ? unit * 0.4f : unit * 0.25f, seekFocused ? kTextColor : kFocusColor);

    // Seek preview (marker + "target (+offset)"), or the time under the mouse when hovering.
    // The time is plain text above the track, no box behind it.
    const REFERENCE_TIME bubbleTime = preview ? -1 : (m_rtPreview >= 0) ? m_rtPreview : m_rtHover;
    if (bubbleTime >= 0) {
        const int xb = xFor(bubbleTime);
        {
            // Seek target / mouse hover: a short white pill across the track (60 % of the old
            // line's height, rounded ends)
            const int pw = std::max(4, unit / 7) / 2 * 2; // even width, so it centres on xb
            const int hh = unit * 3 / 10;                  // half height
            RECT rMid = { xb - pw / 2, seekCy - hh + pw / 2, xb + pw / 2, seekCy + hh - pw / 2 };
            FillSolid(hdc, rMid, kTextColor);
            canvas.FillCircleAA(static_cast<float>(xb), static_cast<float>(rMid.top), pw / 2.0f, kTextColor);
            canvas.FillCircleAA(static_cast<float>(xb), static_cast<float>(rMid.bottom), pw / 2.0f, kTextColor);
        }

        CStringW bubble = FormatTime(bubbleTime);
        if (m_rtPreview >= 0) {
            const REFERENCE_TIME delta = m_rtPreview - pos;
            bubble.AppendFormat(L"  (%s%s)", delta < 0 ? L"-" : L"+", FormatTime(delta < 0 ? -delta : delta).GetString());
        }
        const int bw = TextWidth(hdc, fontBubble, bubble) + unit;
        int bx = std::clamp(xb - bw / 2, margin, outW - margin - bw);
        RECT rBubble = { bx, bubbleCy - unit * 4 / 10, bx + bw, bubbleCy + unit * 4 / 10 };
        DrawLine(hdc, fontBubble, kTextColor, bubble, rBubble, DT_CENTER);
    }

    // Chapter name under the line: the chapter at the seek / hover target (bright), otherwise
    // the current chapter (dim), centred under that spot.
    if (hasChapters) {
        const REFERENCE_TIME at = bubbleTime >= 0 ? bubbleTime : pos;
        const Chapter* cur = nullptr;
        for (const Chapter& c : chapters) {
            if (c.rt <= at) {
                cur = &c;
            }
        }
        if (cur && !cur->name.IsEmpty()) {
            // A little smaller than the times, starting below the focused playhead dot (radius 0.4)
            HFONT fontChapter = MakeFont(unit * 504 / 1000, FW_NORMAL); // 5 % up from 0.48
            const int nameW = std::min(TextWidth(hdc, fontChapter, cur->name) + unit, (x1 - x0));
            const int xc = xFor(at);
            const int nx = std::clamp(xc - nameW / 2, x0, std::max(x0, x1 - nameW));
            RECT rName = { nx, seekCy + unit * 45 / 100, nx + nameW, seekCy + unit * 45 / 100 + chapterRowH };
            DrawLine(hdc, fontChapter, bubbleTime >= 0 ? kTextColor : kDimTextColor, cur->name, rName, DT_CENTER);
            ReleaseFont(fontChapter);
        }
    }

    // --- Button row
    struct Btn { BarButton id; LPCWSTR icon; };
    // The font has "10 s back" / "30 s forward" icons; other skip values get a plain
    // arrow with the seconds drawn inside.
    const int skipBack = CTvMode::Get(TvOpt::SkipBackSec);
    const int skipFwd = CTvMode::Get(TvOpt::SkipFwdSec);
    // Buttons and their order come from the "Bar buttons" submenu.
    const bool muted = pMF->IsMuted();
    const int volume = pMF->GetVolume();
    auto iconFor = [&](BarButton b) -> LPCWSTR {
        switch (b) {
            case BarButton::Prev: return kIconPrev;
            case BarButton::Back10: return kIconSkipRing; // drawn mirrored, seconds inside
            case BarButton::PlayPause: return playing ? kIconPause : kIconPlay;
            case BarButton::Fwd30: return kIconSkipRing;
            case BarButton::Next: return kIconNext;
            case BarButton::Stats: return kIconStats;
            case BarButton::VideoInfo: return kIconInfoBadge; // one Info button (video / audio inside)
            case BarButton::AudioInfo: return kIconAudioInfo;
            case BarButton::Subtitles: return kIconCC;
            case BarButton::Audio: return kIconAudio;
            case BarButton::Video: return kIconVideo;
            case BarButton::Chapters: return kIconChapters;
            case BarButton::Settings: return kIconSettings;
            case BarButton::Fullscreen: return pMF->IsFullScreenMode() ? kIconWindowed : kIconFullscreen;
            case BarButton::Power: return kIconPower;
            case BarButton::Shaders: return kIconShaders;
            case BarButton::Volume: return muted || volume <= 0 ? kIconMute : volume < 34 ? kIconVol1 : volume < 67 ? kIconVol2 : kIconVol3;
            default: return L"";
        }
    };
    std::vector<Btn> left, right;
    for (const BarItem& it : BarLayout()) {
        if (it.on && (it.id != BarButton::Chapters || hasChapters)) { // Chapters: only for files that have them
            (IsLeftButton(it.id) ? left : right).push_back({ it.id, iconFor(it.id) });
        }
    }
    HFONT fontSkipNum = MakeFont(unit * 32 / 100, FW_SEMIBOLD);
    HFONT fontBadge = MakeFont(unit * 17 / 10 * 30 / 100, FW_NORMAL, L"Segoe MDL2 Assets");

    const int cell = unit * 17 / 10;
    if (!preview) {
        m_hitButtonR = cell / 2;
    }
    // Volume: mute button, then a level slider and the number
    const int volSliderW = unit * 3;
    const int volTextW = TextWidth(hdc, fontTime, L"100") + unit / 3;
    const int volExtra = unit / 3 + volSliderW + unit / 3 + volTextW;
    auto widthOf = [&](const Btn& b) { return b.id == BarButton::Volume ? cell + volExtra : cell; };

    // Place all buttons first, so the focus circle can be drawn underneath the icons.
    std::vector<std::pair<Btn, int>> placed; // button, centre x of its icon
    int cx = margin;
    for (const auto& b : left) {
        placed.push_back({ b, cx + cell / 2 });
        cx += widthOf(b);
    }
    const int endsAtX = cx;
    // Right side: thin separators (with a little extra room) where the layout has them (TV options >
    // Control bar > Buttons; by default between the kinds of buttons). Every one is shown, also two in
    // a row; only ones at the very start or end of the right side are left out.
    {
        std::vector<Btn> kept;
        for (size_t i = 0; i < right.size(); i++) {
            if (right[i].id == BarButton::Separator && kept.empty()) {
                continue;
            }
            kept.push_back(right[i]);
        }
        while (!kept.empty() && kept.back().id == BarButton::Separator) {
            kept.pop_back();
        }
        right = std::move(kept);
    }
    const int sepGap = unit * 6 / 10;
    int rightW = 0;
    for (const auto& b : right) {
        rightW += b.id == BarButton::Separator ? sepGap : widthOf(b);
    }
    std::vector<int> separators; // x of each separator line
    cx = outW - margin - rightW;
    for (const auto& b : right) {
        if (b.id == BarButton::Separator) {
            separators.push_back(cx + sepGap / 2);
            cx += sepGap;
            continue;
        }
        placed.push_back({ b, cx + cell / 2 });
        cx += widthOf(b);
    }
    for (int sx : separators) {
        const int lineW = std::max(1, unit / 20);
        RECT rSep = { sx - lineW / 2, buttonCy - unit * 4 / 10, sx - lineW / 2 + lineW, buttonCy + unit * 4 / 10 };
        FillSolid(hdc, rSep, kTrackColor);
    }
    if (!preview) {
        m_hitVolume = {};
        m_hitVolumeX0 = m_hitVolumeX1 = 0;
    }

    if (!preview && m_nBarRow != m_nLastBarRowDrawn) {
        m_buttonFade.valid = false; // coming from the seek row: the circle appears at once
        m_nLastBarRowDrawn = m_nBarRow;
    }
    if (barRow == 1) {
        // Crossfade the focus circle from the previous button to the new one (the preview's demo
        // selection just shows the circle: the crossfade state belongs to the real bar)
        bool running = false;
        int prev = -1;
        const float t = demoFocus ? 1.0f : m_buttonFade.Progress(static_cast<int>(m_barButton), running, prev);
        if (running) {
            m_bDirty = true;
        }
        for (const auto& p : placed) {
            const int id = static_cast<int>(p.first.id);
            const float amount = (id == static_cast<int>(focusedButton)) ? t : (id == prev ? 1.0f - t : 0.0f);
            if (amount > 0.0f) {
                canvas.FillCircleAA(static_cast<float>(p.second), static_cast<float>(buttonCy), cell * 0.45f,
                                    Mix(kPanelColor, kFocusColor, amount));
            }
        }
    }

    // TV options "Control bar icon brightness / opacity": the icons of the buttons that aren't selected
    // (the selected one stays full, on its accent circle)
    const float iconBright = CTvMode::Get(TvOpt::BarIconBrightness) / 100.0f;
    const float iconOpacity = CTvMode::Get(TvOpt::BarIconOpacity) / 100.0f;
    const COLORREF iconColor = Mix(kPanelColor, RGB(static_cast<BYTE>(GetRValue(kTextColor) * iconBright), static_cast<BYTE>(GetGValue(kTextColor) * iconBright),
                                                    static_cast<BYTE>(GetBValue(kTextColor) * iconBright)),
                                   iconOpacity);
    const COLORREF fullTextColor = kTextColor;
    auto drawButton = [&](const Btn& b, int cx) {
        // everything below draws the icon in kTextColor: this local one is the icon colour for it
        const bool selected = (barRow == 1 && b.id == focusedButton);
        const COLORREF kTextColor = selected ? fullTextColor : iconColor;
        hitButtons.push_back({ b.id, { cx, barY + buttonCy } });
        RECT r = { cx - cell / 2, buttonCy - cell / 2, cx + cell / 2, buttonCy + cell / 2 };
        if (b.id == BarButton::Back10 || b.id == BarButton::Fwd30) {
            // Skip buttons: a round arrow (clockwise = forward, mirrored = back) with the seconds
            // centred inside, the same for every skip length.
            if (b.id == BarButton::Back10) {
                const int oldMode = SetGraphicsMode(hdc, GM_ADVANCED);
                XFORM mirror = { -1.0f, 0.0f, 0.0f, 1.0f, static_cast<float>(cx * 2), 0.0f };
                SetWorldTransform(hdc, &mirror);
                DrawLine(hdc, fontIcon, kTextColor, b.icon, r, DT_CENTER);
                ModifyWorldTransform(hdc, nullptr, MWT_IDENTITY);
                SetGraphicsMode(hdc, oldMode);
            } else {
                DrawLine(hdc, fontIcon, kTextColor, b.icon, r, DT_CENTER);
            }
            CStringW num;
            num.Format(L"%d", b.id == BarButton::Back10 ? skipBack : skipFwd);
            DrawLine(hdc, fontSkipNum, kTextColor, num, r, DT_CENTER);
        } else if (b.id == BarButton::AudioInfo) {
            // Two glyphs: video / a note, with a small "i" badge in the lower right corner, set on
            // a disc of whatever is behind it (bar or focus circle) so it cuts cleanly into the icon.
            const float bx = cx + cell * 0.21f, by = buttonCy + cell * 0.17f, br = cell * 0.15f;
            GdiFlush();
            const int sx = std::clamp(static_cast<int>(bx), 0, outW - 1), sy = std::clamp(static_cast<int>(by), 0, barH - 1);
            const BYTE* px = canvas.Bits() + (static_cast<size_t>(sy) * outW + sx) * 4;
            const COLORREF behind = RGB(px[2], px[1], px[0]);
            DrawLine(hdc, fontIcon, kTextColor, b.icon, r, DT_CENTER);
            canvas.FillCircleAA(bx, by, br * 1.25f, behind);
            RECT rBadge = { static_cast<LONG>(bx - br * 2), static_cast<LONG>(by - br * 2), static_cast<LONG>(bx + br * 2),
                            static_cast<LONG>(by + br * 2) };
            DrawLine(hdc, fontBadge, kTextColor, kIconInfoBadge, rBadge, DT_CENTER);
        } else if (b.id == BarButton::Stats) {
            // madVR: our own mark, a cube (a pointy-top hexagon in three faces with thin gaps), with
            // "m" on the top face and "V", "R" on the two lower faces, each letter centred in its face
            // and cut out in the colour behind the mark (the bar, or the focus circle).
            GdiFlush();
            const int sy = std::clamp(buttonCy, 0, barH - 1), sx = std::clamp(cx, 0, outW - 1);
            const BYTE* px = canvas.Bits() + (static_cast<size_t>(sy) * outW + sx) * 4;
            const COLORREF behind = RGB(px[2], px[1], px[0]);
            using P = std::pair<float, float>;
            const float r = cell * 0.36f, fcx = static_cast<float>(cx), fcy = static_cast<float>(buttonCy);
            P V[6];
            for (int i = 0; i < 6; i++) {
                const double a = M_PI / 180.0 * (60 * i - 90);
                V[i] = { fcx + static_cast<float>(r * cos(a)), fcy + static_cast<float>(r * sin(a)) };
            }
            const P C = { fcx, fcy };
            const std::vector<P> faces[3] = { { V[5], V[0], V[1], C }, { V[5], C, V[3], V[4] }, { C, V[1], V[2], V[3] } };
            const LPCWSTR letters[3] = { L"m", L"V", L"R" };
            HFONT fontFace = MakeFont(static_cast<int>(cell * 0.21f), FW_BOLD);
            for (int f = 0; f < 3; f++) {
                canvas.FillRoundPolyAA(faces[f], cell * 0.035f, kTextColor, cell * 0.012f);
            }
            // Each letter lies on its face's plane: slanted along the two sides; on the top it stays
            // upright and is only flattened (seen from above at an angle), so it still reads as "m"
            // (x axis, then the letter's "down" axis, as GDI world transforms).
            const float axes[3][4] = { { 1.1f, 0.0f, 0.0f, 0.62f }, { 0.866f, 0.5f, 0.0f, 1.0f }, { 0.866f, -0.5f, 0.0f, 1.0f } };
            const int oldMode = SetGraphicsMode(hdc, GM_ADVANCED);
            for (int f = 0; f < 3; f++) {
                float mx = 0.0f, my = 0.0f;
                for (const auto& q : faces[f]) {
                    mx += q.first / 4.0f;
                    my += q.second / 4.0f;
                }
                if (f == 0) {
                    my -= cell * 0.022f; // "m" has no ascender: it sits a little low in its line
                }
                const XFORM xf = { axes[f][0], axes[f][1], axes[f][2], axes[f][3], mx, my };
                SetWorldTransform(hdc, &xf);
                const LONG half = static_cast<LONG>(cell / 4);
                RECT rl = { -half, -half, half, half };
                DrawLine(hdc, fontFace, behind, letters[f], rl, DT_CENTER);
                ModifyWorldTransform(hdc, nullptr, MWT_IDENTITY);
            }
            SetGraphicsMode(hdc, oldMode);
            ReleaseFont(fontFace);
        } else if (b.id == BarButton::VideoInfo) {
            // The info glyph's drawing isn't centred in its character box: centre its ink instead
            RECT rc = r;
            HGDIOBJ old = SelectObject(hdc, fontIcon);
            GLYPHMETRICS gm = {};
            const MAT2 identity = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
            TEXTMETRICW tm = {};
            GetTextMetricsW(hdc, &tm);
            if (GetGlyphOutlineW(hdc, b.icon[0], GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR) {
                const int inkCx = gm.gmptGlyphOrigin.x + static_cast<int>(gm.gmBlackBoxX) / 2; // from the pen position
                const int inkCy = tm.tmAscent - gm.gmptGlyphOrigin.y + static_cast<int>(gm.gmBlackBoxY) / 2; // from the cell top
                OffsetRect(&rc, gm.gmCellIncX / 2 - inkCx, tm.tmHeight / 2 - inkCy);
            }
            SelectObject(hdc, old);
            DrawLine(hdc, fontIcon, kTextColor, b.icon, rc, DT_CENTER);
        } else {
            // Shaders: dimmed while off
            const bool dim = (b.id == BarButton::Shaders && !ShadersOn(pMF));
            DrawLine(hdc, fontIcon, dim ? kDimTextColor : kTextColor, b.icon, r, DT_CENTER);
        }
        if (b.id == BarButton::Volume) {
            // Level slider (dimmed while muted) and the number
            const int sx0 = cx + cell / 2 + unit / 3;
            const int sx1 = sx0 + volSliderW;
            const int th = std::max(2, unit / 10);
            RECT rT = { sx0, buttonCy - th / 2, sx1, buttonCy - th / 2 + th };
            FillSolid(hdc, rT, kTrackColor);
            const int sxv = sx0 + (sx1 - sx0) * std::clamp(volume, 0, 100) / 100;
            RECT rV = { sx0, rT.top, sxv, rT.bottom };
            FillSolid(hdc, rV, muted ? kDimTextColor : kFocusColor);
            canvas.FillCircleAA(static_cast<float>(sxv), static_cast<float>(buttonCy), unit * 0.18f, muted ? kDimTextColor : kTextColor);
            CStringW num;
            num.Format(L"%d", muted ? 0 : volume);
            RECT rNum = { sx1 + unit / 3, buttonCy - unit / 2, sx1 + unit / 3 + volTextW, buttonCy + unit / 2 };
            DrawLine(hdc, fontTime, muted ? kDimTextColor : kTextColor, num, rNum, DT_LEFT);
            if (!preview) {
                // generous hit area around the slider (output coordinates)
                m_hitVolume = { sx0 - unit / 4, barY + buttonCy - unit / 2, sx1 + unit / 4, barY + buttonCy + unit / 2 };
                m_hitVolumeX0 = sx0;
                m_hitVolumeX1 = sx1;
            }
        }
    };
    for (const auto& p : placed) {
        drawButton(p.first, p.second);
    }
    // Name of the selected / hovered button, under it
    if ((!preview || demoFocus) && labelRowH > 0) {
        CStringW label;
        int labelX = 0;
        if (barRow == 1) {
            for (const auto& p : placed) {
                if (p.first.id == focusedButton) {
                    labelX = p.second;
                    switch (focusedButton) {
                        case BarButton::Back10: label.Format(L"Back %d s", skipBack); break;
                        case BarButton::Fwd30: label.Format(L"Forward %d s", skipFwd); break;
                        case BarButton::PlayPause: label = playing ? L"Pause" : L"Play"; break;
                        case BarButton::Volume: label = muted ? L"Volume (muted)" : L"Volume"; break;
                        case BarButton::Stats: label = L"madVR"; break;
                        case BarButton::Shaders: label = ShadersOn(pMF) ? L"Shaders: on" : L"Shaders: off"; break;
                        default: label = BarButtonName(focusedButton); break;
                    }
                }
            }
        }
        if (!label.IsEmpty()) {
            // In the bar's bottom padding, right under the button
            HFONT fontLabel = MakeFont(unit * 40 / 100, FW_NORMAL);
            const int halfW = TextWidth(hdc, fontLabel, label) / 2 + unit / 4;
            const int lx = std::clamp(labelX, margin / 2 + halfW, outW - margin / 2 - halfW);
            RECT rLabel = { lx - halfW, buttonCy + cell / 2 - unit / 10, lx + halfW, buttonCy + cell / 2 - unit / 10 + labelRowH };
            DrawLine(hdc, fontLabel, kTextColor, label, rLabel, DT_CENTER);
            ReleaseFont(fontLabel);
        }
    }
    // Clock and "Ends at", each optional: "21:12  .  Ends at 23:15"
    {
        CStringW info;
        if (CTvMode::IsOn(TvOpt::ShowClock)) {
            info = EndsAt(0).Mid(8); // "Ends at " + now -> just the time
        }
        if (dur > 0 && CTvMode::IsOn(TvOpt::ShowEndsAt)) {
            if (!info.IsEmpty()) {
                info += L"   ";
                info += kDotSep;
                info += L"   ";
            }
            info += EndsAt(dur - pos);
        }
        if (!info.IsEmpty()) {
            // a clear gap after the last playback button
            RECT rEnds = { endsAtX + unit * 7 / 10, buttonCy - unit / 2, outW / 2, buttonCy + unit / 2 };
            DrawLine(hdc, fontTime, kDimTextColor, info, rEnds, DT_LEFT);
        }
    }

    SelectObject(hdc, oldFont);
    ReleaseFont(fontTime);
    ReleaseFont(fontIcon);
    ReleaseFont(fontBubble);
    ReleaseFont(fontSkipNum);
    ReleaseFont(fontBadge);

    // The bar rises from the bottom edge as it fades in. The preview (under TV options, its own
    // element just below the list) fades with its own fade and slides up from the screen edge.
    PerfMark("draw");
    if (preview) {
        Present(ElemPreview, SlotPreview, 0, barY, 0, kBarAlpha, 4, barH);
    } else {
        Present(ElemMenu, SlotBar, 0, barY, 0, kBarAlpha, 2, outH * 3 / 100);
    }
    if (!preview) {
        m_hitButtons = std::move(hitButtons);
    }
}

// The focused TV option is about how a selected bar button looks: the preview shows one selected
bool CTvMenu::PreviewShowsSelection() const
{
    if (!m_bOpen || m_page != Page::OptionsSection || m_nFocus < 0 || m_nFocus >= static_cast<int>(m_rows.size()) ||
        m_rows[m_nFocus].kind != RowKind::Option) {
        return false;
    }
    const int opt = m_rows[m_nFocus].data;
    return opt == static_cast<int>(TvOpt::ButtonLabels) || opt == static_cast<int>(TvOpt::BarIconBrightness) ||
           opt == static_cast<int>(TvOpt::BarIconOpacity);
}

void CTvMenu::UpdateBarPreview()
{
    // While an option that changes the bottom bar is focused (or the colour picker is open),
    // the bar is shown under the list so each step is visible; likewise a sample subtitle for
    // "Subtitle opacity". Removed as soon as the focus moves on or the page closes.
    const Row* row = (m_page == Page::OptionsSection && m_nFocus >= 0 && m_nFocus < static_cast<int>(m_rows.size())) ? &m_rows[m_nFocus] : nullptr;
    const int opt = (row && row->kind == RowKind::Option) ? row->data : -1;
    static const TvOpt barOptions[] = {
        TvOpt::BarSizePct, TvOpt::BackgroundOpacity, TvOpt::BarOpacity, TvOpt::BackgroundColour, TvOpt::AccentColour,
        TvOpt::AccentBrightness, TvOpt::FontFace,
        TvOpt::TimeDisplay, TvOpt::ShowClock, TvOpt::ShowEndsAt, TvOpt::StatsButton, TvOpt::VideoInfoButton,
        TvOpt::AudioInfoButton, TvOpt::SkipBackSec, TvOpt::SkipFwdSec, TvOpt::ButtonLabels, TvOpt::BarIconBrightness,
        TvOpt::BarIconOpacity,
    };
    bool wantBar = m_page == Page::ColourPicker || (row && (row->kind == RowKind::OpenPicker || row->kind == RowKind::BarItemRow));
    for (TvOpt o : barOptions) {
        wantBar = wantBar || opt == static_cast<int>(o);
    }
    wantBar = wantBar && m_bOpen;
    const bool wantSub = m_bOpen && m_page == Page::SubStyle; // sample subtitle for the whole page

    // Fades in when wanted and out when not (OnTick steps m_fPreviewFade towards the target)
    m_fPreviewTarget = wantBar ? 1.0f : 0.0f;
    if (CTvMode::AnimScale() <= 0.0f) {
        m_fPreviewFade = m_fPreviewTarget; // animations off
    }
    if (wantBar && m_bScrollGlide && m_fPreviewFade >= 1.0f && m_shown[ElemPreview].valid) {
        // the list is gliding and the bar under it hasn't changed: keep the frames light
    } else if (wantBar || m_fPreviewFade > 0.0f) {
        RenderBar(true);
    } else if (m_hbmPreview || m_shown[ElemPreview].valid) {
        HideElement(ElemPreview);
        if (m_hbmPreview) {
            DeleteObject(m_hbmPreview);
            m_hbmPreview = nullptr;
        }
    }
    if (wantSub) {
        RenderSubtitlePreview();
    } else if (m_hbmSubPreview) {
        if (m_pOsd) {
            m_pOsd->OsdSetBitmap(kSubPreviewOsdName, nullptr);
        }
        DeleteObject(m_hbmSubPreview);
        m_hbmSubPreview = nullptr;
    }
}

void CTvMenu::RenderSubtitlePreview()
{
    // A sample line in MPC-HC's default subtitle style, at the chosen opacity. Drawn as two GDI
    // coverage masks (text, outline) composed with each part's own colour and opacity.
    const RECT outRect = GetOutputRect(m_pOsd);
    const int outW = outRect.right - outRect.left;
    const int outH = outRect.bottom - outRect.top;
    const STSStyle& st = AfxGetAppSettings().subtitlesDefStyle;

    // Rebuilt only when the style or the output size changes; otherwise the cached picture is
    // reused and only re-sent when the fade changed (this runs on every Render, i.e. every
    // animation tick and mouse move, and drawing it from scratch is expensive at 4K).
    const float fade = std::clamp(m_fFade, 0.0f, 1.0f);
    CStringW key;
    key.Format(L"%dx%d|%s|%g|%g|%d|%d|%d|%g|%g|%g|%g|%06x|%06x|%06x|%02x%02x%02x|%d", outW, outH, CStringW(st.fontName).GetString(),
               st.fontSize, st.fontScaleY, st.fontWeight, st.fItalic, st.borderStyle, st.outlineWidthX, st.outlineWidthY,
               st.shadowDepthX, st.shadowDepthY, st.colors[0], st.colors[2], st.colors[3], st.alpha[0], st.alpha[2], st.alpha[3],
               st.marginRect.bottom);
    if (key == m_subCacheKey && !m_subCache.empty()) {
        if (m_hbmSubPreview && std::fabs(fade - m_subCacheFade) < 0.002f) {
            return; // nothing changed on screen
        }
        SendSubtitlePreview(fade);
        return;
    }

    const float scale = outH / 288.0f; // default style sizes are in 288-line script pixels
    const int fontPx = std::max(12, static_cast<int>(st.fontSize * scale * st.fontScaleY / 100.0));
    const int outline = static_cast<int>(std::max(st.outlineWidthX, st.outlineWidthY) * scale + 0.5);
    const int shadow = static_cast<int>(std::max(st.shadowDepthX, st.shadowDepthY) * scale + 0.5);
    const int marginPx = static_cast<int>(st.marginRect.bottom * scale + 0.5);
    const bool box = (st.borderStyle == 1); // opaque box instead of an outline
    LPCWSTR text = L"This is how your subtitles look";

    HFONT font = CreateFontW(-fontPx, 0, 0, 0, st.fontWeight, st.fItalic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, CStringW(st.fontName));
    SIZE sz = {};
    {
        HDC hdcScreen = GetDC(nullptr);
        HGDIOBJ old = SelectObject(hdcScreen, font);
        GetTextExtentPoint32W(hdcScreen, text, static_cast<int>(wcslen(text)), &sz);
        SelectObject(hdcScreen, old);
        ReleaseDC(nullptr, hdcScreen);
    }
    const int boxPad = box ? std::max(outline, fontPx / 10) : outline; // the box needs a little room even at width 0
    const int padX = boxPad * 2 + fontPx / 4, padY = boxPad * 2 + fontPx / 8;
    const int w = std::min(outW, static_cast<int>(sz.cx) + padX * 2 + shadow);
    const int h = static_cast<int>(sz.cy) + padY * 2 + shadow;

    CDibCanvas textMask(w, h), outlineMask(w, h), shadowMask(w, h), out(w, h);
    if (!textMask.IsValid() || !outlineMask.IsValid() || !shadowMask.IsValid() || !out.IsValid()) {
        DeleteObject(font); // made with CreateFontW here, not cached
        return;
    }
    // Coverage masks: the text alone, or text + outline (or the box), optionally offset (shadow)
    auto drawMask = [&](CDibCanvas& c, bool withBorder, int off) {
        HDC hdc = c.DC();
        RECT all = { 0, 0, w, h };
        FillSolid(hdc, all, RGB(0, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(hdc, font);
        const int x = padX + off, y = padY + off;
        if (withBorder && box) {
            RECT rb = { x - boxPad - fontPx / 8, y - boxPad, x + static_cast<int>(sz.cx) + boxPad + fontPx / 8,
                        y + static_cast<int>(sz.cy) + boxPad };
            FillSolid(hdc, rb, RGB(255, 255, 255));
        } else if (withBorder && outline > 0) {
            // Text outline in one go: the glyph outlines as a path, filled and stroked with a
            // round pen twice the outline width (instead of hundreds of offset text draws).
            BeginPath(hdc);
            TextOutW(hdc, x, y, text, static_cast<int>(wcslen(text)));
            EndPath(hdc);
            LOGBRUSH lb = { BS_SOLID, RGB(255, 255, 255), 0 };
            HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, outline * 2, &lb, 0, nullptr);
            HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, brush);
            StrokeAndFillPath(hdc);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(pen);
            DeleteObject(brush);
            TextOutW(hdc, x, y, text, static_cast<int>(wcslen(text))); // anti-aliased glyph edges inside
        } else {
            TextOutW(hdc, x, y, text, static_cast<int>(wcslen(text)));
        }
        SelectObject(hdc, old);
        GdiFlush();
    };
    drawMask(textMask, false, 0);
    drawMask(outlineMask, true, 0);
    drawMask(shadowMask, true, shadow);
    DeleteObject(font); // made with CreateFontW here, not cached

    // Compose: text over outline over shadow, each with its own colour and opacity (alpha 0 = opaque).
    const COLORREF tc = st.colors[0], oc = st.colors[2], sc = st.colors[3];
    const float ta = (255 - st.alpha[0]) / 255.0f, oa = (255 - st.alpha[2]) / 255.0f;
    const float sa = shadow > 0 ? (255 - st.alpha[3]) / 255.0f : 0.0f;
    const BYTE* pt = textMask.Bits();
    const BYTE* po = outlineMask.Bits();
    const BYTE* ps = shadowMask.Bits();
    m_subCache.assign(static_cast<size_t>(w) * h * 4, 0);
    BYTE* pd = m_subCache.data();
    for (int i = 0; i < w * h; i++) {
        const float t = pt[i * 4] / 255.0f * ta;
        const float o = po[i * 4] / 255.0f * oa * (1.0f - t);
        const float s = ps[i * 4] / 255.0f * sa * std::max(0.0f, 1.0f - t - o);
        const float a = t + o + s;
        if (a <= 0.0f) {
            continue;
        }
        pd[i * 4 + 0] = static_cast<BYTE>((GetBValue(tc) * t + GetBValue(oc) * o + GetBValue(sc) * s) / a);
        pd[i * 4 + 1] = static_cast<BYTE>((GetGValue(tc) * t + GetGValue(oc) * o + GetGValue(sc) * s) / a);
        pd[i * 4 + 2] = static_cast<BYTE>((GetRValue(tc) * t + GetRValue(oc) * o + GetRValue(sc) * s) / a);
        pd[i * 4 + 3] = static_cast<BYTE>(std::min(255.0f, a * 255.0f));
    }
    m_subCacheKey = key;
    m_subCacheW = w;
    m_subCacheH = h;
    // Centred, with its bottom at the chosen distance from the bottom (like a bottom-aligned subtitle)
    m_subCacheX = std::max(0, (outW - w) / 2);
    m_subCacheY = std::clamp(outH - marginPx - (h - padY + boxPad), 0, std::max(0, outH - h));
    SendSubtitlePreview(fade);
}

// Sends the cached sample subtitle to madVR at the given fade.
void CTvMenu::SendSubtitlePreview(float fade)
{
    if (m_subCache.empty() || !m_pOsd) {
        return;
    }
    CDibCanvas out(m_subCacheW, m_subCacheH);
    if (!out.IsValid()) {
        return;
    }
    GdiFlush();
    BYTE* pd = out.Bits();
    const BYTE* ps = m_subCache.data();
    const size_t n = static_cast<size_t>(m_subCacheW) * m_subCacheH;
    for (size_t i = 0; i < n; i++) {
        pd[i * 4 + 0] = ps[i * 4 + 0];
        pd[i * 4 + 1] = ps[i * 4 + 1];
        pd[i * 4 + 2] = ps[i * 4 + 2];
        pd[i * 4 + 3] = static_cast<BYTE>(ps[i * 4 + 3] * fade + 0.5f);
    }
    HBITMAP hbm = out.Detach();
    m_pOsd->OsdSetBitmap(kSubPreviewOsdName, hbm, nullptr, 0, m_subCacheX, m_subCacheY, false, kZOrder - 1, 0,
                         BITMAP_USER_INTERFACE, nullptr, nullptr, nullptr);
    if (m_hbmSubPreview) {
        DeleteObject(m_hbmSubPreview);
    }
    m_hbmSubPreview = hbm;
    m_subCacheFade = fade;
}

void CTvMenu::ScrollToY(int y, bool follow)
{
    if (m_nScrollMax <= 0 || m_hitScrollH <= m_hitScrollThumbH) {
        return;
    }
    // The thumb's centre follows the mouse.
    const float f = std::clamp((y - m_hitScrollTop - m_hitScrollThumbH / 2.0f) / static_cast<float>(m_hitScrollH - m_hitScrollThumbH), 0.0f, 1.0f);
    const int pos = static_cast<int>(f * m_nScrollMax + 0.5f);
    ShowScrollbar(); // stay visible while in use
    if (IsInfoPage(m_page)) {
        if (pos != m_nInfoScroll) {
            m_nInfoScroll = pos;
            RequestRender();
        }
        return;
    }
    SetScrollTarget(f * m_nScrollMax, follow); // smooth, not line by line; settles on a line when let go
}

void CTvMenu::KeepFocusInView()
{
    if (m_rows.empty()) {
        return;
    }
    const int col = m_rows[m_nFocus].col;
    const int wantLine = std::clamp(m_rows[m_nFocus].line, m_nScroll, m_nScroll + MaxRows() - 1);
    int best = m_nFocus, bestDist = INT_MAX;
    for (int i = 0; i < static_cast<int>(m_rows.size()); i++) {
        const Row& r = m_rows[i];
        if (r.col == col && !IsSkipped(r) && r.line >= m_nScroll && r.line < m_nScroll + MaxRows() &&
            std::abs(r.line - wantLine) < bestDist) {
            best = i;
            bestDist = std::abs(r.line - wantLine);
        }
    }
    m_nFocus = best;
}

void CTvMenu::SetScrollTarget(float target, bool follow)
{
    const float maxScroll = static_cast<float>(std::max(0, LineCount() - MaxRows()));
    if (!m_bScrollGlide) { // start from where the list is now
        m_fScrollShown = m_fScrollTarget = static_cast<float>(m_nScroll);
        m_fScrollVel = 0.0f;
    }
    m_fScrollTarget = std::clamp(target, 0.0f, maxScroll);
    m_nScroll = std::clamp(static_cast<int>(std::lround(m_fScrollTarget)), 0, static_cast<int>(maxScroll));
    m_nScrollInputAt = GetTickCount64();
    if (CTvMode::AnimScale() <= 0.0f) { // Animations Off: whole lines, no glide
        m_bScrollGlide = false;
        m_fScrollShown = m_fScrollTarget = static_cast<float>(m_nScroll);
    } else {
        m_bScrollGlide = true;
        if (follow) { // dragging the thumb: the list follows the mouse exactly
            m_fScrollShown = m_fScrollTarget;
            m_fScrollVel = 0.0f;
        }
    }
    ShowScrollbar();
    const int oldFocus = m_nFocus;
    KeepFocusInView(); // only moves when the highlighted row scrolled out of view
    if (m_nFocus != oldFocus) {
        m_rowFade.valid = false; // no crossfade from a row that is no longer there
    }
    StartTick();
    Render();
}

void CTvMenu::ScrollBy(float lines)
{
    // From where the list is heading, so quick notches add up
    const float from = m_bScrollGlide ? m_fScrollTarget : static_cast<float>(m_nScroll);
    const float maxScroll = static_cast<float>(std::max(0, LineCount() - MaxRows()));
    const float to = std::clamp(from + lines, 0.0f, maxScroll);
    if (std::abs(to - from) < 0.001f) {
        return; // already at that end, or everything fits: the highlight stays where it is
    }
    SetScrollTarget(to, false);
}

float CTvMenu::ScrollbarAmount()
{
    // Visible while the mouse is near the right edge, or for ~0.9 s after a scroll;
    // fades in / out over ~0.18 s.
    const ULONGLONG now = GetTickCount64();
    const bool timed = m_nScrollShownAt != 0 && now - m_nScrollShownAt < 900;
    const float target = (m_bMouseRightEdge || timed) ? 1.0f : 0.0f;
    const ULONGLONG dt = m_nScrollAnimLast ? now - m_nScrollAnimLast : 1000;
    m_nScrollAnimLast = now;
    const float step = g_scrollFadeMs > 0.0f ? dt / g_scrollFadeMs : 1.0f;
    m_fScroll += std::clamp(target - m_fScroll, -step, step);
    // Keep redrawing while fading, and while the timed display counts down.
    if (m_fScroll != target || timed) {
        m_bDirty = true;
    }
    return m_fScroll;
}

int CTvMenu::InfoLevel() const
{
    const LPCTSTR key = (m_page == Page::AudioInfo) ? kAudioInfoLevelKey : kVideoInfoLevelKey;
    return std::clamp(CTvMode::GetValue(key, 0), 0, CTvInfo::kLevels - 1);
}

void CTvMenu::SetInfoLevel(int level)
{
    const LPCTSTR key = (m_page == Page::AudioInfo) ? kAudioInfoLevelKey : kVideoInfoLevelKey;
    CTvMode::SetValue(key, level);
}

void CTvMenu::BuildInfo()
{
    m_nLastInfoBuild = GetTickCount64();
    LARGE_INTEGER b0 = {};
    if (g_perfOn) {
        QueryPerformanceCounter(&b0);
    }
    struct BuildTimer {
        LARGE_INTEGER b0;
        ~BuildTimer() {
            if (g_perfOn) {
                LARGE_INTEGER b1, f;
                QueryPerformanceCounter(&b1);
                QueryPerformanceFrequency(&f);
                g_perfBuildMs += (b1.QuadPart - b0.QuadPart) * 1000.0 / f.QuadPart;
            }
        }
    } buildTimer { b0 };
    const auto kind = (m_page == Page::AudioInfo) ? CTvInfo::Kind::Audio : CTvInfo::Kind::Video;
    CTvInfo::BuildChips(m_pMainFrame, kind, m_chips1, m_chips2);
    CTvInfo::BuildBadges(m_pMainFrame, kind, m_badges);
    m_strInfoFile = CTvInfo::FileLine(m_pMainFrame);
    CTvInfo::Build(m_pMainFrame, kind, InfoLevel(), m_info);
    // the page is clamped when drawing (the page count depends on the layout)
}

void CTvMenu::RenderInfo()
{
    // Kodi-style: a band pinned to the top, full width.
    //   title ...................................... < Level >
    //   [LABEL  value] [LABEL  value] ...              (key facts of the stream)
    //   [LABEL  value] [LABEL  value] ...              (output / decoding)
    //   detail lines in 2-3 columns (Extended / Nightmare), paged with Up/Down
    m_outRect = GetOutputRect(m_pOsd);
    const int outW = m_outRect.right - m_outRect.left;
    const int outH = m_outRect.bottom - m_outRect.top;
    m_lastOutput = { outW, outH };

    // Larger than the lists: 100 % "Menu size" = output height / 24.
    const int u = std::max(18, static_cast<int>(outH / 24.0f * Scale(TvOpt::MenuTextPct)));
    const int pad = u / 2;
    const int margin = std::max(pad * 2, outW / 40);
    const int titleH = u * 11 / 10;
    const int chipH = u * 16 / 10;
    const int lineH = u * 68 / 100;
    const bool showHints = CTvMode::IsOn(TvOpt::KeyHints);
    const int hintH = showHints ? u * 6 / 10 : 0;
    const int cols = outW >= 1600 ? 3 : 2;
    const int colGap = u;
    const int colW = (outW - 2 * margin - (cols - 1) * colGap) / cols;

    // Header (icon, title, file line, level pills), badge line, then the Source -> Output cards
    const int subH = m_strInfoFile.IsEmpty() ? 0 : u * 55 / 100;
    const int badgeH = u * 64 / 100;
    const int cardLineH = u * 62 / 100;
    const int cardHeadH = u * 52 / 100;
    const int cardRows = static_cast<int>(std::max(m_chips1.size(), m_chips2.size()));
    const int cardH = cardRows > 0 ? pad / 2 + cardHeadH + cardRows * cardLineH + pad / 2 : 0;
    const int headH = pad + titleH + subH + (m_badges.empty() ? pad / 3 : pad / 2 + badgeH + pad * 2 / 3) + (cardH > 0 ? cardH + pad : 0);
    (void)chipH;
    // as tall as the content needs, up to the full height of the picture
    const int maxBodyH = std::max(lineH * 3, outH - headH - hintH - pad * 2);
    const int linesPerCol = std::max(1, maxBodyH / lineH);

    // Flow all lines into balanced columns (a section header never ends a column; it moves to
    // the next one), then scroll all columns together, one line at a time.
    // m_nInfoPages = number of scroll positions, m_nInfoScroll = first line shown.
    const int n = static_cast<int>(m_info.size());
    std::vector<std::vector<int>> columns(cols);
    {
        const int target = (n + cols - 1) / cols;
        int i = 0;
        for (int c = 0; c < cols && i < n; c++) {
            const bool last = (c == cols - 1);
            while (i < n && (last || static_cast<int>(columns[c].size()) < target)) {
                if (!last && m_info[i].header && static_cast<int>(columns[c].size()) >= target - 1 && !columns[c].empty()) {
                    break;
                }
                columns[c].push_back(i++);
            }
        }
    }
    size_t usedRows = 0;
    for (const auto& c : columns) {
        usedRows = std::max(usedRows, c.size());
    }
    const int visibleLines = std::min(static_cast<int>(usedRows), linesPerCol);
    m_nInfoPages = std::max(1, static_cast<int>(usedRows) - visibleLines + 1);
    m_nInfoScroll = std::clamp(m_nInfoScroll, 0, m_nInfoPages - 1);
    const int bodyH = visibleLines * lineH;
    const int panelH = headH + (bodyH > 0 ? bodyH + pad : 0) + hintH + pad;

    m_elemRect = { 0, 0, outW, panelH };
    m_hitRowsVisible = 0;
    m_hitButtons.clear();
    m_hitSeekX1 = m_hitSeekX0 = 0;

    PerfMark("layout");
    CDibCanvas& canvas = Content(SlotInfo, outW, panelH);
    if (!canvas.IsValid()) {
        return;
    }
    HDC hdc = canvas.DC();
    SetBkMode(hdc, TRANSPARENT);
    RECT all = { 0, 0, outW, panelH };
    FillSolid(hdc, all, kPanelColor);

    HFONT fontTitle = MakeFont(u * 66 / 100, FW_SEMIBOLD);
    HFONT fontChipLabel = MakeFont(u * 30 / 100, FW_SEMIBOLD);
    HFONT fontChipValue = MakeFont(u * 56 / 100, FW_SEMIBOLD);
    HFONT fontLine = MakeFont(u * 42 / 100, FW_NORMAL);
    HFONT fontHeader = MakeFont(u * 42 / 100, FW_SEMIBOLD);
    HFONT fontHint = MakeFont(u * 32 / 100, FW_NORMAL);
    HGDIOBJ oldFont = SelectObject(hdc, fontTitle);

    auto fillRound = [&](const RECT& rc, COLORREF c, int radius) {
        HBRUSH br = CreateSolidBrush(c);
        HGDIOBJ oldBr = SelectObject(hdc, br);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        RoundRect(hdc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(br);
    };
    // "COLOUR SPACE" -> "Colour space"
    auto sentence = [](CStringW s) {
        s.MakeLower();
        if (!s.IsEmpty()) {
            s.SetAt(0, towupper(s[0]));
        }
        return s;
    };
    const COLORREF kGold = RGB(224, 176, 62), kGoldText = RGB(51, 36, 6);
    const COLORREF cardColor = Mix(kPanelColor, kTextColor, 0.07f);
    const COLORREF pillTrack = Mix(kPanelColor, kTextColor, 0.10f);
    HFONT fontIcon = MakeFont(u * 70 / 100, FW_NORMAL, L"Segoe MDL2 Assets");
    HFONT fontSub = MakeFont(u * 36 / 100, FW_NORMAL);
    HFONT fontBadge = MakeFont(u * 34 / 100, FW_SEMIBOLD);
    HFONT fontPill = MakeFont(u * 36 / 100, FW_SEMIBOLD);

    // --- Header: icon, title and file line; the level switch as pills on the right
    const bool isAudio = (m_page == Page::AudioInfo);
    const int iconW = u;
    RECT rIcon = { margin, pad, margin + iconW, pad + titleH };
    DrawLine(hdc, fontIcon, kFocusColor, isAudio ? kIconAudioInfo : kIconVideoInfo, rIcon, DT_LEFT);
    const int textX = margin + iconW + pad / 2;
    {
        // Pills: as wide as each name, inside a rounded track; the current level filled with the
        // accent colour, the one under the mouse fading towards it.
        const int pillH = u * 70 / 100;
        const int pillPadX = pad;
        int trackW = pad / 4;
        int widths[CTvInfo::kLevels];
        for (int l = 0; l < CTvInfo::kLevels; l++) {
            widths[l] = TextWidth(hdc, fontPill, CTvInfo::LevelName(l)) + pillPadX * 2;
            trackW += widths[l];
        }
        trackW += pad / 4;
        const int trackTop = pad + (titleH - pillH) / 2 - pad / 8;
        RECT rTrack = { outW - margin - trackW, trackTop - pad / 8, outW - margin, trackTop + pillH + pad / 8 };
        fillRound(rTrack, pillTrack, rTrack.bottom - rTrack.top);
        bool running = false;
        int prev = 0;
        const float t = m_arrowFade.Progress(m_nArrowHover, running, prev);
        if (running) {
            m_bDirty = true;
        }
        int x = rTrack.left + pad / 4;
        for (int l = 0; l < CTvInfo::kLevels; l++) {
            RECT rPill = { x, trackTop, x + widths[l], trackTop + pillH };
            const bool cur = (l == InfoLevel());
            const float hover = (l + 1 == m_nArrowHover) ? t : (l + 1 == prev ? 1.0f - t : 0.0f);
            if (cur) {
                fillRound(rPill, kFocusColor, pillH);
            } else if (hover > 0.0f) {
                fillRound(rPill, Mix(pillTrack, kFocusColor, 0.35f * hover), pillH);
            }
            DrawLine(hdc, fontPill, cur ? kTextColor : Mix(kDimTextColor, kTextColor, hover), CTvInfo::LevelName(l), rPill, DT_CENTER);
            m_hitLevelPill[l] = rPill; // the panel sits at 0,0
            x += widths[l];
        }
        // Title as two tabs, "Video" and "Audio" (OK or a click switches): the current one bright
        // with an accent line under it, the other dim
        {
            int tx = textX;
            const LPCWSTR names[3] = { L"Video", L"Audio", L"Scopes" };
            for (int k = 0; k < InfoTabCount(); k++) {
                const bool cur = (k == 1) == isAudio && k != 2;
                const int w = TextWidth(hdc, fontTitle, names[k]);
                RECT rTab = { tx, pad, tx + w, pad + titleH };
                DrawLine(hdc, fontTitle, cur ? kTextColor : kDimTextColor, names[k], rTab, DT_LEFT);
                if (cur) {
                    const int lineH = std::max(2, u / 16);
                    RECT rLine = { tx, pad + titleH - lineH - pad / 6, tx + w, pad + titleH - pad / 6 };
                    fillRound(rLine, kFocusColor, lineH);
                }
                m_hitInfoTab[k] = { tx - pad / 2, pad, tx + w + pad / 2, pad + titleH };
                tx += w + pad * 2;
            }
        }
        if (subH > 0) {
            RECT rSub = { textX, pad + titleH - pad / 4, rTrack.left - pad, pad + titleH + subH - pad / 4 };
            DrawLine(hdc, fontSub, kDimTextColor, m_strInfoFile, rSub, DT_LEFT);
        }
    }
    int y = pad + titleH + subH;

    // --- Badges: the headline facts as pills (accent = size / channels, gold = HDR, object audio, lossless)
    if (!m_badges.empty()) {
        y += pad / 2;
        int x = textX;
        for (const auto& b : m_badges) {
            const int w = TextWidth(hdc, fontBadge, b.text) + pad * 3 / 2;
            if (x + w > outW - margin) {
                break;
            }
            RECT rb = { x, y, x + w, y + badgeH };
            const COLORREF bg = b.tone == 1 ? kFocusColor : b.tone == 2 ? kGold : pillTrack;
            const COLORREF fg = b.tone == 2 ? kGoldText : kTextColor;
            fillRound(rb, bg, badgeH);
            DrawLine(hdc, fontBadge, fg, b.text, rb, DT_CENTER);
            x += w + pad / 3;
        }
        y += badgeH + pad * 2 / 3;
    } else {
        y += pad / 3;
    }

    // --- Source -> Output cards
    if (cardH > 0) {
        const bool two = !m_chips1.empty() && !m_chips2.empty();
        const int arrowW = two ? u : 0;
        const int cardW = two ? (outW - 2 * margin - arrowW) / 2 : outW - 2 * margin;
        auto drawCard = [&](int x, LPCWSTR heading, const std::vector<TvInfoChip>& rows) {
            RECT rc = { x, y, x + cardW, y + cardH };
            fillRound(rc, cardColor, pad);
            RECT rHead = { x + pad, y + pad / 2, x + cardW - pad, y + pad / 2 + cardHeadH };
            DrawLine(hdc, fontChipLabel, kFocusColor, heading, rHead, DT_LEFT);
            int labelW = 0;
            for (const auto& c : rows) {
                labelW = std::max(labelW, TextWidth(hdc, fontLine, sentence(c.label)));
            }
            labelW = std::min(labelW, cardW * 40 / 100);
            for (int i = 0; i < static_cast<int>(rows.size()); i++) {
                const int top = rHead.bottom + i * cardLineH;
                RECT rl = { x + pad, top, x + pad + labelW, top + cardLineH };
                RECT rv = { rl.right + pad, top, x + cardW - pad, top + cardLineH };
                DrawLine(hdc, fontLine, kDimTextColor, sentence(rows[i].label), rl, DT_LEFT);
                DrawLine(hdc, fontLine, kTextColor, rows[i].value, rv, DT_LEFT);
            }
        };
        if (!m_chips1.empty()) {
            drawCard(margin, isAudio ? L"TRACK" : L"SOURCE", m_chips1);
        }
        if (!m_chips2.empty()) {
            if (two) {
                RECT rArrow = { margin + cardW, y, margin + cardW + arrowW, y + cardH };
                DrawLine(hdc, fontIcon, kFocusColor, kIconForward, rArrow, DT_CENTER);
            }
            drawCard(two ? margin + cardW + arrowW : margin, isAudio ? L"PLAYBACK" : L"OUTPUT", m_chips2);
        }
        y += cardH + pad;
    }
    ReleaseFont(fontIcon);
    ReleaseFont(fontSub);
    ReleaseFont(fontBadge);
    ReleaseFont(fontPill);

    // --- Detail lines in columns
    if (bodyH > 0) {
        const int bodyTop = y;
        RECT rRule = { margin, bodyTop - pad / 2, outW - margin, bodyTop - pad / 2 + std::max(1, u / 40) };
        FillSolid(hdc, rRule, Mix(kPanelColor, kTextColor, 0.14f)); // thin divider under the cards
        for (int c = 0; c < static_cast<int>(columns.size()); c++) {
            const int x = margin + c * (colW + colGap);
            const int nameW = colW * 42 / 100;
            const int end = std::min(static_cast<int>(columns[c].size()), m_nInfoScroll + visibleLines);
            for (int r = m_nInfoScroll; r < end; r++) {
                const TvInfoLine& line = m_info[columns[c][r]];
                const int top = bodyTop + (r - m_nInfoScroll) * lineH;
                if (line.header) {
                    // section heading: small accent capitals sitting low, a faint rule after it
                    CStringW head = line.name;
                    head.MakeUpper();
                    RECT rh = { x, top + lineH / 5, x + colW, top + lineH };
                    DrawLine(hdc, fontChipLabel, kFocusColor, head, rh, DT_LEFT);
                    const int lx = x + TextWidth(hdc, fontChipLabel, head) + pad / 2;
                    const int ly = (rh.top + rh.bottom) / 2;
                    if (lx < x + colW) {
                        RECT rl = { lx, ly, x + colW, ly + std::max(1, u / 48) };
                        FillSolid(hdc, rl, Mix(kPanelColor, kTextColor, 0.12f));
                    }
                } else {
                    RECT rName = { x + pad / 2, top, x + nameW, top + lineH };
                    RECT rValue = { x + nameW + pad / 2, top, x + colW, top + lineH };
                    DrawLine(hdc, fontLine, kDimTextColor, line.name, rName, DT_LEFT);
                    DrawLine(hdc, fontLine, kTextColor, line.value, rValue, DT_LEFT);
                }
            }
        }
        if (m_nInfoPages > 1) {
            // Pop-up scrollbar at the right edge: one thumb step per line.
            const int w = std::max(4, u / 7);
            const int x = outW - margin / 2 - w;
            const int thumbH = std::max(w * 3, bodyH * visibleLines / static_cast<int>(usedRows));
            m_hitScrollTrack = { x - margin / 2, bodyTop, outW, bodyTop + bodyH }; // panel is at 0,0
            m_hitScrollTop = bodyTop;
            m_hitScrollH = bodyH;
            m_hitScrollThumbH = thumbH;
            m_nScrollMax = m_nInfoPages - 1;
            const float amount = ScrollbarAmount();
            if (amount > 0.0f) {
                const int thumbY = bodyTop + (bodyH - thumbH) * m_nInfoScroll / (m_nInfoPages - 1);
                RECT rTrack = { x, bodyTop, x + w, bodyTop + bodyH };
                RECT rThumb = { x, thumbY, x + w, thumbY + thumbH };
                FillSolid(hdc, rTrack, Mix(kPanelColor, kTextColor, 0.12f * amount));
                FillSolid(hdc, rThumb, Mix(kPanelColor, kFocusColor, amount));
            }
        }
        y = bodyTop + bodyH + pad;
    }

    // --- Hint
    CStringW hint;
    if (m_nInfoPages > 1) {
        hint.Format(L"%s scroll   %s%s level   OK %s   Back return", kArrowUpDown, kTriLeft, kTriRight, isAudio ? L"video" : L"audio");
    } else {
        hint.Format(L"%s%s level   OK %s   Back return", kTriLeft, kTriRight, isAudio ? L"video" : L"audio");
    }
    RECT rHint = { margin, y, outW - margin, y + hintH };
    if (showHints) {
        DrawLine(hdc, fontHint, kDimTextColor, hint, rHint, DT_LEFT);
    }

    SelectObject(hdc, oldFont);
    ReleaseFont(fontTitle);
    ReleaseFont(fontChipLabel);
    ReleaseFont(fontChipValue);
    ReleaseFont(fontLine);
    ReleaseFont(fontHeader);
    ReleaseFont(fontHint);

    PerfMark("draw");
    Present(ElemMenu, SlotInfo, 0, 0, 0, kPanelAlpha, 3, outH * 2 / 100); // drops in from the top edge
    m_strInfoSig = InfoSignature();
}

// The picker edits the custom accent (target 0) or the custom background colour (target 1)
void CTvMenu::PickerGet(int& h, int& s, int& l) const
{
    if (m_nPickerTarget == 1) {
        h = CTvMode::BgHue(), s = CTvMode::BgSat(), l = CTvMode::BgLight();
    } else {
        h = CTvMode::CustomHue(), s = CTvMode::CustomSat(), l = CTvMode::CustomLight();
    }
}

void CTvMenu::PickerSet(int h, int s, int l)
{
    if (m_nPickerTarget == 1) {
        CTvMode::SetCustomBackground(h, s, l);
    } else {
        CTvMode::SetCustom(h, s, l);
    }
}

void CTvMenu::PickerAdjust(int row, int delta)
{
    int h = 0, s = 0, l = 0;
    PickerGet(h, s, l);
    if (row == 0) {
        h = (h + delta + 360) % 360; // hue wraps around
    } else if (row == 1) {
        s = std::clamp(s + delta, 0, 100);
    } else {
        l = std::clamp(l + delta, 0, 100);
    }
    PickerSet(h, s, l);
    Render(); // the whole overlay picks up the new colour at once
}

void CTvMenu::PickerSetFromX(int row, int x)
{
    const RECT& r = m_hitPickerBar[row];
    if (r.right <= r.left) {
        return;
    }
    const float f = std::clamp((x - r.left) / static_cast<float>(r.right - r.left - 1), 0.0f, 1.0f);
    int h = 0, s = 0, l = 0;
    PickerGet(h, s, l);
    if (row == 0) {
        h = std::min(359, static_cast<int>(f * 360.0f));
    } else if (row == 1) {
        s = static_cast<int>(f * 100.0f + 0.5f);
    } else {
        l = static_cast<int>(f * 100.0f + 0.5f);
    }
    PickerSet(h, s, l);
    RequestRender();
}

bool CTvMenu::HandlePickerKey(UINT vk, bool isRepeat)
{
    switch (vk) {
        case VK_UP:
        case VK_DOWN: {
            const int row = std::clamp(m_nFocus + (vk == VK_DOWN ? 1 : -1), 0, 2);
            if (row != m_nFocus) {
                m_nFocus = row;
                Render();
            }
            return true;
        }
        case VK_LEFT:
        case VK_RIGHT: {
            // Small steps on a tap, bigger once the key has been held for a moment.
            const ULONGLONG now = GetTickCount64();
            if (!isRepeat) {
                m_nPickerHoldStart = now;
            }
            const bool fast = isRepeat && now - m_nPickerHoldStart > 600;
            const int step = (m_nFocus == 0) ? (fast ? 15 : 5) : (fast ? 5 : 2);
            PickerAdjust(m_nFocus, vk == VK_RIGHT ? step : -step);
            return true;
        }
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_BACK:
        case VK_BROWSER_BACK:
            if (!isRepeat) {
                Back(); // values are saved as they change
            }
            return true;
        default:
            return false;
    }
}

// Info > Scopes: what madVR receives and what it shows, measured live (CTvScopes, only while open).
// Top: waveform of the displayed picture | measured tone curve (input nits -> output nits).
// Bottom: the last 60 s of frame peak / average, input and output | numbers and file metadata.
void CTvMenu::RenderScopes()
{
    m_outRect = GetOutputRect(m_pOsd);
    const int outW = m_outRect.right - m_outRect.left;
    const int outH = m_outRect.bottom - m_outRect.top;
    m_lastOutput = { outW, outH };

    TvScopeData d;
    CTvScopes::Get(d);
    m_nScopesVersion = d.version;
    const int displayNits = std::max(1, MadVrDisplayNits() > 0 ? MadVrDisplayNits() : 100);
    CStringW hdrFormat, maxCll, maxFall, mastering;
    CTvInfo::HdrMetadata(m_pMainFrame, hdrFormat, maxCll, maxFall, mastering);

    // Output code -> nits: PQ output is absolute; SDR output is relative to the display's peak
    // (madVR's tone mapping target), gamma 2.2
    auto outNits = [&](float code) {
        return d.outputKind ? CTvScopes::PqToNits(code) : displayNits * std::pow(std::clamp(code, 0.0f, 1.0f), 2.2f);
    };
    auto fmtNits = [](float n) {
        CStringW s;
        s.Format(n < 1.0f ? L"%.2f nits" : n < 100.0f ? L"%.1f nits" : L"%.0f nits", n);
        return s;
    };

    const int u = std::max(18, static_cast<int>(outH / 24.0f * Scale(TvOpt::MenuTextPct)));
    const int pad = u / 2;
    const int margin = std::max(pad * 2, outW / 40);
    const int titleH = u * 11 / 10;
    const int subH = u * 55 / 100;
    const bool showHints = CTvMode::IsOn(TvOpt::KeyHints);
    const int hintH = showHints ? u * 6 / 10 : 0;
    const int headH = pad + titleH + subH + pad / 2;
    const int panelH = std::min(outH, std::max(u * 12, outH * 64 / 100));
    const int bodyTop = headH, bodyH = panelH - headH - hintH - pad;
    const int gap = u / 2;
    const int leftW = (outW - 2 * margin - gap) * 56 / 100, rightW = outW - 2 * margin - gap - leftW;
    const int rowAH = (bodyH - gap) * 58 / 100, rowBH = bodyH - gap - rowAH;
    const RECT boxWave = { margin, bodyTop, margin + leftW, bodyTop + rowAH };
    const RECT boxCurve = { margin + leftW + gap, bodyTop, outW - margin, bodyTop + rowAH };
    const RECT boxHist = { margin, bodyTop + rowAH + gap, margin + leftW, bodyTop + bodyH };
    const RECT boxStats = { margin + leftW + gap, bodyTop + rowAH + gap, outW - margin, bodyTop + bodyH };
    (void)rightW;
    (void)rowBH;

    m_elemRect = { 0, 0, outW, panelH };
    m_hitRowsVisible = 0;
    m_hitButtons.clear();
    m_hitSeekX1 = m_hitSeekX0 = 0;

    CDibCanvas& canvas = Content(SlotInfo, outW, panelH);
    if (!canvas.IsValid()) {
        return;
    }
    HDC hdc = canvas.DC();
    SetBkMode(hdc, TRANSPARENT);
    RECT all = { 0, 0, outW, panelH };
    FillSolid(hdc, all, kPanelColor);
    HFONT fontTitle = MakeFont(u * 66 / 100, FW_SEMIBOLD);
    HFONT fontSub = MakeFont(u * 38 / 100, FW_NORMAL);
    HFONT fontBox = MakeFont(u * 36 / 100, FW_SEMIBOLD);
    HFONT fontAxis = MakeFont(u * 28 / 100, FW_NORMAL);
    HFONT fontLine = MakeFont(u * 38 / 100, FW_NORMAL);
    HFONT fontHint = MakeFont(u * 32 / 100, FW_NORMAL);
    HGDIOBJ oldFont = SelectObject(hdc, fontTitle);
    const COLORREF boxBg = Mix(kPanelColor, kTextColor, 0.045f);
    const COLORREF gridColor = Mix(kPanelColor, kTextColor, 0.16f);
    const int lineW = std::max(2, u / 22);

    auto polyline = [&](const std::vector<POINT>& pts, COLORREF c, int w) {
        if (pts.size() < 2) {
            return;
        }
        HPEN pen = CreatePen(PS_SOLID, w, c);
        HGDIOBJ old = SelectObject(hdc, pen);
        Polyline(hdc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(hdc, old);
        DeleteObject(pen);
    };
    auto hline = [&](int x0, int x1, int y, COLORREF c) {
        RECT r = { x0, y, x1, y + std::max(1, u / 40) };
        FillSolid(hdc, r, c);
    };
    auto vline = [&](int x, int y0, int y1, COLORREF c) {
        RECT r = { x, y0, x + std::max(1, u / 40), y1 };
        FillSolid(hdc, r, c);
    };
    // a box with its caption; returns the graph area inside (room for axis labels on the left)
    auto box = [&](const RECT& b, LPCWSTR caption, int labelW) {
        FillSolid(hdc, const_cast<RECT&>(b), boxBg);
        RECT rc = { b.left + pad / 2, b.top + pad / 4, b.right - pad / 2, b.top + pad / 4 + u * 55 / 100 };
        DrawLine(hdc, fontBox, kDimTextColor, caption, rc, DT_LEFT);
        return RECT { b.left + labelW, rc.bottom + pad / 4, b.right - pad / 2, b.bottom - pad / 2 };
    };
    // log10 nits axis, 0.1 .. 10000
    auto logPos = [](float nits, int a, int b) {
        const float f = (std::log10(std::max(nits, 0.1f)) + 1.0f) / 5.0f;
        return a + static_cast<int>((b - a) * std::clamp(f, 0.0f, 1.0f));
    };
    const float kNitsMarks[] = { 0.1f, 1.0f, 10.0f, 100.0f, 1000.0f, 10000.0f };
    auto nitsLabel = [](float n) {
        CStringW s;
        s.Format(n < 1.0f ? L"%.1f" : L"%.0f", n);
        return s;
    };

    // --- Header: tabs (Video, Audio, Scopes) and what is being measured
    {
        int tx = margin;
        const LPCWSTR names[3] = { L"Video", L"Audio", L"Scopes" };
        for (int k = 0; k < 3; k++) {
            const bool cur = (k == 2);
            const int w = TextWidth(hdc, fontTitle, names[k]);
            RECT rTab = { tx, pad, tx + w, pad + titleH };
            DrawLine(hdc, fontTitle, cur ? kTextColor : kDimTextColor, names[k], rTab, DT_LEFT);
            if (cur) {
                RECT rLine = { tx, pad + titleH - std::max(2, u / 16) - pad / 6, tx + w, pad + titleH - pad / 6 };
                FillSolid(hdc, rLine, kFocusColor);
            }
            m_hitInfoTab[k] = { tx - pad / 2, pad, tx + w + pad / 2, pad + titleH };
            tx += w + pad * 2;
        }
        CStringW mode;
        if (!m_pMadVr) {
            mode = L"Scopes need madVR as the video renderer";
        } else if (!d.problem.IsEmpty()) {
            mode = d.problem;
        } else if (!d.haveOutput && !d.haveInput) {
            mode = L"Measuring...";
        } else if (d.outputKind == 1 || d.hdrOutput) {
            mode.Format(L"madVR sends HDR (PQ) to the display  %s  %d-bit output", kDotSep, d.outputBits);
        } else if (d.outputKind == 2) {
            mode.Format(L"madVR output: linear float (scRGB)");
        } else {
            mode.Format(L"madVR tone maps to SDR  %s  display peak %d nits  %s  %d-bit output", kDotSep, displayNits, kDotSep, d.outputBits);
        }
        RECT rSub = { margin, pad + titleH, outW - margin, pad + titleH + subH };
        DrawLine(hdc, fontSub, kDimTextColor, mode, rSub, DT_LEFT);
    }

    // --- Waveform of the displayed picture
    {
        const int labelW = u * 17 / 10;
        const RECT g = box(boxWave, L"Waveform  (the picture as displayed)", labelW);
        const int gw = g.right - g.left, gh = g.bottom - g.top;
        // grid: PQ output by nits, SDR output by % of the signal (with the display's nits for 100 %)
        struct Mark { float code; CStringW label; };
        std::vector<Mark> marks;
        if (d.outputKind) {
            for (float n : kNitsMarks) {
                marks.push_back({ CTvScopes::NitsToPq(n), nitsLabel(n) });
            }
        } else {
            for (int p = 0; p <= 100; p += 25) {
                CStringW s;
                if (p == 100) {
                    s.Format(L"%d nits", displayNits); // 100 % of the signal = the display's peak
                } else {
                    s.Format(L"%d%%", p);
                }
                marks.push_back({ p / 100.0f, s });
            }
        }
        for (const auto& m : marks) {
            const int y = g.bottom - 1 - static_cast<int>(m.code * (gh - 1));
            hline(g.left, g.right, y, gridColor);
            RECT rl = { boxWave.left + pad / 4, y - u / 4, g.left - pad / 4, y + u / 4 };
            DrawLine(hdc, fontAxis, kDimTextColor, m.label, rl, DT_RIGHT);
        }
        if (d.haveOutput && gw > 0 && gh > 0 && !d.wave.empty()) {
            GdiFlush();
            BYTE* bits = canvas.Bits();
            const float scale = std::log(1.0f + std::max(1.0f, d.waveSamplesPerCol / static_cast<float>(TvScopeData::kLevels) * 6.0f));
            const COLORREF hot = Mix(kFocusColor, kTextColor, 0.45f);
            // one colour per waveform cell (192 x 128), then each screen row copies its cells' colours
            // (a log and a colour mix per cell instead of per pixel)
            std::vector<DWORD> cell(static_cast<size_t>(TvScopeData::kCols) * TvScopeData::kLevels, 0);
            for (size_t i = 0; i < cell.size(); i++) {
                const int n = d.wave[i];
                if (n > 0) {
                    const float a = std::min(1.0f, 0.25f + std::log(1.0f + n) / scale);
                    const COLORREF c = a < 0.85f ? Mix(boxBg, hot, a / 0.85f) : Mix(hot, kTextColor, (a - 0.85f) / 0.15f);
                    cell[i] = 0xFF000000u | (static_cast<DWORD>(GetRValue(c)) << 16) | (static_cast<DWORD>(GetGValue(c)) << 8) | GetBValue(c);
                }
            }
            std::vector<int> colOf(gw);
            for (int x = 0; x < gw; x++) {
                colOf[x] = std::clamp(x * TvScopeData::kCols / gw, 0, TvScopeData::kCols - 1) * TvScopeData::kLevels;
            }
            for (int y = 0; y < gh; y++) {
                const int level = std::clamp((gh - 1 - y) * TvScopeData::kLevels / gh, 0, TvScopeData::kLevels - 1);
                DWORD* row = reinterpret_cast<DWORD*>(bits + (static_cast<size_t>(g.top + y) * outW + g.left) * 4);
                for (int x = 0; x < gw; x++) {
                    const DWORD c = cell[colOf[x] + level];
                    if (c) {
                        row[x] = c;
                    }
                }
            }
        }
        PerfMark("waveform");
    }

    // --- Measured tone curve: input nits -> output nits (log / log; the diagonal = unchanged)
    {
        const int labelW = u * 13 / 10;
        const RECT g = box(boxCurve, L"Tone curve  (input -> output, measured)", labelW);
        const RECT plot = { g.left, g.top, g.right, g.bottom - u * 45 / 100 }; // room for the x labels
        for (float n : kNitsMarks) {
            const int x = logPos(n, plot.left, plot.right - 1);
            const int y = plot.bottom - 1 - (logPos(n, 0, plot.bottom - plot.top - 1));
            vline(x, plot.top, plot.bottom, gridColor);
            hline(plot.left, plot.right, y, gridColor);
            RECT rx = { x - u, plot.bottom, x + u, g.bottom };
            DrawLine(hdc, fontAxis, kDimTextColor, nitsLabel(n), rx, DT_CENTER);
            RECT ry = { boxCurve.left + pad / 4, y - u / 4, plot.left - pad / 4, y + u / 4 };
            DrawLine(hdc, fontAxis, kDimTextColor, nitsLabel(n), ry, DT_RIGHT);
        }
        auto px = [&](float nits) { return logPos(nits, plot.left, plot.right - 1); };
        auto py = [&](float nits) { return plot.bottom - 1 - logPos(nits, 0, plot.bottom - plot.top - 1); };
        polyline({ { px(0.1f), py(0.1f) }, { px(10000.0f), py(10000.0f) } }, Mix(boxBg, kDimTextColor, 0.5f), std::max(1, lineW / 2));
        if (!d.outputKind) { // the display's peak (SDR output)
            hline(plot.left, plot.right, py(static_cast<float>(displayNits)), Mix(boxBg, kFocusColor, 0.6f));
        }
        if (d.haveCurve) {
            std::vector<POINT> pts;
            for (int b = 0; b < TvScopeData::kCurveBins; b++) {
                const float inN = CTvScopes::CurveBinNits(b);
                if (d.curveOut[b] >= 0.0f && inN >= 0.1f) {
                    pts.push_back({ px(inN), py(std::max(0.1f, outNits(d.curveOut[b]))) });
                }
            }
            polyline(pts, kFocusColor, lineW);
            for (const auto& p : pts) {
                canvas.FillCircleAA(static_cast<float>(p.x), static_cast<float>(p.y), lineW * 1.2f, kTextColor);
            }
            if (d.haveInput) { // this frame's peak
                vline(px(d.inPeakNits), plot.top, plot.bottom, Mix(boxBg, kTextColor, 0.5f));
            }
        } else {
            const LPCWSTR why = !m_pMadVr ? L"" : (d.hdrOutput ? L"HDR passthrough: madVR doesn't tone map"
                                                               : d.haveInput ? L"SDR video: nothing is tone mapped" : L"Measuring...");
            RECT rm = plot;
            DrawLine(hdc, fontLine, kDimTextColor, why, rm, DT_CENTER);
        }
    }

    // --- History: the last 60 s of frame peak and average, input and output
    {
        const int labelW = u * 13 / 10;
        const RECT g = box(boxHist, L"Last 60 s  (frame peak / average)", labelW);
        // legend in the caption line, right-aligned
        struct Leg { LPCWSTR name; COLORREF c; };
        const Leg legs[4] = { { L"input peak", kTextColor }, { L"input avg", kDimTextColor },
                              { L"output peak", kFocusColor }, { L"output avg", Mix(kFocusColor, kDimTextColor, 0.5f) } };
        int lx = boxHist.right - pad / 2;
        for (int k = 3; k >= 0; k--) {
            const int w = TextWidth(hdc, fontAxis, legs[k].name);
            RECT rl = { lx - w, boxHist.top + pad / 4, lx, boxHist.top + pad / 4 + u * 55 / 100 };
            DrawLine(hdc, fontAxis, legs[k].c, legs[k].name, rl, DT_RIGHT);
            RECT sw = { lx - w - u / 2, (rl.top + rl.bottom) / 2 - lineW, lx - w - u / 6, (rl.top + rl.bottom) / 2 + lineW };
            FillSolid(hdc, sw, legs[k].c);
            lx -= w + u;
        }
        for (float n : kNitsMarks) {
            const int y = g.bottom - 1 - logPos(n, 0, g.bottom - g.top - 1);
            hline(g.left, g.right, y, gridColor);
            RECT ry = { boxHist.left + pad / 4, y - u / 4, g.left - pad / 4, y + u / 4 };
            DrawLine(hdc, fontAxis, kDimTextColor, nitsLabel(n), ry, DT_RIGHT);
        }
        if (!d.history.empty()) {
            const ULONGLONG now = d.history.back().t;
            auto hx = [&](ULONGLONG t) { return g.right - 1 - static_cast<int>((g.right - g.left - 1) * std::min<ULONGLONG>(60000, now - t) / 60000); };
            auto hy = [&](float nits) { return g.bottom - 1 - logPos(nits, 0, g.bottom - g.top - 1); };
            std::vector<POINT> a, b, c, e;
            for (const auto& h : d.history) {
                const int x = hx(h.t);
                a.push_back({ x, hy(h.inPeak) });
                b.push_back({ x, hy(h.inAvg) });
                if (h.outPeak > 0.0f) { // no output measured yet at the very start
                    c.push_back({ x, hy(outNits(h.outPeak)) });
                    e.push_back({ x, hy(outNits(h.outAvg)) });
                }
            }
            polyline(b, kDimTextColor, lineW);
            polyline(e, legs[3].c, lineW);
            polyline(a, kTextColor, lineW);
            polyline(c, kFocusColor, lineW);
        }
    }

    // --- Numbers
    {
        const RECT g = box(boxStats, L"Now", 0);
        const int rowH = u * 54 / 100;
        int y = g.top;
        auto row = [&](LPCWSTR label, const CStringW& value) {
            if (y + rowH > boxStats.bottom - pad / 4) {
                return;
            }
            RECT rl = { boxStats.left + pad / 2, y, boxStats.left + (boxStats.right - boxStats.left) * 48 / 100, y + rowH };
            RECT rv = { rl.right, y, boxStats.right - pad / 2, y + rowH };
            DrawLine(hdc, fontLine, kDimTextColor, label, rl, DT_LEFT);
            DrawLine(hdc, fontLine, kTextColor, value, rv, DT_RIGHT);
            y += rowH;
        };
        auto outText = [&](float code) {
            CStringW s = fmtNits(outNits(code));
            if (!d.outputKind) {
                CStringW pc;
                pc.Format(L"  (%.0f%%)", code * 100.0f);
                s += pc;
            }
            return s;
        };
        row(L"Input peak", d.haveInput ? fmtNits(d.inPeakNits) : CStringW(L"..."));
        row(L"Input average", d.haveInput ? fmtNits(d.inAvgNits) : CStringW(L"..."));
        row(L"Output peak", d.haveOutput ? outText(d.outPeakCode) : CStringW(L"..."));
        row(L"Output average", d.haveOutput ? outText(d.outAvgCode) : CStringW(L"..."));
        row(L"Brightest input so far", d.haveInput ? fmtNits(d.inMaxSeenNits) : CStringW(L"..."));
        row(L"File MaxCLL / MaxFALL", (maxCll.IsEmpty() && maxFall.IsEmpty()) ? CStringW(L"not in the file")
                                                                                : (maxCll.IsEmpty() ? CStringW(L"?") : maxCll) + L" / " + (maxFall.IsEmpty() ? CStringW(L"?") : maxFall));
        row(L"Mastering display", mastering.IsEmpty() ? CStringW(L"not in the file") : mastering);
        if (!hdrFormat.IsEmpty()) {
            row(L"HDR format", hdrFormat);
        }
        CStringW frames;
        frames.Format(L"%d drawn  %s  %d dropped", d.framesDrawn, kDotSep, d.framesDropped);
        row(L"madVR frames", frames);
    }

    if (showHints) {
        RECT rHint = { margin, panelH - hintH - pad / 2, outW - margin, panelH - pad / 2 };
        DrawLine(hdc, fontHint, kDimTextColor, L"OK  next tab   Back  return   (measuring only while this page is open)", rHint, DT_LEFT);
    }

    SelectObject(hdc, oldFont);
    ReleaseFont(fontTitle);
    ReleaseFont(fontSub);
    ReleaseFont(fontBox);
    ReleaseFont(fontAxis);
    ReleaseFont(fontLine);
    ReleaseFont(fontHint);
    PerfMark("draw");
    Present(ElemMenu, SlotInfo, 0, 0, 0, kPanelAlpha, 3, outH * 2 / 100); // drops in from the top edge
}

void CTvMenu::RenderPicker()
{
    m_outRect = GetOutputRect(m_pOsd);
    const int outW = m_outRect.right - m_outRect.left;
    const int outH = m_outRect.bottom - m_outRect.top;
    m_lastOutput = { outW, outH };

    int hue = 0, sat = 0, light = 0;
    PickerGet(hue, sat, light);

    // Same scale as the lists ("Menu size")
    const int unit = std::max(16, static_cast<int>(outH / 13.0f * 0.5f * Scale(TvOpt::MenuTextPct)));
    const int pad = unit / 3;
    const int titleH = unit;
    const int rowH = unit * 16 / 10;           // label line + bar
    const int barH = std::max(8, unit * 30 / 100);
    const bool showHints = CTvMode::IsOn(TvOpt::KeyHints);
    const int hintH = showHints ? unit * 7 / 10 : 0;
    const int panelW = std::min(outW - unit, std::max(unit * 14, outW * 40 / 100 * CTvMode::Get(TvOpt::MenuTextPct) / 100));
    const int panelH = pad + titleH + 3 * rowH + hintH + pad;
    const int panelX = (outW - panelW) / 2;
    const int panelY = (outH - panelH) / 2;
    m_elemRect = { panelX, panelY, panelX + panelW, panelY + panelH };
    m_hitRowsVisible = 0;
    m_hitButtons.clear();
    m_hitSeekX1 = m_hitSeekX0 = 0;

    CDibCanvas canvas(panelW, panelH);
    if (!canvas.IsValid()) {
        return;
    }
    HDC hdc = canvas.DC();
    SetBkMode(hdc, TRANSPARENT);
    RECT all = { 0, 0, panelW, panelH };
    FillSolid(hdc, all, kPanelColor);

    HFONT fontTitle = MakeFont(unit * 52 / 100, FW_SEMIBOLD);
    HFONT fontRow = MakeFont(unit * 44 / 100, FW_NORMAL);
    HFONT fontHint = MakeFont(unit * 31 / 100, FW_NORMAL);
    HGDIOBJ oldFont = SelectObject(hdc, fontTitle);

    // Title, with a preview swatch and the hex value on the right
    const COLORREF chosen = CTvMode::Hsl(hue, sat, light);
    RECT rTitle = { pad * 2, pad, panelW - pad * 2, pad + titleH };
    DrawLine(hdc, fontTitle, kDimTextColor, m_nPickerTarget == 1 ? L"Custom background colour" : L"Custom accent colour", rTitle, DT_LEFT);
    const int swatchW = unit * 2;
    RECT rSwatch = { rTitle.right - swatchW, rTitle.top + pad / 2, rTitle.right, rTitle.bottom - pad / 2 };
    FillSolid(hdc, rSwatch, chosen);
    CStringW hex;
    hex.Format(L"#%02X%02X%02X", GetRValue(chosen), GetGValue(chosen), GetBValue(chosen));
    RECT rHex = { rTitle.left, rTitle.top, rSwatch.left - pad, rTitle.bottom };
    DrawLine(hdc, fontRow, kTextColor, hex, rHex, DT_RIGHT);

    // Three rows: label + value, then the gradient bar with a marker
    struct PickerRow { LPCWSTR name; int value; int max; };
    const PickerRow rows[3] = {
        { L"Hue", hue, 359 },
        { L"Saturation", sat, 100 },
        { L"Lightness", light, 100 },
    };
    for (int i = 0; i < 3; i++) {
        const int top = pad + titleH + i * rowH;
        const bool focused = (i == m_nFocus);
        if (focused) {
            RECT rf = { pad, top, panelW - pad, top + rowH };
            FillSolid(hdc, rf, Mix(kPanelColor, kTextColor, 0.07f));
            RECT edge = { pad, top, pad + std::max(3, unit / 12), top + rowH };
            FillSolid(hdc, edge, kFocusColor);
        }
        RECT rLabel = { pad * 2, top, panelW - pad * 2, top + rowH * 50 / 100 };
        DrawLine(hdc, fontRow, focused ? kTextColor : kDimTextColor, rows[i].name, rLabel, DT_LEFT);
        CStringW v;
        v.Format(L"%d", rows[i].value);
        v += (i == 0) ? CStringW(kDegree) : CStringW(L" %");
        DrawLine(hdc, fontRow, focused ? kTextColor : kDimTextColor, v, rLabel, DT_RIGHT);

        // Gradient
        const int bx0 = pad * 2, bx1 = panelW - pad * 2;
        const int by = top + rowH * 58 / 100;
        for (int x = bx0; x < bx1; x++) {
            const float f = (x - bx0) / static_cast<float>(bx1 - bx0 - 1);
            COLORREF c;
            if (i == 0) {
                c = CTvMode::Hsl(static_cast<int>(f * 359), sat, light);
            } else if (i == 1) {
                c = CTvMode::Hsl(hue, static_cast<int>(f * 100), light);
            } else {
                c = CTvMode::Hsl(hue, sat, static_cast<int>(f * 100));
            }
            RECT col = { x, by, x + 1, by + barH };
            FillSolid(hdc, col, c);
        }
        // Marker: white line with a dark edge so it shows on any colour
        const int mx = bx0 + (bx1 - bx0 - 1) * rows[i].value / rows[i].max;
        RECT outer = { mx - 3, by - 3, mx + 4, by + barH + 3 };
        RECT inner = { mx - 1, by - 3, mx + 2, by + barH + 3 };
        FillSolid(hdc, outer, RGB(20, 20, 20));
        FillSolid(hdc, inner, RGB(255, 255, 255));
        // Mouse: a taller hit area than the bar itself
        m_hitPickerBar[i] = { panelX + bx0, panelY + top + rowH * 45 / 100, panelX + bx1, panelY + top + rowH };
    }

    if (showHints) {
        CStringW hint;
        hint.Format(L"%s choose   %s%s adjust   Back done", kArrowUpDown, kTriLeft, kTriRight);
        RECT rHint = { pad * 2, pad + titleH + 3 * rowH, panelW - pad * 2, panelH - pad };
        DrawLine(hdc, fontHint, kDimTextColor, hint, rHint, DT_LEFT);
    }

    SelectObject(hdc, oldFont);
    ReleaseFont(fontTitle);
    ReleaseFont(fontRow);
    ReleaseFont(fontHint);

    const float fade = std::clamp(m_fFade, 0.0f, 1.0f);
    canvas.ApplyRoundedAlpha(pad, static_cast<BYTE>(kPanelAlpha * fade + 0.5f), static_cast<BYTE>(255 * fade + 0.5f));

    HBITMAP hbm = canvas.Detach();
    m_pOsd->OsdSetBitmap(kOsdName, hbm, nullptr, 0, panelX, panelY + SlideOffset(outH * 2 / 100), false, kZOrder, 0,
                         BITMAP_USER_INTERFACE, nullptr, nullptr, nullptr);
    if (m_hbmMenu) {
        DeleteObject(m_hbmMenu);
    }
    m_hbmMenu = hbm;
    m_shown[ElemMenu].valid = false; // drawn the older way (no kept drawing)
}

void CTvMenu::RenderPauseScreen()
{
    // A card at the lower left: "Paused", the title (large), "0:42:10 / 2:01:33  .  -1:19:23  .
    // Ends at 21:45", and a thin progress line. Same panel look as the menus.
    CMainFrame* pMF = m_pMainFrame;
    m_outRect = GetOutputRect(m_pOsd);
    const int outW = m_outRect.right - m_outRect.left;
    const int outH = m_outRect.bottom - m_outRect.top;
    m_lastOutput = { outW, outH };

    const REFERENCE_TIME pos = pMF->GetPos();
    const REFERENCE_TIME dur = pMF->GetDur();
    const CStringW title = CTvInfo::MediaTitle(pMF); // the file's title tag, else its name
    CStringW timeLine = FormatTime(pos);
    if (dur > 0) {
        timeLine += L" / " + FormatTime(dur) + L"   " + kMidDot + L"   -" + FormatTime(dur - pos);
        const CStringW ends = EndsAt(dur - pos);
        if (!ends.IsEmpty()) {
            timeLine += CStringW(L"   ") + kMidDot + L"   " + ends;
        }
    }

    // Sized next to the menus (follows "Menu size"): the title a little larger than a list row
    const int unit = std::max(12, static_cast<int>(outH / 22.0f * 0.55f * Scale(TvOpt::MenuTextPct)));
    const int pad = unit / 2;
    HFONT fontLabel = MakeFont(unit * 55 / 100, FW_SEMIBOLD);
    HFONT fontTitle = MakeFont(unit * 13 / 10, FW_NORMAL);
    HFONT fontTime = MakeFont(unit * 65 / 100, FW_NORMAL);
    int titleW = 0, timeW = 0;
    {
        HDC hdcScreen = GetDC(nullptr);
        titleW = TextWidth(hdcScreen, fontTitle, title);
        timeW = TextWidth(hdcScreen, fontTime, timeLine);
        SelectObject(hdcScreen, GetStockObject(SYSTEM_FONT));
        ReleaseDC(nullptr, hdcScreen);
    }
    const int boxW = std::clamp(std::max(titleW, timeW) + pad * 5, outW * 22 / 100, outW * 50 / 100);
    const int labelH = unit * 8 / 10, titleH = unit * 17 / 10, timeH = unit, lineGap = unit / 2;
    const int boxH = pad * 2 + labelH + titleH + timeH + lineGap + pad;

    CDibCanvas canvas(boxW, boxH);
    if (!canvas.IsValid()) {
        return;
    }
    HDC hdc = canvas.DC();
    SetBkMode(hdc, TRANSPARENT);
    RECT all = { 0, 0, boxW, boxH };
    FillSolid(hdc, all, kPanelColor);
    const int x0 = pad * 5 / 2, x1 = boxW - pad * 5 / 2;
    int y = pad * 2;
    RECT rLabel = { x0, y, x1, y + labelH };
    DrawLine(hdc, fontLabel, kFocusColor, L"PAUSED", rLabel, DT_LEFT);
    y += labelH;
    RECT rTitle = { x0, y, x1, y + titleH };
    DrawLine(hdc, fontTitle, kTextColor, title, rTitle, DT_LEFT);
    y += titleH;
    RECT rTime = { x0, y, x1, y + timeH };
    DrawLine(hdc, fontTime, kDimTextColor, timeLine, rTime, DT_LEFT);
    y += timeH + lineGap / 2;
    if (dur > 0) {
        RECT rTrack = { x0, y, x1, y + std::max(2, unit / 10) };
        FillSolid(hdc, rTrack, kTrackColor);
        RECT rFill = rTrack;
        rFill.right = x0 + static_cast<int>((x1 - x0) * (static_cast<double>(std::clamp<REFERENCE_TIME>(pos, 0, dur)) / dur));
        FillSolid(hdc, rFill, kFocusColor);
    }

    const float fade = std::clamp(m_fFade, 0.0f, 1.0f);
    canvas.ApplyRoundedAlpha(pad, static_cast<BYTE>(kPanelAlpha * fade + 0.5f), static_cast<BYTE>(255 * fade + 0.5f));
    HBITMAP hbm = canvas.Detach();
    const int boxX = std::max(pad * 2, outW / 24);
    const int boxY = outH - boxH - outH * 8 / 100;
    m_elemRect = { boxX, boxY, boxX + boxW, boxY + boxH };
    m_hitButtons.clear();
    m_hitRowsVisible = 0;
    m_hitSeekX1 = m_hitSeekX0 = 0;
    m_pOsd->OsdSetBitmap(kOsdName, hbm, nullptr, 0, boxX, boxY + std::min(unit, SlideOffset(outH * 2 / 100)), false, kZOrder, 0,
                         BITMAP_USER_INTERFACE, nullptr, nullptr, nullptr);
    if (m_hbmMenu) {
        DeleteObject(m_hbmMenu);
    }
    m_hbmMenu = hbm;
    m_shown[ElemMenu].valid = false; // drawn the older way (no kept drawing)
}

void CTvMenu::RenderMiniSeek()
{
    // A small rounded box near the bottom: "1:02:30 / 2:03:57   +0:30" and a thin progress line.
    CMainFrame* pMF = m_pMainFrame;
    m_outRect = GetOutputRect(m_pOsd);
    const int outW = m_outRect.right - m_outRect.left;
    const int outH = m_outRect.bottom - m_outRect.top;
    m_lastOutput = { outW, outH };

    const REFERENCE_TIME pos = pMF->GetPos();
    const REFERENCE_TIME dur = pMF->GetDur();
    const REFERENCE_TIME target = (m_rtPreview >= 0) ? m_rtPreview
                                  : (m_rtInstantTarget >= 0 && GetTickCount64() - m_nInstantAt < 1500) ? m_rtInstantTarget : pos;
    const REFERENCE_TIME delta = target - pos;

    // Follows "Menu size" (120 % = the original size of this box).
    const int unit = std::max(12, static_cast<int>(outH / 22.0f * CTvMode::Get(TvOpt::MenuTextPct) / 120.0f));
    const int pad = unit / 2;
    HFONT fontTime = MakeFont(unit * 7 / 10, FW_SEMIBOLD);
    HFONT fontDelta = MakeFont(unit * 6 / 10, FW_NORMAL);

    CStringW timeText = FormatTime(target);
    if (dur > 0) {
        timeText += L" / " + FormatTime(dur);
    }
    CStringW deltaText;
    deltaText.Format(L"%s%s", delta < 0 ? L"-" : L"+", FormatTime(delta < 0 ? -delta : delta).GetString());

    // Measure with a screen DC, then size the box.
    int timeW = 0, deltaW = 0;
    {
        HDC hdcScreen = GetDC(nullptr);
        timeW = TextWidth(hdcScreen, fontTime, timeText);
        deltaW = TextWidth(hdcScreen, fontDelta, deltaText);
        SelectObject(hdcScreen, GetStockObject(SYSTEM_FONT));
        ReleaseDC(nullptr, hdcScreen);
    }
    const int boxW = pad * 3 + timeW + unit + deltaW;
    const int boxH = unit * 2;

    CDibCanvas canvas(boxW, boxH);
    if (!canvas.IsValid()) {
        ReleaseFont(fontTime);
        ReleaseFont(fontDelta);
        return;
    }
    HDC hdc = canvas.DC();
    SetBkMode(hdc, TRANSPARENT);
    RECT all = { 0, 0, boxW, boxH };
    FillSolid(hdc, all, kPanelColor);

    const int textCy = boxH * 42 / 100;
    RECT rTime = { pad * 3 / 2, textCy - unit / 2, pad * 3 / 2 + timeW, textCy + unit / 2 };
    DrawLine(hdc, fontTime, kTextColor, timeText, rTime, DT_LEFT);
    RECT rDelta = { boxW - pad * 3 / 2 - deltaW, textCy - unit / 2, boxW - pad * 3 / 2, textCy + unit / 2 };
    DrawLine(hdc, fontDelta, kFocusDimTextColor, deltaText, rDelta, DT_RIGHT);

    if (dur > 0) {
        const int lineY = boxH - pad - unit / 10;
        const int x0 = pad * 3 / 2, x1 = boxW - pad * 3 / 2;
        RECT rTrack = { x0, lineY, x1, lineY + std::max(2, unit / 10) };
        FillSolid(hdc, rTrack, kTrackColor);
        RECT rFill = rTrack;
        rFill.right = x0 + static_cast<int>((x1 - x0) * (static_cast<double>(std::clamp<REFERENCE_TIME>(target, 0, dur)) / dur));
        FillSolid(hdc, rFill, kFocusColor);
    }

    ReleaseFont(fontTime);
    ReleaseFont(fontDelta);

    const float fade = std::clamp(m_fFade, 0.0f, 1.0f);
    canvas.ApplyRoundedAlpha(pad, static_cast<BYTE>(kPanelAlpha * fade + 0.5f), static_cast<BYTE>(255 * fade + 0.5f));

    HBITMAP hbm = canvas.Detach();
    const int boxX = (outW - boxW) / 2;
    const int boxY = outH - boxH - unit * 2;
    m_elemRect = { boxX, boxY, boxX + boxW, boxY + boxH };
    m_hitButtons.clear();
    m_hitRowsVisible = 0;
    m_hitSeekX1 = m_hitSeekX0 = 0;
    m_pOsd->OsdSetBitmap(kOsdName, hbm, nullptr, 0, boxX, boxY + std::min(unit, SlideOffset(outH * 2 / 100)), false, kZOrder, 0,
                         BITMAP_USER_INTERFACE, nullptr, nullptr, nullptr);
    if (m_hbmMenu) {
        DeleteObject(m_hbmMenu);
    }
    m_hbmMenu = hbm;
    m_shown[ElemMenu].valid = false; // drawn the older way (no kept drawing)
}

// ---------------------------------------------------------------------------
// Helpers

// Info: one panel with Video / Audio / Scopes tabs (OK steps through them, or click a tab; Scopes only
// with madVR); the way back stays the same
void CTvMenu::SwitchInfoKind()
{
    const int cur = m_page == Page::AudioInfo ? 1 : m_page == Page::Scopes ? 2 : 0;
    ShowInfoTab((cur + 1) % InfoTabCount());
}

// Quit page > Restart player: start the player again with the current file at the current place (and
// fullscreen if it is now), then exit this one. The new one is started 2 s later by a hidden cmd, so this
// one has let go of the web interface port and the single-instance check by then.
void CTvMenu::RestartPlayer()
{
    CMainFrame* pMF = m_pMainFrame;
    if (!pMF) {
        return;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    CStringW args;
    if (pMF->GetLoadState() == MLS::LOADED && !pMF->lastOpenFile.IsEmpty()) {
        const REFERENCE_TIME pos = pMF->m_wndSeekBar.HasDuration() ? pMF->m_wndSeekBar.GetPos() : 0;
        args.Format(L"\"%s\" /start %lld", CStringW(pMF->lastOpenFile).GetString(), pos / 10000);
    }
    if (pMF->IsFullScreenMode()) {
        args += L" /fullscreen";
    }
    CStringW cmd;
    cmd.Format(L"cmd.exe /d /c ping -n 3 127.0.0.1 >nul & start \"\" \"%s\" %s", exe, args.GetString());
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.GetBuffer(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    cmd.ReleaseBuffer();
    ForceClose();
    pMF->PostMessage(WM_COMMAND, ID_FILE_EXIT);
}

void CTvMenu::ShowInfoTab(int tab)
{
    const Page parent = m_parent;
    ShowPage(tab == 1 ? Page::AudioInfo : (TV_SCOPES && tab == 2 && m_pMadVr) ? Page::Scopes : Page::VideoInfo, 0);
    m_parent = parent;
}

int CTvMenu::MadVrDisplayNits()
{
    const auto& defs = MadVrDefs();
    if (m_strMadVrDevice.IsEmpty() && m_pMadVr) {
        m_strMadVrDevice = MadVrActiveDevice(&m_strMadVrDeviceName);
        m_strMadVrDeviceActive = m_strMadVrDevice;
    }
    for (int i = 0; i < static_cast<int>(defs.size()); i++) {
        if (defs[i].kind != MvKind::Heading && defs[i].id && wcscmp(defs[i].id, L"hdr\\hdrNits") == 0) {
            CStringW v;
            return MadVrGet(i, v) ? _wtoi(v) : 0;
        }
    }
    return 0;
}

// The TV option changed while the menu is up: clear everything from the old place, draw in the new
void CTvMenu::SwitchOverlay()
{
    const float fade = m_fFade;
    RemoveOsd();
    ChooseOverlay();
    m_fFade = fade;
    m_lastOutput = {};
    if (m_nTickTimer) {
        StopTick(); // the tick rate depends on where it's drawn
        StartTick();
    }
}

// The overlay is drawn inside madVR (TV option "Draw the TV overlay": Inside madVR, the default)
// or in our own windows over the video (Own window, or whenever madVR isn't the renderer).
bool CTvMenu::ChooseOverlay()
{
    m_pMadVr = FindMadVrOsd(m_pMainFrame);
    // (environment MPCHC_TV_OVERLAY_WINDOW=1 forces the window, for testing without changing settings)
    const bool wantWindow = CTvMode::Get(TvOpt::OverlayMode) == 1 || !m_pMadVr ||
                            GetEnvironmentVariableW(L"MPCHC_TV_OVERLAY_WINDOW", nullptr, 0) > 0;
    HWND video = (m_pMainFrame && m_pMainFrame->m_pVideoWnd) ? m_pMainFrame->m_pVideoWnd->m_hWnd : nullptr;
    if (wantWindow) {
        if (!video) {
            return false;
        }
        if (!m_pWinOsd || m_hWinOsdVideo != video) {
            m_pWinOsd.Attach(new CTvOsdWindow(video)); // starts with one reference
            m_hWinOsdVideo = video;
        }
        m_pOsd = m_pWinOsd;
    } else {
        m_pOsd = m_pMadVr;
    }
    m_bWindowOsd = wantWindow;
    m_lastOrigin = { LONG_MIN, LONG_MIN };
    return m_pOsd != nullptr;
}

CComPtr<IMadVROsdServices> CTvMenu::FindMadVrOsd(CMainFrame* pMainFrame)
{
    // Look for the madVR filter itself instead of going through m_pCAP: that also works
    // when madVR is added as an External Filter (no MPC-HC allocator presenter, m_pCAP == nullptr).
    CComPtr<IEnumFilters> pEnum;
    if (!pMainFrame->m_pGB || FAILED(pMainFrame->m_pGB->EnumFilters(&pEnum))) {
        return nullptr;
    }
    for (CComPtr<IBaseFilter> pBF; pEnum->Next(1, &pBF, nullptr) == S_OK; pBF.Release()) {
        CComQIPtr<IMadVROsdServices> pOsd = pBF;
        if (pOsd) {
            return CComPtr<IMadVROsdServices>(pOsd);
        }
    }
    return nullptr;
}

bool CTvMenu::IsTargetWindow(CMainFrame* pMainFrame, HWND hWnd)
{
    // Only keys aimed at the player itself (main frame or video area), not e.g. the playlist panel.
    if (hWnd == pMainFrame->m_hWnd) {
        return true;
    }
    HWND hView = pMainFrame->m_wndView.m_hWnd;
    return hView && (hWnd == hView || ::IsChild(hView, hWnd));
}
