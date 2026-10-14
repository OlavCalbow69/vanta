# Stable detector update

This feature build retains stable Vanta's Direct, WindMouse and Axis movement.
It adds contour rejection, mask-top aiming, configurable closing/dilation, and
purple/yellow/red presets. It is a native Windows x64 build; Python is not needed.

## Your 1440p Sunshine setup

1. On the Receiver page, pair with Sunshine and connect at 2560x1440. New local
   settings default to 120 FPS and 150,000 kbps; these settings are saved.
2. On Capture, select Sunshine stream and a centered ROI (640 pixels by default).
   Detection consumes the original decoded pixels; only the UI preview is resized.
3. On TestMove, enable its controls and select a color. Enable Detector preview
   to tune detection without holding the activation button or sending movement.
4. Apply the 1440p preset if loading an older profile. Green boxes are accepted,
   red boxes are rejected, yellow marks are offset aim points, and the white
   circle identifies the nearest eligible point. Diagnostics list rejection reasons.
   Expand Contour measurements to compare actual area, box fill and true solidity.
5. Save a named profile on Configs. Existing profiles retain their legacy detector
   until Enhanced contour detector is enabled or the 1440p preset is applied.

The reference resolution is 2560x1440. Width limits and X offsets scale by decoded
width; height limits and Y offsets by decoded height; areas by both scale factors.
Aspect is measured in reference coordinates. Changing only the ROI from 120 to 640
does not scale a target, thresholds, or offsets. Keep the game's FOV unchanged when
comparing resolutions; retune the reference settings if its projection changes.

## Detector controls

The preset starts with area 5-3500 px2, width 1-160 px, height 1-240 px, merge
proximity 16 px, and mask-top aim with X +1 / Y +7 reference-pixel offsets.
All filter switches and limits are editable. The flat filter rejects height/width
below 0.25 above width 27. The orb filter requires width above 20, height/width
0.70-1.25, bounding-box fill >=0.65, AND convex-hull solidity >=0.90.
Solidity is area/convex-hull area; fill is area/bounding-box area. Compact but
irregular silhouettes are not rejected merely because their box is square.

Processing order is HSV union, circular FOV mask, optional close, one dilation,
FOV mask again, contour filters, contour-preserving merging, and aim calculation.
Closing defaults to 3x3. Closing/dilation independently support 1x1, 3x3, 5x5, 7x7,
measured in actual decoded pixels. Mask-top X averages pixels in the first four
occupied rows of the filled blob, and Y is its first occupied row. Bounding-box
percentage remains selectable; both modes apply independent X/Y offsets last.

Color IDs 0/1 keep the original narrow/wide purple thresholds. Yellow uses
HSV [20,100,100]-[40,255,255]. Red combines [0,100,100]-[10,255,255] and
[170,100,100]-[179,255,255]. Hue is OpenCV 0-179. The purple-specific exact-color
blacklist is not applied to red or yellow. Capture, TestClick and TestMove use
the same multi-range mask helper.

Before an older file is overwritten, a one-time `.schema-N.bak` copy preserves it.
Profiles are schema 8; local settings are schema 7. Older profiles keep the
original 4x4 dilation, bounding-box percentage and old tuning. Use this build
to load the new schemas; earlier executables do not understand them.

## MAKCU 4.094

The existing serial integration probes 4,000,000 then 115,200 baud. When a V4
device needs a speed change, it sends volatile km.baud(4000000), closes/reopens,
and repeats discovery. Streams start after discovery. A single reader handles
text, physical-button packets and binary replies. Binary 0x04 supplies the actual
firmware number shown on Mouse output, rather than mistaking km.MAKCU for a version.
The application does not flash firmware or change persistent device settings.

Stable movement still uses km.move. Firmware 4.094 may interpolate movement over
several USB reports; this build does not promise an exact 1 ms USB report shape.
See the [official protocol](https://makcu.com/en/api/) and
[firmware capability reference](https://makcu.com/api/versions).

## Verification

On 2026-10-14, Debug/Release x64 builds and all six checks below passed in this
workspace. Both ZIPs were extracted and retested with PATH restricted to Windows
directories; their file manifests and archive SHA-256 hashes also matched.
This is a deployment smoke test, not a separate clean-Windows-machine test.
No MAKCU serial device was present, so real 4.094 hardware, live 1440p Sunshine
and practice-scene validation remain pending. The original Python bot files
are unavailable here; synthetic fixtures do not establish exact Python parity.

Run the following against either Debug or Release:

```powershell
.\vanta.exe --detector-self-test
.\vanta.exe --detector-ui-self-test
.\vanta.exe --config-self-test
.\vanta.exe --makcu-self-test
.\vanta.exe --receiver-capture-self-test
.\vanta.exe --receiver-self-test
```

For an attached, otherwise unused management port, `vanta.exe --makcu-probe COMn`
connects, logs the reported firmware, and disconnects without injecting movement
or clicks. The physical board, live Sunshine stream and practice-scene behavior
still need hardware verification. Synthetic tests do not establish real-world
false-target reduction or landing precision.

Package a built configuration with
`scripts\package-stable-detector.ps1 -Configuration Release`. Keep vanta.exe,
vanta-updater.exe and opencv_world500.dll together; the package includes licenses,
build information and a SHA-256 manifest.
The package also includes OpenCV's official x64 Microsoft C++ runtime
dependencies. Keep the included DLLs beside vanta.exe. No Python installation
is needed. A separate clean Windows machine still needs a final deployment check.
