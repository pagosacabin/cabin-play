#!/usr/bin/env python3
"""0239: per-boot table from one arm's report read-backs.

Each boot's OWN result is the FOLD_FIRST_INIT line the board printed for that
boot; FOLD_SLOT lines are the persisted table and are quoted separately (the
table's last line is the board's own cross-reset tally).
"""
import glob, os, re, sys

arm = sys.argv[1]
d = os.path.dirname(os.path.abspath(__file__)) + f"/arm{arm}"
files = sorted(glob.glob(d + "/report-*.txt"), key=lambda p: int(re.search(r'report-(\d+)', p).group(1)))

print(f"arm {arm}: {len(files)} report read-back(s)\n")
print(f"{'boot':>4}  {'this boot':>42}  {'e2e':>8}")
ok = 0
for f in files:
    n = int(re.search(r'report-(\d+)', f).group(1))
    txt = open(f, errors='replace').read()
    m = re.search(r'FOLD_FIRST_INIT: (arm=\d+.*)$', txt, re.M)
    e2e = re.search(r'FOLD_E2E_MOUNT: (.*)', txt)
    if not m:
        print(f"{n:>4}  -- no FOLD_FIRST_INIT line --")
        continue
    res = m.group(1).strip()
    if 'ok=OK' in res:
        ok += 1
    e = 'mounted' if e2e and 'mounted' in e2e.group(1) else ('FAILED' if e2e else '-')
    print(f"{n:>4}  {res:>42}  {e:>8}")
print(f"\nboots read: {len(files)}; first-call init OK: {ok}")
last = open(files[-1], errors='replace').read() if files else ''
for ln in last.splitlines():
    if ln.startswith(('FOLD_SLOTS', 'FOLD_SUMMARY')):
        print("board's own tally at that read-back:", ln)
