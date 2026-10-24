#!/usr/bin/env python3
"""Which incidents had an effect, and which only came and went (read only).

  curl -fsSL .../scripts/incident-noise.py | sudo python3 - [DAYS] [DB]

For the last DAYS (7): of the channel incidents with CC errors, how many saw
damage on the output of the same channel within a minute (dropped pictures,
frozen, black, silent, no data, sync); of the "dropping" ones, how many had
a cause at the source (CC errors, no data, no input) within a minute; and
the channels with the most of each.
"""
import sqlite3
import sys
import time

days = int(sys.argv[1]) if len(sys.argv) > 1 else 7
path = sys.argv[2] if len(sys.argv) > 2 else "/var/lib/caricoder3/history.db"
d = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
since = int(time.time()) - days * 86400
now = int(time.time())
EFFECT = ("dropping", "frozen", "black", "silent", "no_data", "av_shift")
CAUSE = ("cc_errors", "no_data", "no_input")


def near(rule_set, what):
    q = ",".join("?" * len(rule_set))
    return d.execute(f"""
        select a.chan, exists(select 1 from incidents b where b.chan = a.chan and b.id != a.id
                              and b.rule in ({q}) and b.t <= coalesce(a.closed, ?) + 60
                              and coalesce(b.closed, ?) >= a.t - 60)
        from incidents a where a.rule = ? and a.t > ? and a.chan is not null""",
                     (*rule_set, now, now, what, since)).fetchall()


for what, rules, label in (("cc_errors", EFFECT, "with damage on the output"),
                           ("dropping", CAUSE, "with a cause at the source")):
    rows = near(rules, what)
    hit = sum(r[1] for r in rows)
    print(f"{what}: {len(rows)} on channels, {hit} {label}, {len(rows) - hit} without")
    per = {}
    for chan, h in rows:
        p = per.setdefault(chan, [0, 0])
        p[0] += 1
        p[1] += h
    for chan, (n, h) in sorted(per.items(), key=lambda x: -x[1][0])[:8]:
        print(f"    {chan}: {n} ({h} {label.split()[0]} {' '.join(label.split()[1:])})")
srv = d.execute("select count(*) from incidents where rule = 'cc_errors' and t > ? and chan is null", (since,)).fetchone()[0]
print(f"cc_errors on sources/the server (no channel): {srv}")
print("dropping by hour of day:", " ".join(f"{h}:{n}" for h, n in d.execute(
    "select cast(strftime('%H', t, 'unixepoch', 'localtime') as int) h, count(*) from incidents "
    "where rule = 'dropping' and t > ? group by h order by h", (since,))))
