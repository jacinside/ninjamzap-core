# video-sync test harness — SUPERSEDED

This harness was built to verify the per-frame H.264 sub-interval sync
pipeline (sync marker, GUID matching, prev_ds_guid, GAP-HOLD, swap-defer,
drop-resync). That pipeline has been removed.

The current model is one fragmented MP4 container per NINJAM interval.
NINJAM's existing interval delivery handles audio/video sync — there is
no sub-interval timing left to test at the C++ layer. End-to-end checks
now live in the iOS client.

The directory is left in place for historical reference and is untracked
in this submodule; nothing here builds against the current `njclient.cpp`.
