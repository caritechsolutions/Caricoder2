#!/usr/bin/env python3
"""When a channel dropped or late pictures, and what else was going on (read only).

  curl -fsSL .../scripts/channel-drops.py | sudo python3 - CHANNEL [HOURS] [DB]

For each 10 s of the last HOURS (24) in which CHANNEL dropped or late
pictures: its input rate and counters, the other channels that dropped
in the same 10 s, and the server's CPU and video engines then.
"""
import sqlite3
import sys
import time

if len(sys.argv) < 2:
    sys.exit(__doc__)
chan = sys.argv[1]
hours = float(sys.argv[2]) if len(sys.argv) > 2 else 24
path = sys.argv[3] if len(sys.argv) > 3 else "/var/lib/caricoder3/history.db"
d = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
since = int(time.time() - hours * 3600)
cid = d.execute("select id from chans where name = ?", (chan,)).fetchone()
if not cid:
    sys.exit(f"no channel {chan} in the history")
cid = cid[0]
mets = dict(d.execute("select id, name from metrics where name = 'cpu.percent' or name like 'gpu%.encoder'"
                      " or name like 'gpu%.decoder'"))
rows = d.execute("select t, inb, cc, ovf, drp, late, lost from s10 where cid = ? and t >= ? and drp + late > 0"
                 " order by t", (cid, since)).fetchall()
n10 = d.execute("select count(*) from s10 where cid = ? and t >= ?", (cid, since)).fetchone()[0]
print(f"{chan}: {len(rows)} of {n10} ten-second rows with dropped or late pictures in {hours:g} h")
print("time      in kb/s  cc ovf drop late lost | others dropping then | cpu% engines%")
for t, inb, cc, ovf, drp, late, lost in rows:
    others = d.execute("select c.name, s.drp + s.late from s10 s join chans c on c.id = s.cid where s.t = ? and s.cid != ?"
                       " and s.drp + s.late > 0", (t, cid)).fetchall()
    m = dict(d.execute(f"select mid, v from m10 where t = ? and mid in ({','.join('?' * len(mets)) or 'null'})",
                       (t, *mets)).fetchall())
    cpu = next((m[k] for k, v in mets.items() if v == "cpu.percent" and k in m), None)
    eng = " ".join(f"{mets[k][:-8].replace('gpu', 'g')}{'e' if mets[k].endswith('encoder') else 'd'}{m[k]:.0f}"
                   for k in sorted(m) if mets.get(k, "").startswith("gpu"))
    o = ", ".join(f"{n}:{v}" for n, v in others[:6]) + (f" +{len(others) - 6}" if len(others) > 6 else "")
    print(f"{time.strftime('%H:%M:%S', time.localtime(t))} {inb:7d} {cc:3d} {ovf:3d} {drp:4d} {late:4d} {lost:4d} | "
          f"{o or '-'} | {cpu if cpu is None else round(cpu)} {eng}")
