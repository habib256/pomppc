# Usage : python3 tools/re/frames.py frames.csv [fenêtre=100] — ms/image, dessins/image, replis par tranche
import sys
rows=[l.strip().split(',') for l in open(sys.argv[1]) if l[0].isdigit()]
W=int(sys.argv[2]) if len(sys.argv)>2 else 100
prev=None
for i in range(0,len(rows)-W,W):
    a=rows[i]; b=rows[i+W]
    n=int(b[0])-int(a[0])
    dt=(float(b[2])-float(a[2]))/n
    dr=(int(b[4])-int(a[4]))/n
    fb=int(b[5])-int(a[5])
    print("frames %5s-%5s  t=%6.0fs  %6.1f ms/img  %6.0f draws/img  fallbacks %d"%(a[0],b[0],float(a[2])/1000,dt,dr,fb))
