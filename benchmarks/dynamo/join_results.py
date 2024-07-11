"""
A tool to merge multiple csv files (generated by torchbench.py/etc) into a single csv file.
Performs an outer join based on the benchmark name, filling in any missing data with zeros.
"""
import argparse
import functools
import operator
from pathlib import Path

import pandas as pd


def longest_common_prefix(strs):
    shortest_str = min(strs, key=len)
    for i, char in enumerate(shortest_str):
        for other in strs:
            if other[i] != char:
                return shortest_str[:i]
    return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--field", "-f", default="speedup", type=str)
    parser.add_argument("--output", "-o", type=str)
    parser.add_argument("inputs", nargs="*")
    args = parser.parse_args()

    prefix = longest_common_prefix([Path(inp).stem for inp in args.inputs])
    frames = []
    fields = []
    for inp in args.inputs:
        field = Path(inp).stem[len(prefix) :]
        fields.append(field)
        frames.append(
            pd.read_csv(inp)
            .filter(["name", args.field])
            .rename(columns={args.field: field})
        )

    df = frames[0]
    for other in frames[1:]:
        df = df.merge(other, how="outer", on="name")
    df = df.fillna(0)

    # drop rows where all backends failed
    df = df[functools.reduce(operator.or_, [df[f] != 0 for f in fields])]

    prefix = prefix.strip("_") or "output"
    output = args.output or f"{prefix}.csv"
    print(f"Writing {output}")
    df.to_csv(output, index=False)


if __name__ == "__main__":
    main()
