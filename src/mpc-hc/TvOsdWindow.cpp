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
#include "TvOsdWindow.h"
#include <vector>
#include <algorithm>

namespace
{
    const wchar_t kClass[] = L"MPC-HC.TvOverlayLayer";

    LRESULT CALLBACK LayerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
            case WM_NCHITTEST:
                return HTTRANSPARENT; // clicks go to the video window underneath
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            default:
                return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    void RegisterLayerClass()
    {
        static bool done = false;
        if (done) {
            return;
        }
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LayerProc;
        wc.hInstance = AfxGetInstanceHandle();
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        done = true;
    }
}

CTvOsdWindow::CTvOsdWindow(HWND videoWnd)
    : m_videoWnd(videoWnd)
{
    RegisterLayerClass();
}

CTvOsdWindow::~CTvOsdWindow()
{
    RemoveAll();
}

STDMETHODIMP CTvOsdWindow::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == __uuidof(IMadVROsdServices)) {
        *ppv = static_cast<IMadVROsdServices*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CTvOsdWindow::AddRef()
{
    return InterlockedIncrement(&m_ref);
}

STDMETHODIMP_(ULONG) CTvOsdWindow::Release()
{
    const LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        delete this;
    }
    return r;
}

HWND CTvOsdWindow::CreateLayer(HWND owner)
{
    // Layered (per-pixel alpha), click-through, never activated, not in the taskbar; owned by
    // the player's main window so it stays above it (and goes away / minimises with it).
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP, 0, 0,
                           1, 1, owner, nullptr, AfxGetInstanceHandle(), nullptr);
}

void CTvOsdWindow::RemoveAll()
{
    for (auto& kv : m_elems) {
        if (kv.second.hwnd) {
            DestroyWindow(kv.second.hwnd);
        }
        if (kv.second.hbm) {
            DeleteObject(kv.second.hbm);
        }
    }
    m_elems.clear();
}

// Higher zOrder on top: walk from the lowest and put each one above the previous.
void CTvOsdWindow::Restack()
{
    std::vector<Element*> order;
    for (auto& kv : m_elems) {
        if (kv.second.hwnd && IsWindowVisible(kv.second.hwnd)) {
            order.push_back(&kv.second);
        }
    }
    std::sort(order.begin(), order.end(), [](const Element* a, const Element* b) { return a->zOrder > b->zOrder; });
    HWND after = HWND_TOP;
    for (Element* e : order) { // topmost first, each one below the previous
        SetWindowPos(e->hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        after = e->hwnd;
    }
}

STDMETHODIMP CTvOsdWindow::OsdSetBitmap(LPCSTR name, HBITMAP leftEye, HBITMAP /*rightEye*/, COLORREF /*colorKey*/, int posX,
                                        int posY, bool /*posRelativeToVideoRect*/, int zOrder, DWORD /*duration*/, DWORD /*flags*/,
                                        OSDMOUSECALLBACK /*callback*/, LPVOID /*callbackContext*/, LPVOID /*reserved*/)
{
    if (!name) {
        return E_INVALIDARG;
    }
    auto it = m_elems.find(name);
    if (!leftEye) {
        if (it != m_elems.end()) {
            if (it->second.hwnd) {
                DestroyWindow(it->second.hwnd);
            }
            if (it->second.hbm) {
                DeleteObject(it->second.hbm);
            }
            m_elems.erase(it);
        }
        return S_OK;
    }
    DIBSECTION ds = {};
    if (GetObjectW(leftEye, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32) {
        return E_INVALIDARG; // the overlay only hands over 32-bit DIB sections
    }
    const int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    // The overlay always hands over top-down DIB sections (first row = top). GetObject doesn't
    // reliably say so (it can report a positive height for them), so don't ask it.
    const bool topDown = true;
    HWND owner = GetAncestor(m_videoWnd, GA_ROOT);
    Element& e = m_elems[name];
    const bool isNew = !e.hwnd;
    if (isNew) {
        e.hwnd = CreateLayer(owner);
        if (!e.hwnd) {
            m_elems.erase(name);
            return E_FAIL;
        }
    }
    if (e.w != w || e.h != h || !e.hbm) {
        if (e.hbm) {
            DeleteObject(e.hbm);
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        e.hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&e.bits), nullptr, 0);
        e.w = w;
        e.h = h;
        if (!e.hbm) {
            return E_OUTOFMEMORY;
        }
    }
    // The overlay's bitmaps have straight alpha; layered windows want it premultiplied.
    GdiFlush();
    const BYTE* src = static_cast<const BYTE*>(ds.dsBm.bmBits);
    for (int y = 0; y < h; y++) {
        const BYTE* in = src + static_cast<size_t>(topDown ? y : h - 1 - y) * ds.dsBm.bmWidthBytes;
        DWORD* out = reinterpret_cast<DWORD*>(e.bits + static_cast<size_t>(y) * w * 4);
        // panels are mostly long runs of the same background pixel: reuse the last result
        DWORD lastIn = ~*reinterpret_cast<const DWORD*>(in), lastOut = 0;
        for (int x = 0; x < w; x++, in += 4) {
            const DWORD v = *reinterpret_cast<const DWORD*>(in);
            if (v != lastIn) {
                lastIn = v;
                const DWORD a = in[3];
                if (a == 0) {
                    lastOut = 0;
                } else if (a == 255) {
                    lastOut = 0xFF000000 | (static_cast<DWORD>(in[2]) << 16) | (static_cast<DWORD>(in[1]) << 8) | in[0];
                } else {
                    lastOut = (a << 24) | (((in[2] * a + 127) / 255) << 16) | (((in[1] * a + 127) / 255) << 8) | ((in[0] * a + 127) / 255);
                }
            }
            out[x] = lastOut;
        }
    }
    POINT origin = { posX, posY };
    ClientToScreen(m_videoWnd, &origin);
    SIZE size = { w, h };
    POINT zero = { 0, 0 };
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, e.hbm);
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const BOOL ok = UpdateLayeredWindow(e.hwnd, screen, &origin, &size, mem, &zero, 0, &blend, ULW_ALPHA);
    if (GetEnvironmentVariableW(L"MPCHC_TV_PERF", nullptr, 0) > 0) { // timing log: did Windows take it?
        const DWORD err = ok ? 0 : GetLastError();
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"mpc-hc-tv-perf.log");
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path, L"a") == 0 && fp) {
            BYTE amax = 0;
            for (int i = 0; i < w * h; i += 997) {
                amax = std::max(amax, e.bits[i * 4 + 3]);
            }
            fprintf(fp, "%llu layer %s ok=%d err=%lu at=%ld,%ld size=%dx%d maxAlpha=%u\n", GetTickCount64(), name, ok, err, origin.x,
                    origin.y, w, h, amax);
            fclose(fp);
        }
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (isNew) {
        ShowWindow(e.hwnd, SW_SHOWNOACTIVATE);
    }
    if (isNew || e.zOrder != zOrder) {
        e.zOrder = zOrder;
        Restack();
    }
    return ok ? S_OK : E_FAIL;
}

STDMETHODIMP CTvOsdWindow::OsdGetVideoRects(RECT* fullOutputRect, RECT* activeVideoRect)
{
    RECT r = {};
    if (!m_videoWnd || !GetClientRect(m_videoWnd, &r)) {
        return E_FAIL;
    }
    if (fullOutputRect) {
        *fullOutputRect = r;
    }
    if (activeVideoRect) {
        *activeVideoRect = r;
    }
    return S_OK;
}

STDMETHODIMP CTvOsdWindow::OsdSetRenderCallback(LPCSTR /*name*/, IOsdRenderCallback* /*callback*/, LPVOID /*reserved*/)
{
    return E_NOTIMPL;
}

STDMETHODIMP CTvOsdWindow::OsdRedrawFrame()
{
    return S_OK;
}
