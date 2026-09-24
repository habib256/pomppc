# Usage : python3 tools/re/sym.py CHEMIN/GLDriver-POMPPC 0xOFF [0xOFF…] — décalage dans le bundle → symbole (nm -n)
import sys,subprocess,bisect
bin_=sys.argv[1]; offs=[int(x,16) for x in sys.argv[2:]]
syms=[]
for l in subprocess.run(['nm','-n',bin_],capture_output=True,text=True).stdout.splitlines():
    p=l.split()
    if len(p)==3 and p[1] in 'tT': syms.append((int(p[0],16),p[2]))
addrs=[a for a,_ in syms]
for o in offs:
    i=bisect.bisect_right(addrs,o)-1
    print('%08x -> %s+0x%x'%(o, syms[i][1] if i>=0 else '?', o-syms[i][0] if i>=0 else 0))
