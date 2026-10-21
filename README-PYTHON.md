# Python mode: supplied colorbot recreation

The Python tab replaces Bomb Timer. Detection and movement run in native C++;
no Python installation or bundled interpreter is needed. Its original calculations
can send through stock MAKCU or the optional [Medius backend](README-MEDIUS.md).
The original desktop/Win32/serial Python dependencies are not loaded.
The reference source is kept only under tests/reference for comparison.

## Build 1.1.0.13: Fuse inputs, narrow purple and lock verification

Python now has **Physical input: Existing behavior / Fuse inputs**. Existing
installations retain Existing behavior. With supported Medius, Fuse passes matching
manual movement at 100% and blocks opposing movement per guided axis. Zero-output
axes remain manual, including Horizontal Only and Hybrid's later Y phase.
See [setup, recovery, diagnostics and laptop checks](README-PYTHON-FUSE.md).

Choose **Purple (narrow)** in `active_preset` for HSV **144,106,172–151,255,255**.
Its JSON key is `purple_narrow`. It uses Python's existing mask, morphology and
contour processing, without the separate Vanta detector's purple blacklist.
Older JSON gains this preset when loaded; existing selections and custom presets,
including a saved `purple_narrow`, are preserved. Save JSON persists the addition.
The bundled default still selects the original purple preset.

Medius masks are checked before the first correction and every 100 ms while
correcting, with due checks completed before another command is accepted.
Mask drift or readback failure stops output and enters bounded recovery.
Mouse Output shows requested masks, last firmware-confirmed masks, their age and
release/recovery reasons. A disabled movement-lock option is stated explicitly.
Firmware confirmation is not proof that physical movement was blocked on the
controlled PC: that still requires the laptop comparison in README-INPUT-LOCKS.md.

Python diagnostics include its axis mode, effective axes, target, requested
correction and activation state. The lock section records actual directional
scales, bearing settings, readback timing, failures, drift and release reasons.
Fuse is a shared-control option; it does not change the pure processing algorithm.

## Use

1. Pair/start Sunshine on Stream Receiver. Keep native decoded resolution.
2. Open Python. The supplied settings are embedded and distributed in
   python-config.json. Load JSON can also import the original config.json.
3. Check the mask/contour/aim preview and the detected Device in Mouse output.
   MAKCU uses automatic COM-port detection. A remembered port is preferred only
   while it is detected; otherwise the available detected device is selected.
   The COM-port lock has been removed. Legacy Python makcu_port and
   makcu_override_port fields are ignored. To select between detected devices,
   disable Python, disconnect MAKCU, choose the Device, and connect again.
4. Enable Python movement, then hold the configured physical MAKCU button.
   Supplied aim_key 0x05 means X1 (Side Button 1). Releasing it stops movement.
5. Disable Python before returning to normal TestMove. Enabling one movement
   mode disables the other. An output-owner/generation guard also rejects stale
   reports and prevents both workers from sending movement together.

Python never arms itself on startup, JSON import, restore, or profile load.
Save JSON writes the original-style schema with unknown metadata preserved.
Local settings (schema 10) and shared profiles (schema 9) also store Python settings,
but never its enable state. Legacy schema 8 files get a .schema-8.bak before
being overwritten; old detector tuning is retained and old timer fields dropped.
TestClick remains independent: disable it to reproduce the script's no-shoot
behavior. Python never clicks or injects keyboard input. Physical-input masks
are managed centrally through the Mouse Output lock settings; see
[README-INPUT-LOCKS.md](README-INPUT-LOCKS.md).

Python has an **Axis mode** selector beside smooth_factor:

- **X + Y** uses the supplied movement formula on both axes.
- **Horizontal only** sets generated Y to zero.
- **Hybrid** permits X + Y for **Hybrid vertical time (ms)** after activation
  is observed, then sets generated Y to zero while X continues until release.
  The interval defaults to 300 ms and accepts 1..10000 ms. Releasing and pressing
  the activation button starts another window; losing a target does not restart it.

These output options retain the same masks, greedy horizontal merging, target
selection, crop, offsets and calculated X movement. With physical-input locks
enabled, manual Y stays available in Horizontal Only and returns after pending
Hybrid Y drains; Standard and the initial Hybrid window lock both axes while
correcting. Hybrid is a Vanta extension to the supplied script;
it does not add TestMove's extra speed, smoothing, filters or target tracking.

JSON saves **axis_mode** as a string (standard, horizontal_only or hybrid), plus
**hybrid_vertical_time_ms**. Local settings and shared profiles retain both.
Old JSON without axis_mode uses its horizontal_only boolean, or X + Y when that
field is also absent. New configurations default to X + Y. The legacy boolean
is also saved for compatibility. Legacy COM-port fields are removed when Python
settings are reserialized; unknown unrelated metadata is preserved. Existing
configuration schemas remain unchanged.

Binary 1.1.0.5 fixes Hybrid's duration control, saves axis_mode as a string and
handles configs with or without the legacy COM fields. Debug/Release validation
is compilation only; manually validate Hybrid and saved settings on the laptop.

## Reference behavior retained

- Centered 2R by 2R crop using actual decoded source dimensions. Supplied R=60
  means 120x120, independent of Vanta's Capture ROI (640 by default).
- Exact supplied HSV ranges: purple 130/60/70 to 165/255/255; yellow
  25/80/80 to 35/255/255; red 0/90/90 to 10/255/255 OR
  170/90/90 to 180/255/255.
- Fixed 3x3 rectangular close, then one 3x3 dilation, then the circular FOV.
  This order intentionally differs from normal Vanta enhanced detection.
- External/simple contours; min_contour_area through 2000 area; width at most
  120, height at most 180; flat rejection at h/w < 0.25 and width > 20;
  orb rejection at h/w 0.70..1.25, box fill > 0.65 and width > 15.
  The script calls box fill "solidity"; this is not convex-hull solidity.
- Greedy first horizontal merge within 12 pixels, no Y-distance restriction.
  There is no additional transitive merge or sticky target tracker.
- Filled merged masks. X averages all mask pixels from the top row through
  top+3; Y is the top row. Apply X/Y offsets, truncate X toward zero, choose
  the nearest result. Do not recheck the offset point against the FOV.
- Calculate int(error * smooth_factor) on each axis, truncating toward zero.
  X + Y sends both results; Horizontal only suppresses Y; Hybrid suppresses Y
  after its activation window. X retains the same calculated result in every mode.
  Supplied smoothing is 0.4030150753768844, offsets +1/+7, min area 1.
  Preserve zeros on individual axes; send nothing when both are zero.

No 1440p scaling, Vanta purple blacklist, extra speed, deadzone, curve planner,
prediction, target-height percentage, or normal detector settings are applied.
Changing Sunshine resolution therefore changes the apparent size just as it
would when changing the original desktop's pixel resolution.

The original stores filter_orbs but never tests it: orb rejection is always on.
screen_width/height and hex_variations/name are metadata only. They are retained,
not given invented effects. Source screen centering used actual Windows metrics;
the port uses actual Sunshine dimensions instead.

## Intentional integration differences

The original polls desktop capture and Win32 key state continuously; this mode
consumes only fresh decoded Sunshine frames and MAKCU physical button state.
It does not repeat stale frames. Stream FPS, decode/network latency, dropped
frames, and serial transport mean temporal behavior is not one-to-one.
There is no exact 1 kHz report-delivery guarantee.

Mouse key mappings: 0x01 left, 0x02 right, 0x04 middle, 0x05 X1, 0x06 X2.
Other imported hex keys are preserved but cannot arm because MAKCU's mouse
monitor does not provide keyboard key state.
Debug enables movement tracing. auto_reconnect controls native reconnect;
an explicit arm still attempts one connection when reconnect is disabled.
Detected devices must complete the normal MAKCU identity handshake.
The mode shares Vanta's single device connection and serial reader.
The imported reconnect setting is applied on arm to the shared MAKCU connection;
imported COM-port fields are ignored.
Invalid config, missing frames/buttons/device, oversized crop, or signed-16-bit
report overflow never produces movement. There is no firmware flash.

## Verification

On the laptop, select Hybrid and hold activation: check X + Y during the chosen
interval, X only afterward, and a fresh interval after release/repress. Save the
JSON, reload it, and restart Vanta to check the mode and interval persist. Also
check a legacy horizontal_only configuration and automatic COM-port selection.

The dated results below describe the previous detector/Python build. See
[streaming-240](README-STREAMING.md) for the new crop delivery path and current
CPU-only versus receiver hardware validation instructions.

On 2026-10-15, the test-only harness compared 204 deterministic cases against
the supplied update_settings/get_color_mask/find_target methods. Masks, accepted
contour measurements/order, aim points and integer reports matched exactly.
It AST-selects only those pure methods, never executes the original constructor,
imports, main loop, desktop capture or hardware access. Reference OpenCV is
4.12.0; bundled native OpenCV is 5.0. The fixtures cover hue boundaries, morphology,
FOV boundaries, all rejection types, merges, offsets, truncation, resolution,
unused fields, irregular shapes and seeded scenes.
Both Debug/Release passed all seven native checks, updater tests and 120-frame
GUI checks. The Python GUI fixture also tests both movement-mode transitions
and disarming on JSON reload. Extracted bundles passed native/GUI/updater checks
with PATH restricted to Windows directories; manifests/archive hashes matched.

Run native tests without Python:

```powershell
.\vanta.exe --python-self-test
.\vanta.exe --config-self-test
.\vanta.exe --receiver-capture-self-test
.\vanta.exe --self-test 120 --page-test 7
```

Optional developer parity test (test dependencies only, not a runtime dependency):

```powershell
py -m venv build\python-parity-env
.\build\python-parity-env\Scripts\python.exe -m pip install numpy==2.2.6 opencv-python-headless==4.12.0.88
.\build\python-parity-env\Scripts\python.exe tests\python_parity.py --exe build\Release\vanta.exe
```

No physical serial device was available here. Live Sunshine, actual MAKCU
activation/connection/reconnect and practice-scene movement still need hardware
validation. These tests do not prove identical frame timing or real-world accuracy.
