"""Builds the L4D2VR test room addon (l4d2vr_testroom.vpk) and installs it in the game.

    python tools/testroom/build_vpk.py [game folder]

The addon is a campaign of one map, "L4D2VR Test Room", listed with the other add-on campaigns (Play Campaign), so it
can be played with friends who have the same VPK in left4dead2/addons. The map, l4d2vr_testroom, is a copy of the
game's c2m1_highway (taken from the game folder, so the VPK is ~40 MB) with more things in its entity lump:
a logic_script that runs scripts/vscripts/l4d2vr_testroom.nut (no infected, no bots) and an output on the
info_director that calls L4D2VR_TestRoom() a second after gameplay starts, which puts every weapon and item on tables
around the survivors, plus a logic_timer that calls L4D2VR_Refill() every half second to put back what was taken
from the tables. The mission file (missions/l4d2vr_testroom.txt) also turns on every melee weapon.

The cubemaps embedded in the map are in materials/maps/c2m1_highway/, but the engine looks for the ones models use
(weapons, props) in materials/maps/<name of the loaded map>/: without copies under the new name, models are drawn white.

From the console:  map l4d2vr_testroom
"""
import io
import os
import re
import struct
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = sys.argv[1] if len(sys.argv) > 1 else r'C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2'
SOURCE_MAP = 'c2m1_highway'
MAP = 'l4d2vr_testroom'
VPK_NAME = 'l4d2vr_testroom.vpk'
OLD_VPK_NAME = 'l4d2vrtest.vpk'  # the test room as a game mode on c2m1_highway, replaced by this campaign

ADDON_INFO = b'''"AddonInfo"
{
	addonSteamAppID		550
	addontitle			"L4D2VR Test Room"
	addonversion		2
	addonauthor			"L4D2VR"
	addonDescription	"A campaign with one map: every weapon, melee weapon, grenade and item on tables, doors to open, no infected and no bots, to test the VR mod alone or with friends. Console: map l4d2vr_testroom"
	addonContent_Campaign	1
	addonContent_Map		1
	addonContent_Script		1
}
'''

# Added to the map's entities. Outputs in L4D2 entity lumps are separated by the ESC character.
SCRIPT_ENTITY = '''{
"origin" "10740 7870 -480"
"targetname" "l4d2vr_testroom"
"vscripts" "l4d2vr_testroom"
"classname" "logic_script"
}
{
"origin" "10740 7870 -470"
"targetname" "l4d2vr_testroom_refill"
"RefireTime" "0.5"
"UseRandomTime" "0"
"StartDisabled" "0"
"OnTimer" "l4d2vr_testroom\x1bRunScriptCode\x1bL4D2VR_Refill()\x1b0\x1b-1"
"classname" "logic_timer"
}
'''
PLACE_OUTPUT = '"OnGameplayStart" "l4d2vr_testroom\x1bRunScriptCode\x1bL4D2VR_TestRoom()\x1b1\x1b-1"\n'


def read(relative):
    with open(os.path.join(HERE, relative), 'rb') as f:
        return f.read()


def game_file(*parts):
    """The newest copy of a game file: the update folder overrides left4dead2"""
    for folder in ('update', 'left4dead2'):
        path = os.path.join(GAME, folder, *parts)
        if os.path.exists(path):
            return path
    raise FileNotFoundError(os.path.join(*parts))


def source_entities(bsp):
    """The entity lump of the source map, from the update's lump file (maps/<map>_l_0.lmp) when there is one"""
    lump = os.path.join(GAME, 'update', 'maps', SOURCE_MAP + '_l_0.lmp')
    if os.path.exists(lump):
        with open(lump, 'rb') as f:
            data = f.read()
        offset, lump_id, version, length, revision = struct.unpack_from('<5i', data)
        if lump_id == 0 and offset + length <= len(data):
            return data[offset:offset + length]
    version, offset, length, fourcc = struct.unpack_from('<iii4s', bsp, 8)
    return bsp[offset:offset + length]


def patch_entities(entities):
    text = entities.rstrip(b'\0').decode('latin1')
    director = re.search(r'\{[^{}]*"classname" "info_director"[^{}]*\}', text)
    if not director:
        raise RuntimeError('no info_director in ' + SOURCE_MAP)
    block = director.group(0)
    text = text.replace(block, block[:-1] + PLACE_OUTPUT + '}', 1)
    text = text.rstrip('\n') + '\n' + SCRIPT_ENTITY
    return text.encode('latin1') + b'\0'


def build_bsp():
    """The source map with the patched entity lump appended at the end of the file (L4D2 lumps: version, offset,
    length, fourCC). The other lumps don't move, so their offsets stay valid."""
    with open(game_file('maps', SOURCE_MAP + '.bsp'), 'rb') as f:
        bsp = bytearray(f.read())
    ident, version = struct.unpack_from('<4si', bsp)
    if ident != b'VBSP' or version != 21:
        raise RuntimeError('unexpected BSP %r %d' % (ident, version))
    entities = patch_entities(source_entities(bsp))
    bsp += b'\0' * (-len(bsp) % 4)
    struct.pack_into('<iii4s', bsp, 8, 0, len(bsp), len(entities), b'\0\0\0\0')
    bsp += entities
    return bytes(bsp)


def cubemaps(bsp):
    """The cubemap textures of the map's pakfile (lump 40, a zip), under the new map's name:
    {'materials/maps/l4d2vr_testroom/c1088_5344_-895.vtf': bytes, ...}"""
    version, offset, length, fourcc = struct.unpack_from('<iii4s', bsp, 8 + 16 * 40)
    source = 'materials/maps/%s/' % SOURCE_MAP
    found = {}
    with zipfile.ZipFile(io.BytesIO(bsp[offset:offset + length])) as pak:
        for name in pak.namelist():
            if name.lower().startswith(source) and name.lower().endswith('.vtf') and '/' not in name[len(source):]:
                found['materials/maps/%s/%s' % (MAP, name[len(source):])] = pak.read(name)
    if not any(name.endswith('/cubemapdefault.vtf') for name in found):
        raise RuntimeError('no cubemaps in the pakfile of ' + SOURCE_MAP)
    return found


def build_vpk(files):
    """VPK version 1 with the data in the same file. files: {'path/name.ext': bytes}"""
    tree = {}
    for full, data in files.items():
        directory, filename = os.path.split(full)
        name, extension = os.path.splitext(filename)
        tree.setdefault(extension[1:], {}).setdefault(directory or ' ', []).append((name, data))

    entries = b''
    blobs = []
    offset = 0
    for extension in sorted(tree):
        entries += extension.encode() + b'\0'
        for directory in sorted(tree[extension]):
            entries += directory.encode() + b'\0'
            for name, data in tree[extension][directory]:
                entries += name.encode() + b'\0'
                # crc, preload bytes, archive (0x7fff: this file), offset after the tree, length, terminator
                entries += struct.pack('<IHHIIH', zlib.crc32(data) & 0xFFFFFFFF, 0, 0x7FFF, offset, len(data), 0xFFFF)
                blobs.append(data)
                offset += len(data)
            entries += b'\0'
        entries += b'\0'
    entries += b'\0'
    return struct.pack('<III', 0x55AA1234, 1, len(entries)) + entries + b''.join(blobs)


def set_in_addon_list(game_dir, vpk_name, enabled):
    """The game only mounts addons that are on in addonlist.txt. enabled None: remove the line."""
    path = os.path.join(game_dir, 'left4dead2', 'addonlist.txt')
    line = '\t"%s"\t\t"%d"\n' % (vpk_name, 1 if enabled else 0)
    if not os.path.exists(path):
        if enabled is None:
            return
        with open(path, 'w', newline='\r\n') as f:
            f.write('"AddonList"\n{\n' + line + '}\n')
        return
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    pattern = re.compile(r'[ \t]*"%s"[ \t]*"[01]"[ \t]*\r?\n?' % re.escape(vpk_name))
    if enabled is None:
        text = pattern.sub('', text)
    elif pattern.search(text):
        text = pattern.sub(lambda m: line, text, 1)
    else:
        end = text.rfind('}')
        text = text[:end] + line + text[end:]
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def build_nav(bsp_size):
    """The source map's navigation mesh, with the BSP size in its header updated: the game checks it to tell whether
    the mesh was built for this BSP"""
    with open(game_file('maps', SOURCE_MAP + '.nav'), 'rb') as f:
        nav = bytearray(f.read())
    magic, version, subversion = struct.unpack_from('<III', nav)
    if magic != 0xFEEDFACE or version < 10:
        raise RuntimeError('unexpected nav %x %d' % (magic, version))
    struct.pack_into('<I', nav, 12, bsp_size)
    return bytes(nav)


if __name__ == '__main__':
    bsp = build_bsp()
    files = {
        'addoninfo.txt': ADDON_INFO,
        'missions/l4d2vr_testroom.txt': read('missions/l4d2vr_testroom.txt'),
        'maps/%s.bsp' % MAP: bsp,
        'maps/%s.nav' % MAP: build_nav(len(bsp)),
        'scripts/vscripts/l4d2vr_testroom.nut': read('scripts/vscripts/l4d2vr_testroom.nut'),
    }
    files.update(cubemaps(bsp))
    addons = os.path.join(GAME, 'left4dead2', 'addons')
    out = os.path.join(addons, VPK_NAME)
    with open(out, 'wb') as f:
        f.write(build_vpk(files))
    set_in_addon_list(GAME, VPK_NAME, True)

    old = os.path.join(addons, OLD_VPK_NAME)
    if os.path.exists(old):
        os.remove(old)
        print('removed', old)
    set_in_addon_list(GAME, OLD_VPK_NAME, None)
    print('installed', out, os.path.getsize(out), 'bytes')
