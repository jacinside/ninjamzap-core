# video-sync integration tests

End-to-end tests of NinjamZap's per-frame H.264 video sync pipeline (sync
marker, GUID matching, prev_ds_guid, GAP-HOLD, swap-defer, drop-resync).
The tests build `njclient.cpp` for the host (macOS), boot a NINJAM server in
Docker, and drive real connect/join/upload-interval sequences via the
`TestClient` harness.

## Build & run

```bash
make            # builds video_sync_tests
make test       # boots ninjamzap-server-test in Docker, runs all scenarios
make clean
```

`make test` requires a running Docker daemon. The server container is started
by `docker/run-server.sh` and torn down on exit.

### Run a single scenario

```bash
make test -- "[scenario8]"          # tag filter
make test -- "08_play_promote"      # name substring
NJ_TEST_DEBUG=1 make test           # verbose SYNCLOG echo to stderr
```

### Override knobs (env vars)

| Variable | Default | Used by |
|----------|---------|---------|
| `NJ_TEST_NCLIENTS` | 4 | 12 |
| `NJ_TEST_STRESS_FRAME_KB` | 25 | 12 |
| `NJ_TEST_STRESS_FPS` | 20 | 12 |
| `NJ_TEST_STRESS_SECONDS` | 8 | 12 |
| `NJ_TEST_NRECEIVERS` | 3 | 10 |
| `NJ_TEST_SHORT_BPM` | 480 | 18 |
| `NJ_TEST_SHORT_BPI` | 2 | 18 |
| `NJ_TEST_LONG_BPM` | 60 | 19 |
| `NJ_TEST_LONG_BPI` | 8 | 19 |
| `NJ_TEST_DROP_NCLIENTS` | 5 | 20 |
| `NJ_TEST_DROP_SECONDS` | 8 | 20 |
| `NJ_TEST_DROP_FRAME_KB` | 20 | 20 |
| `NJ_TEST_COMBO_BPM` | 120 | 23 |
| `NJ_TEST_COMBO_BPI` | 2 | 23 |
| `NJ_TEST_REPLAY_LOG` | — | 08 (log replay mode) |

## What's covered

Twenty-three Catch2 scenarios under `scenarios/`:

### Core sync

| # | Scenario | Focus |
|---|----------|-------|
| 01 | join_at_beat_0 | Marker + first interval routing |
| 02 | video_one_interval_early | Latency band [0.8×, 2.2×] interval |
| 03 | late_join | Receiver joining mid-session |
| 04 | gap_in_video | HOLD → FALLBACK after no-data intervals |
| 05 | stress_high_framerate | No DROP-RESYNC under dense frame push |
| 06 | user_leave | State reset on disconnect + reconnect |
| 07 | two_senders | Per-user `VideoRecvState` isolation |
| 08 | play_promote_invariant | PROMOTE fires exactly 1 swap after PLAY |
| 09 | pause_resume_burst | Pause/resume cycles; no stuck state |

### Topology

| # | Scenario | Focus |
|---|----------|-------|
| 10 | multi_receiver_fanout | 1 sender → N receivers, state isolation |
| 12 | multi_client_hd_stress | N concurrent HD streams (server tuning knob) |
| 20 | drop_resync_recovery | If DROP-RESYNC fires, receiver must re-sync |
| 21 | video_only_sender | No audio → HOLD(no audio), never DROP-RESYNC |

### Lifecycle

| # | Scenario | Focus |
|---|----------|-------|
| 11 | long_idle_recovery | 6-interval pause then clean resume |
| 13 | sps_pps_mid_stream | Codec reset (new SPS/PPS + IDR) mid-stream |
| 14 | receiver_reconnect | Receiver drop/reconnect while sender streams |
| 15 | sparse_video | 1 frame/interval — floor frame-count check |
| 22 | audio_then_video | Audio-only join, video enabled mid-session |

### BPM / BPI

| # | Scenario | Focus |
|---|----------|-------|
| 16 | bpi_change_mid_session | Vote BPI 4→8; interval doubles |
| 17 | bpm_change_mid_session | Vote BPM 240→120; interval doubles |
| 18 | extreme_short_intervals | BPM=480/BPI=2 (0.25s) — stress + limits |
| 19 | extreme_long_intervals | BPM=60/BPI=8 (8.0s) — pacing + limits |
| 23 | bpm_bpi_combined_change | Vote BPM + BPI simultaneously |

## Harness pieces

- `harness/TestClient.{h,cpp}` — wraps `NJClient` with connect/join/send
  helpers. Public API: `connectAndJoin`, `sendVideoFrame`, `pauseVideo`,
  `resumeVideo`, `sendFakeSPSPPS`, `sendChatMessage`, `getBPM`, `getBPI`,
  `drainVideoFrames`, `intervalSwapCount`.
- `harness/SyncLogCapture.{h,cpp}` — process-wide tap on `SYNCLOG_*` output
  so scenarios can grep for events (`match=PREV`, `PROMOTE`, `BURST`, etc.).
- `harness/FakeFrame.h` — synthetic NAL builder with embedded send-timestamp
  for latency measurement.
- `harness/main_test.cpp` — Catch2 entry point.
- `vendor/catch_amalgamated.{cpp,hpp}` — pinned Catch2 v3.

## Notes

- Tests touch a real Docker NINJAM server, so timing-sensitive scenarios
  (05, 07, 12, 20) can flake under load. Re-run before treating a single
  failure as a bug.
- BPM/BPI vote scenarios (16–19, 23) require `SetVotingThreshold 50` in
  `docker/test.cfg` (already set). With 2 clients one vote = 50% → passes.
- Scenario 18/19 will print "BPM vote not accepted" and pass if the server
  rejects the extreme value — treat the output as a capability boundary report.
- `SYNCLOG` is enabled at compile time via `NINJAMCORE_HOST_BUILD=1` (set in
  the Makefile). On iOS builds this is gated by `SYNCLOG_ENABLED` in
  `njclient.cpp`.
- The harness is built against the sub-interval sync mechanism
  (`VideoRecvState`, GUID matching, `prev_ds_guid`, holdCount FSM). It will
  fail to compile if those structs are removed from `njclient.{cpp,h}`.
