# Physical-input locks: Vanta 1.1.0.13

Python's optional [Fuse inputs](README-PYTHON-FUSE.md) replaces its full movement
lock with per-axis directional filtering on Medius. Existing behavior remains the
default. Click masks, side-button reservations, TestMove and emergency release
remain independent. The full-block descriptions below apply to Existing behavior.

## Actual mask monitoring

The Medius backend obtains all five managed masks with one firmware lock-table
query after a transition batch, plus a bearing-option query. It refreshes this evidence before the first
correction and every 100 ms while correcting, including a due check before another
movement command. Stock MAKCU retains its per-command readback protocol.

Mouse Output separates **Requested**, **Applied after last transition** and
**Last firmware readback**, including the readback's age. The last readback is
historical evidence, not a live observation of the controlled PC's cursor.
Partial directional scales, mismatched masks, stale readback timestamps and failed
queries stop output, invalidate the Medius session and trigger bounded cleanup.
An unresolved cleanup remains visible until a fresh connection verifies recovery.

Diagnostics record `firmware_blocked_mask`, `firmware_passthrough_mask`,
`firmware_mask_age_us`, `firmware_mask_verified`, `firmware_mask_fresh`, raw positive
and negative/with/against scales, bearing configuration, connection epochs, readback failures/drift and correction
release reasons. The timing CSV includes physical input mask readback percentiles.

Merely holding activation does not lock movement when there is no valid nonzero
correction. Standard blocks X/Y; Horizontal Only blocks X; Hybrid restores manual Y
only after outstanding Y has drained. A saved disabled movement-lock setting
remains disabled and is displayed explicitly. Fuse explicitly replaces Python's
movement-lock policy with its selected directional filter, even when that global
movement-lock checkbox is off.

### Build 1.1.0.13 laptop checks

1. Select Python's **Purple (narrow)** preset and compare its mask with the original
   purple preset. Save/reload and verify selected/custom colors remain.
2. Enable movement locking and Standard. During a valid nonzero correction, confirm
   requested X/Y and recent firmware-confirmed X/Y, then verify physical movement
   cannot influence those axes while generated movement still works.
3. Check Horizontal Only and Hybrid: manual Y remains available or returns after
   the vertical-window drain. Activation release, target loss and zero correction
   must restore manual control.
4. Check stalls, feature disable, reconnect, emergency release and shutdown; no old
   output, stuck button or physical mask may remain. Preserve side-button sharing.
5. If manual input still passes, save diagnostics while it happens and retain
   `vanta.log`. Compare requested/confirmed masks, ages, disabled settings and
   recovery messages. Confirmed masks that still pass input fail hardware/firmware
   acceptance; firmware readback alone does not establish physical blocking.

Builds and opt-in checks are compiled only on the development PC. Vanta, Sunshine,
hardware checks and receiver performance tests are not executed there.

## Existing coordination behavior

Physical-input coordination was introduced for stock MAKCU in build 1.1.0.6. It
does not change Python's detector, crop geometry or movement arithmetic, and it
does not flash firmware. The optional Medius setup is in README-MEDIUS.md. Validation on the development PC is
compilation only. Execute the manual checks below on the receiving laptop.

## Controls and behavior

Mouse Output has three saved options, all enabled by default:

- **Lock movement while correcting**: suppress physical X/Y while a fresh,
  nonzero correction is being delivered. Standard, Direct and WindMouse use
  X+Y. Horizontal Only uses X. Hybrid initially uses X+Y, then drains old Y
  before restoring manual Y and continuing X. These are physical-axis masks;
  a mask can suppress physical motion even when that correction's component is
  zero. Valid target loss, deadzone/zero output and processing pauses end the
  correction session. A single missed capture wait does not toggle the mask.
- **Lock generated-click button**: mask physical left-button input around a
  generated left click. The existing randomized 14–34 ms hold is retained.
  Releasing the injected button precedes unmasking. Other buttons and the wheel
  are not blocked by this option.
- **Reserve activation side buttons**: reserve X1/X2 only when assigned to an
  enabled Python, TestMove or TestClick feature. Both physical edges remain
  available to Vanta's serial button monitor while their ordinary mouse-button
  events are blocked on the main PC. Shared reservations remain until their
  last owner is disabled. A stream stall ends movement/click output but leaves
  reservations for enabled features in place.

Initial connection, reconnection and emergency release require a newly observed
released state followed by a press before automated output can resume. Release
and press the activation button if the first held press does not activate.
For Medius, a genuine native positive edge identifying one newly pressed button
also establishes its preceding released state when an initial snapshot was
missing. Cached held state never provides this evidence; ambiguous initially held
buttons must be released first. A button held through emergency release stays
blocked until a real release and new press. Stock cached-mask behavior is retained.
An already forwarded button press cannot be erased by enabling a mask later.
Removing a mask restores the real button's current state; disabling a reservation
while physically holding that button may therefore expose its held state to the
main PC. Release activation before changing bindings or disabling a reservation.

**Release all Vanta locks** disables automation and starts cleanup. Watch the
status until blocked inputs and uncertain state are clear. Re-enable the desired
feature explicitly to resume. Settings remain in local schema 10; shared-profile
schema 9 and Python JSON format are unchanged. Older saved configs use the three
new defaults. Connecting with every lock option off gives write-only output
without a recovery helper; enabling protection in that mode initializes it
through reconnection. An existing helper remains until that connection ends.

## Release and recovery

The controller verifies masks with actual firmware queries; SDK cached mask
values and successful serial writes are not USB-delivery acknowledgements.
It applies only mask changes, so it does not bracket every frame with lock
commands. Click holds and pending-movement waits do not hold the shared device
mutex. Processing without fresh corrections expires after 100 ms.

Normal movement release waits for `km.moving()` to report idle, polling at least
5 ms apart with 20 ms query deadlines and a 500 ms drain limit. A failed or
malformed query invalidates output and requires a fresh serial session before
another readback; late numeric replies cannot satisfy a new query. Unconfirmed
recovery remains visible, even after best-effort release writes.

The hidden helper is the same executable in `--input-recovery` mode. It starts
before normal console, updater or graphics initialization. Vanta needs a ready
helper before taking physical-input locks. After abnormal parent termination or
explicit handoff it attempts recovery for up to eight seconds, subject to Windows
driver/connection timeouts. It uses the exact Windows device instance, a
per-device ownership guard and session token; it never picks an arbitrary COM
port or operates on a newer session. Recovery only sends neutralization/unlock
commands, never movement or button-down commands.

The helper requires the laptop and control cable to remain available. It cannot
guarantee cleanup during laptop power loss, USB removal, or a hung Vanta process
that retains the COM port. Terminating a hung process lets recovery attempt to
take over. Restart this build to verify cleanup, or replug the device if recovery
remains unavailable. Vanta owns X, Y, left, X1 and X2 mask state while connected;
startup restores those managed inputs to passthrough before establishing leases.
Unrelated mouse inputs are untouched. Stock MAKCU V4 lacks the old V3
`km.release` lock timer. No firmware upgrade is performed by this build.

## Diagnostics

- Main log: `vanta.log` beside the executable.
- Recovery log: `vanta-input-recovery.log` beside the executable, appended by
  the hidden helper without replacing the main log.
- **Save diagnostics** includes `makcu_and_features.makcu.input_locks`: requested/applied/
  uncertain masks, owners, readiness, helper health, drain state, failures,
  stale rejections, drain timeouts and timing summaries. Mask bits are X=1,
  Y=2, left=4, X1=8 and X2=16. Owner IDs are manual=0, Python=1,
  TestMove=2 and TestClick=3.
- The outer `movement_submission_p95_ms` includes controller submission work.
  Nested timings distinguish coordinator wait, firmware queries, mask
  transitions and actual movement serial writes. These are receiver-local
  measurements; none establishes complete host-to-display or input latency.

If mask/protocol probing fails, locked automation is prevented from starting
and the error is logged. Compare the report with the actual firmware version;
the last earlier laptop log showed 4.094 and the researched official version
was 4.102. Firmware behavior and 20 ms readback responsiveness still require
verification with the user's receiver, driver and wireless dongle.

## Laptop comparison checklist

1. Keep the previous ZIP and settings backup. Start this build, enable the
   desired feature, release/press activation and inspect lock/helper status.
2. In a normal mouse-button viewer on the main PC, verify reserved X1/X2 press
   and release events are absent while Vanta's physical-button indicator and
   activation still work. Test both buttons, shared assignments, disabling one
   owner and changing a binding. Release buttons before changing settings.
3. Check Python and TestMove Standard, Horizontal Only and Hybrid with a moving
   physical mouse. Verify manual Y remains usable in horizontal operation and
   returns after outstanding Hybrid Y drains. Check deadzone, no target, pauses
   and zero rounded movement restore manual control.
4. Enable TestClick with movement. Check overlapping clicks, held physical
   left, activation release during a generated click and emergency release.
   There must be no stuck injected left button or shared-lock click-hold stall.
5. Exercise stream stalls beyond 100 ms, stop/reconnect, resize, changed stream
   resolution, disabling features and normal exit. Old-generation output must
   be rejected. Verify no mask or injected button remains owned after cleanup.
6. With the control connection still plugged in, terminate Vanta in Task
   Manager while correction/click output is active. Inspect the recovery log
   and verify manual control returns. Immediately restarting Vanta must not
   allow an old helper to unlock the new session. Also inspect helper failure
   and unavailable-device behavior; do not interpret an unconfirmed status as
   successful recovery.
7. Save diagnostics for the same moving content with protection on and off.
   Compare received, decoded, published and processed FPS, submission wait,
   mask-transition/query times and actual serial-write time. No settings are
   automatically reduced. Input firmware changes do not directly raise capture
   or decode FPS.

Optional deterministic checks are available through `--input-lock-self-test`
and `--makcu-self-test`; config round-trip coverage is in `--config-self-test`.
They are compiled but are not executed on the development PC. Hardware recovery
and all performance acceptance remain unverified until the laptop checks.

For rollback, release all Vanta locks and exit cleanly before starting the older
package. If recovery is unconfirmed, restore passthrough using this build or
replug first: the older build does not know about the new physical-input masks.
