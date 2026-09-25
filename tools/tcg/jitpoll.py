#!/usr/bin/env python3
"""jitpoll.py SOCKET PERIODE DUREE > fichier — relève `info jit` (HMP) toutes
les PERIODE secondes pendant DUREE secondes, une ligne par relevé :

    t  tb_count  tb_flush  tb_invalidate  tlb_full  tlb_partial  tlb_elided  code_size

Les débits (traductions/s, invalidations/s, vidages de TLB/s) sont les
différences de deux lignes. Sans greffon : ne perturbe presque rien."""
import re, socket, sys, time

sock, per, dur = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])

def hmp(s, cmd):
    s.sendall(cmd.encode() + b"\n")
    buf, t0 = b"", time.time()
    while b"TLB elided" not in buf and time.time() - t0 < 20:
        try:
            buf += s.recv(65536)
        except socket.timeout:
            pass
    time.sleep(0.2)
    try:
        buf += s.recv(65536)
    except socket.timeout:
        pass
    return buf.decode("utf-8", "replace")

s = socket.socket(socket.AF_UNIX); s.settimeout(0.5); s.connect(sock)
time.sleep(0.5)
try:
    s.recv(65536)
except socket.timeout:
    pass
pat = {k: re.compile(v) for k, v in dict(
    tbc=r"TB count\s+(\d+)", tbf=r"TB flush count\s+(\d+)",
    tbi=r"TB invalidate count\s+(\d+)", full=r"TLB full flushes\s+(\d+)",
    part=r"TLB partial flushes\s+(\d+)", eli=r"TLB elided flushes\s+(\d+)",
    code=r"gen code size\s+(\d+)").items()}
t0 = time.time()
print("t tb_count tb_flush tb_inval tlb_full tlb_part tlb_elided code_size", flush=True)
while time.time() - t0 <= dur:
    out = hmp(s, "info jit")
    v = []
    for k in ("tbc", "tbf", "tbi", "full", "part", "eli", "code"):
        m = pat[k].search(out)
        v.append(m.group(1) if m else "?")
    print("%.1f %s" % (time.time() - t0, " ".join(v)), flush=True)
    time.sleep(per)
