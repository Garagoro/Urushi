import pefile

up_path = r'C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online 2\UnityPlayer.dll'
pe = pefile.PE(up_path)
data = pe.get_memory_mapped_image()

text_sec = None
for s in pe.sections:
    if s.Name.decode().strip('\x00') == '.text':
        text_sec = s
        break

start = text_sec.VirtualAddress
end = start + text_sec.Misc_VirtualSize

def make_sig(rva, max_len=48):
    for l in range(8, max_len):
        b = data[rva:rva+l]
        count = 0
        idx = start
        while True:
            pos = data.find(b, idx, end)
            if pos == -1:
                break
            count += 1
            if count > 1:
                break
            idx = pos + 1
        if count == 1:
            hex_str = ' '.join(f'{x:02X}' for x in b)
            return hex_str
    return None

targets = {
    'FindActiveTransformWithPath': 0x669CE0,
    'CloneSingle': 0xC4230,
    'Destroy': 0xC3A30,
    'SetActive': 0xBDFC0,
    'SetLayer': 0xBDFA0,
    'GameObject_get_transform': 0xBDEC0,
    'Transform_get_position': 0xCCA20,
    'Transform_set_position': 0xCCA50,
    'Transform_get_rotation': 0xCCB40,
    'Transform_set_rotation': 0xCCB90,
    'GetMainCameraNative': 0x592900,
    'InternalRaycast': 0x12E080,
    'ExecutePlayerLoop': 0x5AC580,
    'InstanceIDToObject': 0x107480,
    'Material_GetRenderQueue': 0x95EB0,
    'Material_SetRenderQueue': 0x95EF0,
    'Material_SetShaderPassEnabled': 0x98530,
    'Texture_GetNativeTexturePtr': 0xA8710
}

for name, rva in targets.items():
    sig = make_sig(rva)
    print(f'{name} (0x{rva:X}): "{sig}"')
