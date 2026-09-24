# Usage : python3 tools/re/dumpdec.py VIDAGE [pc-ou-index] [tout] — lancé depuis la racine du dépôt (lit patches/qgpu/qgpu_proto.h)
import sys,struct,re
ops={}
for m in re.finditer(r'#define QGPU_OP_(\w+)\s+0x([0-9A-Fa-f]+)', open('patches/qgpu/qgpu_proto.h').read()):
    ops[int(m.group(2),16)]=m.group(1)
sk={}
for m in re.finditer(r'#define QGPU_SK_(\w+)\s+(\d+)\s', open('patches/qgpu/qgpu_proto.h').read()):
    sk.setdefault(int(m.group(2)),m.group(1))
def dec(path, want=None, ctx=3, allcmds=False):
    d=open(path,'rb').read()
    h=struct.unpack('>16I',d[:64]); ncmd=h[3]
    cmd=d[64:64+ncmd]; words=struct.unpack('>%dI'%(ncmd//4),cmd)
    q=0; idx=0; out=[]
    while q<len(words):
        hd=words[q]; op=hd>>16; l=hd&0xffff
        if not l: break
        a=words[q+1:q+l]
        name=ops.get(op,hex(op))
        s='%5d pc=%4d %s %s'%(idx,q,name,' '.join('%x'%x for x in a[:12]))
        if name=='SET_STATE': s+='  <%s>'%sk.get(a[0],'?')
        out.append((q,idx,s))
        q+=l; idx+=1
    if allcmds:
        for _,_,s in out: print(s)
    else:
        for i,(qq,ii,s) in enumerate(out):
            if want is not None and (qq==want or ii==want):
                for j in range(max(0,i-ctx),min(len(out),i+2)): print(out[j][2])
                print('---')
if __name__=='__main__':
    dec(sys.argv[1], int(sys.argv[2]) if len(sys.argv)>2 else None, allcmds=(len(sys.argv)>3))
