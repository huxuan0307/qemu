#!/usr/bin/env python3

import argparse
import concurrent.futures
import os
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import yaml


REDIRECTION_BENCHES = {
    ("gamess", None),
    ("gobmk", None),
    ("leslie3d", "default"),
    ("milc", "default"),
    ("xalancbmk", "default"),
}


@dataclass(frozen=True)
class Task:
    benchmark: str
    subtask: str
    command: str
    cwd: Path
    csv_path: Path
    log_path: Path


def prepare_task_inputs(task: Task):
    if task.benchmark != "wrf":
        return

    script = Path(__file__).resolve().parent / "prepare-wrf-inputs.py"
    subprocess.run(
        [sys.executable, str(script), "--run-dir", str(task.cwd), "--overwrite"],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def needs_shell(benchmark: str, subtask: str) -> bool:
    return (benchmark, None) in REDIRECTION_BENCHES or (benchmark, subtask) in REDIRECTION_BENCHES


def load_tasks(commands_yaml: Path, spec_root: Path, out_dir: Path):
    with commands_yaml.open("r", encoding="utf-8") as handle:
        data = yaml.safe_load(handle)

    task0 = data["task0_commands"]
    tasks = []
    for benchmark, subtasks in task0.items():
        cwd = spec_root / "cmd" / benchmark
        for subtask, entry in subtasks.items():
            subtask_name = str(subtask)
            csv_name = f"{benchmark}_{subtask}_ref_insn_freq.csv"
            log_name = f"{benchmark}_{subtask_name}.log"
            tasks.append(Task(
                benchmark=benchmark,
                subtask=subtask_name,
                command=entry["command"],
                cwd=cwd,
                csv_path=out_dir / csv_name,
                log_path=out_dir / log_name,
            ))
    return tasks


def parse_selected_tasks(values):
    selected = set()

    for value in values:
        if "/" not in value:
            raise ValueError(f"invalid task selector: {value}")
        benchmark, subtask = value.split("/", 1)
        selected.add((benchmark, subtask))

    return selected


def run_one(task: Task, qemu: Path, plugin: Path):
    plugin_spec = f"{plugin},outfile={task.csv_path},sort=true"
    start = time.time()

    task.csv_path.parent.mkdir(parents=True, exist_ok=True)
    prepare_task_inputs(task)

    with task.log_path.open("w", encoding="utf-8") as log_handle:
        log_handle.write(f"[task] {task.benchmark}/{task.subtask}\n")
        log_handle.write(f"[cwd] {task.cwd}\n")
        log_handle.write(f"[command] {task.command}\n")
        log_handle.write(f"[csv] {task.csv_path}\n")

        if needs_shell(task.benchmark, task.subtask):
            env = os.environ.copy()
            env["QEMU_BIN"] = str(qemu)
            env["PLUGIN_SPEC"] = plugin_spec
            wrapped = f'exec "$QEMU_BIN" -d plugin -plugin "$PLUGIN_SPEC" {task.command}'
            completed = subprocess.run(
                ["bash", "-lc", wrapped],
                cwd=task.cwd,
                stdout=log_handle,
                stderr=subprocess.STDOUT,
                env=env,
            )
        else:
            argv = shlex.split(task.command)
            completed = subprocess.run(
                [str(qemu), "-d", "plugin", "-plugin", plugin_spec, *argv],
                cwd=task.cwd,
                stdout=log_handle,
                stderr=subprocess.STDOUT,
            )

    elapsed = time.time() - start
    csv_ok = task.csv_path.exists() and task.csv_path.stat().st_size > 0
    return {
        "benchmark": task.benchmark,
        "subtask": task.subtask,
        "returncode": completed.returncode,
        "csv_ok": csv_ok,
        "csv_path": str(task.csv_path),
        "log_path": str(task.log_path),
        "elapsed_sec": round(elapsed, 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Batch run SPEC CPU2006 commands with riscv_insn_freq plugin")
    parser.add_argument("--commands-yaml", required=True)
    parser.add_argument("--spec-root", required=True)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--jobs", type=int, default=16)
    parser.add_argument("--task", action="append", default=[],
                        help="only run the selected benchmark/subtask, e.g. gobmk/13x13")
    args = parser.parse_args()

    commands_yaml = Path(args.commands_yaml).resolve()
    spec_root = Path(args.spec_root).resolve()
    qemu = Path(args.qemu).resolve()
    plugin = Path(args.plugin).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    tasks = load_tasks(commands_yaml, spec_root, out_dir)
    if args.task:
        selected = parse_selected_tasks(args.task)
        tasks = [task for task in tasks if (task.benchmark, task.subtask) in selected]
    summary_path = out_dir / "summary.tsv"

    print(f"loaded_tasks={len(tasks)}")
    print(f"out_dir={out_dir}")
    sys.stdout.flush()

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_map = {executor.submit(run_one, task, qemu, plugin): task for task in tasks}
        for future in concurrent.futures.as_completed(future_map):
            result = future.result()
            results.append(result)
            status = "OK" if result["returncode"] == 0 and result["csv_ok"] else "FAIL"
            print(f"[{status}] {result['benchmark']}/{result['subtask']} rc={result['returncode']} csv_ok={result['csv_ok']} elapsed={result['elapsed_sec']}s")
            sys.stdout.flush()

    results.sort(key=lambda item: (item["benchmark"], item["subtask"]))
    with summary_path.open("w", encoding="utf-8") as handle:
        handle.write("benchmark\tsubtask\treturncode\tcsv_ok\telapsed_sec\tcsv_path\tlog_path\n")
        for item in results:
            handle.write(
                f"{item['benchmark']}\t{item['subtask']}\t{item['returncode']}\t{int(item['csv_ok'])}\t{item['elapsed_sec']}\t{item['csv_path']}\t{item['log_path']}\n"
            )

    success = [item for item in results if item["returncode"] == 0 and item["csv_ok"]]
    failed = [item for item in results if item["returncode"] != 0 or not item["csv_ok"]]
    print(f"success={len(success)}")
    print(f"failed={len(failed)}")
    print(f"summary={summary_path}")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())