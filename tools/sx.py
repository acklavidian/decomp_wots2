#!/usr/bin/env python3
"""Disassemble a compiled event script (script/<name>.sx, magic TH00).

    tools/sx.py dis  <file.sx | script name>      # byte code, nested by function
    tools/sx.py src  <script name>                # the script's source (.sol) as UTF-8
    tools/sx.py list                              # script/scriptlist

A script name (e.g. Shop/ootemon/yadoya) is read from extracted/VOLUME.DAT.
See "Event scripts" in the project notes for the format and what each op does.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def read_script(name: str, ext: str) -> bytes:
    if os.path.isfile(name):
        return open(name, "rb").read()
    import volume
    vol = volume.Volume(os.path.join(HERE, "..", "extracted", "VOLUME.DAT"))
    return vol.read_path(f"script/{name}{ext}")


OPS = {
    0x00: ("end", ""), 0x54: ("nop", ""), 0x20: ("obj", ""), 0x21: ("int", "i"), 0x22: ("wstring", "w"),
    0x23: ("def", "d"), 0x24: ("float", "f"), 0x25: ("list", "i"), 0x30: ("getroot", ""),
    0x31: ("getparent", ""), 0x32: ("getlocal", ""), 0x33: ("getargv", "h"), 0x34: ("getargs", ""),
    0x35: ("getresult", ""), 0x36: ("popresult", ""), 0x37: ("popstack", ""), 0x38: ("getattr", "n"),
    0x39: ("setattr", "n"), 0x50: ("sendmsg", "n"), 0x51: ("operate", ""), 0x52: ("new", ""),
    0x53: ("exit", "h"),
}


def name_at(code: bytes, pc: int):
    """A name operand: s16 length + bytes, or a negative offset back to an earlier one."""
    n, = struct.unpack_from("<h", code, pc)
    pc += 2
    if n < 0:
        at = pc + n
        m, = struct.unpack_from("<h", code, at - 2)
        return code[at:at + m].decode("latin1"), pc
    return code[pc:pc + n].decode("latin1"), pc + n


def disassemble(code: bytes, start: int, end: int, depth: int, out):
    pc = start
    pad = "    " * depth
    while pc < end:
        at = pc
        op = code[pc]
        pc += 1
        if op not in OPS:
            out.append(f"{at:05x} {pad}?? {op:#04x}")
            continue
        name, kind = OPS[op]
        if kind == "":
            out.append(f"{at:05x} {pad}{name}")
        elif kind == "i":
            v, = struct.unpack_from("<i", code, pc); pc += 4
            out.append(f"{at:05x} {pad}{name} {v}")
        elif kind == "h":
            v, = struct.unpack_from("<h", code, pc); pc += 2
            out.append(f"{at:05x} {pad}{name} {v}")
        elif kind == "f":
            v, = struct.unpack_from("<f", code, pc); pc += 4
            out.append(f"{at:05x} {pad}{name} {v:g}")
        elif kind == "n":
            s, pc = name_at(code, pc)
            out.append(f"{at:05x} {pad}{name} {s}")
        elif kind == "w":
            n, = struct.unpack_from("<h", code, pc); pc += 2
            s = code[pc:pc + 2 * n].decode("utf-16-le", "replace"); pc += 2 * n
            out.append(f"{at:05x} {pad}{name} {s!r}")
        elif kind == "d":
            n, = struct.unpack_from("<i", code, pc); pc += 4
            out.append(f"{at:05x} {pad}def {{   ; {n} bytes")
            disassemble(code, pc, pc + n, depth + 1, out)
            out.append(f"      {pad}}}")
            pc += n


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("dis", "src", "list"):
        sys.exit(__doc__)
    if sys.argv[1] == "list":
        import volume
        vol = volume.Volume(os.path.join(HERE, "..", "extracted", "VOLUME.DAT"))
        data = vol.read_path("script/scriptlist")
        count, = struct.unpack_from("<I", data, 0)
        at = 4
        for _ in range(count):
            n, = struct.unpack_from("<I", data, at)
            print(data[at + 4:at + 4 + n].decode("latin1"))
            at += 4 + n
        return
    if sys.argv[1] == "src":
        sys.stdout.write(read_script(sys.argv[2], ".sol").decode("utf-16"))
        return
    data = read_script(sys.argv[2], ".sx")
    if data[:4] != b"TH00":
        sys.exit("not a TH00 script")
    word, size, size2, word2 = struct.unpack_from("<4I", data, 4)
    print(f"; TH00  {word:#x}  code {size} bytes  ({size2}, {word2:#x})")
    out = []
    disassemble(data[0x14:0x14 + size], 0, size, 0, out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
