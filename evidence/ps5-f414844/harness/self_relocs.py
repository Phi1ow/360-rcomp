import struct,sys
d=open(sys.argv[1],"rb").read()
n=struct.unpack_from("<H",d,0x18)[0]
ents=[struct.unpack_from("<QQQQ",d,0x20+i*32) for i in range(n)]
elf=0x20+n*32
ph=[struct.unpack_from("<IIQQQQQQ",d,elf+64+i*56) for i in range(struct.unpack_from("<H",d,elf+0x38)[0])]
dyn=[p for p in ph if p[0]==2][0]
# find LOAD containing dynamic
li=[i for i,p in enumerate(ph) if p[0]==1 and p[3]<=dyn[3]<p[3]+p[5]][0]
base=ph[li][3]
seg=[d[o:o+fs] for p,o,fs,ms in ents if (p>>20)&0xfff==li and p&0x800][0]
at=lambda va:va-base
o=at(dyn[3]); tags={}
while True:
    t,v=struct.unpack_from("<qQ",seg,o); o+=16
    if t==0: break
    tags.setdefault(t&0xffffffffffffffff,[]).append(v)
strtab,symtab=tags[5][0],tags[6][0]
def s(off):
    e=seg.index(b"\0",at(strtab)+off); return seg[at(strtab)+off:e].decode(errors="replace")
print("NEEDED:",[s(v) for v in tags.get(1,[])])
want=[int(x,16) for x in sys.argv[2:]]
rela,relasz=tags[7][0],tags[8][0]
for k in range(relasz//24):
    off,info,add=struct.unpack_from("<QQq",seg,at(rela)+k*24)
    sym=info>>32
    nm=s(struct.unpack_from("<I",seg,at(symtab)+sym*24)[0]) if sym else "-"
    if not want or off in want: print(hex(off),info&0xffffffff,nm,hex(add))
