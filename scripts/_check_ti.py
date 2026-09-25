import struct, sys

f = open('build-xbox360-m2/.oxdklink/title.elf','rb').read()
# ELF32 big-endian
e_phoff = struct.unpack('>I', f[28:32])[0]
e_phentsize = struct.unpack('>H', f[42:44])[0]
e_phnum = struct.unpack('>H', f[44:46])[0]

segs = []
for i in range(e_phnum):
    off = e_phoff + i*e_phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack('>8I', f[off:off+32])[:6]
    segs.append((p_vaddr, p_filesz, p_offset, p_memsz))

def dump(vaddr, n=48):
    for vaddr0, filesz, offset, memsz in segs:
        if vaddr0 <= vaddr < vaddr0 + filesz:
            fo = offset + (vaddr - vaddr0)
            raw = f[fo:fo+n]
            for i in range(0, n, 4):
                w = struct.unpack('>I', raw[i:i+4])[0]
                print(f'  {vaddr+i:08x}: {w:08x}')
            return
    for vaddr0, filesz, offset, memsz in segs:
        if vaddr0 <= vaddr < vaddr0 + memsz:
            print(f'  {vaddr:08x}: BSS/NOBITS (zero-filled)')
            return
    print(f'  {vaddr:08x}: NOT MAPPED')

syms = {
  '_ZTVN10__cxxabiv117__class_type_infoE': 0x827855b0,
  '_ZTVN10__cxxabiv120__si_class_type_infoE': 0x827855d8,
  '_ZTIN4brls3BoxE': 0x8276a580,
  '_ZTIN4brls4ViewE': 0x8276d6b4,
  '_ZTI11GalleryView': 0x8275ccc4,
  '_ZTV11GalleryView': 0x8275cb30,
}
for name, addr in syms.items():
    print(name, hex(addr))
    dump(addr)
