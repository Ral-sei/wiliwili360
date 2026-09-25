import sys, hashlib, os
src = "build-xbox360-m2/CMakeFiles/wiliwili.dir/wiliwili/source/platform/xbox360/xbox360_m3_shims.cpp.o"
ostat = os.stat(src)
stage = "build-xbox360-m2/.oxdklink/inputs"
digest = hashlib.sha1(os.path.abspath(src).encode()).hexdigest()[:10]
staged = os.path.join(stage, digest + "-" + os.path.basename(src))
st = os.stat(staged)
print("up_to_date:", st.st_size == ostat.st_size and int(st.st_mtime) == int(ostat.st_mtime))
print("sizes:", st.st_size, ostat.st_size)
print("digest:", digest, "staged:", staged)


f = open('build-xbox360-m2/.oxdklink/title.elf','rb').read()
e_phoff = struct.unpack('>I', f[28:32])[0]
e_phentsize = struct.unpack('>H', f[42:44])[0]
e_phnum = struct.unpack('>H', f[44:46])[0]

segs = []
for i in range(e_phnum):
    off = e_phoff + i*e_phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack('>IIIIII', f[off:off+24])
    segs.append((p_vaddr, p_offset, p_filesz, p_memsz, p_type))

pat = struct.pack('>I', 0x8001FFF8)
idx = 0
count = 0
while True:
    idx = f.find(pat, idx)
    if idx < 0:
        break
    # find vaddr
    va = None
    for vaddr0, off, filesz, memsz, ptype in segs:
        if off <= idx < off + filesz and ptype == 1:
            va = vaddr0 + (idx - off)
            break
    if va:
        print(f'hit at vaddr 0x{va:08x} (file 0x{idx:x})')
        count += 1
        if count > 40:
            break
    idx += 1
print('total hits:', count)

