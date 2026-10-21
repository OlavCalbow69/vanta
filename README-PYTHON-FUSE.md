# Python Fuse inputs — build 1.1.0.13

Fuse inputs provides shared mouse control for accessibility tasks such as selecting
colored plants or flowers. Python keeps its existing color detection, target
selection and integer movement calculations. The user can contribute physical
movement in the same direction on each guided axis. This feature makes no claims
about concealing automation or detection by other software.

## Enable on the receiving laptop

1. Extract `VantaPythonFuse-Release.zip` into its own folder. Keep the previous ZIP
   and configuration for comparison. This build defaults to **Existing behavior**.
2. Connect the mouse through Medius to the controlled PC. Select **Medius 3.4.6**
   in Mouse Output; verify both firmware chips, protocol 9, and confirmed native
   and clone 1,000 Hz rates. Vanta does not install firmware or change polling rate.
3. In Python, choose **Physical input → Fuse inputs**, then enable movement.
   Unsupported/disconnected devices show a reason and cannot arm this mode.
4. Release and press the configured activation button. Guidance continues while
   held even when your physical mouse is still. Release activation for full
   manual control after outstanding motion drains. **Release all Vanta locks**
   disables automation and starts emergency cleanup.

The optional JSON field is `"physical_input_mode": "fuse"`; `"existing"` is the
default for missing fields. Local schema 10 and shared-profile schema 9 are
unchanged. Existing colors, custom settings, Horizontal Only and Hybrid remain.
Python does not arm itself when starting or importing a configuration.

## Shared-control rule

The decision is independent on X and Y, using the sign of Python's accepted
correction. Matching physical counts pass at 100%; opposing counts pass at 0%.
An axis with a zero correction passes physical movement fully. Physical input is
merged by Medius firmware; Vanta never copies and reinjects it.

| Python correction | Physical input | Conceptual combined displacement |
|---|---|---|
| X = -4 | X = -3 | X = -7 |
| X = -4 | X = +3 | X = -4 |
| X = 0 | X = +3 | X = +3 |
| X = -4, Y = +2 | X = -3, Y = -5 | X = -7, Y = +2 |
| Horizontal Only | Any Y movement | Manual Y passes |
| Hybrid after its vertical window | Any Y movement | Manual Y passes after old Y drains |

These examples explain the filter, not a promise that counts arrive in a single
USB report. Device field limits and normal firmware delivery still apply. The
firmware does not acknowledge each delivered human contribution.

Fuse replaces Python's full X/Y movement lock even when Mouse Output's **Lock
movement while correcting** is enabled. Existing behavior, TestMove, manual output,
generated-click masks and shared activation-side-button reservations retain their
own settings. A reserved side button remains observable by Vanta's serial monitor.

## Transitions and recovery

Vanta configures Medius bearing to **per axis, 100 ms** before relative filtering.
Guided axes use fixed positive/negative 100%, with-bearing 100%, against-bearing
0%. Unguided axes use 100% for all four slots. Each correction is sent once using
ordinary native MOVE; there is no new host resampler or high-rate output loop.

A direction reversal, zero/nonzero axis change, Hybrid transition or input-mode
change stops new submissions. Existing motion drains, the old bearing is cleared,
and manual axes are restored. Only a newly arrived source observation can start
the next Fuse correction. Corrections arriving during the drain are discarded;
there is no catch-up burst. A normal movement transition preserves an independent
valid click; faults and emergency release cancel both.

Firmware lock-table and bearing-option readback verify the complete policy before
output and every 100 ms while active. Queries have a 20 ms timeout; pending-motion
queries are spaced by 5 ms with a 500 ms drain limit. A failure or disagreement
invalidates output, attempts bounded cleanup and requires reconnection before
accepting further query replies. HEALTH includes independent clicks, so a valid
click may conservatively delay a motion transition.

Vanta snapshots the original bearing/render/spread/riding settings before changing
them. Confirmed cleanup or the crash helper restores them and clears all four
directional slots of managed X/Y plus managed Left/X1/X2. It leaves other buttons
and wheel settings alone. The helper uses a versioned, size-checked recovery record,
exact device identity and a session guard. Recovery needs the laptop and control
cable to remain available; a process still holding the COM port can prevent it.

## Diagnostics

Expand **Input control status** in Python, or inspect Mouse Output. It shows guided
directions, manual axes, transition/drain state, requested blocks, firmware-confirmed
scales, readback age and helper/recovery status.

Use **Save diagnostics** in Stream Receiver. JSON and timing/rate CSV files normally
go into `diagnostics` beside `vanta.exe`, with the existing LocalAppData fallback.
Keep `vanta.log` and `vanta-input-recovery.log` when reporting an issue. JSON includes
all four requested/confirmed scales, bearing settings, mode, observation/generation
metadata, readback failures, release reasons and ten-second transition timing
percentiles. Transition timing starts when Vanta requests the transition and ends
when the old motion drain and bearing clear finish; it is not full input latency.

Capture FPS, Python processed FPS and accepted movement commands per second are
separate. Rates decay to zero during a stall. A 1,000 Hz mouse is not 1,000 video
frames per second, and accepted commands are not measured delivered movement.

## Laptop comparison checklist

- Compare **Existing behavior** with the movement lock enabled/disabled, then Fuse.
  Verify the same colors, target, crop, correction strength and Purple (narrow).
- While guiding left/right/up/down, move with and against the guidance. Confirm
  matching movement contributes and opposing movement is blocked per axis. Check
  diagonals where only one component matches, and an exactly zero component.
- Check guidance while the mouse is still. Check Horizontal Only and Hybrid:
  manual Y remains available or returns after the vertical window and old Y drain.
- Reverse the target direction quickly; release/repress activation during a
  transition. Confirm old corrections never resume and no catch-up burst occurs.
- Exercise target loss, zero correction, feature disable, settings/mode changes,
  stream stalls, reconnect and shared X1/X2 reservations.
- Run an independent TestClick during guidance and direction changes. Confirm its
  14–34 ms generated hold ends properly, including cancellation and emergency release.
- Check normal shutdown and forced Vanta termination. Confirm no injected button,
  directional scale or physical lock remains. Reconnect and ensure an old helper
  cannot change the newer session. Check original firmware options were restored.
- Save diagnostics during failures and compare preview-visible/hidden throughput.
  Firmware readback alone does not prove actual filtering on the controlled PC.

Development validation is compilation and packaging only: Debug/Release x64 and
opt-in deterministic/fake-device checks compile, but are not executed here. Vanta,
hardware checks, Sunshine sessions and receiver performance tests are not launched.
Previous packages are preserved; stock-only packages need stock firmware restored.
