#!/usr/bin/env python3
"""Turns the web client's files into C arrays.

Embedded rather than read from disk at run time: the server is compiled
into three emulators, and a file path that has to be right relative to
wherever somebody launched Cemu from is a support question waiting to
happen. The files are small, and this way there is nothing to install.

Three of them now rather than one. The page used to be a single
index.html with ten kilobytes of stylesheet and fifty of script inside
it, which meant four kilobytes of actual markup buried in the middle of
a file nothing could search, highlight or lint properly. Splitting them
costs two more requests on a link that is already carrying video.

    embed_page.py web/index.html web/app.css web/app.js web_page.h
"""
import os
import sys

srcs, dst = sys.argv[1:-1], sys.argv[-1]

def symbol(path):
    return "BS_WEB_" + os.path.basename(path).replace(".", "_").upper()

TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
}

with open(dst, "w") as out:
    out.write("/* Generated from %s -- do not edit. */\n"
              % ", ".join(os.path.basename(s) for s in srcs))
    out.write("#ifndef BOTTOM_SCREEN_WEB_PAGE_H\n"
              "#define BOTTOM_SCREEN_WEB_PAGE_H\n\n")
    out.write("#include <stddef.h>\n\n")
    for src in srcs:
        with open(src, "rb") as f:
            data = f.read()
        name = symbol(src)
        out.write("static const unsigned char %s[] = {\n" % name)
        for i in range(0, len(data), 16):
            row = ", ".join("0x%02x" % b for b in data[i:i + 16])
            out.write("    %s,\n" % row)
        out.write("};\n")
        out.write("#define %s_LEN ((size_t)sizeof(%s))\n\n" % (name, name))

    # A table rather than a chain of comparisons in the server, so that
    # adding a file to the page is one line in the Makefile and nothing
    # else. The paths are what a browser asks for.
    out.write("typedef struct {\n"
              "    const char *path;\n"
              "    const char *type;\n"
              "    const unsigned char *body;\n"
              "    size_t len;\n"
              "} BsWebFile;\n\n")
    out.write("static const BsWebFile BS_WEB_FILES[] = {\n")
    for src in srcs:
        base = os.path.basename(src)
        name = symbol(src)
        path = "/" if base == "index.html" else "/" + base
        ext = os.path.splitext(base)[1]
        out.write('    { "%s", "%s", %s, %s_LEN },\n'
                  % (path, TYPES.get(ext, "application/octet-stream"), name, name))
    out.write("};\n")
    out.write("#define BS_WEB_FILE_COUNT "
              "((size_t)(sizeof(BS_WEB_FILES) / sizeof(BS_WEB_FILES[0])))\n\n")
    out.write("#endif\n")
