#!/usr/bin/env python3

import argparse
import csv
import shutil
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Collect valid SPEC06 instruction-frequency CSVs and build an aggregate matrix.")
    parser.add_argument(
        "--summary",
        type=Path,
        default=Path("/nfs/home/huxuan/repos/qemu/.copilot/output/spec06_merged_effective_summary.tsv"),
        help="Path to the merged effective summary TSV.",
    )
    parser.add_argument(
        "--collect-dir",
        type=Path,
        default=Path("/nfs/home/huxuan/repos/qemu/.copilot/output/spec06_valid_csvs"),
        help="Directory to store the collected valid CSV files.",
    )
    parser.add_argument(
        "--aggregate-csv",
        type=Path,
        default=Path("/nfs/home/huxuan/repos/qemu/.copilot/output/spec06_valid_insn_matrix.csv"),
        help="Output CSV path for the aggregated instruction-frequency matrix.",
    )
    return parser.parse_args()


def resolve_csv_path(row, output_root: Path) -> Path:
    csv_path = row["csv_path"].strip()
    if csv_path:
        return Path(csv_path)

    benchmark = row["benchmark"]
    subtask = row["subtask"]
    source = row["source"]
    return output_root / source / f"{benchmark}_{subtask}_ref_insn_freq.csv"


def load_valid_rows(summary_path: Path):
    with summary_path.open("r", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))

    valid_rows = [row for row in rows if row["final_status"] == "OK"]
    return valid_rows


def load_counts(csv_path: Path):
    counts = {}
    with csv_path.open("r", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            counts[row["mnemonic"]] = row["count"]
    return counts


def main():
    args = parse_args()
    summary_path = args.summary.resolve()
    collect_dir = args.collect_dir.resolve()
    aggregate_csv = args.aggregate_csv.resolve()
    output_root = summary_path.parent

    valid_rows = load_valid_rows(summary_path)
    if len(valid_rows) != 56:
        raise SystemExit(f"expected 56 valid rows, got {len(valid_rows)}")

    collect_dir.mkdir(parents=True, exist_ok=True)
    aggregate_csv.parent.mkdir(parents=True, exist_ok=True)

    test_names = []
    test_counts = {}
    all_mnemonics = set()

    for row in valid_rows:
        benchmark = row["benchmark"]
        subtask = row["subtask"]
        test_name = f"{benchmark}/{subtask}"
        source_csv = resolve_csv_path(row, output_root)
        if not source_csv.exists():
            raise FileNotFoundError(f"missing CSV for {test_name}: {source_csv}")

        target_csv = collect_dir / source_csv.name
        shutil.copy2(source_csv, target_csv)

        counts = load_counts(source_csv)
        all_mnemonics.update(counts)
        test_names.append(test_name)
        test_counts[test_name] = counts

    with aggregate_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["mnemonic", *test_names])
        for mnemonic in sorted(all_mnemonics):
            writer.writerow([mnemonic, *[test_counts[test_name].get(mnemonic, "0") for test_name in test_names]])

    print(f"summary={summary_path}")
    print(f"collected_files={len(test_names)}")
    print(f"collect_dir={collect_dir}")
    print(f"aggregate_csv={aggregate_csv}")
    print(f"mnemonics={len(all_mnemonics)}")


if __name__ == "__main__":
    main()