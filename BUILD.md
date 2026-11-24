# Building MPC-HC TV

MPC-HC TV is MPC-HC (clsid2/mpc-hc, tag `2.8.5`) plus the TV Mode code on the `tv-mode` branch.
Upstream's `docs/Compilation.md` is the reference for the toolchain; this file records the choices
that work for this fork. Release packaging is in `docs/RELEASE.md`.

## 1. Toolchain (once)

- **Visual Studio 2022 or 2026** (Community is fine), workload *Desktop development with C++*, and these
  individual components (all the same version, x86 & x64):
  - `MSVC v143 - VS 2022 C++ x64/x86 build tools`
  - `C++ ATL for v143 build tools (x86 & x64)`
  - `C++ MFC for v143 build tools (x86 & x64)`
  - a Windows 10 or 11 SDK (e.g. 10.0.26100.0)

  MPC-HC 2.8.5 builds the player with **v143**, also under VS 2026 (`src/platform.props`), so the v143
  components are needed even when VS 2026's own default is v145.
- **Git** (Git for Windows, or the copy that comes with Visual Studio).
- **NASM 2.16.x** on `%PATH%` (`nasm -v`).

## 2. Get the source

Use a short path without spaces.

```bat
cd /d C:\
git clone https://github.com/fshometheater/mpc-hc-tv.git mpc-hc
cd mpc-hc
git checkout tv-mode
git submodule update --init --recursive
```

Create `build.user.bat` (ignored by git). Auto-detection may need help; for example with VS 2026 and
Git from Visual Studio:

```bat
@ECHO OFF
SET "MPCHC_GIT=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git"
SET "MPCHC_VS_PATH=C:\Program Files\Microsoft Visual Studio\18\Community"
SET "MPCHC_WINSDK_VER=10.0.26100.0"
```

## 3. LAV Filters and the other bundled files

At 2.8.5, **LAV Filters does not build from source** (upstream problem: the pre-generated ffmpeg file
lists in `LAVFilters\msvc\ffmpeg\` are older than the ffmpeg submodule, and VS 2026's v145 compiler
fails on dav1d and ffmpeg). This fork therefore uses the LAV Filters, MediaInfo, MPC Video Renderer and
translations of the **official MPC-HC 2.8.5 release**:

1. Download `MPC-HC.2.8.5.x64.zip` (and `MPC-HC.2.8.5.x86.zip` for 32-bit) from the
   [clsid2/mpc-hc 2.8.5 release](https://github.com/clsid2/mpc-hc/releases/tag/2.8.5) and check the
   SHA-256 GitHub lists.
2. Build the player once (§4), then copy everything from the zip **except `mpc-hc64.exe` / `mpc-hc.exe`**
   into `bin\mpc-hc_x64\` (or `bin\mpc-hc_x86\`): `LAVFilters64\` (`LAVFilters\`), `Lang\`, `MPCVR\`,
   `Shaders\`, `Shaders11\`, `Toolbars\`, `MediaInfo.dll`, `mpciconlib.dll`, `D3DX9_43.dll`, `COPYING.txt`.

## 4. Build the player

Build only the player project (every `build.bat` configuration builds the whole solution, including
the LAV Filters projects that fail). In a Visual Studio developer prompt, from the source folder:

```bat
:: 64-bit (vsdevcmd -arch=amd64)
MSBuild.exe mpc-hc.sln /m /v:minimal /t:Apps\mpc-hc /p:Configuration=Release;Platform=x64
:: 32-bit (vsdevcmd -arch=x86 -host_arch=amd64)
MSBuild.exe mpc-hc.sln /m /v:minimal /t:Apps\mpc-hc /p:Configuration=Release;Platform=Win32
```

Or in Visual Studio: right-click the **mpc-hc** project > Build. Output: `bin\mpc-hc_x64\mpc-hc64.exe`
(`bin\mpc-hc_x86\mpc-hc.exe`); the files copied in §3 stay in place.

If the build scripts can't call each other ("'build.bat' is not recognized"), clear the environment
variable `NoDefaultCurrentDirectoryInExePath` in that prompt first.

## 5. First run

- `mpc-hc64.exe /?` shows the command-line help; Help > About shows "MPC-HC TV" and MPC-HC's build.
- To test without touching your normal player settings, put an empty `mpc-hc64.ini` next to the exe:
  MPC-HC then keeps its settings in that file (portable mode). Never commit it.
- TV Mode: start with `/tv`, or View > TV Mode, or View > Presets > TV (key 5).
- madVR is not part of the build; add it the usual way (Options > Playback > Output, or External Filters).

## 6. Merging a new upstream release

```bat
git remote add upstream https://github.com/clsid2/mpc-hc.git   & rem once
git fetch upstream --tags
git checkout tv-mode
git merge <new-tag>
git submodule update --init --recursive
```

TV changes in existing MPC-HC files are small and marked `// TV MODE`; most TV code is in the new
`Tv*.cpp` / `Tv*.h` files. Rebuild and re-check §5. The official zip of the new version replaces the
files from §3 (the translation DLLs must match MPC-HC's version).

## 7. Third-party pieces used by the TV code

- **MediaInfo** (info panels): the `MediaInfo.dll` next to the exe (§3) and MPC-HC's own
  `include/MediaInfo/MediaInfoDLL.h`.
- **NVIDIA NVAPI** (info panel "SIGNAL"): `src/mpc-hc/TvNvApi.h` holds the few declarations used, with
  NVIDIA's MIT notice. `nvapi64.dll` comes with the NVIDIA driver and is loaded at run time, so no
  NVIDIA SDK is needed; PCs without an NVIDIA GPU just don't show that item.

Never commit: `bin/`, `*.ini`, `build.user.bat`, media files, madVR or other third-party binaries.
