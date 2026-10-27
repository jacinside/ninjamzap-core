# Channel alignment: a user's channels play the interval they were recorded in

A NINJAM user can send several interval-mode channels (a multitrack drum kit, a
voice and an instrument, a mix and a click). They are recorded together, so a
receiver should play them together: interval *k* of every channel at the same
swap, and the video of interval *k* with them (see [`VIDEO_SYNC.md`](VIDEO_SYNC.md)).

## 1. Why channels drift apart

The protocol carries each channel's interval as an independent download with its
own random GUID. Nothing says which downloads belong to the same interval.

- **Sender.** A channel's BEGIN is sent once its encoder has produced the first
  upload block. With sound that takes a second or a few; with digital silence (a
  muted channel, a fader at 0) the block only fills when the interval ends.
- **Receiver.** Each channel has its own queue. A download joins it once enough of
  it is buffered to start playing, or when its END arrives. At each interval swap
  every channel takes the head of its own queue.

A channel that is not ready at a swap plays nothing, and the download that arrives
just after waits in its queue: from then on that channel plays one interval behind
its siblings, and nothing brings it back. What makes a channel late at one swap
varies — muting or unmuting mid-interval, a quiet input, a network hiccup, leaving
voice chat — but the result is the same permanent offset.

## 2. Interval-tagged GUIDs (sender)

A sender tags the GUID of every interval-mode upload with the interval it was
recorded in:

| Bytes | Content |
|---|---|
| 0–1 | random |
| 2–7 | session: random, drawn at each connection, shared by all the client's channels |
| 8–11 | interval number, big-endian, incremented at every local interval boundary |
| 12–13 | `'N'`, `'Z'` |
| 14 | version: `1` |
| 15 | channel index |

The leading random bytes keep GUID prefixes distinct, so tools that abbreviate a GUID
to its first bytes, and servers that file archived intervals by its first hex digit,
still tell intervals apart.

All channels recorded in the same interval carry the same session and interval
number. The GUID stays unique per channel and interval, so servers and clients that
treat GUIDs as opaque are unaffected. Voice-chat (live) and session-mode uploads
keep fully random GUIDs.

## 3. Alignment at the swap (receiver)

After each swap, for each user whose playing interval-mode downloads are tagged with
the same session:

1. Read the interval number of the download each channel now plays. Leave out
   channels in voice chat or session mode, video channels, channels with nothing
   to play, channels already waiting (step 3) and untagged downloads (the rest of
   an interval that started in voice chat).
2. Target: the **earliest** of those intervals. If every channel behind the latest
   one already has the latest interval queued, the target is the latest instead.
3. A channel **ahead** of the target waits one swap: it keeps its download, plays
   silence, and plays it at the next swap, together with its siblings.
4. A channel **behind** the target (only when the target is the latest, step 2)
   drops its queued downloads older than it.

A channel is never dropped for being late: a channel whose download becomes playable
late in the interval (digital silence, a quiet or gated input, a slow link) is waited
for, and the user's channels settle one interval later, which gives it a full
interval of margin from then on. Following a fixed reference channel instead would
drop a late non-reference channel at every swap and silence it for good.

Skipping forward is preferred whenever the interval is already queued, so
corrections don't add an interval of delay each time (the queues hold three
intervals; a channel that keeps waiting would overflow its queue and lose one).
Either way the cost is one interval of audio on the channels that move, once. A
download that replaces a skipped one doesn't get the interval-boundary crossfade
(128 samples), so a correction may click once.

Channels leaving voice chat, (re)subscribed channels and channels that start
sending mid-session need no special handling: their first tagged download is
placed by the same rule.

Downloads from senders that don't tag their GUIDs keep the per-channel queues.

## 4. Video

The video marker names the GUID of the audio interval recorded with it
([`VIDEO_SYNC.md`](VIDEO_SYNC.md) §6). Matching treats a waiting channel's download
as not yet playing, so the video waits with it; with the channels aligned, the
marker's interval is the one all the user's channels play.

## 5. Verification

`tests/video-sync/scenarios/39_multichannel_align.cpp`: a sender with eight
interval channels, each tagging its interval with a tone in its own band, and
video. The receiver checks, for every displayed video frame, which interval each
channel is playing, and that a channel that should sound is not missing. Plans:
mute/unmute every channel at different points of the interval, voice chat on/off on
several channels with channel 1 muted or sounding, a channel that only sounds in
the last 15% of each interval, and a mix; at several receiver phases, with and without network jitter
(`tc netem`). Real intervals (13.7 s) via `docker/test-multi.cfg`: `make test-real`.
