#!/usr/bin/env python3
"""sgx_link.py が作った .sgx（SGX の像）を検査する（2026-11-17）。

使い方:
    python3 scripts/check_sgx.py build/sgx/doom.sgx
    python3 scripts/check_sgx.py --name Doom --flags 1 --abi-major 5 build/sgx/doom.sgx

見るもの: 見出しの印（SGX1）・ABI・像の大きさ・入口・再配置の表。ファームの check_header と
同じ決まり（sgx_api.h・src/sgx/sgx.c）を、ビルドのあとに確かめるためのもの。
"""
import argparse
import struct
import sys

SGX_MAGIC = 0x31584753  # "SGX1"
HEADER = struct.Struct("<IHHIIIIIII32sIIII")  # sgx_header_t（84 バイト）
HEADER_SIZE = HEADER.size
MAX_LOAD = 8 * 1024 * 1024  # ファームの SGX_LOAD_MAX の目安


def fail(msg):
    print(f"check_sgx: {msg}", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description="check an SGX image")
    ap.add_argument("image")
    ap.add_argument("--name", help="expected program name in the header")
    ap.add_argument("--flags", type=lambda s: int(s, 0), help="expected SGX_F_* flags")
    ap.add_argument("--abi-major", type=int, help="expected SGX ABI major")
    args = ap.parse_args()

    try:
        data = open(args.image, "rb").read()
    except OSError as e:
        return fail(f"cannot read {args.image}: {e}")

    if len(data) < HEADER_SIZE:
        return fail(f"{args.image}: too short for an SGX header ({len(data)} bytes)")
    (magic, abi_major, abi_minor, header_size, link_base, image_size, bss_size, entry,
     stack_size, flags, name, reloc_count, _r0, _r1, _r2) = HEADER.unpack_from(data)

    if magic != SGX_MAGIC:
        return fail(f"bad magic 0x{magic:08x} (expected 0x{SGX_MAGIC:08x})")
    if header_size < HEADER_SIZE or header_size > image_size:
        return fail(f"bad header_size {header_size}")
    if link_base != 0:
        return fail(f"bad link_base 0x{link_base:x} (expected 0)")
    if image_size < HEADER_SIZE or image_size + bss_size > MAX_LOAD:
        return fail(f"bad image_size {image_size} / bss_size {bss_size}")
    if len(data) != image_size + reloc_count * 4:
        return fail(f"file size {len(data)} != image {image_size} + relocations {reloc_count} * 4")
    if entry < header_size or entry >= image_size or not (entry & 1):
        return fail(f"bad entry 0x{entry:x}")

    if args.abi_major is not None and abi_major != args.abi_major:
        return fail(f"ABI major is {abi_major}, expected {args.abi_major}")
    if args.flags is not None and flags != args.flags:
        return fail(f"flags are 0x{flags:x}, expected 0x{args.flags:x}")

    try:
        name_str = name.split(b"\0", 1)[0].decode("utf-8")
    except UnicodeDecodeError:
        return fail("the name in the header is not UTF-8")
    if args.name is not None and name_str != args.name:
        return fail(f"name is {name_str!r}, expected {args.name!r}")

    rel = struct.unpack_from(f"<{reloc_count}I", data, image_size) if reloc_count else ()
    last = -1
    for i, off in enumerate(rel):
        if off & 3 or off + 4 > image_size:
            return fail(f"bad relocation #{i}: 0x{off:x}")
        if off < last:
            return fail(f"relocations are not sorted at #{i}: 0x{off:x}")
        last = off

    print(f"check_sgx: {args.image}: OK")
    print(f"  name={name_str!r} abi={abi_major}.{abi_minor} image={image_size} B "
          f"bss={bss_size} B stack={stack_size} B flags=0x{flags:x}")
    print(f"  entry=0x{entry:x} relocations={reloc_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
