#!/usr/bin/env python3
"""Convert a binary file to a const uint8_t C translation unit."""
import argparse
import pathlib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("symbol")
    args = parser.parse_args()
    data = pathlib.Path(args.input).read_bytes()
    values = ["0x%02xu" % byte for byte in data]
    lines = ["#include <stddef.h>", "#include <stdint.h>", "",
             "const uint8_t %s[] = {" % args.symbol]
    for start in range(0, len(values), 12):
        lines.append("    " + ", ".join(values[start:start + 12]) + ",")
    lines.extend(["};", "const size_t %s_len = sizeof(%s);" %
                  (args.symbol, args.symbol), ""])
    pathlib.Path(args.output).write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
