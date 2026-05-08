#!/usr/bin/env python3

import argparse
import shutil
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="为 SPEC CPU2006 482.sphinx3 生成 ctlfile 和标准 .raw 输入文件"
    )
    parser.add_argument(
        "--src-dir",
        type=Path,
        required=True,
        help="源目录，包含 *.le.raw 或 *.be.raw 文件",
    )
    parser.add_argument(
        "--dst-dir",
        type=Path,
        required=True,
        help="目标目录，生成 ctlfile 与 basename.raw",
    )
    parser.add_argument(
        "--endian",
        choices=("auto", "le", "be"),
        default="auto",
        help="选择使用的小端或大端原始数据，默认按当前主机字节序自动判断",
    )
    parser.add_argument(
        "--ctl-name",
        default="ctlfile",
        help="生成的控制文件名，默认 ctlfile",
    )
    return parser.parse_args()


def resolve_endian(mode: str) -> str:
    if mode != "auto":
        return mode
    return "le" if sys.byteorder == "little" else "be"


def collect_sources(src_dir: Path, endian: str) -> list[Path]:
    pattern = f"*.{endian}.raw"
    sources = sorted(src_dir.glob(pattern))
    if not sources:
        raise FileNotFoundError(f"在 {src_dir} 下未找到 {pattern} 文件")
    return sources


def output_name(src: Path, endian: str) -> str:
    suffix = f".{endian}.raw"
    if not src.name.endswith(suffix):
        raise ValueError(f"文件名不符合预期: {src.name}")
    return src.name[: -len(suffix)] + ".raw"


def main() -> int:
    args = parse_args()
    src_dir = args.src_dir.resolve()
    dst_dir = args.dst_dir.resolve()
    endian = resolve_endian(args.endian)

    if not src_dir.is_dir():
        raise NotADirectoryError(f"源目录不存在: {src_dir}")

    dst_dir.mkdir(parents=True, exist_ok=True)
    sources = collect_sources(src_dir, endian)
    ctl_path = dst_dir / args.ctl_name

    with ctl_path.open("w", encoding="utf-8") as ctl_file:
        for src in sources:
            dest_name = output_name(src, endian)
            dest_path = dst_dir / dest_name
            shutil.copyfile(src, dest_path)
            raw_size = src.stat().st_size
            ctl_entry = dest_name[:-4]
            ctl_file.write(f"{ctl_entry} {raw_size}\n")

    print(f"endian={endian}")
    print(f"src_dir={src_dir}")
    print(f"dst_dir={dst_dir}")
    print(f"ctlfile={ctl_path}")
    print(f"generated={len(sources)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())