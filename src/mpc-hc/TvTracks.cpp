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
#include "TvTracks.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "../DSUtil/MediaTypeEx.h"
#include "../Subtitles/RTS.h"
#include "../Subtitles/VobSubFile.h"
#include "../Subtitles/RLECodedSubtitle.h"
#include "../filters/switcher/AudioSwitcher/AudioSwitcher.h"
#include <moreuuids.h>

namespace
{
    CStringW LanguageName(LCID lcid)
    {
        CString s;
        if (lcid && lcid != LCID(-1)) {
            GetLocaleString(lcid, LOCALE_SENGLANGUAGE, s);
        }
        return CStringW(s);
    }

    // Stream names come from the splitter ("A: English [eng] ..." style) or the subtitle file.
    CStringW CleanName(const CStringW& raw)
    {
        CStringW name(raw);
        name.Replace(L"\t", L" - ");
        name.Trim();
        if (name.GetLength() > 2 && name[1] == L':' && (name[0] == L'A' || name[0] == L'S')) {
            name = name.Mid(2).Trim();
        }
        return name;
    }

    void NameFlags(const CStringW& name, bool& isDefault, bool& isForced)
    {
        CStringW lower(name);
        lower.MakeLower();
        isDefault = lower.Find(L"default") >= 0;
        isForced = lower.Find(L"forced") >= 0;
    }

    const WAVEFORMATEX* GetWfe(const AM_MEDIA_TYPE* pmt)
    {
        if (pmt && pmt->formattype == FORMAT_WaveFormatEx && pmt->pbFormat && pmt->cbFormat >= sizeof(WAVEFORMATEX)) {
            return reinterpret_cast<const WAVEFORMATEX*>(pmt->pbFormat);
        }
        return nullptr;
    }

    CStringW AudioCodec(const AM_MEDIA_TYPE* pmt)
    {
        if (!pmt) {
            return L"";
        }
        const WAVEFORMATEX* wfe = GetWfe(pmt);
        CStringW codec(CMediaTypeEx::GetAudioCodecName(pmt->subtype, wfe ? wfe->wFormatTag : 0));
        if (codec == L"Dolby AC3") {
            codec = L"Dolby Digital";
        } else if (codec == L"DD+") {
            codec = L"Dolby Digital+";
        } else if (codec.Left(2) == L"0x") {
            codec.Empty(); // unknown; the raw name usually says more
        }
        return codec;
    }

    CStringW AudioChannels(const AM_MEDIA_TYPE* pmt)
    {
        const WAVEFORMATEX* wfe = GetWfe(pmt);
        if (!wfe || wfe->nChannels == 0) {
            return L"";
        }
        int n = wfe->nChannels;
        int lfe = 0;
        if (wfe->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pmt->cbFormat >= sizeof(WAVEFORMATEXTENSIBLE)) {
            lfe = (reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfe)->dwChannelMask & SPEAKER_LOW_FREQUENCY) ? 1 : 0;
        } else if (n == 6 || n == 8) {
            lfe = 1; // 5.1 / 7.1 without a channel mask
        }
        CStringW s;
        s.Format(L"%d.%d", n - lfe, lfe);
        return s;
    }

    CStringW SubtitleFormatFromType(const AM_MEDIA_TYPE* pmt)
    {
        if (!pmt) {
            return L"";
        }
        const GUID& st = pmt->subtype;
        if (st == MEDIASUBTYPE_UTF8) {
            return L"SRT";
        } else if (st == MEDIASUBTYPE_SSA) {
            return L"SSA";
        } else if (st == MEDIASUBTYPE_ASS || st == MEDIASUBTYPE_ASS2) {
            return L"ASS";
        } else if (st == MEDIASUBTYPE_HDMVSUB) {
            return L"PGS";
        } else if (st == MEDIASUBTYPE_VOBSUB || st == MEDIASUBTYPE_DVD_SUBPICTURE) {
            return L"VobSub";
        } else if (st == MEDIASUBTYPE_DVB_SUBTITLES) {
            return L"DVB";
        } else if (st == MEDIASUBTYPE_WEBVTT) {
            return L"WebVTT";
        }
        return CStringW(CMediaTypeEx::GetSubtitleCodecName(st));
    }

    CStringW SubtitleFormatFromStream(ISubStream* pSubStream)
    {
        CLSID clsid;
        if (!pSubStream || FAILED(pSubStream->GetClassID(&clsid))) {
            return L"";
        }
        if (clsid == __uuidof(CRenderedTextSubtitle)) {
            auto pRTS = static_cast<CRenderedTextSubtitle*>(pSubStream);
            switch (pRTS->m_subtitleType) {
                case Subtitle::SRT: return L"SRT";
                case Subtitle::SSA: return L"SSA";
                case Subtitle::ASS: return L"ASS";
                case Subtitle::VTT: return L"WebVTT";
                case Subtitle::SUB: return L"SUB";
                case Subtitle::SMI: return L"SAMI";
                case Subtitle::USF: return L"USF";
                default: return L"Text";
            }
        } else if (clsid == __uuidof(CVobSubFile) || clsid == __uuidof(CVobSubStream)) {
            return L"VobSub";
        } else if (clsid == __uuidof(CRLECodedSubtitle)) {
            return L"PGS";
        }
        return L"";
    }

    // "Japanese - TrueHD 7.1 - Default"; falls back to the cleaned raw name when there is no language.
    TvTrack MakeTrack(int index, LCID lcid, const CStringW& rawName, const CStringW& format, bool isCurrent)
    {
        TvTrack t;
        t.index = index;
        t.isCurrent = isCurrent;

        CStringW clean = CleanName(rawName);
        CStringW lang = LanguageName(lcid);
        bool isDefault = false, isForced = false;
        NameFlags(clean, isDefault, isForced);

        CStringW title = !lang.IsEmpty() ? lang : clean;
        if (title.IsEmpty()) {
            title.Format(L"Track %d", index + 1);
        }
        if (!format.IsEmpty()) {
            title += L" - " + format;
        }
        if (isForced) {
            title += L" - Forced";
        }
        if (isDefault) {
            title += L" - Default";
        }
        t.title = title;

        // Keep the full original name visible (it carries titles like "Commentary").
        // Without a language the title already is that name.
        if (!lang.IsEmpty() && !clean.IsEmpty() && clean.CompareNoCase(lang) != 0) {
            t.detail = clean;
        }
        return t;
    }
}

bool CTvTracks::GetAudio(CMainFrame* pMainFrame, std::vector<TvTrack>& tracks)
{
    tracks.clear();
    if (pMainFrame->GetLoadState() != MLS::LOADED || pMainFrame->GetPlaybackMode() == PM_DVD) {
        return false;
    }
    IAMStreamSelect* pSS = pMainFrame->m_pAudioSwitcherSS;
    DWORD cStreams = 0;
    if (!pSS || FAILED(pSS->Count(&cStreams)) || cStreams == 0) {
        return false;
    }
    for (long i = 0; i < (long)cStreams; i++) {
        AM_MEDIA_TYPE* pmt = nullptr;
        DWORD dwFlags = 0;
        LCID lcid = 0;
        CComHeapPtr<WCHAR> pName;
        if (FAILED(pSS->Info(i, &pmt, &dwFlags, &lcid, nullptr, &pName, nullptr, nullptr))) {
            break;
        }
        CStringW format = AudioCodec(pmt);
        CStringW channels = AudioChannels(pmt);
        if (!channels.IsEmpty()) {
            format += format.IsEmpty() ? channels : L" " + channels;
        }
        bool isCurrent = (dwFlags & (AMSTREAMSELECTINFO_ENABLED | AMSTREAMSELECTINFO_EXCLUSIVE)) != 0;
        tracks.push_back(MakeTrack(i, lcid, pName ? CStringW(pName) : CStringW(), format, isCurrent));
        if (pmt) {
            DeleteMediaType(pmt);
        }
    }
    return !tracks.empty();
}

void CTvTracks::SelectAudio(CMainFrame* pMainFrame, int index)
{
    pMainFrame->SetAudioTrackIdx(index);
}

bool CTvTracks::GetSubtitles(CMainFrame* pMainFrame, std::vector<TvTrack>& tracks, bool& subtitlesOn)
{
    tracks.clear();
    subtitlesOn = AfxGetAppSettings().fEnableSubtitles;
    if (pMainFrame->GetLoadState() != MLS::LOADED || pMainFrame->m_fAudioOnly || !pMainFrame->m_pCAP) {
        return false;
    }

    const int current = pMainFrame->GetCurrentSubtitleTrackIdx();
    int idx = 0;
    // Where each track comes from, on its second line: in the file, or a separate subtitle file
    auto withSource = [](TvTrack t, bool external) {
        const CStringW source = external ? L"External file" : L"Embedded";
        t.detail = t.detail.IsEmpty() ? source : source + L"  -  " + t.detail;
        return t;
    };

    // Same enumeration order as CMainFrame::GetCurrentSubtitleTrackIdx / SetSubtitle.
    POSITION pos = pMainFrame->m_pSubStreams.GetHeadPosition();
    while (pos) {
        SubtitleInput& subInput = pMainFrame->m_pSubStreams.GetNext(pos);
        if (CComQIPtr<IAMStreamSelect> pSSF = subInput.pSourceFilter) {
            DWORD cStreams = 0;
            if (FAILED(pSSF->Count(&cStreams))) {
                continue;
            }
            for (int j = 0; j < (int)cStreams; j++) {
                AM_MEDIA_TYPE* pmt = nullptr;
                DWORD dwFlags = 0, dwGroup = 0;
                LCID lcid = 0;
                CComHeapPtr<WCHAR> pName;
                if (FAILED(pSSF->Info(j, &pmt, &dwFlags, &lcid, &dwGroup, &pName, nullptr, nullptr))) {
                    continue;
                }
                if (dwGroup == 2) {
                    tracks.push_back(withSource(MakeTrack(idx, lcid, pName ? CStringW(pName) : CStringW(),
                                                          SubtitleFormatFromType(pmt), subtitlesOn && idx == current), false));
                    idx++;
                }
                if (pmt) {
                    DeleteMediaType(pmt);
                }
            }
        } else if (CComPtr<ISubStream> pSubStream = subInput.pSubStream) {
            CStringW format = SubtitleFormatFromStream(pSubStream);
            for (int j = 0, cnt = pSubStream->GetStreamCount(); j < cnt; j++) {
                CComHeapPtr<WCHAR> pName;
                LCID lcid = 0;
                if (FAILED(pSubStream->GetStreamInfo(j, &pName, &lcid))) {
                    pName.Free();
                    lcid = 0;
                }
                // tracks inside the file can also arrive this way (MPC-HC's own renderer); subtitle
                // files MPC-HC loaded itself are listed in m_ExternalSubstreams
                const auto& ext = pMainFrame->m_ExternalSubstreams;
                const bool external = std::find(ext.begin(), ext.end(), static_cast<ISubStream*>(pSubStream)) != ext.end();
                tracks.push_back(withSource(MakeTrack(idx, lcid, pName ? CStringW(pName) : CStringW(), format,
                                                      subtitlesOn && idx == current), external));
                idx++;
            }
        }
    }
    return true;
}

void CTvTracks::SelectSubtitle(CMainFrame* pMainFrame, int index)
{
    pMainFrame->SetSubtitleTrackIdx(index);
}

bool CTvTracks::GetSubtitleDelay(CMainFrame* pMainFrame, int& delayMs)
{
    delayMs = 0;
    if (pMainFrame->GetLoadState() != MLS::LOADED) {
        return false;
    }
    if (pMainFrame->m_pDVS) {
        int speedMul = 0, speedDiv = 0;
        return SUCCEEDED(pMainFrame->m_pDVS->get_SubtitleTiming(&delayMs, &speedMul, &speedDiv));
    }
    if (pMainFrame->m_pCAP) {
        delayMs = pMainFrame->m_pCAP->GetSubtitleDelay();
        return true;
    }
    return false;
}

void CTvTracks::ChangeSubtitleDelay(CMainFrame* pMainFrame, int deltaMs)
{
    pMainFrame->SetSubtitleDelay(deltaMs, true);
}

bool CTvTracks::GetAudioDelay(CMainFrame* pMainFrame, int& delayMs)
{
    delayMs = 0;
    if (pMainFrame->GetLoadState() != MLS::LOADED) {
        return false;
    }
    CComQIPtr<IAudioSwitcherFilter> pASF = FindFilter(__uuidof(CAudioSwitcherFilter), pMainFrame->m_pGB);
    if (!pASF) {
        return false;
    }
    delayMs = static_cast<int>(pASF->GetAudioTimeShift() / 10000); // REFERENCE_TIME -> ms
    return true;
}

void CTvTracks::ChangeAudioDelay(CMainFrame* pMainFrame, int deltaMs)
{
    int current = 0;
    if (GetAudioDelay(pMainFrame, current)) {
        pMainFrame->SetAudioDelay(static_cast<REFERENCE_TIME>(current + deltaMs) * 10000);
    }
}

CStringW CTvTracks::CurrentAudioSummary(CMainFrame* pMainFrame)
{
    std::vector<TvTrack> tracks;
    if (GetAudio(pMainFrame, tracks)) {
        for (const auto& t : tracks) {
            if (t.isCurrent) {
                return t.title;
            }
        }
    }
    return L"";
}

CStringW CTvTracks::CurrentSubtitleSummary(CMainFrame* pMainFrame)
{
    std::vector<TvTrack> tracks;
    bool on = false;
    if (!GetSubtitles(pMainFrame, tracks, on)) {
        return L"Not available";
    }
    if (!on) {
        return L"Off";
    }
    for (const auto& t : tracks) {
        if (t.isCurrent) {
            return t.title;
        }
    }
    return tracks.empty() ? L"None" : L"Off";
}
