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

## What's covered

Thirteen Catch2 scenarios under `scenarios/`:

| # | Scenario | Focus |
|---|----------|-------|
| 01 | join_at_beat_0 | Marker + first interval routing |
| 02 | video_one_interval_early | Sender starts ahead — PREV match |
| 03 | late_join | Receiver joining mid-session |
| 04 | gap_in_video | HOLD → FALLBACK after no-data |
| 05 | stress_high_framerate | Burst detection at BEGIN |
| 06 | user_leave | State reset on disconnect |
| 07 | two_senders | Per-user `VideoRecvState` isolation |
| 08 | play_promote_invariant | PROMOTE/PLAY ordering at SWAP |
| 09 | pause_resume_burst | Toggle off/on followed by catch-up |
| 10 | multi_receiver_fanout | 1 sender → N receivers, state isolation |
| 11 | long_idle_recovery | Reconnect after extended idle |
| 12 | multi_client_hd_stress | N concurrent HD streams (server tuning) |
| 13 | sps_pps_mid_stream | Codec reset mid-stream (IDR + new SPS/PPS) |

## Harness pieces

- `harness/TestClient.{h,cpp}` — wraps `NJClient` with connect/join helpers
  and a `pushVideoFrame()` driver that emits the 20-byte sync marker + frame.
- `harness/SyncLogCapture.{h,cpp}` — process-wide tap on `SYNCLOG_*` output
  so scenarios can grep for events (`match=PREV`, `PROMOTE`, `BURST`, etc.).
- `harness/FakeFrame.h` — synthetic NAL builder for deterministic frames.
- `harness/main_test.cpp` — Catch2 entry point.
- `vendor/catch_amalgamated.{cpp,hpp}` — pinned Catch2 v3.

## Notes

- Tests touch a real Docker NINJAM server, so timing-sensitive scenarios
  (05, 07) can flake. Re-run before treating a single failure as a bug.
- `SYNCLOG` is enabled at compile time via `NINJAMCORE_HOST_BUILD=1` (set in
  the Makefile). On iOS builds this is gated by `SYNCLOG_ENABLED` in
  `njclient.cpp`.
- The harness is built against the pre-deletion sync mechanism
  (`VideoRecvState`, GUID matching, `prev_ds_guid`, holdCount FSM). It will
  fail to compile if those structs are removed from `njclient.{cpp,h}`.
