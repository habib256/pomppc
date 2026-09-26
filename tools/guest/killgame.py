#!/usr/bin/env python3
"""killgame.py — tue le processus Colin McRae depuis l'hôte : stub GDB de QEMU sur socket UNIX,
   on attend qu'un vCPU soit en mode utilisateur dans le jeu (PC ou LR dans le binaire, IndirectX,
   GLEngine ou le plugin), et on met son PC à 0 → erreur de bus → CrashReporter → fin du processus.
   Déplacé de .run/cmr/ (A5 scripts, 26/09/2026) ; les plages d'adresses sont celles de Colin McRae."""
import socket,re,time,os,sys
import subprocess
_ici=os.path.dirname(os.path.abspath(__file__))
_c=subprocess.run(["git","-C",_ici,"rev-parse","--path-format=absolute","--git-common-dir"],capture_output=True,text=True).stdout.strip()
ROOT=_c[:-5] if _c.endswith("/.git") else os.path.dirname(os.path.dirname(_ici))   # dépôt principal (.run/)
SOCK=ROOT+"/.run/mon.sock"; GDB=ROOT+"/.run/gdb.sock"
RANGES=[(0x1000,0x2f3000),(0x1808000,0x1900000),(0x1c2e000,0x1d3f000),(0x1d6c000,0x1d8d000)]
def hmp():
    s=socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(SOCK)
    def until():
        b=b""
        while b"(qemu)" not in b:
            c=s.recv(65536)
            if not c: raise SystemExit("moniteur fermé")
            b+=c
        return b
    until()
    def run(cmd):
        s.sendall(cmd.encode()+b"\n"); out=until().decode("utf-8","replace")
        out=re.sub(r"\x1b\[[0-9;]*[A-Za-z]","",out).replace("\r","")
        return "\n".join(l for l in out.split("\n") if l.strip() and "(qemu)" not in l and "[K" not in l and l.strip()!=cmd)
    return run
h=hmp()
if os.path.exists(GDB): os.remove(GDB)
h("gdbserver unix:%s,server=on,wait=off" % GDB)
def ingame(a): return any(lo<=a<hi for lo,hi in RANGES)
done=False
for attempt in range(40):
    g=socket.socket(socket.AF_UNIX); g.settimeout(10); g.connect(GDB)
    def send(data):
        g.sendall(b"$"+data+b"#%02x" % (sum(data)&255)); buf=b""
        while True:
            c=g.recv(65536)
            if not c: raise SystemExit("stub fermé")
            buf+=c; m=re.search(rb"\$([^#]*)#[0-9a-f]{2}",buf)
            if m: g.sendall(b"+"); return m.group(1)
            if buf==b"+" and data==b"D": return b""
    send(b"?")
    for tid in (1,2):
        send(b"Hg%d"%tid); pc=int(send(b"p40"),16); msr=int(send(b"p41"),16); lr=int(send(b"p43"),16)
        if msr&0x4000 and (ingame(pc) or ingame(lr)):
            send(b"P40=0000000000000000"); print("thread %d pc=%x lr=%x -> PC=0" % (tid,pc,lr)); done=True; break
    send(b"D"); g.close()
    if done: break
    time.sleep(0.5)
h("gdbserver none"); print("tué" if done else "jeu non trouvé sur un vCPU", "|", h("info status"))
