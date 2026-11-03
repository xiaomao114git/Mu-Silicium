#!/usr/bin/env python3
"""Attach an XNUP payload to a Mu-Silicium boot image.

BootShim looks for an "XNUP" container right after the UEFI FD (still inside the
gzip-compressed kernel) and copies it to PAYLOAD_BASE, where the loader reads it.

The kernel is gzip(FD) followed by a complete DTB, so the gzip stream is located
through zlib's unused_data: that is exactly where the member ends.

Usage: attach_payload.py <Mu-ktm.img> <out.img> name=file [name=file ...] [--cmdline "..."]
"""
import gzip
import struct
import sys
import zlib

PAGE = 4096
XNUP_MAGIC = 0x50554E58  # "XNUP"


def gunzip_prefix(buf):
    """Return (payload_bytes, consumed) for the gzip member at the start of buf."""
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)  # 16 = gzip wrapper
    out = d.decompress(buf)
    out += d.flush()
    if not d.eof:
        raise ValueError("gzip stream is truncated")
    return out, len(buf) - len(d.unused_data)


def build_xnup(entries):
    """entries: list of (name, data). Returns the container bytes."""
    body = b""
    for name, data in entries:
        name_buf = name.encode("ascii")[:15].ljust(16, b"\0")
        body += name_buf + struct.pack("<II", len(data), 0)
        body += data + b"\0" * ((16 - len(data) % 16) % 16)
    body += b"\0" * 24          # an empty name ends the list
    return struct.pack("<II", XNUP_MAGIC, len(body)) + body


def main():
    argv = sys.argv[1:]
    cmdline = None
    flags = []
    if "--cmdline" in argv:
        i = argv.index("--cmdline")
        cmdline = argv[i + 1]
        del argv[i:i + 2]
    # --usb: make the loader stay resident and take commands over USB instead of
    # sending the phone straight back down. Needed whenever the loader is written to
    # a boot partition, where a reset would start it again in a loop.
    if "--usb" in argv:
        argv.remove("--usb")
        flags.append(("usb", b"1\0"))
    if len(argv) < 3:
        sys.exit(__doc__)

    img_path, out_path = argv[0], argv[1]
    items = []
    for spec in argv[2:]:
        if "=" not in spec:
            sys.exit("payload item must be name=file, got %r" % spec)
        name, path = spec.split("=", 1)
        items.append((name, open(path, "rb").read()))
    if cmdline is not None:
        items.append(("cmdline", cmdline.encode() + b"\0"))
    items.extend(flags)

    img = bytearray(open(img_path, "rb").read())
    if img[:8] != b"ANDROID!":
        sys.exit("not an android boot image")

    kernel_size = struct.unpack_from("<I", img, 8)[0]
    print("boot image : %d bytes  kernel=%d  v=%d"
          % (len(img), kernel_size, struct.unpack_from("<I", img, 40)[0]))

    kernel = bytes(img[PAGE:PAGE + kernel_size])
    fd, consumed = gunzip_prefix(kernel)
    tail = kernel[consumed:]
    print("kernel     : gzip %d bytes + tail %d bytes" % (consumed, len(tail)))
    print("FD         : %d bytes" % len(fd))
    if len(fd) < 1024 * 1024:
        sys.exit("FD is only %d bytes - the firmware volume was lost" % len(fd))

    for name, data in items:
        shown = (data.rstrip(b"\0").decode("utf-8", "replace")
                 if name == "cmdline" else "%d bytes" % len(data))
        print("payload    : %s = %s" % (name, shown))

    xnup = build_xnup(items)
    print("XNUP       : %d bytes" % len(xnup))

    # gzip, not raw zlib: the bootloader expects the same container it accepted before
    new_kernel = gzip.compress(fd + xnup, compresslevel=9) + tail
    print("new kernel : %d bytes" % len(new_kernel))

    pages = (len(new_kernel) + PAGE - 1) // PAGE
    out = bytearray(img[:PAGE])
    struct.pack_into("<I", out, 8, len(new_kernel))
    out += new_kernel + b"\0" * (pages * PAGE - len(new_kernel))
    out += img[PAGE + kernel_size:]
    open(out_path, "wb").write(out)
    print("written    : %s  %d bytes" % (out_path, len(out)))


if __name__ == "__main__":
    main()
