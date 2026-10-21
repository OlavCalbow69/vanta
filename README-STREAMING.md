# Vanta streaming-240 receiver

This build targets 2560×1440 at 240 stream/processing FPS. The receiver UI and
previews run independently at up to 60 FPS. A laptop's 1920×1080, 144 Hz panel
does not change the negotiated remote frame size or limit processing to 144 FPS.
The 237 FPS and 12.5 ms p95 acceptance targets are **not measured results**.
Receiver hardware and performance acceptance must be run on the laptop.

## Start and compare

1. Extract `VantaStreaming240Diagnostics-Release.zip` into a new folder. Keep
   `VantaStreaming240-Release.zip`, `VantaStableDetector-Release.zip` and their
   extracted folders for comparison/rollback.
   Use `BUILD-INFO.txt` and `MANIFEST.sha256` to identify each build.
2. Open Stream Receiver, pair with the Sunshine PC, and choose the
   **1440p/240** preset. It selects 150,000 kbps and Auto codec. Existing saved
   configurations keep their resolution, FPS, bitrate and H.264 preference.
3. Auto intersects refreshed host capabilities with the chosen GPU's hardware
   profiles/resolution. Preference order is AV1, HEVC, H.264. The GTX 1650 Ti
   should select HEVC when Sunshine offers it; AV1 is unavailable on that GPU.
   Forced codecs stay fixed and fail explicitly if unsupported. No CPU decode
   fallback, automatic bitrate reduction or automatic resolution/FPS reduction
   is performed.
4. Choose Auto adapter to prefer capable NVIDIA hardware. Explicit adapter
   changes take effect after restarting Vanta. Diagnostics identify the adapter,
   driver, attached displays, synchronization and supported codecs. On laptops
   with an integrated display GPU, desktop composition may still cross adapters.
5. In Capture, select a centered 640×640 native region for the main throughput
   comparison. Python independently requests exactly `2*fov_radius` pixels per
   axis; its detector does not receive a second crop. Full-source capture is an
   explicit region choice/subscription and costs substantially more bandwidth.

The stream budget includes common-c's FEC allowance. The encoded-payload rate
counts completed compressed access units, excluding FEC, protocol overhead and
lost data. Observed Ethernet RX/TX measures **all traffic across active Ethernet
interfaces**, including other applications; it is not a stream-only wire rate.
Encryption, the 1392-byte LAN packet setting and router/host tuning are unchanged.
The existing display-mode request checkbox remains user-controlled: disable it
when the Sunshine PC's display mode must stay unchanged.

## Laptop acceptance checklist

- Use identical moving content on the host and allow at least 15 seconds of
  steady delivery before recording each result. Compare H.264 and HEVC at
  1440p/120, /144 and /240, initially at the same 150,000 kbps budget.
- Repeat with the UI hidden, receiver preview visible, Capture preview visible,
  and detector/Python diagnostic previews visible. The receiver preview uses
  GPU textures and does not require CPU readback. Capture/diagnostic previews
  create CPU demand only while visible; their processing/upload rate is capped.
  Capture preview filtering runs on a separate worker with a single pending
  image, so it cannot block raw frame acquisition/publication.
- Ensure a CPU consumer is active when measuring CPU-published FPS. For example,
  enable TestMove with the 640×640 capture region; it creates stream-rate frame
  demand even while waiting for its physical activation button. Consumer-
  processed FPS is zero while that algorithm is waiting for its button/device.
  Disable movement output as appropriate for the content used in your test.
- Check the centered native crop, Python's independent FOV and full-source
  option. Compare masks, contours and coordinates against the previous build
  using the same settings. Include crop boundaries and odd visible dimensions.
- Exercise stop/reconnect, a receiver window resize, remote resolution changes,
  idle frames, a temporary delivery stall, and exit during activity. Old stream
  generations must not appear in consumers or previews after reconnection.
- At 1440p/240 with 640×640 CPU demand, incoming delivery must sustain 240 FPS
  before applying the **≥237 decoded and CPU-published FPS** target. Require
  p95 receiver-arrival-to-publication **≤12.5 ms**, without growing queues.
  Record the actual limitation when either target is missed. Settings remain
  fixed during overload.

Then compare 75,000, 100,000 and 150,000 kbps one at a time, keeping codec,
resolution, FPS, content and preview state fixed. Save each result under a
distinct filename/folder. The intermediate LAN device and its link capacity
need to be identified manually if diagnostics reveal a slow negotiated link;
Vanta does not change it.

## Read and export diagnostics

Reporting is automatic while a stream is active, including when the UI is hidden.
After at least eight seconds of streaming and four seconds of continuous FPS or
latency trouble, Vanta saves a report. Repeated automatic issue reports are
limited to one per minute and six per session. A decoder failure can bypass the
initial wait. Stopping/disconnecting also saves a session summary after at least
three seconds of observations. **Stream Receiver → Save diagnostics** saves the
currently collected history immediately. The page shows a live stage diagnosis
and the actual saved path.

Files go in `diagnostics` beside `vanta.exe`. If that directory is unwritable,
the writer tries `%LOCALAPPDATA%\Vanta\diagnostics`. Each report has three files:

- `vanta-report-*.json`: findings with evidence and suggested next checks,
  hardware/settings, active region requests, actual CPU crop/frame metadata,
  MAKCU connection/output state, CPU/memory load, and up to 120 recent samples.
- `vanta-report-*-timings.csv`: ten-second median/p95/p99 stage summaries.
- `vanta-report-*-rates.csv`: approximately one-second FPS/counter history and
  per-consumer states for comparing before/after activation or preview changes.

Copy all three with `BUILD-INFO.txt` and `vanta.log`. Export formatting and file
I/O run on a background writer with a bounded pending queue. It drains on clean
shutdown. Reports reuse startup GPU capability information; saving a report
does not launch GPU tests or create additional decoder devices.

- Requested, received, decoded, CPU-published and each consumer's processed
  rates are distinct. Rates are aggregated once per second and fall to zero
  when delivery/processing stops. Timing summaries use the last ten seconds.
- CSV stages report median/p95/p99 and sample counts for reassembly,
  compressed queue wait, FFmpeg/driver submission, observed GPU decode completion,
  GPU conversion/readback/map, CPU image copy, publication dispatch, arrival to
  publication, optional host processing and consumer work/frame age. Python's
  pure algorithm wall time is separated from its whole iteration and MAKCU
  movement mutex/serial-write timing. Durations include scheduling/preemption;
  observed GPU completion is not pure GPU execution time.
  These are **not complete host-to-display latency** measurements. Optional host
  processing is a separate metric and does not establish network transit time.
- Packet arrival, assembly, decoder submission/output, readback and publication
  use common-c's local monotonic microsecond clock. Host presentation timestamps
  identify frames and are not subtracted from the receiver clock.
- Transport loss counts missing completed frame numbers after FEC recovery.
  Compressed overload, decoder errors, IDR requests and deliberate replacement
  of obsolete decoded outputs are separate counters. The compressed queue has
  three packets. GPU decode completion also limits outstanding surface work to
  three frames. Recovery flushes dependencies on the decoder thread, requests
  an IDR immediately, then retries every 500 ms up to three times; recovery ends
  only after verified hardware output. An unhealthy stream is stopped.
- JSON records actual Windows build, CPU, RAM, GPUs/drivers/capabilities,
  display ownership, local display modes and Ethernet link speeds. Requested
  stream geometry, decoded visible geometry, texture allocation and CPU ROI
  remain distinct. Dimensions such as 1918×1087 are retained and reported.

Read the first stage whose rate falls behind: low **received** FPS places the
shortfall before successful receiver assembly; healthy received but low **decoded**
FPS points to decoder/queue/recovery work; healthy decoded but low **CPU-published**
FPS points to readback/publication; healthy publication but low **consumer** FPS
points to that feature's iteration, output calls or intentional pauses. The
report records supporting counters and timings instead of claiming an exact
host-versus-network cause from received FPS alone. Use identical moving content;
an idle source may intentionally deliver fewer frames.

No CPU demand, capped preview requests, and features waiting for a physical
activation button/device are reported as expected states. Rate warnings allow
the greater of 3 FPS or 1% for one-second sampling differences; the separate
manual acceptance target remains at least 237 FPS for the 240 FPS profile.

For the reported Python/MAKCU problem, connect the detected MAKCU Device in
Mouse output, then enable Python. The COM-port lock has been removed and legacy
override_port settings are ignored. Python uses the current connection and
ignores imported makcu_port/makcu_override_port fields. To change between detected
devices, disable Python, disconnect MAKCU, select **Mouse output > Device**,
and connect before rearming.
Compare a report with Python disabled against one with it
enabled and its activation button held on appropriate test content.
The Device selector contains detected ports only. A remembered selection is
preferred while present; a missing saved COM5 cannot prevent detection of COM3.
Connection logs preserve Windows API names, error codes and messages separately
for each attempted baud rate, or identify an identity/firmware handshake failure.
The scan records actual detected ports and the current selection.
The same details are included under MAKCU in stream diagnostics reports.
Saving FPS diagnostics still requires a stream observation; a startup log with
zero captured frames contains no stream-performance measurements.
The Windows serial health check also initializes the DCB size and synchronizes
access to the native handle. Windows requires callers to set that structure's
size ([Microsoft DCB reference](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-dcb)).

The Capture tab square does **not** change Python's FOV, target coordinates or
movement calculation. Python requests its own centered `2*fov_radius` square.
A larger visible Capture/Detector preview or another capture consumer can enlarge
the shared readback union and add GPU/CPU work, which can reduce delivery FPS.
Reports include both each subscription and the actual last published crop.

Version 1.1.0.3 removes a fixed 20 ms sleep between capped capture requests that
could limit preview-only CPU delivery to about 50 FPS before readback overhead.
Capture now waits for the earliest subscription/preview deadline or a demand
change, with cancellation. With no pixel demand it sleeps until demand changes;
showing the Capture preview wakes the worker. Capped requests have their own
deadlines and do not impose a second global rate limit on faster requests.
The receiver's stream-rate processing target and the 60 FPS preview cap are
unchanged. A high-resolution Windows waitable timer is used when supported,
with regular-timer/condition-variable fallback reported in diagnostics
([Microsoft timer reference](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)).
JSON/CSV timing summaries include capture deadline wake lateness. Per-second
logs now state CPU demand (stream-rate, capped or none), the cap, ROI size,
readback time, wake lateness, and active consumer rates/states. The arrival-to-
publication timing is labelled separately from GPU/readback timing.

The laptop's 1.1.0.2 log confirmed MAKCU connected on COM3 with firmware 4.094.
HEVC received and decoded rates tracked at roughly 133–203 FPS after startup,
with no continuing frame gaps or decoder failures; CPU publication was 47–48 FPS.
Python activation was not recorded, so this is not a stream-rate Python
processing benchmark. The generated diagnostics reports contain the actual
subscriptions and timing breakdown needed to interpret those CPU rates.
The source-side reason for delivery below 240 FPS cannot be isolated from that
receiver log. The host subsequently sent 0x80030023, which Sunshine/common-c
define as graceful host termination; this does not identify why the host closed.
If closure was unexpected, compare Sunshine's host log at that timestamp.
The subsequent GPU drain timeout remains unverified on receiver hardware;
new timeout logs identify the affected context, device status and fence values.
No GPU timeout or 240 FPS performance fix is claimed from compile-only validation.

When shared NV12 surfaces/fences are supported, decode/readback and UI use
separate devices on the same adapter. Otherwise diagnostics report the protected
single-device fallback. If direct YUV sampling is unavailable, reusable GPU BGRA
conversion textures provide the preview. CPU readback uses three staging slots,
completion events/fences where available, and nonblocking query/map fallback.
Immutable CPU snapshots retain pooled storage until their consumers release it.

## Build and checks

Dependencies: Moonlight common-c
`aeba988e0a302145eff2e70fa3e11cd98ec01f5d`; vcpkg
`a5f736224e85b32e25a8ffc52562333180396642`; FFmpeg **9.0.2#1**, static
`ffmpeg[core,avcodec]:x64-windows-static`. Vanta Debug and Release both use
the static release CRT (`/MT`) and the Release dependency libraries. FFmpeg
decodes 8-bit SDR 4:2:0 H.264 High, HEVC Main and AV1 Main through D3D11VA.
Media Foundation remains only in the opt-in H.264 encoder fixture.

Run `scripts/bootstrap-sunshine-receiver.ps1`, build `vanta.sln` as Debug and
Release x64, then package with:

```powershell
.\scripts\package-stable-detector.ps1 -Configuration Release -SkipTests
```

The bootstrap repairs copied submodule gitfile metadata without resetting source
files and refuses to overwrite modified dependency sources when changing pins.
FFmpeg notices, port patches and dependency build information are bundled.

Opt-in deterministic checks, provided for manual runs on the laptop. They are
not executed as part of this package's local compile-only validation:

```powershell
.\vanta.exe --stream-pipeline-self-test
.\vanta.exe --fps-diagnostics-self-test
.\vanta.exe --config-self-test
.\vanta.exe --python-self-test
.\vanta.exe --detector-self-test
.\vanta.exe --detector-ui-self-test
.\vanta.exe --makcu-self-test
```

Opt-in checks **on the receiving laptop**:

```powershell
.\vanta.exe --receiver-self-test
.\vanta.exe --receiver-capture-self-test
```

The first creates Media Foundation encoding fixtures and verifies real FFmpeg
D3D11 decoding, associated metadata and native GPU-to-CPU readback. The second
uses a synthetic region provider to exercise asynchronous subscriptions,
independent Python/full-source crops, changing/odd geometry, immutable snapshots,
generation rejection and shutdown with a receiver graphics device. Neither
opens a Sunshine session. `--receiver-stream-smoke HOST` does open a pre-paired
live session, with display-mode matching disabled; run it only deliberately.

Local configuration schema is 10. Upgrade retains a `.schema-9.bak` before
rewriting older settings; shared-profile schema stays 9. To compare releases
with identical settings, back up the Vanta settings directory before switching
executables and restore the older backup before running a schema-9 binary.

## Build status, 2026-10-18

- Binary file version is **1.1.0.6**, included in startup logs and report build
  identification. Debug and Release x64 compile locally;
  this package uses the `SkipTests` route. The final binaries have not been
  executed or performance-tested here, per the requested compile-only workflow.
- Provided diagnostics checks cover idle/capped demand, input waits, stage bottlenecks,
  fresh versus historical counter changes, warm-up/persistence/cooldown/report
  limits, and actual JSON/CSV export with escaping and metadata association.
  Deadline arithmetic, idle-wait wake/cancellation, late capture wake diagnosis
  and host graceful-closure classification also have opt-in checks.
  MAKCU checks cover selecting detected ports despite a stale saved COM number,
  retaining a detected selection, and movement ownership. They are opt-in and
  do not require device I/O; validation of this final package is left to the laptop.
- TestMove's detector algorithm sources are unchanged. Python offers X + Y,
  Horizontal only and Hybrid output modes. Hybrid allows Y during its configurable
  activation window (300 ms default), then keeps X active with generated Y at zero.
  Detection, target selection and crop geometry remain unchanged, and diagnostics
  record the mode and interval. Version 1.1.0.5 fixes the duration control's compile
  error and axis_mode JSON string serialization, and guards removal of legacy COM
  fields. Manual validation of these settings is left to the laptop.
  That prior build's 204-fixture parity results remain historical evidence,
  rather than a test result for these new binaries.
- Receiver GPU/live-stream tests, MAKCU input injection, display-mode changes
  and laptop FPS/latency acceptance are left for manual receiver validation.
