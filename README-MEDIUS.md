# Medius native output — build 1.1.0.13

Build 1.1.0.13 adds optional [Python Fuse inputs](README-PYTHON-FUSE.md) for
per-axis shared control. It retains narrow purple and ongoing actual firmware mask
verification. See [lock monitoring and laptop checks](README-INPUT-LOCKS.md) for
requested/confirmed mask status, readback ages, drift recovery and acceptance.

Python and TestMove use the movement calculations from the saved pre-ABCurves
build 1.1.0.6. Python retains Standard, Horizontal Only and Hybrid; TestMove retains
Direct, WindMouse and Axis control. The streaming upgrades, input locks, emergency
release, recovery helper, PIN Copy button and recent output diagnostics remain.

ABCurves Renderer/Continuous, resampling, calibration, profiles, models and the
private Python runtime have been removed. No radians-per-count value is needed.
Saved ABCurves settings are ignored and removed on the next save. Stream settings,
detector settings and the selected backend are preserved. Missing backend settings
still default to stock MAKCU. Local/shared configuration schemas stay unchanged.

## Setup on the receiving laptop

1. Install official Medius **3.4.6 on both chips** manually through the official
   [dashboard](https://medius.k4tech.net/dashboard). Firmware is not in this ZIP.
2. Keep the mouse and cloned device at a confirmed **1,000 Hz**. Vanta checks both
   reported periods and protocol 9; it does not change polling rate or firmware.
3. Extract this ZIP to a separate folder. Copy your saved configuration if needed.
   In Mouse Output choose **Medius 3.4.6** and connect using automatic discovery.
   Close another application that is holding the control COM port.
4. Confirm both chip versions and the native output status. Release and press an
   activation button after connecting or changing a binding. A held button at
   connection time does not arm automation.
5. First use Mouse Output's **Move** and **Left click** controls with automation
   disabled, then enable Python or TestMove. The two movement sources remain
   mutually exclusive. No stream is required for manual output controls.

## Output and recovery

Each fresh, valid, nonzero correction is sent once through the SDK's ordinary
signed-16-bit MOVE command. There is no four-millisecond streaming buffer or host
refill deadline. Existing movement multipliers, truncation, smoothing, axis modes
and target selection keep their original meanings.

Vanta captures firmware bearing/render/spread/riding options. It disables render/spread/riding with readback
while connected, sets a verified 100 ms per-axis bearing when Fuse is used, and restores the captured values during confirmed cleanup or
recovery. The SDK automatic reconnect and held-input replay are disabled. The
existing recovery helper must be ready before protected output can arm.

Generated clicks use a **separate finite firmware clip**: press, a 14–34 ms hold,
then release. There is no periodic host refill. Physical left remains masked until
firmware confirms the finite click has ended. Normal movement completion does not
cancel a valid independent click. Emergency recovery cancels both movement and
clicks. Only Vanta-managed axes and buttons are restored; no global reset is used.

Firmware HEALTH reports injection activity, but does not expose pending X/Y counts
or an acknowledgement that each MOVE reached the host USB endpoint. Vanta uses
that actual activity readback conservatively before restoring manual axes. An
independent click may delay that unlock briefly; new clicks are deferred during
the drain. A failed query invalidates the connection before further readbacks.
Cleanup preserves the existing bounded deadlines and unresolved recovery status.

The helper can recover after process termination while the laptop and cable remain
available. It cannot guarantee cleanup after power loss, cable removal or a hung
process retaining the COM port. See [input lock details](README-INPUT-LOCKS.md).

## Diagnostics

Use **Save diagnostics** in Stream Receiver. Automatic FPS reports also remain
enabled. Reports normally go to `diagnostics` beside `vanta.exe`; if that location
is unwritable, Vanta uses its LocalAppData fallback. See [streaming diagnostics](README-STREAMING.md).
The console logger's `vanta.log` and the helper's separate log remain available.

JSON includes firmware identity, rate confirmation, accepted native move commands,
finite click starts/completions/cancellations, activity readback, failures, locks,
ownership and Python's output gates. Timing summaries retain ten-second rolling
median/p95/p99 measurements. Rate CSV separates native move commands from capture
and detector FPS. A 1,000 Hz mouse does not mean 1,000 processed video frames or
1,000 nonzero reports. Send timings measure serial acceptance, not complete USB
delivery or end-to-end display latency.

## Laptop comparison checklist

- Verify manual Move/Left click, then Python Standard, Horizontal Only and Hybrid.
  Check both correction directions and values over 127 counts. Confirm the same
  targets and configured movement strength as the pre-ABCurves build.
- Compare TestMove Direct, WindMouse and Axis control. In Horizontal Only and
  after Hybrid's vertical window, confirm manual Y returns after pending motion.
- Verify activation release/repress, shared X1/X2 reservations, target loss,
  zero correction, intentional pauses, temporary stream stalls and reconnect.
- Exercise TestClick with movement, cancellation while held, emergency release,
  normal shutdown and forced termination. Confirm no generated left hold or
  physical mask remains. Confirm the old helper does not alter a new session.
- Compare stream/detector FPS and native command rate with previews hidden/shown.
  Save diagnostics if movement is blocked or receiver delivery falls behind.

Development validation is **compilation and packaging only**: Debug/Release x64
and opt-in check code compile. Vanta, hardware checks, Sunshine sessions and
receiver performance tests are not run on the development PC. Laptop acceptance
is still required. This change makes no new streaming-FPS performance claim.

Previous ZIPs are preserved. Stock MAKCU packages require stock firmware; do not
use a stock-only rollback package against Medius firmware. The source before this
change is saved under `work/before-abcurves-removal-20261020-163317`.

## Dependencies

Pinned Medius SDK **3.4.6**, commit
`837e4ed99275f2783b2de06940198628be80bccf`, compiled as a static `/MT` library with
Rust 1.99.1. `DEPENDENCIES-MEDIUS.json` records library, header, Cargo lock and patch
hashes. `licenses/` contains the SDK and Rust notices. Reproduce preparation with
`scripts/bootstrap-mouse-output.ps1 -CompileChecks`; it compiles SDK checks with
`--no-run`. Package with `scripts/package-stable-detector.ps1 -Configuration Release
-PackageName VantaPythonFuse -SkipTests`.
