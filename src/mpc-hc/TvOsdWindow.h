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

#include <d3d9.h>
#include <mvrInterfaces.h>
#include <map>
#include <string>

// The TV overlay drawn in its own windows on top of the video, instead of inside madVR.
// It offers the same small interface the overlay uses with madVR (IMadVROsdServices: put a
// bitmap at a position, remove it, tell the output size), so the overlay code doesn't care.
//
// Each element ("MPC-HC.TvMenu", the bar preview, ...) becomes a layered, click-through,
// non-activating popup window owned by the player, placed over the video window. Windows
// composes them at the display's refresh rate, whatever the video frame rate or renderer.
// Limits: not visible in exclusive / D3D fullscreen modes; in HDR output Windows shows them
// at its SDR brightness.
class CTvOsdWindow : public IMadVROsdServices
{
public:
    explicit CTvOsdWindow(HWND videoWnd);
    virtual ~CTvOsdWindow();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMadVROsdServices
    STDMETHODIMP OsdSetBitmap(LPCSTR name, HBITMAP leftEye, HBITMAP rightEye, COLORREF colorKey, int posX, int posY,
                              bool posRelativeToVideoRect, int zOrder, DWORD duration, DWORD flags,
                              OSDMOUSECALLBACK callback, LPVOID callbackContext, LPVOID reserved) override;
    STDMETHODIMP OsdGetVideoRects(RECT* fullOutputRect, RECT* activeVideoRect) override;
    STDMETHODIMP OsdSetRenderCallback(LPCSTR name, IOsdRenderCallback* callback, LPVOID reserved) override;
    STDMETHODIMP OsdRedrawFrame() override;

    HWND VideoWindow() const { return m_videoWnd; }
    void RemoveAll();

private:
    struct Element {
        HWND hwnd = nullptr;
        int zOrder = 0;
        HBITMAP hbm = nullptr; // premultiplied copy shown by the window
        BYTE* bits = nullptr;
        int w = 0, h = 0;
    };
    void Restack();
    static HWND CreateLayer(HWND owner);

    LONG m_ref = 1;
    HWND m_videoWnd = nullptr;
    std::map<std::string, Element> m_elems;
};
