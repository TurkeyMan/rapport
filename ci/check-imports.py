#!/usr/bin/env python3
"""Fails if a Windows executable or DLL imports a DLL that isn't Windows'
own: a C or C++ runtime (MSVC's or MinGW's), or a library that should have
been linked in. For self-contained builds, and for shell extensions, which
load into explorer.exe where none of ours can be found.

  check-imports.py [--allow NAME.dll]... [--allow-beside] FILE...

--allow names a DLL that ships beside the files (it's checked too, if given);
--allow-beside allows any DLL in the same directory as the file importing it.
Reads the PE import table itself, so it runs anywhere Python does."""
import os
import re
import struct
import sys

# Windows' own DLLs that our builds use. A name not here fails: add it when
# a new system DLL is genuinely needed.
SYSTEM = {
    'advapi32.dll', 'bcrypt.dll', 'comctl32.dll', 'comdlg32.dll', 'crypt32.dll', 'd2d1.dll',
    'd3d11.dll', 'dnsapi.dll', 'dwmapi.dll', 'dwrite.dll', 'dxgi.dll', 'gdi32.dll', 'gdiplus.dll', 'imm32.dll', 'iphlpapi.dll',
    'kernel32.dll', 'msimg32.dll', 'ncrypt.dll', 'ntdll.dll', 'ole32.dll', 'oleacc.dll',
    'oleaut32.dll', 'propsys.dll', 'rpcrt4.dll', 'secur32.dll', 'setupapi.dll', 'shcore.dll',
    'shell32.dll', 'shlwapi.dll', 'user32.dll', 'userenv.dll', 'uxtheme.dll', 'version.dll',
    'windowscodecs.dll', 'winhttp.dll', 'winmm.dll', 'winspool.drv', 'ws2_32.dll',
    # MinGW's C runtime is Windows' own msvcrt.dll.
    'msvcrt.dll',
}


def imports(path):
    data = open(path, 'rb').read()
    if data[:2] != b'MZ':
        raise ValueError('not a PE file')
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    import_rva = struct.unpack_from('<I', data, directories + 8)[0]
    table = optional + optional_size
    spans = [struct.unpack_from('<IIII', data, table + 40 * i + 8) for i in range(sections)]

    def offset(rva):
        for size, virtual, raw_size, raw in spans:
            if virtual <= rva < virtual + max(size, raw_size):
                return rva - virtual + raw
        raise ValueError(f'RVA {rva:#x} is in no section')

    names = []
    if import_rva:
        at = offset(import_rva)
        while True:
            name_rva = struct.unpack_from('<I', data, at + 12)[0]
            if name_rva == 0:
                break
            start = offset(name_rva)
            names.append(data[start:data.index(b'\0', start)].decode('ascii'))
            at += 20
    return names


def main(argv):
    allowed, files, beside = set(), [], False
    args = iter(argv)
    for a in args:
        if a == '--allow':
            allowed.add(next(args).lower())
        elif a == '--allow-beside':
            beside = True
        else:
            files.append(a)
    if not files:
        print(__doc__, file=sys.stderr)
        return 2
    bad = 0
    for f in files:
        here = {n.lower() for n in os.listdir(os.path.dirname(os.path.abspath(f)))} if beside else set()
        for dll in imports(f):
            name = dll.lower()
            if name in SYSTEM or name in allowed or name in here or re.match(r'api-ms-win-(core|shcore|eventing)-', name):
                continue
            print(f'{f}: imports {dll}, which Windows doesn\'t provide', file=sys.stderr)
            bad += 1
    if not bad:
        print(f'{len(files)} file(s): system imports only')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
