#!/usr/bin/env python3
"""Validate a W00-C JSON result using the checked-in strict validators."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from harness_lib import json_load
from schema_validation import ValidationError, validate_document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", nargs="+", type=Path)
    args = parser.parse_args()
    failed = False
    for document in args.documents:
        try:
            validate_document(json_load(document))
        except (OSError, json.JSONDecodeError, ValidationError) as error:
            print("{}: INVALID: {}".format(document, error), file=sys.stderr)
            failed = True
        else:
            print("{}: valid".format(document))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
