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
#include "TvProbe.h"
#include <d3d9.h>
#include <mvrInterfaces.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>

namespace
{
    std::mutex g_logLock;

    void Log(const char* fmt, ...)
    {
        std::lock_guard<std::mutex> lock(g_logLock);
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"mpc-hc-tv-probe.log");
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path, L"a") == 0 && fp) {
            fprintf(fp, "%llu ", GetTickCount64());
            va_list args;
            va_start(args, fmt);
            vfprintf(fp, fmt, args);
            va_end(args);
            fprintf(fp, "\n");
            fclose(fp);
        }
    }

    double Ms(LARGE_INTEGER a, LARGE_INTEGER b)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    }

    float Half(WORD h)
    {
        const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
        const float s = (h & 0x8000) ? -1.0f : 1.0f;
        if (e == 0) {
            return s * m / 16777216.0f; // 2^-24
        }
        if (e == 31) {
            return s * 65504.0f;
        }
        return s * std::ldexp(1.0f + m / 1024.0f, e - 15);
    }

    // Brightest channel of every pixel of a locked surface: its maximum and mean (0..1, or above
    // for float formats)
    void ScanSurface(D3DFORMAT fmt, const BYTE* bits, int pitch, int w, int h, float& maxV, float& meanV)
    {
        double sum = 0.0;
        maxV = 0.0f;
        for (int y = 0; y < h; y++) {
            const BYTE* row = bits + static_cast<size_t>(y) * pitch;
            for (int x = 0; x < w; x++) {
                float v = 0.0f;
                if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
                    const BYTE* p = row + x * 4;
                    v = std::max({ p[0], p[1], p[2] }) / 255.0f;
                } else if (fmt == D3DFMT_A2R10G10B10 || fmt == D3DFMT_A2B10G10R10) {
                    const DWORD d = reinterpret_cast<const DWORD*>(row)[x];
                    v = std::max({ d & 0x3FF, (d >> 10) & 0x3FF, (d >> 20) & 0x3FF }) / 1023.0f;
                } else if (fmt == D3DFMT_A16B16G16R16F) {
                    const WORD* p = reinterpret_cast<const WORD*>(row) + x * 4;
                    v = std::max({ Half(p[0]), Half(p[1]), Half(p[2]) });
                } else if (fmt == D3DFMT_A16B16G16R16) {
                    const WORD* p = reinterpret_cast<const WORD*>(row) + x * 4;
                    v = std::max({ p[0], p[1], p[2] }) / 65535.0f;
                }
                maxV = std::max(maxV, v);
                sum += v;
            }
        }
        meanV = static_cast<float>(sum / std::max(1, w * h));
    }

    // Route 1: madVR's OSD render callback. After madVR is done with a frame, its render target is
    // shrunk on the GPU into a small copy; the copy made one frame earlier is read back (so the GPU
    // has had a frame to finish it) and scanned.
    class CProbeCallback : public IOsdRenderCallback
    {
        LONG m_ref = 1;
        CComPtr<IDirect3DDevice9> m_dev;
        CComPtr<IDirect3DSurface9> m_small[2], m_sys[2];
        D3DFORMAT m_fmt = D3DFMT_UNKNOWN;
        int m_idx = 0;
        bool m_havePrev = false;
        static constexpr int kW = 480, kH = 270;
        // per-batch statistics
        int m_frames = 0, m_failures = 0;
        double m_sumStretch = 0, m_sumRead = 0, m_sumScan = 0, m_maxTotal = 0;
        float m_lastMax = 0, m_lastMean = 0;
        UINT m_rtW = 0, m_rtH = 0;

        void ReleaseSurfaces()
        {
            for (auto& s : m_small) {
                s.Release();
            }
            for (auto& s : m_sys) {
                s.Release();
            }
            m_havePrev = false;
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
            Log("callback SetDevice %s", dev ? "device given (D3D9)" : "NULL (release resources)");
            return S_OK;
        }

        STDMETHODIMP ClearBackground(LPCSTR, REFERENCE_TIME, RECT*, RECT*) override { return CALLBACK_EMPTY; }

        STDMETHODIMP RenderOsd(LPCSTR, REFERENCE_TIME, RECT* fullOutputRect, RECT* activeVideoRect) override
        {
            if (!m_dev) {
                return CALLBACK_EMPTY;
            }
            LARGE_INTEGER t0, t1, t2, t3;
            QueryPerformanceCounter(&t0);
            CComPtr<IDirect3DSurface9> rt;
            D3DSURFACE_DESC desc = {};
            if (FAILED(m_dev->GetRenderTarget(0, &rt)) || !rt || FAILED(rt->GetDesc(&desc))) {
                m_failures++;
                return CALLBACK_EMPTY;
            }
            if (desc.Format != m_fmt || !m_small[0]) {
                ReleaseSurfaces();
                m_fmt = desc.Format;
                m_rtW = desc.Width;
                m_rtH = desc.Height;
                bool ok = true;
                for (int i = 0; i < 2; i++) {
                    ok = ok && SUCCEEDED(m_dev->CreateRenderTarget(kW, kH, m_fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &m_small[i], nullptr));
                    ok = ok && SUCCEEDED(m_dev->CreateOffscreenPlainSurface(kW, kH, m_fmt, D3DPOOL_SYSTEMMEM, &m_sys[i], nullptr));
                }
                Log("render target %ux%u format %d (21 A8R8G8B8, 22 X8R8G8B8, 35 A2R10G10B10, 31 A2B10G10R10, 113 A16B16G16R16F) "
                    "multisample %d; small copies %s; fullOutputRect %ldx%ld activeVideoRect %ld,%ld-%ld,%ld",
                    desc.Width, desc.Height, static_cast<int>(desc.Format), static_cast<int>(desc.MultiSampleType), ok ? "created" : "FAILED",
                    fullOutputRect ? fullOutputRect->right - fullOutputRect->left : -1, fullOutputRect ? fullOutputRect->bottom - fullOutputRect->top : -1,
                    activeVideoRect ? activeVideoRect->left : -1, activeVideoRect ? activeVideoRect->top : -1,
                    activeVideoRect ? activeVideoRect->right : -1, activeVideoRect ? activeVideoRect->bottom : -1);
                if (!ok) {
                    ReleaseSurfaces();
                    m_failures++;
                    return CALLBACK_EMPTY;
                }
            }
            // shrink this frame's video area into the current small copy
            RECT src = activeVideoRect ? *activeVideoRect : RECT { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
            src.left = std::clamp<LONG>(src.left, 0, desc.Width);
            src.right = std::clamp<LONG>(src.right, src.left, desc.Width);
            src.top = std::clamp<LONG>(src.top, 0, desc.Height);
            src.bottom = std::clamp<LONG>(src.bottom, src.top, desc.Height);
            const HRESULT hrS = (src.right > src.left && src.bottom > src.top)
                                ? m_dev->StretchRect(rt, &src, m_small[m_idx], nullptr, D3DTEXF_LINEAR) : E_FAIL;
            QueryPerformanceCounter(&t1);
            // read back the copy made one frame ago
            float maxV = -1, meanV = -1;
            const int prev = m_idx ^ 1;
            if (m_havePrev && SUCCEEDED(m_dev->GetRenderTargetData(m_small[prev], m_sys[prev]))) {
                QueryPerformanceCounter(&t2);
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(m_sys[prev]->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                    ScanSurface(m_fmt, static_cast<const BYTE*>(lr.pBits), lr.Pitch, kW, kH, maxV, meanV);
                    m_sys[prev]->UnlockRect();
                }
            } else {
                QueryPerformanceCounter(&t2);
            }
            QueryPerformanceCounter(&t3);
            m_havePrev = SUCCEEDED(hrS);
            m_idx = prev;
            if (FAILED(hrS)) {
                m_failures++;
            }
            m_frames++;
            m_sumStretch += Ms(t0, t1);
            m_sumRead += Ms(t1, t2);
            m_sumScan += Ms(t2, t3);
            m_maxTotal = std::max(m_maxTotal, Ms(t0, t3));
            if (maxV >= 0) {
                m_lastMax = maxV;
                m_lastMean = meanV;
            }
            if (m_frames == 120) {
                Log("callback: 120 frames, per frame avg ms: shrink %.3f, readback %.3f, scan %.3f; worst total %.3f; failures %d; "
                    "last frame brightest channel max %.4f mean %.4f",
                    m_sumStretch / 120, m_sumRead / 120, m_sumScan / 120, m_maxTotal, m_failures, m_lastMax, m_lastMean);
                m_frames = m_failures = 0;
                m_sumStretch = m_sumRead = m_sumScan = m_maxTotal = 0;
            }
            return CALLBACK_EMPTY;
        }
    };

    CComPtr<IUnknown> g_madVr;
    CProbeCallback* g_callback = nullptr;
    std::thread g_grabThread;
    std::atomic<bool> g_stop { false };

    void LogDropped(const char* when)
    {
        if (CComQIPtr<IQualProp, &IID_IQualProp> q = g_madVr) {
            int dropped = 0, drawn = 0;
            q->get_FramesDroppedInRenderer(&dropped);
            q->get_FramesDrawn(&drawn);
            Log("%s: madVR frames drawn %d, dropped %d", when, drawn, dropped);
        }
    }

    // Route 2: IMadVRFrameGrabber at 25 %, alternating "no HDR -> SDR conversion" (input side) and
    // madVR's normal processing, every 3 s
    void GrabLoop(CComPtr<IUnknown> madVr)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            CComQIPtr<IMadVRFrameGrabber> grabber = madVr;
            if (!grabber) {
                Log("grab: madVR has no IMadVRFrameGrabber");
            }
            for (int n = 0; grabber && !g_stop && n < 8; n++) {
                for (int w = 0; w < 30 && !g_stop; w++) {
                    Sleep(100);
                }
                if (g_stop) {
                    break;
                }
                const bool noConversion = (n % 2 == 0);
                const DWORD flags = FLAGS_NO_SUBTITLES | (noConversion ? FLAGS_NO_HDR_SDR_CONVERSION : 0);
                LPVOID dib = nullptr;
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                const HRESULT hr = grabber->GrabFrame(ZOOM_25_PERCENT, flags, CHROMA_UPSCALING_BILINEAR, IMAGE_DOWNSCALING_BILINEAR,
                                                      IMAGE_UPSCALING_BILINEAR, 0, &dib, nullptr);
                QueryPerformanceCounter(&t1);
                if (SUCCEEDED(hr) && dib) {
                    const BITMAPINFOHEADER* bih = static_cast<const BITMAPINFOHEADER*>(dib);
                    const int w = bih->biWidth, h = std::abs(bih->biHeight), bpp = bih->biBitCount;
                    const BYTE* px = static_cast<const BYTE*>(dib) + bih->biSize;
                    const int stride = ((w * bpp / 8) + 3) & ~3;
                    int maxC = 0;
                    double sum = 0;
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            const BYTE* p = px + static_cast<size_t>(y) * stride + x * (bpp / 8);
                            const int c = std::max({ p[0], p[1], p[2] });
                            maxC = std::max(maxC, c);
                            sum += c;
                        }
                    }
                    Log("grab %s: %.1f ms, %dx%d %d bpp, brightest channel max %d mean %.1f (0-255)",
                        noConversion ? "WITHOUT HDR->SDR conversion (input side)" : "with madVR's normal processing",
                        Ms(t0, t1), w, h, bpp, maxC, sum / std::max(1, w * h));
                    LocalFree(dib);
                } else {
                    Log("grab %s: FAILED hr=0x%08lX after %.1f ms", noConversion ? "without conversion" : "normal", hr, Ms(t0, t1));
                }
                LogDropped("after grab");
            }
        }
        CoUninitialize();
    }
}

void CTvProbe::Start(IFilterGraph* pGraph)
{
    if (GetEnvironmentVariableW(L"MPCHC_TV_PROBE", nullptr, 0) == 0 || !pGraph || g_madVr) {
        return;
    }
    CComPtr<IEnumFilters> pEnum;
    if (FAILED(pGraph->EnumFilters(&pEnum))) {
        return;
    }
    for (CComPtr<IBaseFilter> pBF; pEnum->Next(1, &pBF, nullptr) == S_OK; pBF.Release()) {
        if (CComQIPtr<IMadVROsdServices>(pBF)) {
            g_madVr = pBF;
            break;
        }
    }
    if (!g_madVr) {
        Log("probe: madVR is not in the graph (renderer isn't madVR)");
        return;
    }
    bool hdrOut = false;
    CStringW version;
    if (CComQIPtr<IMadVRInfo> info = g_madVr) {
        info->GetBool("hdrOutput", &hdrOut);
        LPWSTR v = nullptr;
        int chars = 0;
        if (SUCCEEDED(info->GetString("version", &v, &chars)) && v) {
            version = v;
            LocalFree(v);
        }
    }
    Log("probe start: madVR %S, hdrOutput (HDR sent to the display) = %d", version.GetString(), hdrOut ? 1 : 0);
    LogDropped("start");
    if (CComQIPtr<IMadVROsdServices> osd = g_madVr) {
        g_callback = new CProbeCallback();
        const HRESULT hr = osd->OsdSetRenderCallback("MPC-HC.TvProbe", g_callback, nullptr);
        Log("OsdSetRenderCallback: hr=0x%08lX", hr);
    }
    g_stop = false;
    g_grabThread = std::thread(GrabLoop, g_madVr);
}

void CTvProbe::Stop()
{
    if (!g_madVr) {
        return;
    }
    g_stop = true;
    if (g_grabThread.joinable()) {
        g_grabThread.join();
    }
    LogDropped("stop");
    if (CComQIPtr<IMadVROsdServices> osd = g_madVr) {
        osd->OsdSetRenderCallback("MPC-HC.TvProbe", nullptr, nullptr);
    }
    if (g_callback) {
        g_callback->Release();
        g_callback = nullptr;
    }
    g_madVr.Release();
    Log("probe stop");
}
