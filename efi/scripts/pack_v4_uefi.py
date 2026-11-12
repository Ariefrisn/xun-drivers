import argparse
import gzip
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

UNPACK_BOOTIMG = Path(r"d:\Project\XUN\tools\mkbootimg\unpack_bootimg.py")
MKBOOTIMG = Path(r"d:\Project\XUN\tools\mkbootimg\mkbootimg.py")


def main():
    parser = argparse.ArgumentParser(description="Repack Mu header-v1 image into Android boot header-v4 for Xiaomi xun ABL.")
    parser.add_argument("input_img", type=Path, help="Input Mu-xun.img (boot header v1)")
    parser.add_argument("output_img", type=Path, help="Output Mu-xun-v4.img (boot header v4)")
    parser.add_argument("--os-version", default="13.0.0", help="Android OS version string (default: 13.0.0)")
    parser.add_argument("--os-patch-level", default="2025-01", help="Security patch level (default: 2025-01)")
    args = parser.parse_args()

    if not args.input_img.is_file():
        raise FileNotFoundError(f"Input image not found: {args.input_img}")

    with tempfile.TemporaryDirectory(prefix="mu_v4pack_") as td:
        tmp = Path(td)
        subprocess.run(
            [sys.executable, str(UNPACK_BOOTIMG), "--boot_img", str(args.input_img), "--out", str(tmp)],
            check=True,
        )

        raw_kernel = (tmp / "kernel").read_bytes()
        # In Mu-Silicium header-v1, kernel = gzip(BootShim + FD) + optional appended DTB.
        # gzip.decompress stops cleanly at EOF if no trailing data; with trailing DTB,
        # zlib.decompressobj reads only the gzip member and leaves trailing DTB unconsumed.
        import zlib
        decomp = zlib.decompressobj(wbits=16 + zlib.MAX_WBITS)
        image_bytes = bytearray(decomp.decompress(raw_kernel))
        image_bytes.extend(decomp.flush())

        pad_len = (len(image_bytes) + 0xFFF) & ~0xFFF
        if len(image_bytes) < pad_len:
            image_bytes.extend(b"\x00" * (pad_len - len(image_bytes)))

        if len(image_bytes) < 64 or image_bytes[56:60] != b"ARMd":
            raise ValueError(f"Invalid ARM64 Image magic at [56:60]: {bytes(image_bytes[56:60])!r}")

        # BootShim starts at offset 0 and is position-independent; text_offset = 0, flags = 0xA
        struct.pack_into("<Q", image_bytes, 8, 0)
        struct.pack_into("<Q", image_bytes, 16, len(image_bytes))
        struct.pack_into("<Q", image_bytes, 24, 0xA)

        out_kernel = tmp / "Image"
        out_kernel.write_bytes(image_bytes)

        subprocess.run(
            [
                sys.executable,
                str(MKBOOTIMG),
                "--header_version", "4",
                "--kernel", str(out_kernel),
                "--os_version", args.os_version,
                "--os_patch_level", args.os_patch_level,
                "-o", str(args.output_img),
            ],
            check=True,
        )

    size = args.output_img.stat().st_size
    print(f"Packed header-v4 image: {args.output_img} ({size} bytes, uncompressed Image={len(image_bytes)} bytes)")


if __name__ == "__main__":
    main()

