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
#include "TvInfo.h"
#include "TvTracks.h"
#include "TvNvApi.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "MediaInfo/MediaInfoDLL.h"
#include "LAVFilters/src/include/LAVVideoSettings.h"
#include "LAVFilters/src/include/LAVAudioSettings.h"
#include <mvrInterfaces.h>
#include <dxgi1_6.h>
#include <wininet.h>
#include <atomic>
#include <map>
#include <memory>
#include <thread>

// Keep this file ASCII-only (see TvMenu.cpp).

namespace
{
    // ------------------------------------------------------------------
    // MediaInfo snapshot, built on a background thread

    struct MiStream {
        std::map<std::wstring, std::wstring> fields; // MediaInfo field name -> value
        std::vector<std::pair<std::wstring, std::wstring>> complete; // MediaInfo's own "complete" text view

        CStringW Get(LPCWSTR name) const {
            auto it = fields.find(name);
            return it == fields.end() ? CStringW() : CStringW(it->second.c_str());
        }
        // The human-readable "/String" variant when there is one.
        CStringW Text(LPCWSTR name) const {
            CStringW s = Get(CStringW(name) + L"/String");
            return s.IsEmpty() ? Get(name) : s;
        }
    };

    struct MiSnapshot {
        CStringW error;
        MiStream general;
        std::vector<MiStream> video, audio;
    };

    struct MiJob {
        CStringW path;
        std::atomic<bool> done { false };
        MiSnapshot snap;
    };

    std::shared_ptr<MiJob> g_job; // UI thread only

    void ReadStreams(MediaInfoDLL::MediaInfo& mi, MediaInfoDLL::stream_t kind, std::vector<MiStream>& out)
    {
        const size_t n = mi.Count_Get(kind);
        for (size_t i = 0; i < n; i++) {
            MiStream s;
            const size_t params = mi.Count_Get(kind, i);
            for (size_t p = 0; p < params; p++) {
                std::wstring name = mi.Get(kind, i, p, MediaInfoDLL::Info_Name);
                std::wstring value = mi.Get(kind, i, p, MediaInfoDLL::Info_Text);
                if (!name.empty() && !value.empty()) {
                    s.fields[name] = value;
                }
            }
            out.push_back(std::move(s));
        }
    }

    // Splits MediaInfo's text report into its sections ("General", "Video", "Audio #2", ...)
    // and hands the "name : value" lines of each to the matching stream, in order.
    void ParseComplete(const std::wstring& report, MiSnapshot& snap)
    {
        enum class Sec { None, General, Video, Audio, Other } sec = Sec::None;
        size_t videoIdx = 0, audioIdx = 0;
        MiStream* target = nullptr;
        size_t start = 0;
        while (start <= report.size()) {
            size_t end = report.find(L'\n', start);
            if (end == std::wstring::npos) {
                end = report.size();
            }
            std::wstring line = report.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            const size_t colon = line.find(L" : ");
            if (colon == std::wstring::npos) {
                // Section header
                if (line.rfind(L"General", 0) == 0) {
                    sec = Sec::General;
                    target = &snap.general;
                } else if (line.rfind(L"Video", 0) == 0) {
                    sec = Sec::Video;
                    target = videoIdx < snap.video.size() ? &snap.video[videoIdx] : nullptr;
                    videoIdx++;
                } else if (line.rfind(L"Audio", 0) == 0) {
                    sec = Sec::Audio;
                    target = audioIdx < snap.audio.size() ? &snap.audio[audioIdx] : nullptr;
                    audioIdx++;
                } else {
                    sec = Sec::Other;
                    target = nullptr;
                }
                continue;
            }
            if (!target) {
                continue;
            }
            std::wstring name = line.substr(0, colon);
            while (!name.empty() && name.back() == L' ') {
                name.pop_back();
            }
            target->complete.emplace_back(name, line.substr(colon + 3));
        }
        (void)sec;
    }

    bool IsWebAddress(const CStringW& path)
    {
        return path.Left(7).CompareNoCase(L"http://") == 0 || path.Left(8).CompareNoCase(L"https://") == 0;
    }

    // What may be shown on screen of a path or address: no query string (a media server's stream
    // address carries its access key there)
    CStringW WithoutQuery(const CStringW& path)
    {
        const int q = path.FindOneOf(L"?#");
        return q > 0 ? path.Left(q) : path;
    }

    // A media server (e.g. Emby) may hand the player a stream address instead of a file path, and
    // MPC-HC's MediaInfo.dll can't open web addresses. So the file is read here in pieces with HTTP
    // range requests and given to MediaInfo, which says where to read next. Limits keep it light on
    // the server: 1 MB pieces, at most 64 MB and 256 requests.
    bool OpenFromWeb(MediaInfoDLL::MediaInfo& mi, const CStringW& url, CStringW& error)
    {
        constexpr DWORD kChunk = 1u << 20;
        constexpr ULONGLONG kMaxBytes = 64ull << 20;
        constexpr int kMaxRequests = 256;
        HINTERNET net = InternetOpenW(L"MPC-HC TV", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (!net) {
            error = L"MediaInfo: no network access for " + WithoutQuery(url);
            return false;
        }
        DWORD timeout = 8000;
        InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        std::vector<BYTE> buf(kChunk);
        ULONGLONG total = 0;
        // Reads up to kChunk bytes from `from` into buf; false if the server can't do that
        auto readAt = [&](ULONGLONG from, DWORD& got) -> bool {
            got = 0;
            wchar_t range[80];
            swprintf_s(range, L"Range: bytes=%llu-%llu\r\n", from, from + kChunk - 1);
            HINTERNET f = InternetOpenUrlW(net, url, range, static_cast<DWORD>(-1),
                                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
            if (!f) {
                return false;
            }
            DWORD status = 0, len = sizeof(status);
            HttpQueryInfoW(f, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
            bool ok = status == 206 || (status == 200 && from == 0); // 200 later = the server ignores ranges
            if (ok && total == 0) {
                wchar_t text[128] = {};
                len = sizeof(text);
                if (status == 206 && HttpQueryInfoW(f, HTTP_QUERY_CONTENT_RANGE, text, &len, nullptr)) {
                    if (const wchar_t* slash = wcsrchr(text, L'/')) {
                        total = _wcstoui64(slash + 1, nullptr, 10); // "bytes 0-1048575/12345678"
                    }
                } else if (len = sizeof(text); HttpQueryInfoW(f, HTTP_QUERY_CONTENT_LENGTH, text, &len, nullptr)) {
                    total = _wcstoui64(text, nullptr, 10);
                }
            }
            while (ok && got < kChunk) {
                DWORD n = 0;
                if (!InternetReadFile(f, buf.data() + got, kChunk - got, &n) || n == 0) {
                    break;
                }
                got += n;
            }
            InternetCloseHandle(f);
            return ok && got > 0;
        };

        DWORD got = 0;
        if (!readAt(0, got)) {
            InternetCloseHandle(net);
            error = L"MediaInfo could not read the stream " + WithoutQuery(url);
            return false;
        }
        const MediaInfo_int64u size = total ? total : static_cast<MediaInfo_int64u>(-1);
        mi.Open_Buffer_Init(size, 0);
        ULONGLONG pos = 0, read = got;
        int requests = 1;
        for (;;) {
            const size_t state = mi.Open_Buffer_Continue(buf.data(), got);
            if (state & 0x08) {
                break; // MediaInfo has what it needs
            }
            const MediaInfo_int64u jump = mi.Open_Buffer_Continue_GoTo_Get();
            if (jump != static_cast<MediaInfo_int64u>(-1)) {
                pos = jump;
                mi.Open_Buffer_Init(size, pos); // it wants another part of the file
            } else {
                pos += got;
            }
            if ((total && pos >= total) || read >= kMaxBytes || requests >= kMaxRequests || !readAt(pos, got)) {
                break;
            }
            read += got;
            requests++;
        }
        mi.Open_Buffer_Finalize();
        InternetCloseHandle(net);
        return true;
    }

    // The file's name for showing: no folders, no query string, %20 etc. decoded, no extension
    // (a web address shows only its last path part, never the server or an access key)
    CStringW DisplayName(const CStringW& path)
    {
        CStringW name = WithoutQuery(path);
        name = name.Mid(std::max(name.ReverseFind(L'\\'), name.ReverseFind(L'/')) + 1);
        if (name.Find(L'%') >= 0) {
            DWORD n = name.GetLength() + 1;
            CStringW out;
            if (SUCCEEDED(UrlUnescapeW(name.GetBuffer(), out.GetBuffer(n), &n, 0))) {
                out.ReleaseBuffer();
                name.ReleaseBuffer();
                name = out;
            } else {
                out.ReleaseBuffer(0);
                name.ReleaseBuffer();
            }
        }
        if (name.ReverseFind(L'.') > 0) {
            name = name.Left(name.ReverseFind(L'.'));
        }
        return name;
    }

    void RunMediaInfo(std::shared_ptr<MiJob> job)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // never compete with playback
        MediaInfoDLL::MediaInfo mi;
        if (!mi.IsReady()) {
            // MediaInfo.dll is loaded from the player's own folder and needs all of its functions:
            // say whether it's missing there or a version without them (e.g. only the .exe copied)
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            CStringW dll(exe);
            dll = dll.Left(dll.ReverseFind(L'\\') + 1) + L"MediaInfo.dll";
            job->snap.error = (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
                              ? L"MediaInfo.dll is missing next to mpc-hc64.exe: copy it from the build folder"
                              : L"MediaInfo.dll next to mpc-hc64.exe can't be used (older or 32-bit): copy the one from the build folder";
        } else {
            mi.Option(L"ParseSpeed", L"0.5");
            mi.Option(L"Complete", L"1");
            const bool web = IsWebAddress(job->path);
            CStringW webError;
            if (web ? !OpenFromWeb(mi, job->path, webError) : mi.Open(std::wstring(job->path.GetString())) == 0) {
                // with the path, so a launcher's way of passing the file (a URL, a mapped share) is visible;
                // a web address without its query string (it can carry an access key)
                job->snap.error = web ? webError : L"MediaInfo could not read " + job->path;
            } else {
                std::vector<MiStream> general;
                ReadStreams(mi, MediaInfoDLL::Stream_General, general);
                if (!general.empty()) {
                    job->snap.general = std::move(general[0]);
                }
                ReadStreams(mi, MediaInfoDLL::Stream_Video, job->snap.video);
                ReadStreams(mi, MediaInfoDLL::Stream_Audio, job->snap.audio);
                ParseComplete(mi.Inform(), job->snap);
                mi.Close();
            }
        }
        job->done = true;
    }

    const MiSnapshot* Snapshot(const CStringW& path)
    {
        if (g_job && g_job->done && g_job->path == path) {
            return &g_job->snap;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Small helpers

    void Add(std::vector<TvInfoLine>& lines, LPCWSTR name, const CStringW& value)
    {
        if (!value.IsEmpty()) {
            lines.push_back({ name, value, false });
        }
    }

    void Header(std::vector<TvInfoLine>& lines, LPCWSTR name)
    {
        lines.push_back({ name, L"", true });
    }

    CStringW Join(const CStringW& a, const CStringW& b, LPCWSTR sep = L" ")
    {
        if (a.IsEmpty()) {
            return b;
        }
        if (b.IsEmpty()) {
            return a;
        }
        return a + sep + b;
    }

    template <class T>
    CComPtr<T> FindInGraph(IFilterGraph* pGraph)
    {
        CComPtr<IEnumFilters> pEnum;
        if (!pGraph || FAILED(pGraph->EnumFilters(&pEnum))) {
            return nullptr;
        }
        for (CComPtr<IBaseFilter> pBF; pEnum->Next(1, &pBF, nullptr) == S_OK; pBF.Release()) {
            CComQIPtr<T> p = pBF;
            if (p) {
                return CComPtr<T>(p);
            }
        }
        return nullptr;
    }

    CStringW ChannelLayoutName(int channels, DWORD mask)
    {
        CStringW s;
        if (channels <= 0) {
            return s;
        }
        const int lfe = (mask & SPEAKER_LOW_FREQUENCY) ? 1 : 0;
        s.Format(L"%d.%d", channels - lfe, lfe);
        return s;
    }

    // ------------------------------------------------------------------
    // Display

    struct DisplayInfo {
        CStringW deviceName;    // \\.\DISPLAY1
        CStringW friendlyName;  // e.g. "LG TV SSCR2"
        int width = 0, height = 0;
        double refresh = 0.0;
        int bitsPerColor = 0;
        DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RESERVED;
        float maxLuminance = 0.0f;
        bool haveDxgi = false;
    };

    CStringW FriendlyMonitorName(const CStringW& gdiDeviceName)
    {
        UINT32 nPaths = 0, nModes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) {
            return L"";
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) != ERROR_SUCCESS) {
            return L"";
        }
        for (UINT32 i = 0; i < nPaths; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || gdiDeviceName.CompareNoCase(source.viewGdiDeviceName) != 0) {
                continue;
            }
            DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = paths[i].targetInfo.adapterId;
            target.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
                return target.monitorFriendlyDeviceName;
            }
        }
        return L"";
    }

    DisplayInfo QueryDisplay(HWND hWnd)
    {
        DisplayInfo d;
        HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hMon, &mi)) {
            return d;
        }
        d.deviceName = mi.szDevice;
        d.friendlyName = FriendlyMonitorName(d.deviceName);

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            d.width = dm.dmPelsWidth;
            d.height = dm.dmPelsHeight;
            d.refresh = dm.dmDisplayFrequency;
        }

        // DXGI 1.6 output description: real signal bit depth and colour space (HDR10 or SDR).
        // Loaded at run time so no new import library is needed.
        typedef HRESULT(WINAPI * PCreateFactory)(REFIID, void**);
        if (HMODULE hDxgi = LoadLibraryW(L"dxgi.dll")) {
            if (auto pCreate = reinterpret_cast<PCreateFactory>(GetProcAddress(hDxgi, "CreateDXGIFactory1"))) {
                CComPtr<IDXGIFactory1> pFactory;
                if (SUCCEEDED(pCreate(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory)))) {
                    CComPtr<IDXGIAdapter1> pAdapter;
                    for (UINT a = 0; !d.haveDxgi && pFactory->EnumAdapters1(a, &pAdapter) != DXGI_ERROR_NOT_FOUND; a++, pAdapter.Release()) {
                        CComPtr<IDXGIOutput> pOutput;
                        for (UINT o = 0; pAdapter->EnumOutputs(o, &pOutput) != DXGI_ERROR_NOT_FOUND; o++, pOutput.Release()) {
                            CComQIPtr<IDXGIOutput6> pOutput6 = pOutput;
                            DXGI_OUTPUT_DESC1 desc = {};
                            if (pOutput6 && SUCCEEDED(pOutput6->GetDesc1(&desc)) && d.deviceName.CompareNoCase(desc.DeviceName) == 0) {
                                d.bitsPerColor = desc.BitsPerColor;
                                d.colorSpace = desc.ColorSpace;
                                d.maxLuminance = desc.MaxLuminance;
                                d.haveDxgi = true;
                                break;
                            }
                        }
                    }
                }
            }
            FreeLibrary(hDxgi);
        }
        return d;
    }

    // ------------------------------------------------------------------
    // NVIDIA: the actual signal sent to the display (Windows doesn't report this)

    struct NvSignal {
        bool ok = false;
        CStringW format;      // "RGB", "YCbCr 4:2:2", ...
        int bpc = 0;          // bits per component on the cable, 0 = unknown
        CStringW range;       // "Full", "Limited"
        CStringW colorimetry; // "BT.2020 YCC", ...
    };

    NvSignal QueryNvSignal(const CStringW& gdiDeviceName)
    {
        using namespace TvNvApi;
        NvSignal s;
#ifdef _WIN64
        static HMODULE hNv = LoadLibraryW(L"nvapi64.dll"); // installed by every NVIDIA driver
#else
        static HMODULE hNv = LoadLibraryW(L"nvapi.dll");
#endif
        static PQueryInterface query = hNv ? reinterpret_cast<PQueryInterface>(GetProcAddress(hNv, "nvapi_QueryInterface")) : nullptr;
        static bool initialized = false;
        if (!query || gdiDeviceName.IsEmpty()) {
            return s; // no NVIDIA driver on this PC
        }
        if (!initialized) {
            auto pInit = reinterpret_cast<PInitialize>(query(kId_NvAPI_Initialize));
            if (!pInit || pInit() != 0) {
                return s;
            }
            initialized = true;
        }
        auto pGetId = reinterpret_cast<PGetDisplayIdByDisplayName>(query(kId_NvAPI_DISP_GetDisplayIdByDisplayName));
        auto pColor = reinterpret_cast<PDispColorControl>(query(kId_NvAPI_Disp_ColorControl));
        NvU32 displayId = 0;
        if (!pGetId || !pColor || pGetId(CW2A(gdiDeviceName), &displayId) != 0) {
            return s; // e.g. a display on another GPU
        }
        NV_COLOR_DATA_V3 cd = {};
        cd.version = NV_COLOR_DATA_VER3;
        cd.size = sizeof(cd);
        cd.cmd = NV_COLOR_CMD_GET;
        if (pColor(displayId, &cd) != 0) {
            return s;
        }
        s.ok = true;
        switch (cd.data.colorFormat) {
            case NV_COLOR_FORMAT_RGB: s.format = L"RGB"; break;
            case NV_COLOR_FORMAT_YUV422: s.format = L"YCbCr 4:2:2"; break;
            case NV_COLOR_FORMAT_YUV444: s.format = L"YCbCr 4:4:4"; break;
            case NV_COLOR_FORMAT_YUV420: s.format = L"YCbCr 4:2:0"; break;
            default: s.format.Format(L"Format %u", cd.data.colorFormat); break;
        }
        switch (cd.data.bpc) {
            case NV_BPC_6: s.bpc = 6; break;
            case NV_BPC_8: s.bpc = 8; break;
            case NV_BPC_10: s.bpc = 10; break;
            case NV_BPC_12: s.bpc = 12; break;
            case NV_BPC_16: s.bpc = 16; break;
            default: break;
        }
        s.range = cd.data.dynamicRange == NV_DYNAMIC_RANGE_VESA ? L"Full" : cd.data.dynamicRange == NV_DYNAMIC_RANGE_CEA ? L"Limited" : L"";
        switch (cd.data.colorimetry) {
            case NV_COLOR_COLORIMETRY_RGB: s.colorimetry = L"RGB"; break;
            case NV_COLOR_COLORIMETRY_YCC601: s.colorimetry = L"BT.601 YCC"; break;
            case NV_COLOR_COLORIMETRY_YCC709: s.colorimetry = L"BT.709 YCC"; break;
            case NV_COLOR_COLORIMETRY_BT2020RGB: s.colorimetry = L"BT.2020 RGB"; break;
            case NV_COLOR_COLORIMETRY_BT2020YCC: s.colorimetry = L"BT.2020 YCC"; break;
            case NV_COLOR_COLORIMETRY_BT2020cYCC: s.colorimetry = L"BT.2020 cYCC"; break;
            default: s.colorimetry.Format(L"Colorimetry %u", cd.data.colorimetry); break;
        }
        return s;
    }

    CStringW ColorSpaceName(DXGI_COLOR_SPACE_TYPE cs)
    {
        switch (cs) {
            case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                return L"HDR10 (BT.2020, PQ)";
            case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
                return L"SDR (sRGB / BT.709)";
            case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
                return L"scRGB (linear)";
            default: {
                CStringW s;
                s.Format(L"Other (%d)", static_cast<int>(cs));
                return s;
            }
        }
    }
}

namespace
{
    const wchar_t kTimes[] = { 0x00D7, 0 }; // multiplication sign
    const wchar_t kDot[] = { L' ', 0x00B7, L' ', 0 }; // " . " separator

    CStringW ShortHdr(const MiStream& v)
    {
        const CStringW f = v.Get(L"HDR_Format") + L" " + v.Get(L"HDR_Format_Compatibility");
        CStringW r;
        if (f.Find(L"Dolby Vision") >= 0) {
            r = L"Dolby Vision";
            if (f.Find(L"HDR10+") >= 0) {
                r += CStringW(kDot) + L"HDR10+";
            } else if (f.Find(L"HDR10") >= 0) {
                r += CStringW(kDot) + L"HDR10";
            }
        } else if (f.Find(L"HDR10+") >= 0) {
            r = L"HDR10+";
        } else if (f.Find(L"HDR10") >= 0 || f.Find(L"2086") >= 0) {
            r = L"HDR10";
        } else {
            const CStringW tc = v.Get(L"transfer_characteristics");
            r = tc.Find(L"PQ") >= 0 ? L"HDR10" : tc.Find(L"HLG") >= 0 ? L"HLG" : L"SDR";
        }
        return r;
    }

    CStringW ShortCodec(const MiStream& v)
    {
        CStringW profile = v.Get(L"Format_Profile");
        const int at = profile.Find(L'@');
        if (at >= 0) {
            profile = profile.Left(at); // "Main 10@L5.1@High" -> "Main 10"
        }
        return Join(v.Get(L"Format"), profile);
    }

    // "BT.2020 . PQ" from the stream's colour description
    CStringW ShortColour(const MiStream& v)
    {
        CStringW primaries = v.Get(L"colour_primaries");
        CStringW transfer = v.Get(L"transfer_characteristics");
        if (transfer.Find(L"PQ") >= 0) {
            transfer = L"PQ";
        } else if (transfer.Find(L"HLG") >= 0) {
            transfer = L"HLG";
        } else if (transfer.Find(L"BT.709") >= 0 || transfer.Find(L"BT.601") >= 0) {
            transfer = L"SDR gamma";
        }
        return primaries.IsEmpty() ? transfer : (transfer.IsEmpty() ? primaries : primaries + kDot + transfer);
    }

    // "4:2:0 . 10-bit"
    CStringW ShortChroma(const MiStream& v)
    {
        CStringW chroma = v.Get(L"ChromaSubsampling");
        const int comma = chroma.Find(L',');
        if (comma >= 0) {
            chroma = chroma.Left(comma); // "4:2:0 (Type 2)" style extras stay, ", ..." lists don't
        }
        const CStringW bits = v.Get(L"BitDepth");
        return bits.IsEmpty() ? chroma : (chroma.IsEmpty() ? bits + L"-bit" : chroma + kDot + bits + L"-bit");
    }

    CStringW OutputColourSpace(DXGI_COLOR_SPACE_TYPE cs)
    {
        switch (cs) {
            case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                return CStringW(L"BT.2020") + kDot + L"PQ (HDR10)";
            case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
                return CStringW(L"BT.709") + kDot + L"sRGB (SDR)";
            case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
                return L"scRGB (linear)";
            default:
                return L"";
        }
    }

    // LAV's codec name while bitstreaming ("truehd", "eac3", "dts") -> what the receiver shows
    CStringW BitstreamName(LPCSTR codec)
    {
        const CStringA c(codec);
        return c == "ac3" ? CStringW(L"Dolby Digital") : c == "eac3" ? CStringW(L"Dolby Digital Plus")
               : (c == "truehd" || c == "mlp") ? CStringW(L"Dolby TrueHD") : c == "dts" ? CStringW(L"DTS / DTS-HD") : CStringW(codec);
    }

    CStringW AudioChannelsShort(const MiStream& a)
    {
        const int n = _wtoi(a.Get(L"Channel(s)"));
        if (n <= 0) {
            return L"";
        }
        const CStringW layout = a.Get(L"ChannelLayout");
        const int lfe = (layout.Find(L"LFE") >= 0 || n == 6 || n == 8) ? 1 : 0;
        CStringW s;
        s.Format(L"%d.%d", n - lfe, lfe);
        return s;
    }
}

void CTvInfo::BuildChips(CMainFrame* pMainFrame, Kind kind, std::vector<TvInfoChip>& row1, std::vector<TvInfoChip>& row2)
{
    row1.clear();
    row2.clear();
    const MiSnapshot* snap = Snapshot(pMainFrame->m_wndPlaylistBar.GetCurFileName());
    auto chip = [](std::vector<TvInfoChip>& row, LPCWSTR label, const CStringW& value) {
        if (!value.IsEmpty()) {
            row.push_back({ label, value });
        }
    };

    if (kind == Kind::Video) {
        if (snap && !snap->video.empty()) {
            const MiStream& v = snap->video[0];
            const int w = _wtoi(v.Get(L"Width")), h = _wtoi(v.Get(L"Height"));
            if (w > 0) {
                CStringW res;
                res.Format(L"%d %s %d", w, kTimes, h);
                chip(row1, L"RESOLUTION", res);
            }
            chip(row1, L"CODEC", ShortCodec(v));
            chip(row1, L"COLOUR", ShortColour(v)); // HDR format: a badge (BuildBadges)
            chip(row1, L"CHROMA", ShortChroma(v));
            const CStringW fps = v.Get(L"FrameRate");
            chip(row1, L"FRAME RATE", fps.IsEmpty() ? fps : fps + L" fps");
            CStringW br = v.Text(L"BitRate");
            chip(row1, L"BIT RATE", br.IsEmpty() ? snap->general.Text(L"OverallBitRate") : br);
        }

        // Output: what the display is getting right now
        const DisplayInfo d = QueryDisplay(pMainFrame->m_pVideoWnd ? pMainFrame->m_pVideoWnd->m_hWnd : pMainFrame->m_hWnd);
        double refresh = d.refresh;
        SIZE mode = { d.width, d.height };
        bool hdrKnown = false, hdr = false;
        if (CComPtr<IMadVRInfo> pMvr = FindInGraph<IMadVRInfo>(pMainFrame->m_pGB)) {
            double r = 0.0;
            if (SUCCEEDED(pMvr->GetDouble("refreshRate", &r)) && r > 0.0) {
                refresh = r;
            }
            SIZE sz = {};
            if (SUCCEEDED(pMvr->GetSize("displayModeSize", &sz)) && sz.cx > 0) {
                mode = sz;
            }
            bool b = false;
            if (SUCCEEDED(pMvr->GetBool("hdrOutput", &b))) {
                hdrKnown = true;
                hdr = b;
            }
        }
        if (mode.cx > 0) {
            CStringW m;
            m.Format(L"%d %s %d @ %.3f Hz", mode.cx, kTimes, mode.cy, refresh);
            chip(row2, L"DISPLAY", m);
        }
        CStringW out = hdrKnown ? CStringW(hdr ? L"HDR" : L"SDR") : CStringW();
        if (d.haveDxgi) {
            CStringW bits;
            bits.Format(L"%d-bit", d.bitsPerColor);
            out = out.IsEmpty() ? bits : out + kDot + bits;
        }
        chip(row2, L"OUTPUT", out);
        if (d.haveDxgi) {
            chip(row2, L"COLOUR SPACE", OutputColourSpace(d.colorSpace));
        }
        // What actually goes over the cable (NVIDIA only): "YCbCr 4:2:2 . 12-bit . Limited"
        const NvSignal nv = QueryNvSignal(d.deviceName);
        if (nv.ok) {
            CStringW sig = nv.format;
            if (nv.bpc > 0) {
                CStringW b;
                b.Format(L"%d-bit", nv.bpc);
                sig += CStringW(kDot) + b;
            }
            if (!nv.range.IsEmpty()) {
                sig += CStringW(kDot) + nv.range;
            }
            chip(row2, L"SIGNAL", sig);
        }
        if (CComPtr<ILAVVideoStatus> pLav = FindInGraph<ILAVVideoStatus>(pMainFrame->m_pGB)) {
            LPCWSTR dec = pLav->GetActiveDecoderName();
            chip(row2, L"DECODER", dec ? CStringW(dec) : CStringW());
        }
        return;
    }

    // Audio: the playing track
    int current = 0;
    std::vector<TvTrack> tracks;
    if (CTvTracks::GetAudio(pMainFrame, tracks)) {
        for (const auto& t : tracks) {
            if (t.isCurrent) {
                current = t.index;
            }
        }
    }
    if (snap && current < static_cast<int>(snap->audio.size())) {
        const MiStream& a = snap->audio[current];
        chip(row1, L"LANGUAGE", a.Text(L"Language"));
        const CStringW commercial = a.Get(L"Format_Commercial_IfAny");
        chip(row1, L"FORMAT", commercial.IsEmpty() ? a.Get(L"Format") : commercial);
        chip(row1, L"CHANNELS", AudioChannelsShort(a));
        chip(row1, L"SAMPLE RATE", a.Text(L"SamplingRate"));
        chip(row1, L"BIT RATE", a.Text(L"BitRate"));
    }
    if (CComPtr<ILAVAudioStatus> pLav = FindInGraph<ILAVAudioStatus>(pMainFrame->m_pGB)) {
        LPCSTR codec = nullptr, decodeFmt = nullptr, outFmt = nullptr;
        int ch = 0, rate = 0, outCh = 0, outRate = 0;
        DWORD mask = 0, outMask = 0;
        const bool haveDecode = SUCCEEDED(pLav->GetDecodeDetails(&codec, &decodeFmt, &ch, &rate, &mask));
        // LAV returns S_FALSE while it bitstreams (passes the stream through untouched)
        const HRESULT hrOut = pLav->GetOutputDetails(&outFmt, &outCh, &outRate, &outMask);
        if (hrOut == S_FALSE) {
            chip(row2, L"DECODING", L"None (bitstreaming)");
            CStringW o = L"Bitstream to receiver";
            if (codec && *codec) {
                o += CStringW(kDot) + BitstreamName(codec);
            }
            chip(row2, L"OUTPUT", o);
        } else {
            if (haveDecode && codec) {
                chip(row2, L"DECODING", CStringW(codec));
            }
            if (hrOut == S_OK && outFmt) {
                CStringW o(outFmt);
                if (outCh > 0) {
                    o += CStringW(kDot) + ChannelLayoutName(outCh, outMask);
                }
                chip(row2, L"OUTPUT", o);
            }
        }
    }
}

void CTvInfo::BuildBadges(CMainFrame* pMainFrame, Kind kind, std::vector<TvInfoBadge>& badges)
{
    badges.clear();
    const MiSnapshot* snap = Snapshot(pMainFrame->m_wndPlaylistBar.GetCurFileName());
    if (!snap) {
        return;
    }
    auto add = [&](const CStringW& text, int tone) {
        if (!text.IsEmpty()) {
            badges.push_back({ text, tone });
        }
    };
    if (kind == Kind::Video) {
        if (snap->video.empty()) {
            return;
        }
        const MiStream& v = snap->video[0];
        const int w = _wtoi(v.Get(L"Width")), h = _wtoi(v.Get(L"Height"));
        add(w >= 3800 ? L"4K" : h >= 1400 ? L"1440p" : h >= 1000 || w >= 1900 ? L"1080p" : h >= 700 ? L"720p" : h > 0 ? L"SD" : L"", 1);
        // "Dolby Vision . HDR10" -> two badges
        CStringW hdr = ShortHdr(v);
        for (int sep; !hdr.IsEmpty(); hdr = sep >= 0 ? hdr.Mid(sep + static_cast<int>(wcslen(kDot))) : CStringW()) {
            sep = hdr.Find(kDot);
            const CStringW part = sep >= 0 ? hdr.Left(sep) : hdr;
            add(part, part == L"SDR" ? 0 : 2);
        }
        add(v.Get(L"Format"), 0);
        const CStringW fps = v.Get(L"FrameRate");
        add(fps.IsEmpty() ? fps : fps + L" fps", 0);
        return;
    }
    int current = 0;
    std::vector<TvTrack> tracks;
    if (CTvTracks::GetAudio(pMainFrame, tracks)) {
        for (const auto& t : tracks) {
            if (t.isCurrent) {
                current = t.index;
            }
        }
    }
    if (current >= static_cast<int>(snap->audio.size())) {
        return;
    }
    const MiStream& a = snap->audio[current];
    const CStringW all = a.Get(L"Format_Commercial_IfAny") + L" " + a.Get(L"Format") + L" " + a.Get(L"Format_AdditionalFeatures");
    if (all.Find(L"Atmos") >= 0 || all.Find(L"JOC") >= 0) {
        add(L"Atmos", 2);
    } else if (all.Find(L"DTS:X") >= 0 || all.Find(L"XLL X") >= 0) {
        add(L"DTS:X", 2);
    }
    if (a.Get(L"Compression_Mode") == L"Lossless") {
        add(L"Lossless", 2);
    }
    add(AudioChannelsShort(a), 1);
    add(a.Text(L"Language"), 0);
}

CStringW CTvInfo::FileLine(CMainFrame* pMainFrame)
{
    const CStringW path = pMainFrame->m_wndPlaylistBar.GetCurFileName();
    const MiSnapshot* snap = Snapshot(path);
    // the title tag when the file has one (a stream address only has a generic name), else the file name
    const CStringW name = MediaTitle(pMainFrame);
    const CStringW dur = snap ? snap->general.Text(L"Duration") : CStringW();
    return dur.IsEmpty() ? name : name + kDot + dur;
}

LPCWSTR CTvInfo::LevelName(int level)
{
    switch (level) {
        case 0: return L"Basic";
        case 1: return L"Extended";
        default: return L"Nightmare";
    }
}

void CTvInfo::Prepare(CMainFrame* pMainFrame)
{
    const CStringW path = pMainFrame->m_wndPlaylistBar.GetCurFileName();
    if (path.IsEmpty() || (g_job && g_job->path == path)) {
        return;
    }
    // A new job; a still-running old one finishes on its own (it owns its data).
    auto job = std::make_shared<MiJob>();
    job->path = path;
    g_job = job;
    std::thread(RunMediaInfo, job).detach();
}

bool CTvInfo::IsReady(CMainFrame* pMainFrame)
{
    return Snapshot(pMainFrame->m_wndPlaylistBar.GetCurFileName()) != nullptr;
}

void CTvInfo::AddDisplay(CMainFrame* pMainFrame, int level, std::vector<TvInfoLine>& lines)
{
    Header(lines, L"Display");
    const DisplayInfo d = QueryDisplay(pMainFrame->m_pVideoWnd ? pMainFrame->m_pVideoWnd->m_hWnd : pMainFrame->m_hWnd);

    // madVR knows the exact refresh rate and whether it outputs HDR.
    CComPtr<IMadVRInfo> pMvr = FindInGraph<IMadVRInfo>(pMainFrame->m_pGB);
    double refresh = d.refresh;
    bool hdrOut = false, haveHdrOut = false;
    SIZE mode = { d.width, d.height };
    if (pMvr) {
        double r = 0.0;
        if (SUCCEEDED(pMvr->GetDouble("refreshRate", &r)) && r > 0.0) {
            refresh = r;
        }
        bool b = false;
        if (SUCCEEDED(pMvr->GetBool("hdrOutput", &b))) {
            hdrOut = b;
            haveHdrOut = true;
        }
        SIZE sz = {};
        if (SUCCEEDED(pMvr->GetSize("displayModeSize", &sz)) && sz.cx > 0) {
            mode = sz;
        }
    }

    CStringW modeText;
    if (mode.cx > 0) {
        modeText.Format(L"%d x %d @ %.3f Hz", mode.cx, mode.cy, refresh);
    }
    Add(lines, L"Mode", modeText);
    if (haveHdrOut) {
        Add(lines, L"Output", hdrOut ? CStringW(L"HDR (madVR sends PQ)") : CStringW(L"SDR"));
    }
    if (d.haveDxgi) {
        CStringW bits;
        bits.Format(L"%d-bit", d.bitsPerColor);
        Add(lines, L"Bit depth", bits);
        if (level >= 1) {
            Add(lines, L"Colour space", ColorSpaceName(d.colorSpace));
        }
    }
    if (level >= 1) {
        // The signal on the cable, from the NVIDIA driver (Windows doesn't report it)
        const NvSignal nv = QueryNvSignal(d.deviceName);
        if (nv.ok) {
            Add(lines, L"Signal format", nv.format);
            if (nv.bpc > 0) {
                CStringW b;
                b.Format(L"%d-bit", nv.bpc);
                Add(lines, L"Signal bit depth", b);
            }
            Add(lines, L"Signal range", nv.range);
            Add(lines, L"Colorimetry", nv.colorimetry);
        } else {
            Add(lines, L"Signal format", L"Not reported (needs an NVIDIA GPU)");
        }
    }
    if (level >= 1) {
        Add(lines, L"Display", Join(d.friendlyName, d.deviceName.IsEmpty() ? CStringW() : L"(" + d.deviceName + L")"));
        if (d.haveDxgi && d.maxLuminance > 0.0f) {
            CStringW lum;
            lum.Format(L"%.0f nits (as reported by the display)", d.maxLuminance);
            Add(lines, L"Peak brightness", lum);
        }
    }
    if (level >= 1 && pMvr) {
        LPWSTR ver = nullptr;
        int chars = 0;
        if (SUCCEEDED(pMvr->GetString("version", &ver, &chars)) && ver) {
            Add(lines, L"madVR", CStringW(ver));
            LocalFree(ver);
        }
    }
}

void CTvInfo::AddVideoPlayback(CMainFrame* pMainFrame, int level, std::vector<TvInfoLine>& lines)
{
    Header(lines, L"Playback");
    if (CComPtr<ILAVVideoStatus> pLav = FindInGraph<ILAVVideoStatus>(pMainFrame->m_pGB)) {
        LPCWSTR dec = pLav->GetActiveDecoderName();
        CStringW decoder = dec ? CStringW(dec) : CStringW();
        CComBSTR device;
        if (SUCCEEDED(pLav->GetHWAccelActiveDevice(&device)) && device.Length()) {
            decoder = Join(decoder, L"(" + CStringW(device) + L")");
        }
        Add(lines, L"Decoder", Join(L"LAV Video", decoder, L": "));
    }
    if (level >= 1) {
        if (CComPtr<IMadVRInfo> pMvr = FindInGraph<IMadVRInfo>(pMainFrame->m_pGB)) {
            SIZE sz = {};
            if (SUCCEEDED(pMvr->GetSize("originalVideoSize", &sz)) && sz.cx > 0) {
                CStringW s;
                s.Format(L"%d x %d", sz.cx, sz.cy);
                Add(lines, L"Decoded size", s);
            }
            ULONGLONG fr = 0;
            if (SUCCEEDED(pMvr->GetUlonglong("frameRate", &fr)) && fr > 0) {
                CStringW s;
                s.Format(L"%.3f fps", 10000000.0 / fr);
                Add(lines, L"Frame rate (output)", s);
            }
            LPWSTR matrix = nullptr;
            int chars = 0;
            if (SUCCEEDED(pMvr->GetString("yuvMatrix", &matrix, &chars)) && matrix) {
                Add(lines, L"YCbCr matrix (madVR)", CStringW(matrix));
                LocalFree(matrix);
            }
        }
    }
}

void CTvInfo::AddAudioPlayback(CMainFrame* pMainFrame, int level, std::vector<TvInfoLine>& lines)
{
    Header(lines, L"Playback");
    CComPtr<ILAVAudioStatus> pLav = FindInGraph<ILAVAudioStatus>(pMainFrame->m_pGB);
    if (!pLav) {
        Add(lines, L"Decoder", L"(not LAV Audio)");
        return;
    }
    LPCSTR codec = nullptr, decodeFmt = nullptr, outFmt = nullptr;
    int ch = 0, rate = 0, outCh = 0, outRate = 0;
    DWORD mask = 0, outMask = 0;
    const bool haveDecode = SUCCEEDED(pLav->GetDecodeDetails(&codec, &decodeFmt, &ch, &rate, &mask));
    const HRESULT hrOut = pLav->GetOutputDetails(&outFmt, &outCh, &outRate, &outMask);
    const bool haveOutput = hrOut == S_OK;
    if (hrOut == S_FALSE) {
        // LAV returns S_FALSE while it bitstreams
        Add(lines, L"Output", L"Bitstreaming " + (codec && *codec ? BitstreamName(codec) : CStringW()) + L" (passthrough to the receiver)");
        return;
    }
    if (haveDecode && codec) {
        CStringW s(codec);
        if (level >= 1 && decodeFmt && *decodeFmt) {
            s += L" (" + CStringW(decodeFmt) + L")";
        }
        if (ch > 0) {
            CStringW c;
            c.Format(L", %s, %d Hz", ChannelLayoutName(ch, mask).GetString(), rate);
            s += c;
        }
        Add(lines, L"Decoding", s);
    }
    if (haveOutput && outFmt) {
        CStringW s(outFmt);
        if (outCh > 0) {
            CStringW c;
            c.Format(L", %s, %d Hz", ChannelLayoutName(outCh, outMask).GetString(), outRate);
            s += c;
        }
        Add(lines, L"Output", s);
    }
}

// A readable title: the file's own title tag (MediaInfo "Title" / "Movie"), else the file name without
// folders and extension (also for URLs, %20 etc. decoded), never a full path
CStringW CTvInfo::MediaTitle(CMainFrame* pMainFrame)
{
    const CStringW path = pMainFrame->m_wndPlaylistBar.GetCurFileName();
    if (const MiSnapshot* snap = Snapshot(path)) {
        for (LPCWSTR field : { L"Title", L"Movie" }) {
            CStringW t = snap->general.Get(field);
            t.Trim();
            if (!t.IsEmpty()) {
                return t;
            }
        }
    }
    return DisplayName(path);
}

void CTvInfo::HdrMetadata(CMainFrame* pMainFrame, CStringW& format, CStringW& maxCll, CStringW& maxFall, CStringW& mastering)
{
    format.Empty(), maxCll.Empty(), maxFall.Empty(), mastering.Empty();
    const MiSnapshot* snap = Snapshot(pMainFrame->m_wndPlaylistBar.GetCurFileName());
    const MiStream* v = (snap && !snap->video.empty()) ? &snap->video[0] : nullptr;
    if (!v) {
        return;
    }
    format = v->Text(L"HDR_Format");
    maxCll = v->Text(L"MaxCLL");
    maxFall = v->Text(L"MaxFALL");
    mastering = v->Get(L"MasteringDisplay_Luminance");
}

void CTvInfo::Build(CMainFrame* pMainFrame, Kind kind, int level, std::vector<TvInfoLine>& lines)
{
    lines.clear();
    level = std::clamp(level, 0, kLevels - 1);
    const MiSnapshot* snap = Snapshot(pMainFrame->m_wndPlaylistBar.GetCurFileName());

    if (level == 0) {
        // Basic = the chips only; just say why some may be missing.
        if (!snap) {
            Add(lines, L"Status", L"Reading media info...");
        } else if (!snap->error.IsEmpty()) {
            Add(lines, L"Status", snap->error);
        }
        return;
    }

    if (kind == Kind::Video) {
        Header(lines, L"Video");
        const MiStream* v = (snap && !snap->video.empty()) ? &snap->video[0] : nullptr;
        if (!snap) {
            Add(lines, L"Status", L"Reading media info...");
        } else if (!snap->error.IsEmpty()) {
            Add(lines, L"Status", snap->error);
        } else if (!v) {
            Add(lines, L"Status", L"No video stream");
        }
        if (v && level < 2) {
            CStringW res;
            const int w = _wtoi(v->Get(L"Width")), h = _wtoi(v->Get(L"Height"));
            if (w > 0) {
                res.Format(L"%d x %d", w, h);
                if (w >= 3800) {
                    res += L" (4K)";
                } else if (w >= 1900) {
                    res += L" (1080p)";
                }
            }
            Add(lines, L"Resolution", res);
            Add(lines, L"Frame rate", v->Text(L"FrameRate"));
            Add(lines, L"Codec", Join(v->Get(L"Format"), v->Get(L"Format_Profile")));
            CStringW hdr = v->Text(L"HDR_Format");
            if (hdr.IsEmpty()) {
                const CStringW tc = v->Get(L"transfer_characteristics");
                hdr = tc.Find(L"PQ") >= 0 ? CStringW(L"HDR10") : tc.Find(L"HLG") >= 0 ? CStringW(L"HLG") : CStringW(L"SDR");
            }
            Add(lines, L"HDR", hdr);
            Add(lines, L"Bit rate", v->Text(L"BitRate"));
            if (level >= 1) {
                Add(lines, L"Bit depth", v->Text(L"BitDepth"));
                Add(lines, L"Chroma", v->Text(L"ChromaSubsampling"));
                Add(lines, L"Colour primaries", v->Get(L"colour_primaries"));
                Add(lines, L"Transfer", v->Get(L"transfer_characteristics"));
                Add(lines, L"Matrix", v->Get(L"matrix_coefficients"));
                Add(lines, L"Colour range", v->Get(L"colour_range"));
                Add(lines, L"Mastering primaries", v->Get(L"MasteringDisplay_ColorPrimaries"));
                Add(lines, L"Mastering luminance", v->Get(L"MasteringDisplay_Luminance"));
                Add(lines, L"MaxCLL", v->Text(L"MaxCLL"));
                Add(lines, L"MaxFALL", v->Text(L"MaxFALL"));
                Add(lines, L"Scan type", v->Text(L"ScanType"));
                Add(lines, L"Aspect ratio", v->Text(L"DisplayAspectRatio"));
                Add(lines, L"Stream size", v->Text(L"StreamSize"));
                Add(lines, L"Title", v->Get(L"Title"));
            }
        }
        if (snap && level >= 1) {
            Header(lines, L"Container");
            const MiStream& g = snap->general;
            Add(lines, L"Format", g.Get(L"Format"));
            Add(lines, L"File size", g.Text(L"FileSize"));
            Add(lines, L"Duration", g.Text(L"Duration"));
            Add(lines, L"Overall bit rate", g.Text(L"OverallBitRate"));
            Add(lines, L"Title", g.Get(L"Title"));
        }
        AddVideoPlayback(pMainFrame, level, lines);
        AddDisplay(pMainFrame, level, lines);
        if (v && level >= 2) {
            Header(lines, L"Video (MediaInfo, complete)");
            for (const auto& f : v->complete) {
                lines.push_back({ f.first.c_str(), f.second.c_str(), false });
            }
            Header(lines, L"General (MediaInfo, complete)");
            for (const auto& f : snap->general.complete) {
                lines.push_back({ f.first.c_str(), f.second.c_str(), false });
            }
        }
        return;
    }

    // Audio: the track that is playing now
    int current = 0;
    std::vector<TvTrack> tracks;
    if (CTvTracks::GetAudio(pMainFrame, tracks)) {
        for (const auto& t : tracks) {
            if (t.isCurrent) {
                current = t.index;
            }
        }
    }
    Header(lines, L"Audio");
    const MiStream* a = (snap && current < static_cast<int>(snap->audio.size())) ? &snap->audio[current] : nullptr;
    if (!snap) {
        Add(lines, L"Status", L"Reading media info...");
    } else if (!snap->error.IsEmpty()) {
        Add(lines, L"Status", snap->error);
    } else if (!a) {
        Add(lines, L"Status", L"No audio stream");
    }
    if (a && level < 2) {
        Add(lines, L"Language", a->Text(L"Language"));
        CStringW fmt = a->Get(L"Format_Commercial_IfAny");
        Add(lines, L"Format", fmt.IsEmpty() ? Join(a->Get(L"Format"), a->Get(L"Format_AdditionalFeatures")) : fmt);
        Add(lines, L"Channels", Join(a->Text(L"Channel(s)"), a->Get(L"ChannelLayout").IsEmpty() ? CStringW() : L"(" + a->Get(L"ChannelLayout") + L")"));
        Add(lines, L"Sample rate", a->Text(L"SamplingRate"));
        if (level >= 1) {
            Add(lines, L"Title", a->Get(L"Title"));
            Add(lines, L"Codec", Join(a->Get(L"Format"), a->Get(L"Format_AdditionalFeatures")));
            Add(lines, L"Bit depth", a->Text(L"BitDepth"));
            Add(lines, L"Bit rate", Join(a->Text(L"BitRate"), a->Text(L"BitRate_Mode").IsEmpty() ? CStringW() : L"(" + a->Text(L"BitRate_Mode") + L")"));
            Add(lines, L"Compression", a->Text(L"Compression_Mode"));
            Add(lines, L"Delay to video", a->Text(L"Delay_relative_to_video"));
            Add(lines, L"Default", a->Text(L"Default"));
            Add(lines, L"Forced", a->Text(L"Forced"));
            Add(lines, L"Stream size", a->Text(L"StreamSize"));
        }
    }
    AddAudioPlayback(pMainFrame, level, lines);
    if (a && level >= 2) {
        Header(lines, L"Audio (MediaInfo, complete)");
        for (const auto& f : a->complete) {
            lines.push_back({ f.first.c_str(), f.second.c_str(), false });
        }
    }
}
