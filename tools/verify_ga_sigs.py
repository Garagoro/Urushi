# Verify every GameAssembly signature used by URUSHI against a release and an alpha GameAssembly.dll.
# Usage: python tools/verify_ga_sigs.py <release GameAssembly.dll> <alpha GameAssembly.dll>
# Keep the table in sync with the BranchRva fallbacks in core/*.cpp (see core/game_build.h).
import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from ga_pe import Img
R=Img(sys.argv[1]); A=Img(sys.argv[2])
S=[ # name, pat, kind, (rel,alpha), pickFirst
("TypeInfoDefinitionTable","48 63 C8 48 8B 05 ? ? ? ? 48 8B 04 C8 C3","rip:3:7:3",(0x87EAEA0,0x9C2F5C0)),
("SetEffectsEnabled","48 8B 89 68 01 00 00 48 85 C9 74 ? 45 33 C0 E8 ? ? ? ? 48 8B BF 70 01 00 00","entry",(0x1E02570,0x23D2460)),
("SwitchingUpdate","4C 8B 46 28 4D 85 C0 0F 84 ? ? ? ? 33 C9 48 8B 15 ? ? ? ? E8 ? ? ? ? 48 89 44 24 50","entry",(0xE4FA70,0x13110F0)),
("IsFarCar","48 89 5C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC B0 01 00 00 4C 8B E9","direct",(0xE50760,0x1311E50)),
("Apply[release sig]","80 79 20 00 75 ? 80 79 21 00 C6 41 20 01 74 ? 48 8B 01 48 8B 90 C0 01 00 00 48 FF A0 B8 01 00 00","direct",(0xEA05F0,None)),
("Apply[alpha sig]","80 79 20 00 75 1E 80 79 21 00 C6 41 20 01 74 14 48 8B 11 48 8B 82 B8 01 00 00","direct",(None,0x1362FD0)),
("ProviderGetTransform","48 83 C1 18 E8 ? ? ? ? 48 8B 7B 20 48 85 FF 74 ? 33 D2 48 8B CB E8 ? ? ? ?","rip:0x17:5:1",(0xE57BA0,0x1319350)),
("TransformGetPosInj","48 8B D5 E8 ? ? ? ? 48 85 DB 74 ? F2 0F 10 00","rip:3:5:1",(0x5F72710,0x6FB9930)),
("VirtualInvokeData","45 0F B7 C3 48 8B CB E8 ? ? ? ? 48 8B 48 08 48 8B D7 E8 ? ? ? ? 4C 8B C0 0F 28 CE","rip:7:5:1",(0x9238B0,0xA736D0)),
("INetCarContainerTI","48 8B 15 ? ? ? ? B9 11 00 00 00 4C 8B C3 48 8B 5C 24 30","rip:0:7:3",(0x843A128,0x9797458)),
("DeformSleepTI","74 ? 48 8B 15 ? ? ? ? E8 ? ? ? ? 48 8B 0D ? ? ? ? 83 B9 ? ? ? ? 00 75 ? E8 ? ? ? ? 80 3D ? ? ? ? 00","rip:0xE:7:3",(0x83E7018,0x984C918)),
("CastRay.mi","4C 8B 0D ? ? ? ? 4C 8D 45 D0 48 8D 55 10 44 0F 29 6D 10 48 8B CE","rip:0:7:3",(0x8387960,0x9849F98)),
("CastRay.fn","4C 8B 0D ? ? ? ? 4C 8D 45 D0 48 8D 55 10 44 0F 29 6D 10 48 8B CE","rip:0x41:5:1",(0x2717C40,0x2F28300)),
("InitRuntimeMetadata","48 83 EC 28 E8 ? ? ? ? F0 83 0C 24 00 48 83 C4 28 C3 CC","direct",(0x9264A0,0xA76750)),
("TypeNamesSS","48 BA E6 2A 33 62 A9 C5 B6 9D 49 B8 80 32 50 01 13 1B EB 85 33 C9 E8 ? ? ? ? 48 8B 0D","rip:0x1B:7:3",(0x83F57D8,0x97DCAC8)),
("ChunkMetaClass","48 BA 90 60 B6 2D 2B 4E 0F E0 33 C9 E8 ? ? ? ? 48 8B 0D","rip:0x11:7:3",(0x843CB48,0x982E7E0)),
("EcsClass","48 8B 01 81 E2 FF 1F 00 00 8B 04 90 C3","rip:0x45:7:3",(0x8428C98,0x9770E30)),
("SphereCast*","48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 90 00 00 00 80 3D ? ? ? ? 00 49 8B D9 0F 29 B4 24 80 00 00 00 48 8B FA 0F 28 F2 48 8B F1 75 ? 48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05 ? ? ? ? 01 48 8B 84 24 D0 00 00 00 4C 8D 4C 24 60 4C 8B 05 ? ? ? ? 48 8D 54 24 70 4C 89 44 24 40 0F 28 D6 48 8B CE","direct",(0x5B57270,0x6B97C00)),
("ForceVelocityChange","48 83 EC 38 4C 8B 89 48 01 00 00 4D 85 C9 ? ? F2 0F 10 02","direct",(0x1DCE130,0x239D160)),
("ForceTransformChangeMat","48 89 5C 24 08 57 48 83 EC 60 48 8B F9 48 8B DA 48 8B 89 40 01 00 00","direct",(0x1DCE0B0,0x239D0E0)),
("CarRepair","40 53 41 55 48 83 EC 38 80 3D ? ? ? ? 00 44 0F B6 EA 48 8B D9","direct",(0x12FEC20,0x17C3EE0)),
("VisualRepair*","48 89 6C 24 30 48 89 74 24 38 48 89 7C 24 40 4C 89 74 24 48 48 85 DB 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? B9 02 00 00 00 4C 8B C3","entry",(0x1108340,0x15CFBC0)),
("FullCarRepair","48 8B 4C 24 20 E8 ? ? ? ? 48 81 C4 C0 00 00 00","rip:5:5:1",(0x1E570D0,0x2426DD0)),
]
def res(I,pat,kind):
    out=[]
    for a in I.find(pat):
        if kind=='direct': out.append(a)
        elif kind=='entry': out.append(I.entry(a))
        else: _,o,l,ro=kind.split(':'); out.append(I.rip(a+int(o,0),int(l),int(ro)))
    return out
bad=0
for n,p,k,(fr,fa) in S:
    line=[]
    for tag,I,exp in (("REL",R,fr),("ALPHA",A,fa)):
        h=res(I,p,k)
        if exp is None: st='ok(none)' if not h else 'UNEXPECTED'
        else:
            first=h[0] if h else None
            st=('OK' if first==exp else 'MISMATCH') + ('' if len(h)==1 else f'(x{len(h)}, first used)')
        if 'MISMATCH' in st or 'UNEXPECTED' in st: bad+=1
        line.append(f"{tag}:{st}")
    print(f"{n:26} "+"  ".join(line))
print("FAILURES:",bad)
