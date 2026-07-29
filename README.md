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

Open `vanta.sln` in Visual Studio Community 2026 or run:

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
shutdown. Vanta first stops capture and its WinRT/DXGI resources, waits for
pending MAKCU connection work, then releases ImGui, DirectComposition/D3D11,
the outline and overlay windows, and their registered window classes. Shutdown
is idempotent and also handles partial initialization failures.

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
- whether to show the click-through capture-area outline, its color, and its
  1–10 px colored-line thickness

The default centered region is `640x640`. Captured pixels are processed at
the selected source resolution and only the GUI preview is scaled down to at
most `560x315`. Capture no longer has a 30 FPS timer: it is polled every
render iteration, WinRT uses a three-frame pool, and centered monitor/WinRT
regions are cropped during the D3D11 readback to reduce CPU bandwidth. The
console reports measured capture throughput periodically.

The capture outline uses a fixed 1 px black outer edge, the selected colored
line, and a fixed 1 px black inner edge. All three layers remain inside the
exact capture rectangle and share the selected outline opacity.

The OpenCV filter converts BGRA to BGR and then HSV, applies the inclusive
range `[144, 106, 172]` through `[151, 255, 255]`, and removes the 75 supplied
exact blacklist colors as BGR values. With `Black/white mask` enabled,
non-matching pixels are black and matching pixels are white.

Windows Graphics Capture captures the selected window directly. Desktop
Duplication captures a monitor; window mode crops that window's visible screen
rectangle, so overlapping windows can remain in that backend's result.

## MAKCU

The `MAKCU` page scans for supported CH343/CH340 devices and provides
non-blocking connection handling. It shows the selected port, VID/PID, and
firmware version, and includes high-performance mode, relative movement,
left/right click, and wheel test controls. Device actions only occur after an
explicit button press; Vanta scans on startup but never auto-connects. The
legacy `vendor\makcu-cpp` source tree is not compiled or linked.

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
and exits with code zero when successful.

Capture diagnostics:

```powershell
.\build\Release\vanta.exe --self-test 300 --capture-test-winrt
.\build\Release\vanta.exe --self-test 300 --capture-test-duplication
.\build\Release\vanta.exe --self-test 300 --capture-test-winrt-window
.\build\Release\vanta.exe --self-test 300 --capture-test-duplication-window
```
