#!/usr/bin/env python3
import argparse
import collections
import os
from pathlib import Path

import yaml
from tabulate import tabulate


def existing_file(value: str) -> Path:
    """Validate that the provided value is an existing file path."""
    path = Path(value)
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"{value} is not a file")
    return path


def write_step_summary(msg: str) -> None:
    """Write to stdout, and to GITHUB_STEP_SUMMARY if available."""
    print(msg)

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return

    with open(summary_path, "a") as f:
        f.write(f"{msg}\n")


def sumarize_counts(output_yml_path: Path) -> list[tuple[str, int]]:
    """Summarizes the counts of clang-tidy diagnostics from a YAML file."""
    if not output_yml_path.exists():
        raise FileNotFoundError(f"{output_yml_path} not found. Please run clang-tidy first.")

    with output_yml_path.open() as f:
        data = yaml.safe_load(f)

    diags = data.get("Diagnostics", []) or []
    c = collections.Counter(d["DiagnosticName"] for d in diags)
    total = sum(c.values())
    table = [(name, n) for name, n in c.most_common()]
    table.append(("TOTAL", total))
    return table


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Summarize clang-tidy diagnostic counts from a YAML export-fixes file."
    )
    parser.add_argument("output_yml_path", type=existing_file, help="Path to the clang-tidy --export-fixes YAML file")
    args = parser.parse_args()

    table = sumarize_counts(output_yml_path=args.output_yml_path)
    write_step_summary(msg="## Clang-Tidy Diagnostic Summary\n")
    write_step_summary(msg=tabulate(table, headers=["Diagnostic Name", "Count"], tablefmt="github") + "\n")
