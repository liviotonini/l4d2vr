"""Checks every signature in L4D2VR/offsets.h against the DLLs of the installed game, without running it.

    python tools/check_signatures.py ["<Left 4 Dead 2 folder>"]

A signature that isn't found turns its feature off (or shows an error at startup if it isn't optional).
"""
import os
import re
import struct
import sys

DEFAULT_GAME = r'C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2'
FOLDERS = ['bin', os.path.join('left4dead2', 'bin')]
HERE = os.path.dirname(os.path.abspath(__file__))
OFFSET_RE = re.compile(r'Offset\s+(\w+)\s*=\s*\{\s*"([\w.]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*"([^"]+)"\s*(?:,\s*(-?\w+))?\s*(?:,\s*(true|false))?\s*\}')


def load_image(path):
    """Maps the sections of a 32-bit PE the way the loader does, so file data can be addressed by RVA."""
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    sections, optional = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    size = struct.unpack_from('<I', data, pe + 24 + 56)[0]
    image = bytearray(size)
    table = pe + 24 + optional
    for i in range(sections):
        virtual_size, rva, raw_size, raw = struct.unpack_from('<IIII', data, table + i * 40 + 8)
        length = min(raw_size, virtual_size or raw_size)
        image[rva:rva + length] = data[raw:raw + length]
    return bytes(image)


def find_dll(game, name):
    for folder in FOLDERS:
        for candidate in (name, name.lower()):
            path = os.path.join(game, folder, candidate)
            if os.path.exists(path):
                return path
    return None


def compile_signature(signature):
    parts = []
    for token in signature.split():
        parts.append(b'.' if token in ('?', '??') else re.escape(bytes([int(token, 16)])))
    return re.compile(b''.join(parts), re.DOTALL)


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_GAME
    source = open(os.path.join(HERE, '..', 'L4D2VR', 'offsets.h'), encoding='utf-8').read()
    images = {}
    failures = 0
    entries = OFFSET_RE.findall(source)

    for name, module, offset, signature, sig_offset, optional in entries:
        if module not in images:
            path = find_dll(game, module)
            images[module] = load_image(path) if path else None
        image = images[module]
        if image is None:
            print('NO DLL  %-30s %s' % (name, module))
            failures += 1
            continue

        pattern = compile_signature(signature)
        delta = int(sig_offset, 0) if sig_offset else 0
        rva = int(offset, 16)
        at_offset = pattern.match(image, max(rva - delta, 0)) is not None
        matches = [m.start() for m in pattern.finditer(image)]

        if at_offset:
            status = 'ok'
        elif matches:
            status = 'moved to 0x%X' % (matches[0] + delta)
        else:
            status = 'NOT FOUND'
            failures += 1
        if len(matches) > 1 and at_offset:
            status += ' (%d matches, the offset decides)' % len(matches)
        print('%-30s %-20s %s%s' % (name, module, status, '  [optional]' if optional == 'true' else ''))

    print('\n%d signatures, %d not found' % (len(entries), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
