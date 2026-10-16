#!/usr/bin/env python3
"""Prints the failed test cases in JUnit XML reports found under the given directories."""

import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path


def main(directories):
    failures = 0
    for directory in directories:
        for report in sorted(Path(directory).rglob("*.xml")):
            try:
                root = ElementTree.parse(report).getroot()
            except ElementTree.ParseError:
                continue
            for case in root.iter("testcase"):
                for problem in list(case.findall("failure")) + list(case.findall("error")):
                    failures += 1
                    print(f"FAILED {case.get('classname')}.{case.get('name')} ({report.name})")
                    details = (problem.text or problem.get("message") or "").strip().splitlines()
                    for line in details[:15]:
                        print(f"    {line}")
    print(f"{failures} failed test(s).")


if __name__ == "__main__":
    main(sys.argv[1:] or ["."])
