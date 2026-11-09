# The radio's board data: what the firmware accepts, measured

This note records what was measured on the Xiaomi Mi 11 Pro (mars, SM8350,
QCA6490) with the `6490.wlanfw.eval_v1_TOQ` firmware that this phone's own ROM
carries. Every number below comes from a run on the device, not from the Linux
driver's constants.

## What works

- The memory handshake completes. The radio asks for two segments (type 1,
  6,160,384 bytes and type 4, 3,801,088 bytes); a reply that copies the driver's
  header and carries the requested sizes is answered with `result = 0`.
- The regulatory database (24,278 bytes, four pieces of 6,144) is accepted piece
  by piece (`result = 0` for each), and the transfer ends with a 25-byte
  file-level answer.
- The receive path is healthy: ten answers arrived in a row, counted in the
  runtime receive path, so silence after the fourth piece is not a host bug.

## What the firmware refuses

| limit | measurement |
|---|---|
| data per message | 6,144 bytes; a message carrying 8,192 bytes is refused with `result = 1`, `error = 19` |
| messages per file | four; the fifth is never answered, whatever the piece size (6,144 or 2,048), the pacing (20 ms, 200 ms, 5 s), the declared `total_size` (decreasing or constant), or the content (repeated segment) |
| files per boot | one; after a completed file the first message of a second file is ignored |

Four pieces of 6,144 is 24,576 bytes. The board data this phone ships is 58,180
bytes (or 59,868 as an ELF), so it cannot be delivered: four pieces would need
14,545 bytes each, and any message above 6,144 is refused.

## What was ruled out

- **Our receive ring**: counting every packet in and out shows ten answers
  arriving, well past the fourth.
- **The QMI encoding**: the 0x25 request is byte-for-byte what the Linux driver
  builds, including `valid`, `file_id`, `total_size`, `seg_id`, `end` and
  `bdf_type`.
- **Address windows**: the radio's DDR and calibration windows are mapped at the
  addresses the vendor driver uses (`0xa1000000`, `0xa1600000`), separate from
  the MHI rings (`0xa6000000`), and the radio writes into them.
- **Another firmware**: the phone's own firmware partition (`NON-HLOS.bin` from
  the mars ROM) carries exactly two WLAN images, `eval_v1` and `eval_v2`; both
  behave identically. There is no larger-staging build to switch to.
- **A second chance through the delayed path**: replying to the memory request
  with `mem_seg_len = 0` does not make the radio ask again; it answers with the
  invalid message id `0x100` and the firmware loading stops.

## Where this leaves the port

Everything up to the board data is in place: PCIe link, SMMU, MHI, QRTR, the
WLFW service, the memory handshake, the capability exchange and the regulatory
database. The board data is the wall, and it is the radio's wall, not ours.

Two things could still move it, both outside the code in this tree:

1. a QCA6490 firmware build whose board-data staging is larger than 24 KB, or
2. a board-data image small enough to fit, which the vendor set does not contain
   (all 147 variants in the ROM are 59,868 bytes).
