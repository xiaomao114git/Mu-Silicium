#!/usr/bin/env python3
"""Attach an XNUP payload to a Mu-Silicium boot image.

BootShim looks for an "XNUP" container right after the UEFI FD (still inside the
gzip-compressed kernel) and copies it to PAYLOAD_BASE, where XnuLoader reads it.
Without it the handoff has nothing to start.

The kernel is gzip(FD) followed by a complete DTB, so the gzip stream has to be
located by bisection: gunzip the largest prefix that decompresses.

Usage: attach_payload.py <Mu-ktm.img> <loader.efi> <out.img>
"""
import gzip
import struct
import sys
import zlib

PAGE = 4096
XNUP_MAGIC = 0x50554E58  # "XNUP"


def gunzip_prefix(buf):
    """Return (payload_bytes, consumed) for the gzip stream at the start of buf.

    zlib's decompressobj reports exactly where the gzip member ends via
    unused_data, which is what separates the compressed FD from the DTB that
    follows it. A bisection over gzip.decompress() guesses at this and can land on
    a short prefix, silently dropping the firmware volume.
    """
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)  # 16 = gzip wrapper
    out = d.decompress(buf)
    out += d.flush()
    if not d.eof:
        raise ValueError("gzip stream is truncated")
    consumed = len(buf) - len(d.unused_data)
    return out, consumed


def build_xnup(entries):
    """entries: list of (name, data). Returns the container bytes."""
    body = b""
    for name, data in entries:
        name_buf = name.encode("ascii")[:15].ljust(16, b"\0")
        head = name_buf + struct.pack("<II", len(data), 0)
        pad = (16 - (len(data) % 16)) % 16
        body += head + data + b"\0" * pad
    return struct.pack("<II", XNUP_MAGIC, len(body)) + body


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: attach_payload.py <Mu-ktm.img> <loader.efi> <out.img>")
    img_path, loader_path, out_path = sys.argv[1:]

    img = bytearray(open(img_path, "rb").read())
    if img[:8] != b"ANDROID!":
        sys.exit("not an android boot image")

    kernel_size = struct.unpack_from("<I", img, 8)[0]
    header_version = struct.unpack_from("<I", img, 40)[0]
    print(f"boot image : {len(img)} bytes  kernel={kernel_size}  v={header_version}")

    kernel = bytes(img[PAGE:PAGE + kernel_size])
    fd, consumed = gunzip_prefix(kernel)
    tail = kernel[consumed:]
    print(f"kernel     : gzip {consumed} bytes + tail {len(tail)} bytes")
    print(f"FD         : {len(fd)} bytes")

    loader = open(loader_path, "rb").read()
    xnup = build_xnup([("loader.efi", loader)])
    print(f"XNUP       : {len(xnup)} bytes (loader {len(loader)})")

    new_kernel = gzip.compress(fd + xnup, compresslevel=9) + tail
    print(f"new kernel : {len(new_kernel)} bytes")

    pages = (len(new_kernel) + PAGE - 1) // PAGE
    padded = new_kernel + b"\0" * (pages * PAGE - len(new_kernel))

    out = bytearray(img[:PAGE])
    struct.pack_into("<I", out, 8, len(new_kernel))
    out += padded
    out += img[PAGE + kernel_size:]

    open(out_path, "wb").write(out)
    print(f"written    : {out_path}  {len(out)} bytes")


if __name__ == "__main__":
    main()
