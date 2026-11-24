# madVR settings map (madVR 0.92.17 test build 210)

IDs as madVR 0.92.17 test build 210 reports them ([download](https://madshi.net/madVRhdrMeasure210.zip), [announcement](https://www.avsforum.com/posts/64465710/)); the standard 0.92.17 release may differ. To list them for another build, set `MPCHC_TV_MADVR_DUMP=<file>` and open the TV madVR panel.

Found with madVR's own settings enumeration (`IMadVRSettings2`, read-only tool) plus the definition
records in `mvrSettings64.dll` (`<type\id\label\help\default\range-or-list\...>`) and the dropdown item
lists in `madHcCtrl.exe` (Delphi form data: `TComboBox | <Name> | ... | Items.Strings | a | b | ...`).
Device settings are `devices\<device id>\<folder>\<id>`; the TV panel uses the device of the display the
video window is on (EDID match). Global settings use their full path.

Dropdowns that madVR stores as a number store the item index (0 = first item) unless noted.

## devices\<id>\hdr  (madVR page "hdr")
| UI (madVR) | id | type / values |
|---|---|---|
| let madVR decide / passthrough / tone map pixel shaders / 3DLUT | hdrMode | auto, passthrough, toneMapMath, toneMap3dlut |
| send HDR metadata to the display (passthrough) | sendHdrMetadata | bool |
| display peak luminance (nits) | hdrNits | 20..10000 |
| global desaturation | bakedInDesat | 0..20 = 0%..200% step 10 (5 = "50% (recommended)") |
| highlight saturation | highlightSat | 0..9 = -4, -3, -2, -1, balanced, +1 .. +5 |
| color tweaks for fire & explosions | hdrHueShift | 0 high strength, 1 balanced, 2 disabled |
| highlight recovery | hdrHighlightRecovery | 0..5 = disabled, low, medium, high, very high, are you nuts!? |
| highlight recovery strength | hdrHighlightRecoveryStrength | 0..100000 (100) |
| contrast recovery | hdrContrastRecovery | 0..16 = disabled, Manni's L80, Soulnight's Mirror, Neo-XP's Equalizer, Fer15's old Medium, Neo-XP's 1.0 High, Log Very Low, Log Very Low - Low, Log Low, Log Low - Medium, Log Medium, Log Medium - High, Log High, Log High - Very High, Log Very High, Log Very Very Low, Log Very Very Very Low |
| shadow recovery | hdrShadowRecovery | 0..6 = disabled, low, medium, high, very high, extreme, insane |
| dynamic clipping [x] / value | applyDynamicClipping / dynamicRangeRecoveryStrength | bool / 0..100 |
| clip at 4000 nits | clip4000 | bool |
| use "Stim" lum method | useStim | bool |
| output video in HDR format | hdrOutputHdr | bool |
| no scene change if APL is within [x] / percent / nits | noSceneChange / nscPercent / nscNits | bool / int / int |
| No scene cut if M2 < / Definite cut if M1 > / M1 thres / M2 thresh | M2Low2New / M1High / M1Thresh2New / M2Thresh2New | ints |
| adjust Scene cut detection for APL differences | adjSceneCutApl | bool |
| TM curve / punch | tmCurve / tmCurvePunch | 0..6 = pluto, mars, saturn, mercury, phoebe, ganymede, luna / 0..19 = "1".."20" |
| use custom TM curves | useCustomTmCurves | bool (curves themselves: customTmCurves, edited in madVR) |
| no diff, 20% dif, 50%, 100%, 200%, 400%, 800% | dif0speed2, dif20speed2, dif50speed2, dif100speed2, dif200speed2, dif400speed2, dif800speed2 | ints |
| flicker risk multiplier | flickerRiskMul | 1.. |
| dynamic target nits [x] / value | applyTargetNitsSelection / dynamicTuning | bool / 0..500 (verified) |
| don't add peak nits | dontAddPeakNits | bool |
| avgHighlights ceiling [x] / value | disableAvgH (inverted) / avgHFactor | bool / 150..400 = 1.50x..4.00x step 0.25 |
| no compr limit | noComprLimit | int |
| FALL highlight mod | fallHighlight | 0..10 = off, 90%, 80% .. 0% |
| detail threshold | fallDetail | 0..20 = off, 1..20 |
| sky strength / max width / sky hill height min / max | skyStrength / maxSkyWidth / minSky / maxSky | ints |
| limit HSTM [x] / min / max | limitContrastRecovery / limitContrastRecoveryMin / limitContrastRecoveryMax | bool / ints |
| no brightening / no darkening | hstmNoBrighten / hstmNoDarken | bool |
| flicker protection [x] / milliseconds | flickerProtect / flickerProtectMs | bool / int |
| do shadow recovery only if at least [x] / % of pixels / below nits | dynShadowBoost / dynShadowBoostPercent / dynShadowBoostNits | bool / ints |
| custom gamut (not in the TV panel) | customGamut, customGamutRx..By | |

## devices\<id>\calibration
calibrate (text: disable calibration controls for this display | this display is already calibrated |
calibrate this display by using yCMS | calibrate this display by using an external 3dlut file),
disableGpuGammaRamps (bool), displayPrimaries (BT.709 (HD) | BT.601 (SD) | PAL | BT.2020 (UHD) | DCI-P3 |
something else | Auto), displayGammaCurve (pure power curve | BT.709/601 curve | something else),
displayGammaValue (1.60 .. 3.00 step 0.05, as text), report2020 (bool), external3dlutFile* (paths).

## devices\<id>\properties
levels (TV Levels | PC Levels | Custom | Auto; Custom uses black/white 0..48 / 200..255),
displayBitdepth (0..10, 0 = auto), 3dFormat (none | auto | side-by-side | top-and-bottom |
line alternative | column alternative), swapEyes (bool), ipControlType / ipControlAddr, pauseLensMem /
resumeLensMem.

## devices\<id>\displayModes
enableDisplayModeChanger ("switch to matching display mode..."), changeDisplayModeOnPlay,
restoreDisplayMode, restoreDisplayModeOnClose, slowdown, hackD3d (bools); the mode list ("list all
display modes madVR may switch to", e.g. `1080p23, 720p50, 1366x768p59, 1080i29`, OS refresh names)
is the binary `displayModesData` - format still unknown (empty on both devices so far).

## rendering\smoothMotion
smoothMotionEnabled (bool), smoothMotionMode (avoidJudder | almostAlways | always =
"only if there would be motion judder without it" / "... or if the display refresh rate is an exact
multiple of the movie frame rate" / "always").

## scalingParent\upRefine  (upscaling refinement)
refineOnce, upRefSharpenEdges (+Strength 1..40, shown /10), upRefCrispenEdges (+Strength 1..40, +LL
linear light), upRefThinEdges (+Strength 1..80), upRefEnhanceDetail (+Strength 1..40),
upRefSoftenEdges (+Strength 1..3), upRefAddGrain (+Strength 1..4), upRefLumaSharpen (+Strength
10..1000, shown /100), upRefAdaptiveSharpen (+Strength 1..100 shown /10, +Linear), upRefSharpenAB
(+Strength 1..150 %), upRefSharpenAR, superRes (+Strength 1..4, +Linear, +AB / ABStrength, +AR).
