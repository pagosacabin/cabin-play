# n16r8-espnow-rx — task 0230 rung 2 (ESP-NOW receive test, frame board)

The frame board end of rung 2: receives the deterministic 32 × 240 B test
transfer from the ESP32-CAM, verifies every packet's pattern against its own
sequence number, and hashes the reassembled stream so the CAM-side SHA-256 can be
compared with it.

**This is a test build. It drives no display.** The accepted 800x600 gamma photo
build (`../n16r8-photo`, env `n16r8-photo-800x600`) goes back on the board when
the rung ends — see "Frame board end state" below.

## Why the report partition

The FT232R console on this bench delivers nothing usable (0225 finding), so every
log line is also written to the `report` data partition and read back:

```bash
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
        read_flash 0x310000 0x1000 reports/raw.bin
```

Layout: `uint32 magic = 0x30323330 ("0230")`, `uint32 text_len`, then the ASCII
text. The receiver rewrites the partition every time a packet arrives (and once
more at the end), so a read-back any time after the transfer shows the latest
state.

## Packet layout (shared with the CAM transmitter)

| offset | size | meaning |
|---|---|---|
| 0 | 2 | packet sequence, u16 LE |
| 2 | 2 | expected packet count, u16 LE (32) |
| 4 | 4 | transfer id, u32 LE (`0x0230BEEF`) |
| 8 | 232 | payload: `byte i = (seq*7 + i*3 + 0x5a) & 0xff` |

The receiver locks onto the first transfer id it sees (so a repeated pass from
the sender is counted as duplicates, not as a new transfer), rejects packets
whose length is not exactly 240 B, and rebuilds the stream at `seq*240`.

Radio: `WIFI_MODE_STA`, channel 1, `WIFI_PROTOCOL_11B|11G|11N` on both ends
(the S3's default protocol set includes modes an ESP32 peer cannot use, and a
protocol/rate mismatch shows up as *nothing received*, not as an error). The
sender is an ESP32-CAM (classic ESP32) at the 1 Mbps 11b base rate.

## Result, 2026-11-20 17:20 MST and re-run 17:28 MST (rung 2) 
Frame board report (`reports/rung2-rx-2026-11-20.txt`) — the final run, with the
reviewed firmware:

```
REPORT: boot=n16r8-espnow-rx task=0230
REPORT: mac=c4:9e:7e:08:6d:f4 channel=1 rate=1Mbps-11b
REPORT: expect pkts=32 len=240 total=7680 pattern='(seq*7+i*3+0x5a)&0xff'
REPORT: rx_packets=63 distinct=32 rx_bytes=15120
REPORT: bad_pattern=0 dup=31 out_of_range=0
REPORT: tfer_id=0x0230beef expected_packets=32 first_seq=0 last_seq=31
REPORT: sha256_rx=74bb840392a757f743926df1e363626405bb5416482b85e30e817766afaa2f28
REPORT: result=espnow_rx_complete
REPORT: report_write_errors=0
```

CAM console, same run (`../esp32cam-capture/bench/run-2026-11-20-reseat/espnow-tx-p5/report.txt`):

```
REPORT: mac=f0:16:1d:ae:2b:20 channel=1 peer=c4:9e:7e:08:6d:f4
REPORT: transfer_id=0x0230beef pkts=32 len=240 total=7680
REPORT: sha256_tx=74bb840392a757f743926df1e363626405bb5416482b85e30e817766afaa2f28
REPORT: pass=1 send_ok=31 send_fail=1 seq_acked=31 seq_missing=1 of 32
REPORT: pass=2 send_ok=32 send_fail=0 seq_acked=32 seq_missing=0 of 32
REPORT: send_ok_total=63 send_fail_total=1
REPORT: seq_acked=32 seq_missing=0 of 32 (last pass)
REPORT: result=espnow_tx_sent
```

- **Sender's hash == receiver's hash** (`74bb8403…aa2f28`): the 7,680 bytes the
  CAM put on the air are the 7,680 bytes the frame board reassembled, in order.
  That is the rung's success criterion, not the ACK count.
- **One frame of the first pass was lost** (31 of 32 sequences ACKed) and the
  receiver still ended with all 32 distinct packets and the same hash, because
  the second pass re-sent every sequence. `bad_pattern=0` and `dup=31` are
  consistent with that: 63 packets received, 32 distinct.
- The loss is not noise in the report — it is the receiver's own flash writes.
  With a report write per packet (the first version), 2 of 32 were lost; the
  accepted version throttles the write to a 5 s heartbeat and still lost one
  frame in pass 1. **A design consequence for rung 3:** at ~1 loss per 32 frames,
  a photo transfer of a few hundred chunks needs the NACK/retry rule in
  `PROTOCOL.md`, and the receiver should not write flash while a transfer is in
  flight.
- Earlier runs (`.../espnow-tx/`, `.../espnow-tx-p2/`, `...-p3/`) reproduced the
  same hash with 64/64 ACKs under the weaker criterion.

**What this does not prove:** that the *receiver* was running this build is
inferred from the report it wrote; ESP-NOW delivery beyond 7,680 B, encryption,
and RSSI at range are untested. Rung 3 (a real JPEG over this link) is still
open on the camera.

## Peer review (Copilot, 2026-11-20, two passes)

Three passes: `fb336ec`, `99960cd`, `f1cca1b`. Seven findings in total, all fixed,
and the transfer was re-run after each round with the identical hash each time.

1. **Medium — the sender's success condition could report a false success.** It
   counted ACK callbacks in aggregate (`send_ok >= 32`), which two passes over the
   same sequences would satisfy while leaving others unsent. Fixed: sends are
   issued one at a time and the status callback records the sequence in flight,
   so `result=espnow_tx_sent` now requires **every** sequence to be individually
   ACKed (`seq_acked=32 seq_missing=0`). The captured 64/64 was already a real
   result; the *criterion* was the weaker thing.
2. **Low — `write_report()` ignored partition errors.** On a build whose only log
   is that partition, a failed write would have looked like a short report. Fixed:
   every erase/write is checked, the header is read back and compared, and the
   outcome is reported as `report_write_errors` (0 here).

### Second review pass (same commit pair, re-reviewed at 99960cd)

Copilot re-reviewed the fixes and found the fixes themselves incomplete. All three
were real, and all three are fixed in the next commit; the transfer was run a third
time and reproduced the identical hash:

3. **Medium — the ACK attribution was still racy.** The fix waited a fixed 30 ms;
   `esp_now_send` is asynchronous, so a late callback for sequence *n* could be
   credited to *n+1*. Fixed: the sender now **waits for that sequence's callback**
   (200 ms cap, per sequence) before advancing, and a sequence with no callback is
   marked failed rather than skipped. `seq_acked=32 seq_missing=0` is now a
   per-sequence fact, not an aggregate coincidence.
4. **Low — the report read-back compared only the header.** Fixed: the whole body
   is read back and `memcmp`'d against what was written.
5. **Low — `report_write_errors` was logged before the final write**, so an error in
   that last write could only reach the dead console. Fixed: `finalize_report()`
   re-labels the line with the updated count and rewrites until one write completes
   with no new error (max 3 attempts).

That is why the stored report now ends with `REPORT: report_write_errors=0` **after**
`result=` — the ordering is the fix.

### Third review pass (re-reviewed at f1cca1b)

6. **Medium — the per-sequence state was not cleared between passes.** A failure
   recorded in pass 1 left `g_ack_fail[seq]` set, so pass 2's wait was satisfied
   immediately and a late callback could still be mis-attributed. Fixed: both
   per-sequence arrays are cleared at the start of every pass, and the success
   criterion is evaluated on the **last** pass only
   (`seq_acked=32 seq_missing=1 of 32 (last pass)`).

That fix is what exposed finding 7 — a defect in the *receiving* side that the
weaker criterion had been hiding:

Fourth pass on `03c8713`: **No findings — Verdict: SHIP.**

7. **Real packet loss, caused by the receiver's own flash writes.** With strict
   per-sequence accounting, pass 1 came back `seq_acked=30 seq_missing=2`: the
   receiver was erasing and rewriting its 64 kB report partition on *every*
   packet, stalling long enough to drop frames. Fixed: the report write is
   throttled to a 5 s heartbeat plus the final write. After that, pass 1 lost 1
   frame and pass 2 was clean (the run quoted above). The remaining single-frame
   loss is reported as measured, not papered over — it is what rung 3's retry rule
   has to absorb.

## Frame board end state

The test build was flashed for each of the four transfer runs (17:19–17:42 MST) and
the accepted build restored after each one; the last restore was at **17:43 MST**
and the board is on it now. Every restore was **verified from the board's own
report**, not from the fact that a flash command exited 0 —
`reports/frame-restored-800x600-2026-11-20.txt`:

```
mode       : 800x600 raster from the converter (800x600)
embedded   : 480000 B at 0x3c038334
sha256     : runtime 204578cc3339baf944345090680c7178f653be7fa79f8fcb83cf82b8ff3bd306
sha256     : tool    204578cc3339baf944345090680c7178f653be7fa79f8fcb83cf82b8ff3bd306
sha256     : MATCH - streamed framebuffer is the converted photograph
resolution : 800x600 native, 1 fb pixel = 1 DAC byte = 1 output pixel
measured   : 361 vsync in 5.998 s -> 60.18 Hz refresh
fills      : 14475 bounce fills -> 40.1 per frame (expected 40.0)
```

Those numbers are the frozen 0228 proof (`../n16r8-photo/report-800x600.txt`)
reproduced exactly, hash included → the board is running the accepted build. The
`vsync` count moves by one between boots (361/362 → 60.18/60.36 Hz) — the 6 s
window is not frame-locked, so that is measurement grain, not a change in output.

### Trap found while restoring: the 800x600 env cannot be built from the committed tree

`pio run -e n16r8-photo-800x600` compiles against the **committed**
`src/photo64_data.h`, which is the 640x480 raster (307,200 B), and the app aborts
at boot:

```
mode       : 640x480 raster from the converter (800x600)
FATAL      : embedded image is 307200 B, framebuffer needs 480000 B
```

The raster is selected by the converter, not by the build flags, so an 800x600
build needs the image regenerated first:

```bash
python3 tools/convert_photo64.py --source data/hummingbird.jpg \
        --mode 800x600 --space gamma --out /tmp/photo64_800x600.bin
cp /tmp/photo64_data.h /tmp/photo64_meta.h src/     # then build, then restore
git checkout -- src/photo64_data.h src/photo64_meta.h
```

The regenerated 480,000 B raster hashes to `204578cc…bd306`, i.e. byte-identical
to the accepted build's tool hash — so the regeneration is reproducible, and the
committed 640x480 header is simply the wrong input for that env. The committed
headers were left at their 640x480 (640x480-default-build) state; nothing about
the 800x600 raster was committed, because the accepted build's record is
`report-800x600.txt`.

## Rebuild

```bash
pio run -e n16r8-espnow-rx -t upload          # test build (no display)
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 read_flash 0x310000 0x1000 r.bin
```
