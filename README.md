# vanta

`vanta` is a standalone x64 Windows overlay built with Visual Studio 2026.
It does not inject into, target, or depend on another process.

The overlay uses a transparent DirectComposition surface, D3D11, the supplied
modified Dear ImGui fork, FreeType, embedded Poppins and icon fonts, custom
widgets, console/file logging, and the embedded `assets\favicon.ico` icon.
The same embedded Vanta artwork is uploaded as a D3D11 texture and displayed
beside the `VANTA` title in the menu header.
OpenCV 5.0.0 is vendored under `vendor\opencv`; its runtime DLL is copied
beside `vanta.exe` automatically. MAKCU uses the prebuilt 1.3.5 Win64 package
under `vendor\makcu-cpp-1.3.5-win64`; Vanta links its import library through
the ABI-safe opaque C API and copies `makcu-cpp.dll` beside the executable.

## Build

Install Visual Studio Community 2026 with **Desktop development with C++**,
the MSVC `v145` x64 toolset, and Windows SDK `10.0.26100.0`. Open
`vanta.sln` or run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  .\vanta.sln /m /p:Configuration=Release /p:Platform=x64
```

The output is:

```text
build\Release\vanta.exe
build\Release\opencv_world500.dll
build\Release\makcu-cpp.dll
```

## Run

Start the always-on-top overlay:

```powershell
.\build\Release\vanta.exe
```

The transparent DirectComposition surface covers the complete virtual desktop
and remains available while switching between applications. The menu is
constrained to the usable area of the current monitor, including while it is
being dragged between monitors. Only the visible menu owns an input region;
the rest of the desktop remains genuinely click-through.

Press `Insert` to show or hide the menu. When hidden, the overlay is fully
click-through and its window is hidden. Drag the lower-right corner to resize
the menu within the current monitor. Press `End` to close vanta.

Every supported exit route uses the same graceful shutdown path, including
`End`, `WM_CLOSE`, `Ctrl+C`, `Ctrl+Break`, console close, and Windows session
shutdown. Vanta first stops the TestClick and TestMove workers, then capture,
MAKCU/RP2040, configuration, ImGui, DirectComposition/D3D11, the native
outline windows, and their registered window classes. Shutdown is idempotent
and also handles partial initialization failures.

The console output is mirrored to `vanta.log` beside the executable.

## Capture

Vanta opens directly on the `Capture` page and immediately starts Windows
Graphics Capture on the monitor under the cursor. Selecting another backend,
source, window, or monitor restarts capture automatically; there is no manual
Start step.

The Capture page uses the supplied LineFlow sidebar, child panels, animated
combos, checkboxes, sliders, and compact color picker. Its permanent
`Capture output` panel shows live status, source/region dimensions, preview
dimensions, FPS, matching-pixel count when filtering, and the scaled frame.
The custom combo state is safe to close and reopen immediately and long
source lists scroll inside a six-item popup. Reopened combo windows are
explicitly kept above the parent menu. The native capture outline remains
click-through and is ordered directly behind the menu, so it cannot cover or
fight the menu controls.

Capture settings include:

- Windows Graphics Capture or DXGI Desktop Duplication
- a top-level window or monitor
- the full source or a centered square from 128 to 1280 pixels
- original/color-filtered output or a black-and-white mask
- a capture-preview color target independent from TestClick and TestMove
- whether to show the click-through capture-area outline and its color

The default centered region is `640x640`. Captured pixels are processed at
the selected source resolution and only the GUI preview is scaled down to at
most `560x315`. Capture no longer has a 30 FPS timer: it is polled every
render iteration, WinRT uses a three-frame pool, and centered monitor/WinRT
regions are cropped during the D3D11 readback to reduce CPU bandwidth. The
console reports measured capture throughput periodically.

The capture outline uses a fixed 1 px colored line with black edging. The
complete frame remains inside the exact capture rectangle and shares the
selected outline opacity.

The OpenCV filter converts BGRA to BGR and then HSV, supports the original
inclusive range `[144, 106, 172]` through `[151, 255, 255]` and the alternative
range `[135, 63, 102]` through `[155, 255, 255]`, and removes the 75 supplied
exact blacklist colors as BGR values. With `Black/white mask` enabled,
non-matching pixels are black and matching pixels are white.

Windows Graphics Capture captures the selected window directly. Desktop
Duplication captures a monitor; window mode crops that window's visible screen
rectangle, so overlapping windows can remain in that backend's result.

## Mouse output

The `Mouse output` page selects MAKCU 1.3.5 or the RP2040 USB HID bridge.
The MAKCU backend scans for supported CH343/CH340 devices and provides
non-blocking connection handling. It shows the selected port, VID/PID, and
firmware version, and includes high-performance mode, relative movement,
left/right click, and wheel test controls. Auto detect/connect is enabled by
default and retries every two seconds. MAKCU is always preferred; RP2040 is
selected as a fallback when MAKCU is unavailable, and a MAKCU attached later
takes over only after connecting successfully. Auto connection can be disabled
from the page. The legacy `vendor\makcu-cpp` source tree is not compiled or
linked.

Both backends implement clicks as left-down, a 1 ms hold, and left-up. A failed
release is retried three times and remains pending for recovery before later
click/move commands and during the main device tick. Backend switching,
disconnect, rescan, and shutdown force a neutral released state. A TestClick
shot is counted only after the release succeeds.

## TestClick and TestMove

Both workers wait for monotonically newer capture snapshots and never process
the same frame twice. TestMove provides `Direct`, stateful `WindMouse`, and
`Axis control` movement. WindMouse advances once per fresh frame, exposes
gravity, wind, maximum-step, and slowdown-radius controls, and resets its
velocity when the key, target, capture, or mouse device is lost.

Axis control defaults to continuous `X + Y` movement and has independent
horizontal/vertical multipliers plus its own smoothing control. `Horizontal
only` always emits zero vertical movement. `Hybrid` uses a 100–1000 ms window:
both axes move during that window, then vertical movement remains disabled
until the hold key is released and held again or TestMove is disabled and
re-enabled.

Optional target behavior includes `Anti Below Objects`, which rejects aim
points below the capture center; randomized short stops with Full Stop or
Slow Move behavior, chance, duration, and slowdown ranges; and a randomized
0–1000 ms delay before a newly selected target starts moving. The new-target
delay keeps tracking an existing target immediately and uses a separate
pending-target identity so the delay persists correctly across fresh frames.
These controls and merge proximity are grouped under the code-icon
`Experimental` section.

TestMove can show a fixed 1 px, anti-aliased Kill FOV circle with black
edging. The circle is drawn with per-pixel alpha in its own topmost,
click-through, capture-excluded native window. It remains behind the menu,
stays visible while the menu is hidden, and is clamped to the active capture
rectangle.

## Bomb Timer

The `Bomb Timer` page samples a small raw BGRA region from the currently
selected capture source without expanding the centered capture or creating a
second capture session. Its default normalized region maps to
`918,6,79,68` at 1920×1080 and detects RGB `(170,0,0)` with a per-channel
tolerance of 30. Three matching pixels across three fresh frames start one
45-second countdown; three clear frames are required before detection can
re-arm.

The page provides live ROI preview/calibration, detector tuning, manual start
and reset controls, a working 10-second widget test, and a configurable reset
hotkey (Home by default). The countdown is rendered in its own topmost,
capture-excluded native overlay that is permanently click-through and
non-activating. It uses a semi-transparent dark panel, a 1 px
black-accent-black frame, and safe/warning text colors at the fixed 6.9-second
threshold. Three panel styles and three timer fonts include an in-menu preview.
The Bomb Timer page keeps an editor preview visible even while inactive; X/Y
controls move it without making the overlay interactive. Position, style,
font, colors, and opacity are saved locally. No auto-defuse input, UDP listener,
or startup flag is included.

## Configuration

Machine-local settings are loaded before automatic capture starts and saved
atomically at the end of the UI frame in:

```text
%LocalAppData%\Vanta\local.json
```

This includes capture/source identity, mouse backend and selected MAKCU port,
auto-connect preference, Bomb Timer settings/widget position, menu
geometry/options, opacity, and the complete style palette. Missing saved
monitors or windows fall back to an available source.

The `Configs` page manages named, shareable TestClick/TestMove profiles under
`%LocalAppData%\Vanta\profiles`. It supports create/overwrite, load, delete,
import, export, and opening the folder. Profiles exclude counters, connection
state, and machine-local settings. TestClick and TestMove enable states are
included, so loading a profile restores whether each test is active.
Malformed or unsupported-schema JSON is retained for diagnosis and reported
in the log and UI.

## Style

The icon-font glyphs are used in the header, sidebar, and panel titles. The
`Style` page exposes every color from the supplied modified ImGui palette,
including accent, backgrounds, strokes, animation, page, element, label,
description, and text colors. Each entry uses the supplied compact color
picker with its alpha bar, so the original opacity values are editable too.

## Automated renderer test

```powershell
.\build\Release\vanta.exe --self-test 120
```

This creates the always-on-top virtual-desktop surface, renders 120 frames,
and exits with code zero when successful. Renderer/capture self-tests ignore
local configuration and do not write user configuration.

Configuration serialization diagnostics use a temporary directory:

```powershell
.\build\Release\vanta.exe --config-self-test
```

Capture diagnostics:

```powershell
.\build\Release\vanta.exe --self-test 300 --capture-test-winrt
.\build\Release\vanta.exe --self-test 300 --capture-test-duplication
.\build\Release\vanta.exe --self-test 300 --capture-test-winrt-window
.\build\Release\vanta.exe --self-test 300 --capture-test-duplication-window
```
