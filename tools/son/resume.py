#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""Résumé compact du journal SCREAMER_DIAG : une ligne par seconde utile."""
import sys, re

path = sys.argv[1]
t0 = float(sys.argv[2]) if len(sys.argv) > 2 else None
tous = "-a" in sys.argv
for line in open(path, errors="replace"):
    if line.startswith("SEC"):
        f = line.split()
        t = int(f[1]) / 1000
        d = dict(x.split("=", 1) for x in f[2:] if "=" in x)
        if t0 is None:
            t0 = t
        rel = t - t0
        interessant = (d["sil"] != "0/0" or d["ca_vide"] != "0" or d["trous"] != "0" or
                       float(d["cbgap_max"]) > 25 or float(d["qsync_max"]) > 20 or
                       d["perimes"] != "0")
        if tous or interessant:
            print("%7.1f cbgap=%5s tick=%5s ring=%-11s sil=%-9s zero=%-6s trous=%s perim=%-4s ca_vide=%s minpend=%-5s qsync=%s/%sms max=%s pulls=%s cmds=%s" % (
                rel, d["cbgap_max"], d["tick_max"], d["ring"], d["sil"], d["zero"], d["trous"],
                d["perimes"], d["ca_vide"], d["ca_minpend"], d["qsync"], d["qsync_ms"], d["qsync_max"],
                d["pulls"], d["cmds"]))
    elif line.startswith("EVT"):
        f = line.split(None, 2)
        t = int(f[1]) / 1000
        if t0 is None:
            t0 = t
        print("%7.1f   %s" % (t - t0, f[2].rstrip()))
