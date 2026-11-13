import struct, re
class Img:
    def __init__(s,p):
        d=open(p,'rb').read(); e=struct.unpack_from('<I',d,0x3c)[0]
        nsec=struct.unpack_from('<H',d,e+6)[0]; osz=struct.unpack_from('<H',d,e+20)[0]
        s.soi=struct.unpack_from('<I',d,e+24+56)[0]; s.ts=struct.unpack_from('<I',d,e+8)[0]
        s.m=bytearray(s.soi); s.code=[]
        so=e+24+osz
        for i in range(nsec):
            o=so+40*i; vs,va,rs,rp,ch=struct.unpack_from('<IIII',d,o+8)+(struct.unpack_from('<I',d,o+36)[0],)
            s.m[va:va+min(rs,vs)]=d[rp:rp+min(rs,vs)]
            if ch&0x20000000: s.code.append((va,vs))
        s.m=bytes(s.m)
    def find(s,pat,limit=5):
        rx=b''.join(b'.' if t in('?','??') else re.escape(bytes([int(t,16)])) for t in pat.split())
        r=re.compile(rx,re.S); out=[]
        for va,vs in s.code:
            for mm in r.finditer(s.m,va,va+vs):
                out.append(mm.start())
                if len(out)>=limit: return out
        return out
    def u32(s,a): return struct.unpack_from('<I',s.m,a)[0]
    def i32(s,a): return struct.unpack_from('<i',s.m,a)[0]
    def rip(s,a,ln,off): return a+ln+s.i32(a+off)
    def entry(s,a,back=256):
        c=a-1
        while c>a-back and s.m[c] not in (0xCC,0xC3): c-=1
        if s.m[c] in (0xCC,0xC3):
            e=c+1
            while e<a and s.m[e]==0xCC: e+=1
            return e
