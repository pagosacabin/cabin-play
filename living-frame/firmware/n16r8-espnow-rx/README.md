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

Frame board report (`reports/rung2-rx-2026-11-20.txt`):

```
REPORT: boot=n16r8-espnow-rx task=0230
REPORT: mac=c4:9e:7e:08:6d:f4 channel=1 rate=1Mbps-11b
REPORT: expect pkts=32 len=240 total=7680 pattern='(seq*7+i*3+0x5a)&0xff'
REPORT: rx_packets=64 distinct=32 rx_bytes=15360
REPORT: bad_pattern=0 dup=32 out_of_range=0
REPORT: tfer_id=0x0230beef expected_packets=32 first_seq=0 last_seq=31
REPORT: sha256_rx=74bb840392a757f743926df1e363626405bb5416482b85e30e817766afaa2f28
REPORT: result=espnow_rx_complete
```

CAM console, re-run with the reviewed firmware
(`../esp32cam-capture/bench/run-2026-11-20-reseat/espnow-tx-p2/report.txt`; the
first pass is in `.../espnow-tx/report.txt` with the same hash):

```
REPORT: mac=f0:16:1d:ae:2b:20 channel=1 peer=c4:9e:7e:08:6d:f4
REPORT: transfer_id=0x0230beef pkts=32 len=240 total=7680
REPORT: sha256_tx=74bb840392a757f743926df1e363626405bb5416482b85e30e817766afaa2f28
REPORT: pass=1 send_ok=32 send_fail=0
REPORT: pass=2 send_ok=32 send_fail=0
REPORT: send_ok_total=64 send_fail_total=0
REPORT: seq_acked=32 seq_missing=0 of 32
REPORT: result=espnow_tx_sent
```

- **Sender's hash == receiver's hash** (`74bb8403…aa2f28`): the 7,680 bytes the
  CAM put on the air are the 7,680 bytes the frame board reassembled, in order.
- 64 received packets, 32 distinct sequences (0..31): the sender deliberately
  sends its pass twice, and the receiver reports exactly 32 duplicates — no
  packet was lost, and `bad_pattern=0` means every payload byte matched the
  sequence-derived pattern.
- Every send callback returned `ESP_NOW_SEND_SUCCESS` (MAC-level ACK), which is
  the link's own both-ways evidence: the frame board ACKed each frame.

**What this does not prove:** that the *receiver* was running this build is
inferred from the report it wrote; ESP-NOW delivery beyond 7,680 B, encryption,
and RSSI at range are untested. Rung 3 (a real JPEG over this link) is still
open on the camera.

## Peer review (Copilot, 2026-11-20, reviewed at fb336ec)

Two findings, both fixed in the follow-up commit; the transfer was then re-run
with the fixed firmware and reproduced the identical hash.

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

## Frame board end state

Test build flashed at 17:19 MST, transfer at 17:20, then the accepted build
restored at 17:23 MST. Restore was **verified from the board's own report**, not
from the fact that a flash command exited 0 —
`reports/frame-restored-800x600-2026-11-20.txt`:

```
mode       : 800x600 raster from the converter (800x600)
embedded   : 480000 B at 0x3c038334
sha256     : runtime 204578cc3339baf944345090680c7178f653be7fa79f8fcb83cf82b8ff3bd306
sha256     : tool    204578cc3339baf944345090680c7178f653be7fa79f8fcb83cf82b8ff3bd306
sha256     : MATCH - streamed framebuffer is the converted photograph
resolution : 800x600 native, 1 fb pixel = 1 DAC byte = 1 output pixel
measured   : 361 vsync in 5.998 s -> 60.19 Hz refresh
fills      : 14475 bounce fills -> 40.1 per frame (expected 40.0)
```

Those numbers are the frozen 0228 proof (`../n16r8-photo/report-800x600.txt`)
reproduced exactly, hash included → the board is running the accepted build.

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
