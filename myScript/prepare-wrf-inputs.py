#!/usr/bin/env python3

import argparse
import sys
from pathlib import Path
import shutil


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="为 SPEC CPU2006 481.wrf 准备运行目录中的分层输入文件"
    )
    parser.add_argument(
        "--run-dir",
        type=Path,
        required=True,
        help="WRF 运行目录，内部应包含 be/ 或 le/ 子目录",
    )
    parser.add_argument(
        "--endian",
        choices=("auto", "le", "be"),
        default="auto",
        help="选择输入端序，默认按当前主机字节序自动判断",
    )
    parser.add_argument(
        "--header-size",
        default="auto",
        help="选择头大小子目录，支持 auto/32/64，默认 auto",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="覆盖已存在的目标文件",
    )
    return parser.parse_args()


def resolve_endian(mode: str) -> str:
    if mode != "auto":
        return mode
    return "le" if sys.byteorder == "little" else "be"


def copy_tree_files(src_dir: Path, dst_dir: Path, overwrite: bool) -> int:
    if not src_dir.is_dir():
        return 0

    copied = 0
    for entry in sorted(src_dir.iterdir()):
        if not entry.is_file():
            continue
        dst = dst_dir / entry.name
        if dst.exists() and not overwrite:
            continue
        shutil.copyfile(entry, dst)
        copied += 1
    return copied


def resolve_header_sizes(value: str) -> tuple[list[int], bool]:
    if value == "auto":
        # 当前 riscv WRF 运行验证表明 32-bit record marker 版本可正确继续执行，
        # 因此自动模式优先尝试 32，再回退到 64。
        return [32, 64], True
    return [int(value)], False


def main() -> int:
    args = parse_args()
    run_dir = args.run_dir.resolve()
    endian = resolve_endian(args.endian)
    header_sizes, stop_after_first_match = resolve_header_sizes(args.header_size)

    if not run_dir.is_dir():
        raise NotADirectoryError(f"运行目录不存在: {run_dir}")

    copied = 0
    copied += copy_tree_files(run_dir / endian, run_dir, args.overwrite)
    for header_size in header_sizes:
        copied_now = copy_tree_files(run_dir / endian / str(header_size), run_dir, args.overwrite)
        copied += copied_now
        if stop_after_first_match and copied_now:
            break

    print(f"run_dir={run_dir}")
    print(f"endian={endian}")
    print(f"header_sizes={','.join(str(size) for size in header_sizes)}")
    print(f"copied={copied}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())