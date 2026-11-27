# AirBike Bridge setup and reference

Standalone Assault AirBike → Garmin Bluetooth relay with a local HR dashboard. Normal operation needs USB power near the bike; a phone or open browser is unnecessary for receiving and forwarding. The target is the original ESP32-PICO-D4 ATOM Lite, not ATOMS3.

## Daily use

1. Power the ATOM and press Connect on the bike.
2. Pair **AirBike Bridge** on the fēnix 6 as **one Power sensor**, then start Bike Indoor. This supplies power, speed, cadence and distance.
3. Wear the rider's linked Polar H10. Garmin connects directly to the H10; the ATOM uses its second Bluetooth connection. Enable “2 Bluetooth devices” in Polar Flow/Beat during initial band setup.
4. Open `http://airbike-atom.local/` or the IP assigned by local Wi-Fi. Select a rider in Options or choose Auto.

The relay uses a 2000 mm virtual wheel circumference, not the physical fan diameter. Garmin calculates its own activity distance and calories. Dashboard recording is independent of the Garmin activity.

## Dashboard and ride controls

The production page is **`web/index.html`**, automatically embedded into flash by `scripts/embed_web.py`. The archived design study is `web/hr-mock.html` and is not embedded.

- Bike, Data and Garmin statuses show help on tap, hover or keyboard focus. Options and rider share the top header. A separate band row distinguishes connected, stale, contact loss, waiting and rider ambiguity.
- Live HR and power sit side by side. The HR color and zone number use the rider's maximum HR, with zone transitions at 60%, 70%, 80% and 90%. Power color uses the rider's Cycling FTP and the [seven-zone model in Garmin's cycling guide](https://www.garmin.com/en-GB/blog/cycling-training-zones-guide/), with transitions above 55%, 75%, 90%, 105%, 120% and 150% of FTP. Readouts use a neutral color when the corresponding rider setting is missing.
- The chart opens at **15 minutes**. Tap it to switch between 15 and 30 minutes. The ATOM collects samples every five seconds even when no page is open. The left axis is bpm. The right axis starts at 0–200 W and expands to fit peaks in the visible window. Missing readings and long gaps stay visible as breaks in the lines.
- The live chart shows power before a ride starts. It adds heart rate after Start and keeps it visible while the ride is paused. Saving or clearing a ride clears its heart-rate trace. Both channels reset when the selected rider changes or Auto becomes unresolved. The bounded RAM buffer holds at most 361 samples and is lost on power loss. Flash stores ride summaries.

**Start** creates a ride; **Stop** freezes its totals; **Resume** continues it and restarts the auto-stop and ten-minute inactivity timers. Live readings, the chart and Bluetooth forwarding continue throughout. Manual Stop requires an explicit Resume. Clear appears while Stopped and asks confirmation before discarding the current ride without saving it.

**Options → Save & finish** saves the summary to flash. Rides also finish on bike disconnection, console-counter reset or ten minutes without pedalling. Failed saves retain the stopped ride for retry. An unfinished ride is held in RAM and is lost if power is removed before saving.

Automatic start/resume defaults to three seconds of pedalling; automatic stop defaults to ten seconds of inactivity. Both switches and thresholds (1–600 seconds) can be changed between rides. Pedalling requires progressing fresh packets with cadence or power, not coasting speed or HR. A four-second progress watchdog prevents short thresholds firing between normal packets while still rejecting frozen nonzero readings. After Finish/Clear, continued pedalling alone cannot immediately start another automatic ride; a fresh inactive-to-active episode or manual Start is needed.

Recorded duration is elapsed Recording time. It continues across source gaps until stopped, matching recovered cumulative distance/calorie counters when source returns during the same Recording segment. Unknown intervals are excluded from averages and mark the ride partial. Stopped intervals are excluded.

The dashboard uses the console's cumulative distance while it advances, including distance measured by the bike during a Bluetooth gap. If the counter stalls near **20 km** while the workout clock advances, the ATOM adds distance from fresh bike speed samples and their time intervals. If the counter catches up, the dashboard credits each metre once. Gaps after the counter stalls can leave the dashboard total short.

## Riders, bands and history

Options → Settings supports ten rider profiles with name, sex, age, weight in kg, maximum HR and cycling FTP in watts. Copy your cycling FTP from Garmin to set the power colors for each rider. Guest is always available. The editor selects the saved profile after creation so a second Save updates that person.

Link a band under **Polar H10 · rider bands**: choose its owner, wear the band, Find Polar H10, then select it. Each band has one owner. Linking/unlinking and changing riders are blocked during an unfinished ride. Manual selection reconnects only to the named rider's band.

Auto scans remembered bands while Ready and waits five seconds before choosing a single detected owner. Detecting several linked bands requires an explicit rider choice; signal strength never guesses the rider. Dismissing the prompt keeps recording blocked while Garmin forwarding continues. Rider and band stay locked through Recording/Stopped, including band disconnection. Auto detects again after Finish/Clear. Missing HR does not end a ride.

The latest **20 saved rides total** retain rider name/ID at start, duration, distance, calories and average/maximum speed, power and HR. Missing HR is unavailable rather than zero. Legacy summaries migrate as Guest with unavailable HR.

New ride summaries include the start date once the ATOM has synchronized its clock over Wi-Fi. The dashboard displays dates in the browser's time zone. Rides saved before the clock synchronizes and older entries without a timestamp appear undated.

A trash icon beside each history entry opens a confirmation showing rider, duration, distance and ID. Cancel is focused by default. Confirm deletes only that saved ID, preserving the current ride. Firmware stages the deletion, writes and verifies the stored history before publishing the updated list. Failed writes leave RAM history unchanged and report an error. A failed verification can mean the flash write succeeded but could not be verified; a retry still targets the same ID. IDs do not reset when deleting entries. Pending ride saves and unreadable stored history block deletion.

Saved profiles, links, selection mode, timers and history survive restart. Storage uses checksummed/versioned NVS records: history-v2 (2580 bytes), settings-v2 (1732 bytes), and Wi-Fi pair wifi-v1 (106 bytes). Retained v1 keys allow old-data recovery; no full NVS erase or partition change is used. Old-firmware rollback cannot read newer v2 rides or newer Wi-Fi changes.

If Recent rides reports unreadable history, stop the current ride and choose **Recover history**. Confirm to verify a raw backup in flash and initialize fresh history. This saves the stopped ride if it has data. If the backup cannot be verified, the ATOM leaves the original record in place.
If recovery attempts to save the stopped ride but cannot verify the result, retry **Recover history** before clearing the ride. Once the save is verified, you can delete the ride from Recent rides.

## Wi-Fi and iPhone

Options → Wi-Fi Settings gives the setup steps: hold the front button for four seconds, join AirBike-Setup-xxxxxx from a phone, then open **http://192.168.77.1/** and enter 2.4 GHz credentials. A guest network may isolate local devices.

The setup hotspot only serves the Wi-Fi setup page, credential submission and connection status. The dashboard, ride history and controls are available on regular Wi-Fi. Use a separate subnet for regular Wi-Fi if you need the dashboard while the setup hotspot is open.

The setup hotspot is open and submits Wi-Fi credentials over unencrypted HTTP. The dashboard has no login and is intended for a trusted local network.

SSID/password are replaced as one checksummed NVS record after a successful connection, then read back. Failed saving keeps the setup hotspot open with a retry message. It closes one minute after verified saving. If the ATOM opens the hotspot automatically because the saved Wi-Fi is unavailable at boot, it closes the hotspot when that network returns. Existing separate SSID/password keys remain readable until a new record is present. Credentials never appear in dashboard APIs or serial logs.

For an iPhone view without Safari bars, use Safari → Share → Add to Home Screen and launch that icon. Regular tabs retain browser controls. The page includes safe-area padding and no fixed application header/footer. Physical iPhone Home Screen mode remains a hardware acceptance check.

## Keep screen awake

**Options → Settings → Display → Keep screen awake** is enabled by default. Its preference is saved in this browser (not per rider or in the ATOM). Turning it off restores normal automatic screen timeout. The status distinguishes an acquired wake lock, paused/blocked state and the playback fallback.

The standard Screen Wake Lock API is used when available. It requests a lock when the page opens and reacquires when the page becomes visible again. Hiding or leaving the page releases the lock. If the browser releases it for another reason, the next dashboard tap retries without changing your preference.

On the ATOM's local HTTP address, the secure-context-only API is usually unavailable. A local silent-video fallback then starts on the first dashboard tap or key press. Its media comes from [NoSleep.js v0.12.0](https://github.com/richtr/NoSleep.js/tree/v0.12.0) (MIT license and provenance under `web/vendor`). The fallback never reloads the page or requests remote media. It pauses in the background and may require another tap on return. The firmware embeds both the helper script and media, so no CDN or separate asset request is needed on the ATOM. The preview serves the same local scripts.

Browser policy, manual screen locking or power-saving modes can still override this request; the [Screen Wake Lock specification](https://www.w3.org/TR/screen-wake-lock/) permits release. Physical iPhone screen-timeout testing of the HTTP fallback remains an acceptance check. No system screen settings are changed.

## Build, tests and preview

Pinned platform/NimBLE versions are in `platformio.ini`.

```sh
python3 scripts/test.py
pio run -e atom-lite
python3 scripts/preview.py --port 18774
```

The test command runs native suites with address/undefined-behaviour sanitizers, Python integration tests, JavaScript tests, and embedded JavaScript syntax checks. Native tests cover storage codecs/migration, session transitions/timers, protocol/relay packets, band ownership/ambiguity, Wi-Fi failures, persistent deletion and bounded trend sampling. UI logic tests cover real chart values, missing data, personal zone boundaries, rider isolation, duplicate-profile prevention, wake-lock lifecycle/races and fallback playback. The embedding test checks offline scripts and media. Tests use synthetic data and never contact the ATOM.

Preview `http://127.0.0.1:18774/?scenario=hr_focus` serves the **same production HTML**, with separate simulated APIs. Other scenario links cover empty/stale/offline, Auto ambiguity, long values and legacy history. Synthetic trend curves exist only in `scripts/preview.py`. `--hr-focus` explicitly serves the archived design study instead.

To upload, verify the intended USB port before running:

```sh
pio run -e atom-lite -t upload --upload-port PORT
```

Replace `PORT` with the serial port of the connected ATOM.

Opening the serial monitor resets the ATOM, so avoid it during a ride or Wi-Fi setup. Application upload does not erase NVS. Keep locally generated firmware binaries and device logs out of version control.

## Local HTTP API

- `GET /api/live`: bike/relay metrics and status; HR connection/contact/freshness, `max_hr` and rider selection.
- `GET /api/trend`: `{window_s, interval_s, rider_id, ready, max_hr, points}`. Each chronological point is `[age_seconds, hr_or_null, watts_or_null]`. Streamed in small chunks to avoid building a large response in RAM.
- `GET /api/rides`: current ready/recording/stopped state, pending save, `can_clear`, selected rider and saved summaries. Each ride's `started_at` is a Unix timestamp in seconds or null when unknown. Pass the last returned `history_version` to omit the summaries when they are unchanged.
- `POST /api/rides/control`: `action=start|stop|resume|finish`, header `X-AirBike-Action: ride-control`.
- `POST /api/rides/clear`: header `clear-ride`.
- `POST /api/rides/delete`: positive saved `id`, header `delete-ride`; 400 invalid, 404 absent, 409 pending save, 503 storage failure.
- `POST /api/rides/recover`: `confirm=1`, header `recover-history`. Available when stored history is unreadable and recording is stopped.
- `GET/POST /api/settings`: automatic switches and timer seconds, POST header `settings`.
- `GET/POST /api/riders`: profiles. POST header `riders`, `id=0` for creation or an existing ID for editing. Successful POST returns `saved_id`. To retry a create safely, send the same nonzero, 16-digit lowercase hexadecimal `create_key` with each attempt. A repeat returns the original `saved_id` and `already_saved: true` without changing the profile.
- `POST /api/riders/select`: numeric ID (0 Guest) or `auto`, header `rider-select`.
- `POST /api/riders/resolve`: explicit Auto choice by rider ID, header `rider-select`.
- `GET/POST /api/hr/sensor`: scan/link/unlink/status. Header `hr-sensor`; link/unlink require `rider_id`, link uses discovered address/type.

Mutation routes require the local dashboard's Origin and action header. Invalid transitions return 409; malformed fields 400; storage failures 503. APIs are intended for the local network. No workout commands are sent to the bike, and the ATOM does not rebroadcast HR.

The Garmin relay, Polar receiver, BLE packet encoders and band-selection implementation are unchanged by the dashboard integration. Physical acceptance still includes real concurrent bike/H10/watch operation, populated history after power cycling, and on-device streamed trend behaviour.

## Sources

- [ATOM Lite hardware](https://docs.m5stack.com/en/core/Atom-Lite)
- [Observed AirBike protocol](https://github.com/roelofkl/ha-assault-airbike/blob/main/docs/PROTOCOL.md)
- [Polar H10 settings](https://support.polar.com/e_manuals/h10-heart-rate-sensor/polar-h10-user-manual-english/sensor-settings.htm)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- [Espressif NVS power-loss recovery](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/storage/nvs_flash.html#security-tampering-and-robustness)
- [Tabler Icons MIT license](https://tabler.io/license), retained with the embedded icons.


## Post-ride connection diagnostics

**Options → Settings → Diagnostics → Record diagnostics** defaults to **on**. This is a device-wide preference stored separately on the ATOM, not a browser preference. Changing it does not reconfigure riders or sensors. The feature requires firmware built from this source; an older installed build may not include it.

The recorder distinguishes missing valid measurements from a Bluetooth disconnect. It records bike/H10/Garmin connection and subscription events, exact NimBLE reason codes, intentional local disconnections, reconnect-operation duration, valid/rejected packet counts, measurement gaps, local Garmin send failures, Wi-Fi events, and periodic signal/memory/task-health observations. A timeout alone does not prove interference or weak signal, and a notification accepted by the local Bluetooth stack does not prove Garmin recorded it.

After a ride, open **Technical diagnostics** and use **Download current report** while the ATOM remains powered, or **Download saved report** after a restart. The current report has the richer recent timeline. Neither collection nor checkpointing requires a browser, phone, internet connection or USB monitor.

- A diagnostic ride begins on fresh pedalling, retaining recent setup events. Stop/Resume and temporary bike disconnections do not split it. Explicit Save & finish, a resolved rider change, or ten minutes without fresh pedalling finishes the diagnostic capture. An unresolved Auto rider during a dropout does not split an existing capture.
- The recorder checkpoints every five minutes and saves at capture end. A sudden unplug can lose the latest unsaved five minutes (or longer if a storage failure is shown). A finished capture does not immediately restart from the bike's last cached pedal reading.
- RAM holds 128 events and 120 health samples, normally 20 minutes at one sample per ten seconds. Counters continue after the oldest detail is overwritten. The 2 KiB flash report holds session totals/extrema and the most recent 40 events. The health timeline remains RAM-only. A boot with no new ride does not replace the saved report.
- Gap detection starts at 2.5 seconds without valid measurements. Bike staleness is separately marked at the existing four-second threshold; H10 staleness at five seconds. Durations are intervals between valid readings, not a measured number of missing radio packets. Open gaps contribute to maximum duration; completed-gap totals are named explicitly.
- Signal strength is sampled no faster than once every ten seconds. Important bike/H10 gap, failure and disconnect events also retain the most recently sampled RSSI and its age. RSSI 0 means unavailable. Wi-Fi RSSI is a separate measurement from Bluetooth RSSI.
- CPU/health evidence consists of main-loop and relay-task scheduling gaps, free heap, sampled minimum/largest block, boot minimum heap, stack headroom in bytes, and flash operation timings. Scan time is a worker-sampled estimate; event context flags indicate observed scanning/connecting. This is not a radio spectrum recorder or CPU profiler.
- Callbacks only update bounded records/counters. Reports are streamed in chunks of at most 768 bytes. There is no full packet stream, new task, filesystem, partition change or per-notification flash write. Rejected bike payloads are limited to four 24-byte prefixes; no HR payloads or credentials are exported.
- Firmware/static-buffer budgets are checked against the previously installed build: at most 40 KiB extra application flash and 16 KiB diagnostic RAM including export scratch space. The report uses its own NVS namespace and reserves space for critical history/settings writes; storage pressure skips diagnostics rather than erasing data. A failed checkpoint retries on the five-minute schedule.

API: `GET /api/diagnostics` returns compact status/recent events; `GET /api/diagnostics/export?source=current|saved` downloads versioned JSON. `POST /api/diagnostics/settings` takes `enabled=0|1` with `X-AirBike-Action: diagnostics` and the existing same-origin protection. Unknown settings default on; an unreadable preference is flagged. Saved binary records are versioned and checksummed, with write/readback verification. Files contain relative milliseconds since boot, firmware identity and reset reason, not assumed calendar time.

Event payload `a/b/c/d` conventions: disconnect = raw reason / intentional-action code / cached RSSI / RSSI age; gap = time since valid data / connected flag / cached RSSI / RSSI age; link parameters = interval in 1.25 ms units / supervision timeout in 10 ms units / latency / MTU; subscribe = characteristic / subscription value or success / local error or Garmin connection handle / discovery phase; flash = operation / duration µs / success; task delay = main(0) or relay(1) / scheduling gap µs. Flash operations: diagnostics=0, history=1, app settings=2, Wi-Fi=3, diagnostic preference=4. Context flag bits: scanning=1, connection operation=2. Intentional-action codes: band change=1, subscription failure=2, selection changed during discovery=3. Preserve full NimBLE reason numbers: host code 8 is not HCI timeout `0x208`.

Validation includes native sanitizer tests, the actual diagnostic HTTP/runtime compiled with deterministic peripheral doubles, storage corruption/failure/reboot cases, browser controls, and the ESP32 build. Physical simultaneous bike/H10/Garmin operation, actual NVS capacity and checkpoint timing under radio load remain hardware acceptance checks after installation. Protocol decoding, virtual wheel/crank packet generation, ride/session logic and rider settings formats were not changed.
