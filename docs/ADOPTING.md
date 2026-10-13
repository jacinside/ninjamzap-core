# Adopting synchronized video

NinjamZap's video extension carries compressed video as ordinary NINJAM
channel data, synchronized to the audio intervals by matching each video
interval to the GUID of the audio interval recorded with it. Clients that
don't support it ignore the video channels, and audio keeps working for
everyone in the room.

How you adopt it depends on what you maintain. The two branches below live on
[jacinside/ninjam](https://github.com/jacinside/ninjam), a fork of upstream
NINJAM; each is one commit on top of upstream `main`, so you can review and
merge it as a unit.

| You maintain | What to use | What you still provide |
|---|---|---|
| A client built on upstream `njclient` (the reference client, ReaNINJAM and derivatives) | Branch [`feature/video-channels`](https://github.com/jacinside/ninjam/tree/feature/video-channels): video channels in `njclient`, sync marker, GUID matching (including audio queued behind the playing interval), frame callbacks | H.264 encode / decode and display — platform code, not part of `njclient` |
| A client with its own protocol implementation | The specification: [`VIDEO_SYNC.md`](VIDEO_SYNC.md), §6 (sync), §7 (wire format), §10 (implementer's guide and pitfalls) | Everything; the spec is written so you don't need to read this code |
| A new client | This library (GPL v2): the `njclient`-based core plus a C adapter (`NinjamClientAdapter`) that the NinjamZap apps and streaming bot use | Capture, codec and UI |
| A NINJAM server | Branch [`feature/server-video-relay`](https://github.com/jacinside/ninjam/tree/feature/server-video-relay): `AllowVideoChannels` (advertised to clients as `server_caps` bit 1), a larger per-connection message queue, and audio relayed ahead of video so video never delays it | Configuration: `AllowVideoChannels yes`, and `MaxChannels` high enough for audio + video (see §10 pitfalls) |

An unmodified NINJAM server relays video channel data like any other
interval, without looking at it. What it lacks: it never sets `server_caps`
bit 1, which clients check before turning video on (§10, "Detecting whether
the server supports video"); its per-connection queue (512 messages) fills up
with a few video streams; and it gives video the same priority as audio, so on
a busy room video can delay audio. The server branch addresses all three.

Fixes for upstream NINJAM that are not about video (parser bounds checks, chat
sanitization, message leaks, beat-info validation, GUID entropy) are collected
on the fork's [`community`](https://github.com/jacinside/ninjam/tree/community)
branch, upstream `main` plus those fixes. The video branches apply on top of it.

## Testing interoperability

- **Against a live server:** `video.ninjamzap.com:2049` and `:2050` run
  [ninjamzap-server](https://github.com/jacinside/ninjamzap-server), which
  includes the server branch's changes, with video enabled and the anonymous
  channel cap raised to 8, so a third-party client can send and receive video
  there next to the NinjamZap apps.
- **Server implementations:** the end-to-end suite in
  [`tests/video-sync/`](../tests/video-sync/) connects real `njclient` senders
  and receivers to a server and checks sync, late frames, stalls and stress.
  Point it at your server with `NJ_TEST_HOST` / `NJ_TEST_PORT`.
- **Client implementations:** check your receiver against the pitfalls in
  §10, in particular a receiver whose audio falls an extra interval behind
  after a stall — the case that is easiest to miss.

## Compatibility

The wire format is versioned by behavior, not by a flag: new receivers keep
accepting what older senders send. Changes that affect implementers are
recorded in [`CHANGELOG.md`](../CHANGELOG.md) and in the specification.
