#!/usr/bin/env python3
"""Embed the existing four frontend files byte-for-byte; no framework/runtime."""
import pathlib
import sys

root, output = map(pathlib.Path, sys.argv[1:])
assets = [("index.html", "/", "text/html; charset=utf-8"),
          ("style.css", "/style.css", "text/css; charset=utf-8"),
          ("model.js", "/model.js", "application/javascript; charset=utf-8"),
          ("app.js", "/app.js", "application/javascript; charset=utf-8")]
with output.open("w") as out:
    out.write('#include "qdc.h"\n')
    for index, (name, _, _) in enumerate(assets):
        data = (root / name).read_bytes()
        out.write(f"static const unsigned char asset_{index}[] = {{\n")
        for start in range(0, len(data), 20):
            out.write(",".join(str(x) for x in data[start:start + 20]) + ",\n")
        out.write("};\n")
    out.write("const qdc_asset qdc_assets[] = {\n")
    for index, (_, path, mime) in enumerate(assets):
        out.write(f'{{"{path}","{mime}",asset_{index},sizeof(asset_{index})}},\n')
    out.write("};\nconst size_t qdc_asset_count = sizeof(qdc_assets)/sizeof(qdc_assets[0]);\n")
