#!/usr/bin/env python3
"""Lists actor task files (task/alltask, asset 0x8A).

    tools/task.py list                      names and sizes
    tools/task.py dump shop.bin             the commands of one task
    tools/task.py extract shop.bin -o f     the unpacked file

A task file is: u32 (unused), then 11 programs.  A program is a count
followed by its commands, numbered from 0:
    index, opcode, float seconds (0 = one frame, -1 = until done),
    who1, who2, text length, text bytes (16-bit characters, padded to 4),
    5 x {type, value}   type 1: value picks one of the task's arguments
Program 0 runs first; the others are entered by events (see the project notes).
"""
import struct, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, 'extracted', 'SLUS_208.93')
NAME_TABLE = 0x2D7D10

OPS = {
    0x00: 'Jump', 0x02: 'PlayMotion', 0x03: 'SetAI', 0x04: 'PlaySE', 0x05: 'PlayVoice',
    0x06: 'SetGlobal', 0x01: 'Wait', 0x08: 'FindTargetA', 0x09: 'FindTargetB', 0x0A: 'LostTargetA', 0x0B: 'LostTargetB',
    0x0D: 'If', 0x0E: 'SetLocal', 0x10: 'DrawSerif', 0x11: 'Return', 0x13: 'SetPos', 0x14: 'SetAngle',
    0x15: 'SetRange15', 0x16: 'SetRange16', 0x17: 'SetList17', 0x18: 'SubGlobal', 0x19: 'AddGlobal',
    0x1A: 'SetChara', 0x1C: 'SubChara', 0x1D: 'AddChara', 0x1E: 'SetRange1E', 0x1F: 'SetRange1F',
    0x20: 'SetList20', 0x21: 'SetList21', 0x22: 'SetList22', 0x23: 'SetDamageTargetID',
    0x24: 'DrawTalkSelect', 0x25: 'MapOut', 0x26: 'SetBattleMode', 0x27: 'IfTarget',
    0x28: 'SetTalkCameraOff', 0x2A: 'AddLocal', 0x2B: 'SubLocal', 0x2C: 'CheckTalkStart',
    0x2D: 'DrawInfoPlate', 0x2E: 'SetList2E', 0x2F: 'SetRange2F', 0x30: 'SetRoomIn',
    0x31: 'SetPartnerID', 0x32: 'CheckTargetLife', 0x33: 'CheckCollisionTarget', 0x34: 'SetWeaponOff', 0x35: 'SetDirection',
    0x36: 'SetDayFlag', 0x38: 'SubDayFlag', 0x39: 'AddDayFlag', 0x3A: 'CheckPriorityTarget',
    0x3B: 'SetPrision', 0x3C: 'ChrReserve', 0x3E: 'SetFlag', 0x3F: 'SetWorkMessage',
    0x40: 'SetTaskA', 0x43: 'StopActionCount', 0x44: 'SetTaskB', 0x46: 'SetWorkName',
    0x47: 'CheckItem', 0x48: 'Watch48', 0x49: 'DropItem', 0x4C: 'DeleteWorkItem',
    0x4D: 'StopPriorityTask', 0x4E: 'CheckTargetChange', 0x4F: 'SetItem', 0x50: 'SetItemPos',
    0x51: 'AddFlag', 0x52: 'SubFlag', 0x53: 'SetDamageStopFlag', 0x55: 'SetCounter',
    0x56: 'AddCounter', 0x57: 'SetWorkOut', 0x58: 'SetOtherMessage', 0x5A: 'CheckCatchTarget',
    0x5B: 'DeleteNormalItem',
}
WHO = {0: 'player', 1: 'self', 2: 'target', 6: 'partner', 7: 'other', -1: '-'}


def names():
    elf = open(ELF, 'rb').read()
    out, a = [], NAME_TABLE
    while True:
        p, = struct.unpack_from('<I', elf, a - 0x100000 + 0x80)
        if not p:
            return out
        o = p - 0x100000 + 0x80
        out.append(elf[o:elf.index(b'\0', o)].decode())
        a += 4


def lzss(src):
    packed, size = struct.unpack_from('<2I', src, 0)
    out, win, w, p, flags = bytearray(), bytearray(0x1000), 0xFEE, 8, 0
    while len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            flags = src[p] | 0xFF00
            p += 1
        if flags & 1:
            b = src[p]; p += 1
            out.append(b); win[w] = b; w = (w + 1) & 0xFFF
        else:
            lo, hi = src[p], src[p + 1]; p += 2
            pos = lo | ((hi & 0xF0) << 4)
            for k in range((hi & 0xF) + 3):
                b = win[(pos + k) & 0xFFF]
                out.append(b); win[w] = b; w = (w + 1) & 0xFFF
    return bytes(out[:size])


def archive():
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import volume
    return volume.read_asset(0x8A) if hasattr(volume, 'read_asset') else None


def load_all(blob):
    count, align, start, _ = struct.unpack_from('<4I', blob, 0)
    files, off = {}, 0
    for i, name in enumerate(names()):
        size, = struct.unpack_from('<I', blob, 0x10 + 4 * i)
        files[name.split('/')[-1]] = (start + off, size)
        off += ~(align - 1) & (align - 1 + size)
    return files


def parse(data):
    programs, o = [], 4
    for level in range(11):
        count, = struct.unpack_from('<i', data, o); o += 4
        cmds = []
        while count > 0:
            index, op = struct.unpack_from('<2i', data, o)
            seconds, = struct.unpack_from('<f', data, o + 8)
            who1, who2, tlen = struct.unpack_from('<3i', data, o + 12)
            o += 24
            text = ''
            if tlen > 0:
                text = data[o:o + tlen].decode('utf-16le', 'replace').split('\0')[0]
                o += tlen & ~3
            args = []
            for k in range(5):
                t, v = struct.unpack_from('<2i', data, o); o += 8
                args.append(('$%d' % v) if t == 1 else v)
            cmds.append((index, op, seconds, who1, who2, text, args))
            if index == count - 1:
                break
        programs.append(cmds)
    return programs


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    blob_path = os.environ.get('WOTS2_ALLTASK')
    if blob_path:
        blob = open(blob_path, 'rb').read()
    else:
        import subprocess, tempfile
        tmp = tempfile.NamedTemporaryFile(suffix='.bin', delete=False).name
        subprocess.run([os.path.join(ROOT, 'tools', 'volume.py'), 'extract', '0x8a', '-o', tmp],
                       check=True, stdout=subprocess.DEVNULL)
        blob = open(tmp, 'rb').read()
        os.unlink(tmp)
    files = load_all(blob)
    if sys.argv[1] == 'list':
        for name, (off, size) in files.items():
            print('%-36s %6d packed  %6d bytes' % (name, size, struct.unpack_from('<I', blob, off + 4)[0]))
        return
    name = sys.argv[2]
    off, size = files[name]
    data = lzss(blob[off:off + size + 16])
    if sys.argv[1] == 'extract':
        open(sys.argv[sys.argv.index('-o') + 1], 'wb').write(data)
        return
    for level, cmds in enumerate(parse(data)):
        if not cmds:
            continue
        print('program %d:' % level)
        for index, op, seconds, who1, who2, text, args in cmds:
            while args and args[-1] == -1:
                args.pop()
            wait = 'until done' if seconds == -1.0 else ('%gs' % seconds if seconds else '')
            who = ''
            if who1 != -1 or who2 != -1:
                who = ' [%s, %s]' % (WHO.get(who1, who1), WHO.get(who2, who2))
            print('  %3d  %-18s%s %s %s%s' % (index, OPS.get(op, 'op%02X' % op), who,
                  ' '.join(str(a) for a in args), ('"%s" ' % text) if text else '', wait))


if __name__ == '__main__':
    main()
