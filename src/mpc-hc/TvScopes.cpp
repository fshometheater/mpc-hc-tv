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
#include "TvScopes.h"
#include <d3d9.h>
#include <mvrInterfaces.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>

// Measurements for the Scopes page, taken only while the page is open (measured cost on the build PC
// with madVR 0.92.17 at 4K: under ~1.5 ms per analysed frame on madVR's render thread, ~5 ms per grab).
//  - The displayed frame: madVR's OSD render callback runs after madVR is done with each frame; its
//    render target (what goes to the display, already tone mapped) is shrunk on the GPU, and the copy
//    made two frames earlier is read back, so madVR never waits for the GPU.
//  - The input: IMadVRFrameGrabber at 25 % without madVR's HDR -> SDR conversion (the source, PQ), and
//    with it (the tone-mapped output of the same moment), on a worker thread every ~0.7 s.

namespace
{
    std::mutex g_lock;          // guards g_data
    TvScopeData g_data;
    std::atomic<unsigned> g_version { 0 };
    std::atomic<bool> g_stop { false };
    std::atomic<bool> g_hdrOutput { false };
    std::atomic<float> g_lastOutPeak { 0.0f }, g_lastOutAvg { 0.0f };
    CComPtr<IMadVROsdServices> g_madVr;
    std::thread g_thread;
    constexpr char kCallbackName[] = "MPC-HC.TvScopes";

    float Half(WORD h)
    {
        const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
        const float s = (h & 0x8000) ? -1.0f : 1.0f;
        if (e == 0) {
            return s * m / 16777216.0f;
        }
        if (e == 31) {
            return s * 65504.0f;
        }
        return s * std::ldexp(1.0f + m / 1024.0f, e - 15);
    }

    class CScopeCallback : public IOsdRenderCallback
    {
        LONG m_ref = 1;
        CComPtr<IDirect3DDevice9> m_dev;
        static constexpr int kW = TvScopeData::kCols * 2, kH = 216; // 2 pixels per waveform column
        CComPtr<IDirect3DSurface9> m_small[2], m_sys[2];
        D3DFORMAT m_fmt = D3DFMT_UNKNOWN;
        int m_idx = 0;
        bool m_havePrev = false;
        unsigned m_frame = 0;

        void ReleaseSurfaces()
        {
            for (int i = 0; i < 2; i++) {
                m_small[i].Release();
                m_sys[i].Release();
            }
            m_havePrev = false;
        }

        // decodes one pixel to signal code values (gamma or PQ; float output converted to PQ)
        static void Decode(D3DFORMAT fmt, const BYTE* row, int x, float& r, float& g, float& b)
        {
            if (fmt == D3DFMT_A2R10G10B10) {
                const DWORD d = reinterpret_cast<const DWORD*>(row)[x];
                r = ((d >> 20) & 0x3FF) / 1023.0f, g = ((d >> 10) & 0x3FF) / 1023.0f, b = (d & 0x3FF) / 1023.0f;
            } else if (fmt == D3DFMT_A2B10G10R10) {
                const DWORD d = reinterpret_cast<const DWORD*>(row)[x];
                r = (d & 0x3FF) / 1023.0f, g = ((d >> 10) & 0x3FF) / 1023.0f, b = ((d >> 20) & 0x3FF) / 1023.0f;
            } else if (fmt == D3DFMT_A16B16G16R16F) {
                const WORD* p = reinterpret_cast<const WORD*>(row) + x * 4; // scRGB: 1.0 = 80 nits, linear
                r = CTvScopes::NitsToPq(std::max(0.0f, Half(p[0])) * 80.0f);
                g = CTvScopes::NitsToPq(std::max(0.0f, Half(p[1])) * 80.0f);
                b = CTvScopes::NitsToPq(std::max(0.0f, Half(p[2])) * 80.0f);
            } else {
                const BYTE* p = row + x * 4; // A8R8G8B8 / X8R8G8B8
                r = p[2] / 255.0f, g = p[1] / 255.0f, b = p[0] / 255.0f;
            }
        }

        void Analyse(const BYTE* bits, int pitch)
        {
            const int kind = m_fmt == D3DFMT_A16B16G16R16F ? 2
                             : ((m_fmt == D3DFMT_A2R10G10B10 || m_fmt == D3DFMT_A2B10G10R10) && g_hdrOutput) ? 1 : 0;
            std::vector<int> wave(TvScopeData::kCols * TvScopeData::kLevels, 0);
            float peak = 0;
            double sum = 0;
            for (int y = 0; y < kH; y++) {
                const BYTE* row = bits + static_cast<size_t>(y) * pitch;
                for (int x = 0; x < kW; x++) {
                    float r, g, b;
                    Decode(m_fmt, row, x, r, g, b);
                    const float mx = std::max({ r, g, b });
                    peak = std::max(peak, mx);
                    sum += mx;
                    // signal luma: Rec.2020 weights for PQ output, Rec.709 for SDR
                    const float luma = kind ? 0.2627f * r + 0.6780f * g + 0.0593f * b : 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    const int level = std::clamp(static_cast<int>(luma * TvScopeData::kLevels), 0, TvScopeData::kLevels - 1);
                    wave[(x * TvScopeData::kCols / kW) * TvScopeData::kLevels + level]++;
                }
            }
            const float avg = static_cast<float>(sum / (kW * kH));
            g_lastOutPeak = peak;
            g_lastOutAvg = avg;
            std::lock_guard<std::mutex> lock(g_lock);
            g_data.wave.swap(wave);
            g_data.waveSamplesPerCol = kH * (kW / TvScopeData::kCols);
            g_data.outputKind = kind;
            g_data.outputBits = (m_fmt == D3DFMT_A8R8G8B8 || m_fmt == D3DFMT_X8R8G8B8) ? 8 : m_fmt == D3DFMT_A16B16G16R16F ? 16 : 10;
            g_data.outPeakCode = peak;
            g_data.outAvgCode = avg;
            g_data.haveOutput = true;
            g_data.version = ++g_version;
        }

    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == __uuidof(IOsdRenderCallback)) {
                *ppv = static_cast<IOsdRenderCallback*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        {
            const LONG r = InterlockedDecrement(&m_ref);
            if (r == 0) {
                delete this;
            }
            return r;
        }
        STDMETHODIMP SetDevice(IDirect3DDevice9* dev) override
        {
            ReleaseSurfaces();
            m_dev = dev;
            return S_OK;
        }
        STDMETHODIMP ClearBackground(LPCSTR, REFERENCE_TIME, RECT*, RECT*) override { return CALLBACK_EMPTY; }

        STDMETHODIMP RenderOsd(LPCSTR, REFERENCE_TIME, RECT*, RECT* activeVideoRect) override
        {
            if (!m_dev || (m_frame++ % 2) != 0) { // every 2nd frame is plenty for a scope
                return CALLBACK_EMPTY;
            }
            CComPtr<IDirect3DSurface9> rt;
            D3DSURFACE_DESC desc = {};
            if (FAILED(m_dev->GetRenderTarget(0, &rt)) || !rt || FAILED(rt->GetDesc(&desc))) {
                return CALLBACK_EMPTY;
            }
            if (desc.Format != m_fmt || !m_small[0]) {
                ReleaseSurfaces();
                m_fmt = desc.Format;
                for (int i = 0; i < 2; i++) {
                    if (FAILED(m_dev->CreateRenderTarget(kW, kH, m_fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &m_small[i], nullptr)) ||
                        FAILED(m_dev->CreateOffscreenPlainSurface(kW, kH, m_fmt, D3DPOOL_SYSTEMMEM, &m_sys[i], nullptr))) {
                        ReleaseSurfaces();
                        std::lock_guard<std::mutex> lock(g_lock);
                        g_data.problem.Format(L"madVR's output format (%d) can't be read", static_cast<int>(m_fmt));
                        return CALLBACK_EMPTY;
                    }
                }
            }
            RECT src = activeVideoRect ? *activeVideoRect : RECT { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
            src.left = std::clamp<LONG>(src.left, 0, desc.Width);
            src.right = std::clamp<LONG>(src.right, src.left, desc.Width);
            src.top = std::clamp<LONG>(src.top, 0, desc.Height);
            src.bottom = std::clamp<LONG>(src.bottom, src.top, desc.Height);
            const bool copied = src.right > src.left && src.bottom > src.top &&
                                SUCCEEDED(m_dev->StretchRect(rt, &src, m_small[m_idx], nullptr, D3DTEXF_LINEAR));
            const int prev = m_idx ^ 1;
            if (m_havePrev && SUCCEEDED(m_dev->GetRenderTargetData(m_small[prev], m_sys[prev]))) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(m_sys[prev]->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                    Analyse(static_cast<const BYTE*>(lr.pBits), lr.Pitch);
                    m_sys[prev]->UnlockRect();
                }
            }
            m_havePrev = copied;
            m_idx = prev;
            return CALLBACK_EMPTY;
        }
    };

    CScopeCallback* g_callback = nullptr;

    // One 25 % grab; brightest channel per pixel (0..255) into `out`, size into w/h
    bool Grab(IMadVRFrameGrabber* grabber, DWORD flags, std::vector<BYTE>& out, int& w, int& h)
    {
        LPVOID dib = nullptr;
        if (FAILED(grabber->GrabFrame(ZOOM_25_PERCENT, flags, CHROMA_UPSCALING_BILINEAR, IMAGE_DOWNSCALING_BILINEAR,
                                      IMAGE_UPSCALING_BILINEAR, 0, &dib, nullptr)) || !dib) {
            return false;
        }
        const BITMAPINFOHEADER* bih = static_cast<const BITMAPINFOHEADER*>(dib);
        w = bih->biWidth;
        h = std::abs(bih->biHeight);
        const int bpp = bih->biBitCount / 8;
        const BYTE* px = static_cast<const BYTE*>(dib) + bih->biSize;
        const int stride = ((w * bpp) + 3) & ~3;
        out.resize(static_cast<size_t>(w) * h);
        if (bpp >= 3) {
            for (int y = 0; y < h; y++) {
                const BYTE* row = px + static_cast<size_t>(y) * stride;
                for (int x = 0; x < w; x++) {
                    const BYTE* p = row + x * bpp;
                    out[static_cast<size_t>(y) * w + x] = std::max({ p[0], p[1], p[2] });
                }
            }
        }
        LocalFree(dib);
        return bpp >= 3;
    }

    void GrabLoop(CComPtr<IUnknown> madVr)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            CComQIPtr<IMadVRFrameGrabber> grabber = madVr;
            CComQIPtr<IMadVRInfo> info = madVr;
            CComQIPtr<IQualProp, &IID_IQualProp> qual = madVr;
            std::vector<BYTE> in, out;
            while (!g_stop) {
                bool hdrOut = false;
                if (info) {
                    info->GetBool("hdrOutput", &hdrOut);
                }
                g_hdrOutput = hdrOut;
                int wi = 0, hi = 0, wo = 0, ho = 0;
                const DWORD common = FLAGS_NO_SUBTITLES | FLAGS_NO_CUSTOM_SHADERS;
                const bool gotIn = grabber && Grab(grabber, common | FLAGS_NO_HDR_SDR_CONVERSION, in, wi, hi);
                // the tone-mapped output of the same moment (in HDR passthrough nothing is tone mapped)
                const bool gotOut = gotIn && !hdrOut && Grab(grabber, common, out, wo, ho) && wo == wi && ho == hi;

                TvScopeData::Hist hist = {};
                float curveSum[TvScopeData::kCurveBins] = {};
                int curveN[TvScopeData::kCurveBins] = {};
                bool sdrSource = false;
                if (gotIn) {
                    // An SDR file isn't converted at all: both grabs come out the same. Its input is then
                    // gamma (100 nits reference white), not PQ.
                    if (gotOut) {
                        double diff = 0;
                        for (size_t i = 0; i < in.size(); i += 7) {
                            diff += std::abs(static_cast<int>(in[i]) - static_cast<int>(out[i]));
                        }
                        sdrSource = diff / std::max<size_t>(1, in.size() / 7) < 1.0;
                    }
                    float lut[256];
                    for (int c = 0; c < 256; c++) {
                        lut[c] = sdrSource ? 100.0f * std::pow(c / 255.0f, 2.4f) : CTvScopes::PqToNits(c / 255.0f);
                    }
                    double sum = 0;
                    float peak = 0;
                    for (size_t i = 0; i < in.size(); i++) {
                        const float nits = lut[in[i]];
                        peak = std::max(peak, nits);
                        sum += nits;
                        if (gotOut && !sdrSource) {
                            const int bin = std::clamp(static_cast<int>(std::floor((std::log10(std::max(nits, 0.01f)) + 2.0f) * 8.0f)), 0,
                                                       TvScopeData::kCurveBins - 1);
                            curveSum[bin] += out[i] / 255.0f;
                            curveN[bin]++;
                        }
                    }
                    hist.inPeak = peak;
                    hist.inAvg = static_cast<float>(sum / std::max<size_t>(1, in.size()));
                }
                hist.t = GetTickCount64();
                hist.outPeak = g_lastOutPeak;
                hist.outAvg = g_lastOutAvg;
                int drawn = 0, dropped = 0;
                if (qual) {
                    qual->get_FramesDrawn(&drawn);
                    qual->get_FramesDroppedInRenderer(&dropped);
                }
                {
                    std::lock_guard<std::mutex> lock(g_lock);
                    g_data.hdrOutput = hdrOut;
                    g_data.framesDrawn = drawn;
                    g_data.framesDropped = dropped;
                    if (!grabber) {
                        g_data.problem = L"This madVR can't grab frames (no IMadVRFrameGrabber)";
                    }
                    if (gotIn) {
                        g_data.haveInput = true;
                        g_data.inPeakNits = hist.inPeak;
                        g_data.inAvgNits = hist.inAvg;
                        g_data.inMaxSeenNits = std::max(g_data.inMaxSeenNits, hist.inPeak);
                        g_data.haveCurve = gotOut && !sdrSource;
                        for (int b = 0; b < TvScopeData::kCurveBins; b++) {
                            g_data.curveOut[b] = curveN[b] ? curveSum[b] / curveN[b] : -1.0f;
                        }
                        g_data.history.push_back(hist);
                        while (!g_data.history.empty() && hist.t - g_data.history.front().t > 60000) {
                            g_data.history.pop_front();
                        }
                    }
                    g_data.version = ++g_version;
                }
                for (int i = 0; i < 7 && !g_stop; i++) {
                    Sleep(100);
                }
            }
        }
        CoUninitialize();
    }
}

float CTvScopes::PqToNits(float code)
{
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::clamp(code, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

float CTvScopes::NitsToPq(float nits)
{
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

float CTvScopes::CurveBinNits(int bin)
{
    return std::pow(10.0f, (bin + 0.5f) / 8.0f - 2.0f);
}

void CTvScopes::Start(IUnknown* madVr)
{
    if (g_madVr || !madVr) {
        return;
    }
    g_madVr = CComQIPtr<IMadVROsdServices>(madVr);
    if (!g_madVr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_data = TvScopeData();
        g_data.version = ++g_version;
    }
    g_stop = false;
    g_callback = new CScopeCallback();
    if (FAILED(g_madVr->OsdSetRenderCallback(kCallbackName, g_callback, nullptr))) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_data.problem = L"madVR didn't accept the frame callback";
    }
    g_thread = std::thread(GrabLoop, CComPtr<IUnknown>(g_madVr));
}

void CTvScopes::Stop()
{
    if (!g_madVr) {
        return;
    }
    g_stop = true;
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_madVr->OsdSetRenderCallback(kCallbackName, nullptr, nullptr);
    if (g_callback) {
        g_callback->Release();
        g_callback = nullptr;
    }
    g_madVr.Release();
}

bool CTvScopes::Running()
{
    return !!g_madVr;
}

unsigned CTvScopes::Version()
{
    return g_version;
}

void CTvScopes::Get(TvScopeData& out)
{
    std::lock_guard<std::mutex> lock(g_lock);
    out = g_data;
}
