#!/usr/bin/env python3
"""Turns the web page into a C array.

Embedded rather than read from disk at run time: the server is compiled
into three emulators, and a file path that has to be right relative to
wherever somebody launched Cemu from is a support question waiting to
happen. The page is small, and this way there is nothing to install.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()

with open(dst, "w") as out:
    out.write("/* Generated from %s -- do not edit. */\n" % src)
    out.write("#ifndef BOTTOM_SCREEN_WEB_PAGE_H\n#define BOTTOM_SCREEN_WEB_PAGE_H\n\n")
    out.write("#include <stddef.h>\n\n")
    out.write("static const unsigned char BS_WEB_PAGE[] = {\n")
    for i in range(0, len(data), 16):
        row = ", ".join("0x%02x" % b for b in data[i:i + 16])
        out.write("    %s,\n" % row)
    out.write("};\n\n")
    out.write("#define BS_WEB_PAGE_LEN ((size_t)sizeof(BS_WEB_PAGE))\n\n")
    out.write("#endif\n")
