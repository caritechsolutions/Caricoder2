#!/usr/bin/env python3
"""Marks the CC-errors incidents nobody gave a verdict "exclude" (CC errors are
records since 23 Oct 2026). Undone per incident with the verdict "none".

  curl -fsSL .../scripts/exclude-old-cc.py | sudo python3 - [--dry-run] [CONFIG]

Reads the history to find them, then gives the verdicts through the
supervisor's API (its address and password from the config).
"""
import base64
import json
import sqlite3
import sys
import urllib.request

dry = "--dry-run" in sys.argv
args = [a for a in sys.argv[1:] if a != "--dry-run"]
cfg_path = args[0] if args else "/etc/caricoder3/caricoder3.json"
cfg = json.load(open(cfg_path))
db = cfg.get("history", "/var/lib/caricoder3/history.db")
port = int(cfg.get("port", 8550))
host = cfg.get("listen", "0.0.0.0")
host = "127.0.0.1" if host in ("0.0.0.0", "", "::") else host
d = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
ids = [r[0] for r in d.execute("select id from incidents where rule = 'cc_errors' and verdict is null order by id")]
print(f"{len(ids)} CC-errors incidents without a verdict")
if dry or not ids:
    sys.exit(0)
note = "CC errors are records now, not incidents (23 Oct 2026)"
body = "\n".join(json.dumps({"id": i, "verdict": "exclude", "note": note}) for i in ids) + "\n"
req = urllib.request.Request(f"http://{host}:{port}/api/verdicts/import", method="POST", data=body.encode())
if cfg.get("password"):
    req.add_header("Authorization", "Basic " + base64.b64encode(f"admin:{cfg['password']}".encode()).decode())
with urllib.request.urlopen(req, timeout=60) as r:
    res = json.load(r)
print(f"marked exclude: {res.get('applied')}, failed: {res.get('failed')}")
if res.get("errors"):
    print("  ", res["errors"])
