# TestClick repeat update — 1.1.0.14

TestClick requests a new left click after the previous press/release completes,
while its physical activation button stays held and a fresh captured frame
passes its existing ray checks. Releasing activation, disabling TestClick,
changing its settings, a stale stream or output recovery stops new clicks.
There is no requirement to release and press activation between successful
clicks. The optional burst limiter still controls delays and pauses; saved
settings are preserved. Turn it off for continuous repeat.

Detection, HSV ranges, ray geometry and blacklisting are unchanged. Python's
detection/movement and TestMove's movement calculations are unchanged.

## What changed

- **Activation button** replaces the old **Hold key (MAKCU)** label. The selected
  backend is shown directly in TestClick; the old label did not select a backend.
- TestClick's enable switch now advances its configuration generation. Enable,
  disable and binding changes publish immediately, so an old worker cannot
  overwrite them or keep an emergency-release generation armed.
- Activation or configuration changes during click preparation cancel that
  request and restore its mask. They do not falsely turn a healthy serial link
  into a connection fault. Release/repress cannot revive the old request.
- Medius confirms a finite upload and its settings before starting it. Completion
  requires two content edges to have played, an empty streaming ring and a
  released left override. The firmware's cumulative 32-bit content counter is
  compared with each click's baseline, including wraparound.
- A drained engine may be explicitly stopped only after both edges have played
  and no left override remains. This does not discard ordinary MOVE output.
- The whole completion check has a 100 ms bound. Each individual firmware query
  retains its 20 ms timeout; the generated button hold remains **14–34 ms**.
  Failed/uncertain queries still require recovery and a new serial session.
- The caller completes only after firmware release/readback and physical-mask
  restoration. A failed cycle is not added to **Shots**.

These checks describe firmware state, not a per-click USB-delivery acknowledgement.
Physical behavior and the cause of the previously observed failure still need
confirmation on the receiving laptop.

## Status and reports

TestClick now displays **Click status** directly below its backend. It also shows
requests, starts, confirmations, rejections, cancellations and failures. If Shots
remains zero, the status explains which gate or completion stage rejected it.
Repeated messages and successful completions are logged at most once per second;
changes of rejection/recovery reason are logged immediately.

The existing **Save diagnostics** action includes `testclick_output` and the
output coordinator's per-owner `click_output`. Medius diagnostics also include
the last clip state, buffered bytes, played content edges and held-left state.
Reports remain beside `vanta.exe` in `diagnostics`, with the existing LocalAppData
fallback if the executable directory is not writable. Include the report and
`vanta.log` when reporting a failure.

## Laptop checklist

1. Keep the previous ZIP for rollback and extract this package into a separate
   folder. Confirm file version **1.1.0.14** and the intended Mouse Output backend.
2. Connect Medius and wait for physical-button monitoring. Enable TestClick,
   choose an activation button, and release then press it once after connection.
3. Disable the burst limiter. With a continuously valid target, hold activation
   for several seconds. Shots and Confirmed should keep increasing without
   releasing activation. Started may be one ahead while a click is in progress.
4. Move the target out, release activation, disable TestClick and stall the
   stream separately. New clicks should stop in each case; no injected left
   button or physical left mask should remain stuck.
5. Enable the burst limiter and confirm repeated bursts respect the saved delay,
   size and pause. Disable/re-enable TestClick and change bindings while held.
6. Test Python movement and TestClick together. A finished movement correction
   must not cancel an independent valid click. Check Horizontal Only and Hybrid
   still restore manual Y normally.
7. Use **Release all Vanta locks**. Re-enable TestClick, release then press its
   activation button, and verify repeated clicks work again. Also check reconnect,
   normal shutdown and the existing crash-recovery checklist.
8. If any step fails, capture the visible **Click status** and save diagnostics.
   A counter advancing alone does not prove the controlled PC received the click.

## Build-only validation

Debug and Release x64 compile the opt-in `--testclick-self-test`,
`--input-lock-self-test` and `--medius-self-test` checks. They cover repeated
submission under held activation, burst scheduling, generation rearm,
release/repress during preparation, verification ordering, query failures and
finite-clip completion accounting. The development PC must not execute these
checks, launch Vanta or run receiver/hardware tests. Packaging uses `-SkipTests`.

This is a local 2PC development package with automated-release networking
disabled. It does not publish or replace the stable updater release.
