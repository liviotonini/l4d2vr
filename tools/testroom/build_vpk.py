"""Builds the L4D2VR test room addon (l4d2vrtest.vpk) and installs it in the game.

    python tools/testroom/build_vpk.py [game folder]

The addon holds a game mode (modes/l4d2vrtest.txt) and its script (scripts/vscripts/l4d2vrtest.nut): every weapon and item
on tables around the player, no infected, no bots. Load it with the console command  map c2m1_highway l4d2vrtest
(or exec l4d2vrtest, or the launch option +exec l4d2vrtest).
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = sys.argv[1] if len(sys.argv) > 1 else r'C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2'

ADDON_INFO = b'''"AddonInfo"
{
	addonSteamAppID		550
	addontitle			"L4D2VR test room"
	addonversion		1
	addonauthor			"L4D2VR"
	addonDescription	"Every weapon and item on tables around you, no infected and no bots, to test the VR mod. Console: map c2m1_highway l4d2vrtest"
	addonContent_Script	1
}
'''


def read(relative):
    with open(os.path.join(HERE, relative), 'rb') as f:
        return f.read()


def build_vpk(files):
    """VPK version 1 with the data in the same file. files: {'path/name.ext': bytes}"""
    tree = {}
    for full, data in files.items():
        directory, filename = os.path.split(full)
        name, extension = os.path.splitext(filename)
        tree.setdefault(extension[1:], {}).setdefault(directory or ' ', []).append((name, data))

    entries = b''
    blob = b''
    for extension in sorted(tree):
        entries += extension.encode() + b'\0'
        for directory in sorted(tree[extension]):
            entries += directory.encode() + b'\0'
            for name, data in tree[extension][directory]:
                entries += name.encode() + b'\0'
                # crc, preload bytes, archive (0x7fff: this file), offset after the tree, length, terminator
                entries += struct.pack('<IHHIIH', zlib.crc32(data) & 0xFFFFFFFF, 0, 0x7FFF, len(blob), len(data), 0xFFFF)
                blob += data
            entries += b'\0'
        entries += b'\0'
    entries += b'\0'
    return struct.pack('<III', 0x55AA1234, 1, len(entries)) + entries + blob


def enable_in_addon_list(game_dir, vpk_name):
    """The game only mounts addons that are on in addonlist.txt"""
    path = os.path.join(game_dir, 'left4dead2', 'addonlist.txt')
    line = '\t"%s"\t\t"1"\n' % vpk_name
    if not os.path.exists(path):
        with open(path, 'w', newline='\r\n') as f:
            f.write('"AddonList"\n{\n' + line + '}\n')
        return
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    if '"%s"' % vpk_name in text:
        text = text.replace('"%s"\t\t"0"' % vpk_name, '"%s"\t\t"1"' % vpk_name)
    else:
        end = text.rfind('}')
        text = text[:end] + line + text[end:]
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


if __name__ == '__main__':
    files = {
        'addoninfo.txt': ADDON_INFO,
        'modes/l4d2vrtest.txt': read('modes/l4d2vrtest.txt'),
        'scripts/vscripts/l4d2vrtest.nut': read('scripts/vscripts/l4d2vrtest.nut'),
        'cfg/l4d2vrtest.cfg': read('cfg/l4d2vrtest.cfg'),
    }
    out = os.path.join(GAME, 'left4dead2', 'addons', 'l4d2vrtest.vpk')
    with open(out, 'wb') as f:
        f.write(build_vpk(files))
    enable_in_addon_list(GAME, 'l4d2vrtest.vpk')
    print('installed', out, os.path.getsize(out), 'bytes')
