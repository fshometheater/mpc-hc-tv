/*
 * TV mode for MPC-HC (personal fork): the few NVAPI declarations used to show the
 * display signal format (RGB / YCbCr, bits per component) in the info panel.
 *
 * nvapi64.dll ships with every NVIDIA driver and is loaded at run time, so the build
 * needs no NVIDIA SDK or library, and PCs without an NVIDIA GPU simply skip it.
 *
 * The values, structure layouts and interface IDs below are taken from NVIDIA's NVAPI
 * headers (https://github.com/NVIDIA/nvapi: nvapi.h, nvapi_interface.h,
 * nvapi_lite_common.h), which are under this license:
 *
 * SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

namespace TvNvApi
{
    typedef unsigned long NvU32;
    typedef unsigned short NvU16;
    typedef unsigned char NvU8;
    typedef int NvAPI_Status; // NVAPI_OK = 0

    // nvapi_interface.h
    constexpr NvU32 kId_NvAPI_Initialize = 0x0150e828;
    constexpr NvU32 kId_NvAPI_Disp_ColorControl = 0x92f9d80d;
    constexpr NvU32 kId_NvAPI_DISP_GetDisplayIdByDisplayName = 0xae457190;

    // nvapi.h
    enum NV_COLOR_CMD { NV_COLOR_CMD_GET = 1 };
    enum NV_COLOR_FORMAT { NV_COLOR_FORMAT_RGB = 0, NV_COLOR_FORMAT_YUV422, NV_COLOR_FORMAT_YUV444, NV_COLOR_FORMAT_YUV420 };
    enum NV_COLOR_COLORIMETRY {
        NV_COLOR_COLORIMETRY_RGB = 0,
        NV_COLOR_COLORIMETRY_YCC601,
        NV_COLOR_COLORIMETRY_YCC709,
        NV_COLOR_COLORIMETRY_XVYCC601,
        NV_COLOR_COLORIMETRY_XVYCC709,
        NV_COLOR_COLORIMETRY_SYCC601,
        NV_COLOR_COLORIMETRY_ADOBEYCC601,
        NV_COLOR_COLORIMETRY_ADOBERGB,
        NV_COLOR_COLORIMETRY_BT2020RGB,
        NV_COLOR_COLORIMETRY_BT2020YCC,
        NV_COLOR_COLORIMETRY_BT2020cYCC,
    };
    enum NV_DYNAMIC_RANGE { NV_DYNAMIC_RANGE_VESA = 0, NV_DYNAMIC_RANGE_CEA = 1, NV_DYNAMIC_RANGE_AUTO = 0xFF };
    enum NV_BPC { NV_BPC_DEFAULT = 0, NV_BPC_6, NV_BPC_8, NV_BPC_10, NV_BPC_12, NV_BPC_16 };

    // NV_COLOR_DATA_V3 (the first version with bpc); newer drivers accept it too.
    // nvapi.h packs structures at 8 bytes, which is the natural layout here.
    struct NV_COLOR_DATA_V3 {
        NvU32 version;
        NvU16 size;
        NvU8 cmd;
        struct {
            NvU8 colorFormat;
            NvU8 colorimetry;
            NvU8 dynamicRange;
            int bpc; // NV_BPC
        } data;
    };
    static_assert(sizeof(NV_COLOR_DATA_V3) == 16, "NV_COLOR_DATA_V3 layout");
    // MAKE_NVAPI_VERSION(typeName, ver) = sizeof(typeName) | (ver << 16)
    constexpr NvU32 NV_COLOR_DATA_VER3 = static_cast<NvU32>(sizeof(NV_COLOR_DATA_V3) | (3 << 16));

    typedef void* (__cdecl* PQueryInterface)(NvU32 id);
    typedef NvAPI_Status(__cdecl* PInitialize)();
    typedef NvAPI_Status(__cdecl* PDispColorControl)(NvU32 displayId, NV_COLOR_DATA_V3* pColorData);
    typedef NvAPI_Status(__cdecl* PGetDisplayIdByDisplayName)(const char* displayName, NvU32* displayId);
}
