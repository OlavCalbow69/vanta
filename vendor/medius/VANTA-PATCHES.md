# Vanta host SDK changes over 3.4.6

Pinned upstream: 837e4ed99275f2783b2de06940198628be80bccf.

`medius_device_set_manual_recovery(device, true, 20)` must be called immediately
after open, before applying any owned input. The flag is per shared link. It disables
automatic reconnect and the keepalive's cached state restoration. Vanta owns its
watchdog, firmware-session validation, reconnection and fresh reservations.
An unsolicited boot hello disconnects a manually managed link instead of allowing
old input to reach the new device generation. Normal upstream users retain the
default policy. The query timeout is per link, atomically updated and bounded.

`medius_clip_query_output_status` pipelines HEALTH, CLIP and STATS requests before
waiting for any response. All replies retain upstream's opcode, selector and SEQ
matching, and share one absolute query deadline. STATS is requested last to validate
the newest session before output. A partial send/timeout retires every pending registration. Outputs are
written only after all three responses decode. These are separate readbacks,
not an atomic firmware snapshot. Vanta validates the session and delivery counters
at maintenance and motion-drain readbacks without three sequential USB response waits.
The native Vanta backend sends ordinary MOVE commands directly; this query is
used for health/cleanup, not a continuously refilled output timeline.

The C ABI adds the policy setter and output-readback function; firmware and wire protocol are unchanged.
The SDK is built with MSVC's static CRT. Its MIT license and dependency notices
ship with the receiver package. No firmware source or custom firmware is included.
