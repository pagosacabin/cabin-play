#!/usr/bin/env python3
"""0239: pull the driver's own CMD/r1 trace out of a report-partition read-back.

sdspi_host logs one line per command   (V sdspi_host: ... CMDn, arg=0x..., flags=0x...)
sdspi_transaction logs one r1 per reply (V sdspi_transaction: r1 = 0xNN hw_cmd.r[0]=0x...)
Pairing them in order gives the exact command sequence the stock driver issued, which is how
"first call was clean" is separated from "the driver's internal retry absorbed a bad frame".
"""
import re, sys

CMD = re.compile(r'CMD(\d+), arg=0x([0-9a-fA-F]+)')
R1 = re.compile(r'r1 = 0x([0-9a-fA-F]{2}) hw_cmd\.r\[0\]=0x([0-9a-fA-F]+)')

path = sys.argv[1]
lines = open(path, errors='replace').read().splitlines()

seq = []          # (cmd, arg, r1, resp0)
pending = None
for ln in lines:
    m = CMD.search(ln)
    if m and 'sdspi' in ln:
        if pending:
            seq.append(pending + (None, None))
        pending = (int(m.group(1)), m.group(2).lower())
        continue
    m = R1.search(ln)
    if m and pending:
        seq.append(pending + (int(m.group(1), 16), m.group(2).lower()))
        pending = None
if pending:
    seq.append(pending + (None, None))

for i, (cmd, arg, r1, r0) in enumerate(seq, 1):
    print(f"  {i:2d}. CMD{cmd:<2d} arg=0x{arg:<8s} r1={'--' if r1 is None else f'0x{r1:02x}'}"
          f" resp0={r0 or '--'}")

cmd0 = [s for s in seq if s[0] == 0]
cmd8 = [s for s in seq if s[0] == 8]
print(f"SUMMARY cmd0_frames={len(cmd0)} cmd8_frames={len(cmd8)}")
if cmd0:
    print("SUMMARY cmd0_r1=" + ",".join('--' if s[2] is None else f'0x{s[2]:02x}' for s in cmd0))
if cmd8:
    print("SUMMARY cmd8_r1=" + ",".join('--' if s[2] is None else f'0x{s[2]:02x}' for s in cmd8))
    print("SUMMARY cmd8_resp0=" + ",".join(str(s[3]) for s in cmd8))
