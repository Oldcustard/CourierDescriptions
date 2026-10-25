"""Dump an SKSE plugin DLL's SKSEPlugin_Version block and PE link timestamp.

Checks the two things SKSE's PluginManager gates on (skse64 PluginManager.cpp:559-588)
without needing the game: the version-independence flags and the PE TimeDateStamp.
"""
import struct, sys, datetime

SKSE_V5_CUTOFF_LO = 520128000     # skse64: buildTime >= this ...
SKSE_V5_CUTOFF_HI = 1748217600    # ... and < this (2025-05-26 GMT) means "must be recompiled"

def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def parse(path):
    d = open(path, 'rb').read()
    pe = u32(d, 0x3C)
    assert d[pe:pe+4] == b'PE\0\0', 'not a PE file'
    machine        = u16(d, pe + 4)
    num_sections   = u16(d, pe + 6)
    timedatestamp  = u32(d, pe + 8)
    size_opt       = u16(d, pe + 20)
    opt            = pe + 24
    magic          = u16(d, opt)
    assert magic == 0x20B, f'expected PE32+, got {magic:#x}'
    export_rva     = u32(d, opt + 0x70)
    sections = []
    sec = opt + size_opt
    for i in range(num_sections):
        off = sec + i * 40
        name = d[off:off+8].rstrip(b'\0').decode('latin1')
        sections.append((name, u32(d, off+12), u32(d, off+8), u32(d, off+20), u32(d, off+16)))

    def rva2off(rva):
        for name, va, vsize, praw, rawsize in sections:
            if va <= rva < va + max(vsize, rawsize):
                return praw + (rva - va)
        raise ValueError(f'RVA {rva:#x} in no section')

    exp = rva2off(export_rva)
    n_names   = u32(d, exp + 24)
    addr_funcs = u32(d, exp + 28)
    addr_names = u32(d, exp + 32)
    addr_ords  = u32(d, exp + 36)
    exports = {}
    for i in range(n_names):
        name_rva = u32(d, rva2off(addr_names) + i * 4)
        off = rva2off(name_rva)
        name = d[off:d.index(b'\0', off)].decode('latin1')
        ordinal = u16(d, rva2off(addr_ords) + i * 2)
        exports[name] = u32(d, rva2off(addr_funcs) + ordinal * 4)
    return d, rva2off, machine, timedatestamp, exports

def unpack_version(p):
    return (p >> 24) & 0xFF, (p >> 16) & 0xFF, (p >> 4) & 0xFFF, p & 0xF

def main(path):
    d, rva2off, machine, ts, exports = parse(path)
    print(f'file            : {path}')
    print(f'machine         : {machine:#06x} ({"x64" if machine == 0x8664 else "?"})')
    stamp = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc)
    print(f'PE TimeDateStamp: {ts} = {stamp:%Y-%m-%d %H:%M:%S} UTC')
    in_window = SKSE_V5_CUTOFF_LO <= ts < SKSE_V5_CUTOFF_HI
    print(f'  SKSE v5 gate   : timestamp {"INSIDE" if in_window else "outside"} the reject window '
          f'[{SKSE_V5_CUTOFF_LO}, {SKSE_V5_CUTOFF_HI}) -> '
          f'{"flag REQUIRED" if in_window else "passes on timestamp alone"}')
    print(f'exports         : {", ".join(sorted(exports))}')

    rva = exports.get('SKSEPlugin_Version')
    if rva is None:
        print('!! no SKSEPlugin_Version export')
        return 1
    o = rva2off(rva)
    data_version  = u32(d, o + 0x000)
    plugin_version = u32(d, o + 0x004)
    name  = d[o+0x008:o+0x108].split(b'\0')[0].decode('latin1')
    author = d[o+0x108:o+0x208].split(b'\0')[0].decode('latin1')
    email = d[o+0x208:o+0x304].split(b'\0')[0].decode('latin1')
    vi_ex = u32(d, o + 0x304)
    vi    = u32(d, o + 0x308)
    compat = [u32(d, o + 0x30C + i*4) for i in range(16)]
    xse_min = u32(d, o + 0x34C)

    print()
    print(f'dataVersion     : {data_version}')
    print(f'pluginVersion   : {".".join(map(str, unpack_version(plugin_version)))}  (packed {plugin_version:#010x})')
    print(f'pluginName      : {name!r}')
    print(f'author          : {author!r}')
    print(f'supportEmail    : {email!r}')
    print(f'versionIndependence   : {vi:#010b}')
    for bit, label in ((1 << 0, 'AddressLibraryPostAE'), (1 << 1, 'Signatures'), (1 << 2, 'StructsPost629')):
        print(f'    [{"x" if vi & bit else " "}] {label}')
    print(f'versionIndependenceEx : {vi_ex:#010b}')
    for bit, label in ((1 << 0, 'NoStructUse'), (1 << 1, 'AddressLibraryV5')):
        print(f'    [{"x" if vi_ex & bit else " "}] {label}')
    compat_set = [".".join(map(str, unpack_version(c))) for c in compat if c]
    print(f'compatibleVersions    : {compat_set if compat_set else "(none - version independent)"}')
    print(f'xseMinimum            : {".".join(map(str, unpack_version(xse_min))) if xse_min else "(none)"}')

    print()
    uses_addrlib = bool(vi & 1)
    print('SKSE load verdict (per skse64 PluginManager.cpp):')
    if not uses_addrlib and not compat_set:
        print('  !! neither address-library independence nor a compatible-version list')
    if uses_addrlib:
        print('  - claims AddressLibraryPostAE -> SKSE checks the address library exists')
        if vi_ex & 2:
            print('  - declares AddressLibraryV5 -> loads on 1.7.99+ (format 5 library)')
        elif not in_window:
            print('  - no AddressLibraryV5 bit, but linked outside the reject window -> still loads on 1.7.x')
        else:
            print('  !! no AddressLibraryV5 bit and linked inside the window -> REJECTED on 1.7.x')
        if (vi & 4) or (vi_ex & 1):
            print('  - satisfies the >=1.6.629 struct gate (StructsPost629 or NoStructUse)')
        else:
            print('  !! fails the >=1.6.629 struct gate -> "only compatible with versions earlier than 1.6.629"')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
