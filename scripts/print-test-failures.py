#!/usr/bin/env python3
"""Prints the failed test cases in JUnit XML reports found under the given directories.

Exits with an error if the reports contain no test cases at all, which usually means that the
tests didn't run.
"""

import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path


def main(directories):
    tests = 0
    failures = 0
    for directory in directories:
        for report in sorted(Path(directory).rglob("*.xml")):
            try:
                root = ElementTree.parse(report).getroot()
            except ElementTree.ParseError:
                continue
            for case in root.iter("testcase"):
                tests += 1
                for problem in list(case.findall("failure")) + list(case.findall("error")):
                    failures += 1
                    print(f"FAILED {case.get('classname')}.{case.get('name')} ({report.name})")
                    details = (problem.text or problem.get("message") or "").strip().splitlines()
                    for line in details[:15]:
                        print(f"    {line}")
    print(f"{tests} test(s), {failures} failed.")
    if tests == 0:
        print("No tests ran.", file=sys.stderr)
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["."]))
