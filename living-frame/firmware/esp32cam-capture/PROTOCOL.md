# ESP-NOW photo transfer: CAM → frame board (design for rungs 2-3)

Peer-to-peer, no AP and no router. Frame board: `c4:9e:7e:08:6d:f4`
(N16R8 ESP32-S3). CAM: `f0:16:1d:ae:2b:20`. WiFi mode `WIFI_STA`, channel fixed
on both ends (ESP-NOW peers must share a channel; the frame board's accepted
display build never starts WiFi, so the receiver sets the channel explicitly).

## Why chunking

ESP-NOW carries at most **250 bytes** per packet at the ESP-IDF level (1470 for
an encrypted link, but encryption caps you at 250 in practice and adds key
management). A VGA JPEG at quality 12 is tens of kB, so a photo is a few hundred
packets and needs a header, sequencing, reassembly and an end-to-end check.

## Packet layout

Every packet starts with one 16-byte header, little-endian:

| offset | size | field |
|---|---|---|
| 0 | 1 | `magic` = `0xA5` |
| 1 | 1 | `version` = `1` |
| 2 | 2 | `transfer_id` (uint16, increments per photo) |
| 4 | 4 | `total_len` (bytes of the JPEG) |
| 8 | 2 | `chunk_index` (0-based) |
| 10 | 2 | `chunk_count` |
| 12 | 1 | `flags` (bit0 = last chunk) |
| 13 | 1 | `reserved` |
| 14 | 2 | `payload_len` (≤ 234) |

Payload follows immediately: up to **234 bytes** (250 − the 16-byte header
above), so a 40 kB photo is ~172 chunks.

## Sender (CAM)

1. Capture once, hold the fb, compute `sha256` over the JPEG and keep it.
2. Send `START` as chunk 0 of the sequence with `flags.bit1` (start) set and
   `total_len` + `sha256` in the first two packets (a 32-byte hash does not fit
   one header; it rides in the payload of packet 0, with the first 200 JPEG
   bytes following it).
3. Send chunks strictly in order, `chunk_index = 0..n-1`.
4. After the last chunk, wait for the receiver's `DONE` packet containing
   `transfer_id`, received length and the receiver-side SHA-256.
5. Compare hashes. Match → report success. Mismatch or timeout (2 s) → restart
   the transfer from chunk 0, up to 3 attempts, then report failure and keep the
   frame in PSRAM for a re-send on the next command.

## Receiver (frame board)

1. On `START`: allocate `total_len` in PSRAM (fall back to a 512 kB cap and
   report `too_large`), record `transfer_id` and `sha256`.
2. On each chunk: require the expected `chunk_index`; on a gap, discard the
   partial transfer and send `NACK` with the last good index (a re-send is
   cheaper than a reassembly bug).
3. On `flags.bit0`: check `received == total_len`, hash the buffer, compare with
   the sender's hash, then write to SD as `/photos/cam-<transfer_id>.jpg` and
   `fsync` it before replying `DONE`.

## Proof that matters

The only number that proves the pipeline end-to-end is the **SHA-256 at three
points being equal**: camera-side capture, receiver-side reassembly, and the file
read back from the frame board's SD card. Rung 2's small-payload test proves the
link and the ACK/retry path; rung 3 proves the hash chain.

## Reporting on the frame board

The bench FTDI bridge's console is dead (0225), so the receiver writes a report
blob to the `report` flash partition (as in 0225-0228) and the host reads it back
with esptool. Report fields: `transfer_id`, `chunks`, `retries`, `nacks`,
`total_len`, `sha256_rx`, `sha256_sd`, `sd_path`, `result`.
