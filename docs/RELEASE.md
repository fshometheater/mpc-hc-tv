# MPC-HC TV mode: release notes, quirks and publishing checklist

This document is for whoever publishes a build of this fork (GitHub release, installer, zip) and for
the people who install it. Building from source step by step is in [`BUILD.md`](../BUILD.md).

Base: MPC-HC (clsid2) **2.8.5**, with the official 2.8.5 LAV Filters (0.83.x). Windows 10 / 11, x64.

---

## 1. What this build is

A personal fork of MPC-HC that adds a **TV mode**: a remote-friendly overlay (arrows, OK, Back) drawn
over the video, made for watching from the couch. Everything else in MPC-HC is unchanged and still
there for the mouse (normal menus, Options dialog, right-click menu). TV mode can be switched off at
any time (View > TV Mode, or the `/tv` command-line switch to force it on).

What TV mode adds, in short:

- **Control bar** at the bottom: seek line with chapter marks, time, "Ends at", play / skip buttons,
  volume, and buttons for Subtitles, Audio, Video, Info, madVR, Settings, Shaders.
  Buttons can be hidden and reordered (TV options > Control bar > Buttons).
- **Audio and subtitle lists** like the built-in players of Emby / Jellyfin: one row per track with
  language, title, codec / format, channels, default / forced flags, current track marked; "Off" for
  subtitles; subtitle and audio delay; selecting applies at once. MPC-HC's own automatic track
  selection (preferred languages) is untouched.
- **Video page**: video renderer, madVR panel, LAV Video decoder settings, pixel shaders on / off.
- **Info panel** (one Info button; Video / Audio tabs, OK switches): source vs. output, HDR / Dolby
  Vision / Atmos badges, details at three levels (Basic / Extended / Nightmare), scrollable by line.
- **Settings menu** (setup only; what's playing is on the bar's own pages): Video (renderer, madVR,
  LAV Video, shaders), Audio (LAV Audio: bitstreaming, DRC, delay, output formats), TV options,
  Advanced (mouse: MPC-HC options, madVR / LAV settings windows), Desktop Mode.
- **TV options**: Appearance (accent colour, font, opacity, menu and bar size, key hints, menu
  auto-close, animations, where the overlay is drawn), Control bar (when it shows, what's on it, its
  buttons: show / hide, order, separators you can add / move / remove), Player (resume the last file
  on start, pause screen, Back with nothing open, and MPC-HC's own remember file position, open in
  fullscreen, leave fullscreen at the end and after playback, read and written live), Seeking, Audio and subtitles (including a cut-down subtitle style
  page), Remote keys (assign keys by pressing them; grouped: TV menu, playback, jumps, audio, subtitles,
  picture, player). Each row offers Set, Set hold (0.6 s; OK can be held too), Edit (the keys as tags,
  OK removes the picked one; only with two or more keys) and Clear, with the current keys beside them.
- **Holding Back** closes every menu at once, from any depth.
- **madVR panel** (only when madVR is the renderer), in three groups: *Display* (properties,
  calibration, frame matching, display mode list: add 2160p / 1440p / 1080p / current modes, remove
  modes as tags), *Picture* (HDR in two columns like madVR's own page, upscaling refinement, smooth
  motion, trade quality for performance) and *Tools* (statistics Ctrl+J, madVR's own settings window).
  On / off options are switches; options with an on / off and a value show both on one row (OK flips,
  Up / Down change the value, Back is done).
- **LAV Audio / LAV Video pages**: bitstreaming, DRC, audio delay, output formats; hardware decoder,
  RGB output levels, hardware resolutions / codecs, deinterlacing, dithering; plus buttons that open
  LAV's own property windows.
- **View > Presets > TV**: TV Mode with caption and menu bar, no toolbars.
- **Installer option** "Apply recommended TV settings" (see section 6).

The player's **web interface is unchanged** (port 13579, `/variables.html`: `position`, `duration`,
`state`), so external programs that poll it keep working.

---

## 2. The two ways the overlay is drawn (important)

TV options > Appearance > **Draw the TV overlay**:

| | Inside madVR | Own window (default) |
|---|---|---|
| Needs | madVR as the renderer | any renderer (madVR, MPC Video Renderer, EVR...) |
| HDR passthrough (Windows in HDR) | correct (madVR draws it into its HDR picture) | shown at Windows' "SDR content brightness": may look dimmer / flatter |
| HDR tone mapped to SDR (pixel shaders), or SDR video | correct | the same look (nothing is in HDR) |
| Animation smoothness during playback | tied to the **video frame rate** (see below) | the **display refresh rate** |
| madVR exclusive mode | works | **not visible** |
| MPC-HC "D3D fullscreen" mode | n/a | **not visible** |
| Windowed fullscreen (normal) | works | works |

If madVR isn't the renderer, the overlay is drawn in its own window automatically, whatever the
option says.

**Which to pick:** *Inside madVR* if you use HDR **passthrough** (or madVR tone mapping with "output
video in HDR format"), or madVR's exclusive mode. Otherwise, e.g. **tone mapping HDR to SDR with pixel
shaders** (the most common setup), *Own window* looks the same and animates more smoothly. If Windows HDR
is left on all the time, the own-window menu follows Windows' SDR content brightness slider.

### Why animations look choppy inside madVR

madVR composites the overlay into the video frames it renders. During playback it renders one frame
per video frame, so a 23.976 fps film shows overlay changes at ~24 steps per second, even on a 120 Hz
display. madVR also renders a few frames ahead, so a change can appear a little late. When **paused**,
madVR redraws as soon as the overlay changes, so animations look smoother then.

**Recommendation for madVR users:** set TV options > Appearance > **Animations** to **Fast** or **Off**. Off
makes every transition instant (no fades, slides, crossfades or glides), which reads cleaner at
24 updates per second. Fast keeps a hint of motion.

### Slower PCs: the two heaviest smooth motions

Both redraw the control bar on every frame while they move (otherwise it's redrawn twice a second),
and both are On by default:

- TV options > Appearance > **Smooth playhead**: the playhead dot glides to the playing position (after
  a seek, while dragging with the mouse) instead of jumping. Animations Off also turns it off.
- TV options > Seeking > **Smooth seeking when holding**: holding Left / Right moves the seek preview
  every frame, speeding up smoothly. Off = the older steps (about 7 a second). Not used when seeking
  jumps right away.

### Will madVR's smooth motion help? (untested)

Probably, in one case only. When smooth motion is **active**, madVR renders at the display refresh
rate instead of the video rate, so the overlay should update at the display rate too. But:

- smooth motion is only active when its "enable" rule says so (e.g. "only if there would be motion
  judder without it"); with matching display modes it's usually inactive;
- it changes how the **video** moves (frame blending), which you may not want just for menus;
- not verified on real hardware; treat it as a theory until someone tests it.

### Display refresh rate matching limits both modes

With madVR's **Frame matching** / "switch to matching display mode" (or any refresh-rate switching),
a 23.976 fps film switches the display to 23.976 Hz. The display itself then refreshes 24 times a
second, so **no overlay mode can animate faster than that**. The own-window mode only helps when the
display stays at a high refresh rate (e.g. 120 Hz) during playback.

---

## 3. Known quirks and limitations

### Overlay and input
- The overlay is click-through: the mouse still reaches the video window (seek bar drag, clicks on
  rows, wheel). Clicking the video outside the bar still pauses / toggles fullscreen as in MPC-HC.
- Keys with Ctrl or Shift always go to MPC-HC (so MPC-HC shortcuts keep working).
- Own-window mode follows the video window when it moves or resizes; if a window manager tool moves
  the player without a resize, the overlay catches up on the next update (within a fraction of a second).
- Long lists: a focus move that also scrolls the list moves the highlight at once (no crossfade).

### madVR panel
- Only shown while madVR is the renderer (the madVR bar button also hides itself unless set by hand in
  Control bar > Buttons).
- Device settings (HDR, calibration, properties, display modes) are written to the madVR **device the
  video is on**, found by matching the monitor's EDID. If two devices match, nothing is changed and the
  panel says why.
- Changes are written to madVR's settings immediately, the same settings madVR's own window edits.
  Don't edit the same setting in madVR's window at the same time.
- **Display mode list** ("Display modes" submenu): madVR offers no API to write this value, so the
  player exports madVR's settings record, replaces only that value, imports it and compares a fresh
  export; anything unexpected puts the original back. Verified with madVR **0.92.17 test build 210**;
  other versions may refuse (then nothing changes and the panel says so).
- All madVR option IDs and value lists come from **madVR 0.92.17 test build 210** (madshi's HDR
  measurement build: [announcement](https://www.avsforum.com/posts/64465710/),
  [download](https://madshi.net/madVRhdrMeasure210.zip)). The standard 0.92.17 release may differ;
  options the installed madVR doesn't have are hidden.

### LAV pages
- Settings go through LAV's own interface on the running filter. Options the installed LAV doesn't
  have are left out; if a future LAV changes the interface, the pages disappear instead of misbehaving.
- MPC-HC's built-in LAV saves to MPC-HC's settings (like Options > Internal Filters); a separately
  installed LAV saves its own settings.
- Some changes apply from the next file only (hardware decoder, bitstreaming), as in LAV's own dialog.
- Without playback the pages say "start a file"; the "settings window (mouse)" button then opens
  MPC-HC's stand-alone LAV settings.

### Video page
- **Video renderer** changes MPC-HC's renderer setting and applies from the next file.
- The madVR row only appears while madVR is the renderer.

### Control bar extras (each can be turned off in TV options)
- **Pause screen** (Player > "Pause screen" switch, and "Pause screen after" for the delay): a card with the title, time and
  "Ends at" after a few seconds paused. OK plays, Back hides it, arrows or the mouse bring the bar back.
- **Hold OK on the bar** (Control bar, Off / Opens Info / Opens Settings): when on, a short OK acts when
  the key is let go (a fraction of a second later than before). Remote software that only sends key
  presses without repeats can't hold; OK still works as a press.
- **Seeking stops at chapters** (Seeking): a Left/Right step landing within 5 s of the next chapter
  start lands on it. Holding the key glides past chapters as before.
- **Chapters** bar button: only shown for files with chapters.

### Scopes (Info > Scopes, madVR only) - not in this release
- Held back for now: built only with `TV_SCOPES` 1, on the `feature/tv-scopes` branch.
  Release builds from `tv-mode` show Info with Video and Audio tabs only.
- Waveform of the picture as displayed, the tone curve madVR actually applied (input nits -> output
  nits, measured from the picture), the last 60 s of frame peak / average, and the numbers next to the
  file's MaxCLL / MaxFALL and mastering display.
- Measures only while the Scopes tab is open (about 1-2 ms per frame on madVR's side plus two small
  frame grabs a second). Input values come from 8-bit grabs, fine for scopes, not for calibration.
- SDR output is shown in nits using madVR's "display peak luminance" setting. HDR passthrough output
  is untested so far.

### Info panels
- File details come from MediaInfo (read in the background; "Reading media info..." meanwhile).
- Web addresses (a media server passing a stream URL): MediaInfo reads the stream with HTTP range
  requests (1 MB pieces, at most 64 MB); the title comes from the stream's metadata when it has one.
- The cable signal line (YCbCr / RGB, bit depth, range) needs an **NVIDIA** GPU (NVAPI); other GPUs
  just don't show it.
- Bitstreaming is detected from LAV Audio's own report; HDR / Dolby Vision / Atmos badges come from
  MediaInfo's description of the file.
- The info panel refreshes once a second and only redraws when something changed (live values such as the
  measured refresh rate do change).

### Settings and storage
- TV settings live in MPC-HC's normal settings store (registry `HKCU\Software\MPC-HC\MPC-HC\TvMode`,
  or the `TvMode` section of `mpc-hc64.ini` in portable mode). Uninstalling with "delete settings"
  removes them too.
- TV mode remembers its own View layout (toolbars, caption, playlist) separately from Desktop mode.
- Subtitle style options change MPC-HC's own default subtitle style (Options > Subtitles > Default
  Style), live.

### Performance
- Drawing is cheap (2–12 ms per full redraw at 4K on a current mid-range PC); fades and focus
  crossfades re-use the finished picture instead of redrawing. Timing log for troubleshooting: set the
  environment variable `MPCHC_TV_PERF=1` before starting the player; lines go to
  `%TEMP%\mpc-hc-tv-perf.log`.
- Test switch: `MPCHC_TV_OVERLAY_WINDOW=1` forces the own-window overlay without changing settings.

### Not verified on real hardware yet (as of this writing)
- madVR HDR mode switching mid-playback from the panel.
- The subtitle style page on other hardware (it froze in an early build).
- madVR smooth motion's effect on overlay smoothness.
- The installer's wizard pages (the TV settings box) seen by eye; a silent install / uninstall was tested.

---

## 4. Installing and upgrading

- **Installer**: normal MPC-HC setup plus the TV option box (section 6). The box is ticked by default,
  upgrades included: untick it when upgrading to keep your current settings (a silent install applies it).
- **Zip / portable**: unpack, run `mpc-hc64.exe`. Turn TV mode on once with View > TV Mode or View >
  Presets > TV; it stays on. Nothing else is changed automatically.
- **madVR is not included.** Install it separately and pick it in Options > Playback > Output (or add
  it under External Filters). Without madVR the overlay uses its own window.
- **External players (Emby Theater, Kodi, etc.)**: point the external player at `mpc-hc64.exe`; no extra
  arguments are needed (TV Mode and fullscreen come from the player's own settings). `/tv` still forces
  TV Mode for a player that isn't set up for it. The web interface works as in stock MPC-HC.
- Back up first when replacing an existing player: copy its folder and export
  `HKCU\Software\MPC-HC\MPC-HC` (or copy `mpc-hc64.ini`).
- **Copy the whole folder, not just `mpc-hc64.exe`.** `MediaInfo.dll` (Info panels, Properties >
  MediaInfo) is loaded from the player's own folder, and it must be the version from this build: a
  missing, older or 32-bit one shows "MediaInfo.dll is missing / can't be used" in the Info panel.

---

## 5. Building a release

Full steps and the problems hit along the way are in [`BUILD.md`](../BUILD.md). Summary:

1. Visual Studio 2022 / 2026 with the **v143** toolset, MFC and ATL for v143, Windows SDK
   10.0.26100, NASM on PATH (BUILD.md section 1).
2. **LAV Filters can't be built from source at 2.8.5** (upstream build bug), so the official 2.8.5
   release's `LAVFilters64` folder is used. Copy it, `MediaInfo.dll`, `D3DX9_43.dll`, `mpciconlib.dll`,
   `Lang\` and `Shaders\` from the official 2.8.5 zip (SHA-256 in BUILD.md) next to the built exe.
3. Build the player: `MSBuild mpc-hc.sln /t:Apps\mpc-hc /p:Configuration=Release;Platform=x64`.
4. **Installer** (built and tested 2026-11-23 with Inno Setup 6.7.4):
   - Inno Setup now downloads from its GitHub releases (jrsoftware.org/isdl.php links there). A
     per-user install (`innosetup-6.7.4.exe /VERYSILENT /CURRENTUSER`) puts `ISCC.exe` in
     `%LOCALAPPDATA%\Programs\Inno Setup 6`; no admin needed for building.
   - Copy the two optional files the script looks for (both git-ignored, from the official 2.8.5 zip):
     `MediaInfo.dll` -> `distrib\x64\MediaInfo.dll` and `MPCVR\MpcVideoRenderer64.ax` ->
     `distrib\MPCVR\`. Without them the installer is built silently without MediaInfo (Info panels
     fail) or MPC Video Renderer.
   - `cd distrib` then `ISCC.exe /Q /Dx64Build /O<output folder> mpc-hc_setup.iss` ->
     `MPC-HC.2.8.5.<rev>.x64.exe` (~22 MB). The crash reporter is left out unless `MPC_DRDUMP=1`
     (upstream's crash service; keep it out for this fork).
   - Installing needs admin (all users). Test: `/VERYSILENT /SUPPRESSMSGBOXES /DIR=<scratch>
     /MERGETASKS="!desktopicon,!reset_settings,!tv_recommended,!tv_recommended_existing"`, check the
     files, then `unins000.exe /VERYSILENT /SUPPRESSMSGBOXES` (keeps player settings). Check the
     "Apply recommended TV settings" box appears when running it normally.
5. Smoke test: `mpc-hc64.exe /minimized`, close it, exit code 0. Then the checks in section 7.

---

## 6. "Apply recommended TV settings" (installer only)

A box ticked by default, on new installs and upgrades alike. Where player settings
already exist it reads "(overrides some of your current settings to work best with TV Mode)"; untick it there to keep them. On a new install it reads "(overrides some player settings to work best with TV Mode)". When ticked, the player applies this once on its next start:

- History: keep history, remember last playlist, **remember file position**, **remember DVD position**.
- Fullscreen: launch files in fullscreen; **stay** in fullscreen at the end of a file. The control
  window and the hide-controls / docked-panel options keep MPC-HC's own defaults.
- User interface: modern theme on, **Dark**; MPC-HC's own OSD off (TV Mode draws its own messages).
- Logo: this build's TV logo (Options > Player > Logo, internal, last in the list; 1280 x 720,
  transparent, drawn by `docs/tools/make-tv-logo.ps1`). After it in the list: a blank logo of the same
  size, for no logo without the window shrinking (MPC-HC's own "no logo" is 1 x 1).
- These player settings are set while the settings load, so the dark theme and the logo show on the
  first start; the TV layout follows once the window exists.
- Subtitles: the TV default style (Segoe UI Semibold, 80 %, white text at 60 % opacity, outline 0.75 at
  40 % opacity, no shadow, 20 from the bottom). The 60 % is deliberate: full white is too bright on projectors and
  HDR displays. Users who want it brighter raise Subtitles > Style > Text opacity.
- TV Mode on, with the TV layout (caption and menu bar, no toolbars).
- TV option Player > **Resume the last file on start** on (off without the installer).

View > Presets also has **TV** (key 5: TV Mode with caption and menu) and **TV Minimal** (key 6: TV
Mode, borderless like Minimal). Plain 5 and 6 were unused in MPC-HC's key list. The TV Settings panel
ends with **Exit** (closes the player, as File > Exit) under "Switch to Desktop Mode". In TV Mode with no
file loaded, a click on the empty player (the logo) opens the last file from MPC-HC's recent files;
with no recent file (or history off) it opens File > Quick Open.

- **Resume the last file on start** (TV option, Player): when the player starts in TV Mode with
  nothing to play (no file, disc or device on the command line, no `/slave` or `/add`, no other player
  window open), it opens the most recent file of MPC-HC's recent files, but only if MPC-HC kept a
  position for it. MPC-HC forgets the position when a file plays to the end, keeps none with "remember
  file position" off, and keeps none for files shorter than Options > Player > History > "Remember
  position for files longer than" (5 minutes by default). So a finished film leaves the player on the
  logo; an unfinished one carries on where it stopped. A media server starting the player with its own
  file is never affected. The stream address of a media server counts as a recent file too, so a film
  stopped part-way in Emby resumes through that address (outside Emby's own tracking).

- **Back with nothing open** (Esc, Backspace, Browser Back or the extra Back key): TV option Player
  > "Back with nothing open": **Opens the power menu** (default; Exit, Enter / Exit fullscreen, Restart
  player), **Exits fullscreen** (MPC-HC's own; windowed, the key does what MPC-HC has on it) or **Exits
  the player**. Restart player starts the player again with the same file at the same place (and
  fullscreen if it was), 2 s after this one closes. A media server that launched the player (Emby)
  sees that playback end.
- **Control bar default order**: Volume | Subtitles, Audio | Chapters, Video | Info, Settings |
  Fullscreen, Power (new, bottom right corner). Fullscreen's icon shows enter or leave; Power opens the
  power menu (Back returns to the bar). Saved custom layouts keep their order; the new buttons are
  added at the end of the right side.
- **Video page**: after changing the renderer while a file plays, "Restart playback to apply" reopens
  the file at the same place with the same tracks, and the page says which renderer is playing now.

### Updates
- MPC-HC's first-start question "check periodically for updates?" is gone. The installer has
  **"Check for MPC-HC TV updates automatically (once a week)"**, unticked by default; ticked, it sets
  `Settings\UpdaterAutoCheck = 1`. Unticked leaves the setting alone (never set = off). Options >
  Miscellaneous still changes it.
- The check reads `version-tv.txt` from this repository's `tv-mode` branch (backup: jsDelivr), and
  "Download" / Help > Home page open this repository's releases (`include/mpc-hc_config.h`). The
  repository is private for now, so the check fails quietly until it is public.

### Version numbers
- This build has its own version, **MPC-HC TV 0.1.0**, in `include/version_tv.h`. It is shown as
  "MPC-HC 2.8.5 TV 0.1.0": the installer file `MPC-HC-2.8.5-TV-0.1.0-x64.exe`, Windows' app list
  ("MPC-HC 2.8.5 TV 0.1.0 (64-bit)"), and Help > About ("MPC-HC TV 0.1.0", MPC-HC's build below).
- MPC-HC's own version (`include/version.h`, 2.8.5 + git build number) is left as upstream's: the
  language DLLs must match it exactly, the installer uses it for upgrades, and upstream merges change it.
- **For a release**: raise `MPC_TV_VERSION_PATCH` (0.1.1, 0.1.2 ...; MINOR for bigger steps), build,
  and set `version-tv.txt` to the same number with four parts (`0.1.1.0`). The update check offers
  an update when `version-tv.txt` is higher than the player's TV version. `version.txt` stays upstream's.

### Making the release packages (as done for 0.1.0, 2026-11-23)
1. Merge everything into `tv-mode` and build from a `tv-mode` checkout, so the exe says `(tv-mode)`:
   MSBuild `/t:Apps\mpc-hc` with `Platform=x64`, and with `Platform=Win32` for 32-bit (vsdevcmd
   `-arch=x86 -host_arch=amd64`).
2. The bin folders need the third-party files of upstream's official zip of the same MPC-HC version,
   everything except `mpc-hc.exe` / `mpc-hc64.exe`: `MPC-HC.2.8.5.x64.zip` into `bin\mpc-hc_x64\`,
   `MPC-HC.2.8.5.x86.zip` into `bin\mpc-hc_x86\` (check the SHA-256 GitHub lists).
3. `powershell -File docs\tools\make-release.ps1 -Platform x64 -OutDir <folder>`, then `-Platform x86`:
   installer + portable zip each (the zip has upstream's files minus CrashReporter), with SHA-256.
4. Tag `tv-mode` as `<mpc>-tv-<tv>` (e.g. `2.8.5-tv-0.1.0`), push the tag, and
   `gh release create <tag> --draft --prerelease --verify-tag` with the four files; publish on GitHub.

**Note for Emby / Jellyfin / Plex users:** "remember file position" makes MPC-HC resume files itself.
If your media server handles resume (it passes the start position to the player), untick the box or
turn Options > Player > History > "Remember File position" (or TV options > Player > Remember file position) off afterwards, or both will try to resume.

---

## 7. Pre-publish checklist

### Testing
- [ ] Fresh install with the box ticked: TV mode on, layout correct, settings as in section 6.
- [ ] Upgrade install: box ticked with the "overrides some of your current settings" label; unticked = settings untouched.
- [ ] madVR, windowed fullscreen, SDR and HDR file: bar, lists, info panels, madVR panel.
- [ ] MPC Video Renderer (own-window overlay): same pages, nothing upside down, overlay follows the window.
- [ ] Animations Off / Fast / Normal / Slow in both overlay modes.
- [ ] Track switching (audio / subtitles / Off), delays, chapter marks, seeking (keys and mouse drag).
- [ ] Web interface: `http://localhost:13579/variables.html` returns position / duration / state
      throughout.
- [ ] Desktop Mode: MPC-HC behaves exactly like stock (menus, keys, mouse).
- [ ] Remote keys: assign and clear a key; Back / OK extra keys.

### Licences and credits
- [ ] MPC-HC is **GPL-3.0**: publish the full source of this exact build (the GitHub repo / tag) and
      keep `COPYING.txt` in the package.
- [ ] LAV Filters (GPL-2.0+), MediaInfo (BSD-2-Clause), NVIDIA NVAPI declarations in `TvNvApi.h`
      (MIT, notice kept in the file): keep their notices.
- [ ] **Do not bundle madVR** (separate freeware with its own licence); link to its official page instead.

### Names and trademarks
- [ ] Call it a fork, e.g. "MPC-HC TV mode (fork of MPC-HC 2.8.5)", versioned like `2.8.5-tv.1`; don't
      present it as an official MPC-HC release.
- [ ] madVR, LAV, Emby, Jellyfin, Plex, Dolby, DTS, NVIDIA are named only to say what the build works
      with or shows. No logos or artwork from them are used (the madVR bar button is our own cube mark
      with "mVR", not madVR Labs' logo). Not affiliated with madVR Labs / madVR Envy.
- [ ] No icons, fonts or images copied from Emby or Jellyfin (the track lists only follow their
      behaviour).

### Nothing private in the package or repo
- [ ] No settings files (`mpc-hc64.ini`, registry exports), no test media, no logs
      (`mpc-hc-tv-perf.log`), no madVR `settings.bin`.
- [ ] No server addresses, tokens or library details (Emby / Jellyfin), no personal paths or
      machine details in docs, commit messages, PRs or release notes.
- [ ] `bin\` and build outputs are not committed (`.gitignore`).

---

## 8. Draft text for the GitHub release

> **MPC-HC 2.8.5 TV 0.1.0** (fork of MPC-HC 2.8.5)
>
> A remote-friendly TV mode for MPC-HC: control bar, Emby / Jellyfin-style audio and subtitle lists,
> video / audio info panels, a madVR settings panel, LAV decoder pages, and a TV settings menu, all
> usable with arrows, OK and Back. Desktop use is unchanged; switch TV mode on with View > TV Mode,
> View > Presets > TV (on by default with the installer's recommended TV settings).
>
> - Works with any renderer. The overlay is drawn in its own window by default (smoothest animation);
>   with madVR it can be drawn inside madVR instead (TV options > Appearance), best for HDR passthrough.
> - Drawn inside madVR, menu animations follow the video frame rate during playback; if they look
>   choppy, set TV options > Appearance > Animations to Fast or Off.
> - madVR is not included. LAV Filters 0.83 (from MPC-HC 2.8.5) is.
> - The installer can apply recommended TV settings (note: it turns on "remember file position"; turn
>   it off if your media server handles resume).
>
> Source: this repository, tag `2.8.5-tv-0.1.0`. Licence: GPL-3.0 (MPC-HC). Not affiliated with MPC-HC,
> madVR Labs, Emby or Jellyfin.
