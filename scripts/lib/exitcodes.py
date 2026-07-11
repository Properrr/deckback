"""The harness exit-code taxonomy, once, for Python.

`.internal/HARNESS.md` §1 defines it; `scripts/lib.sh` declares it for shell. This is the third
declaration and the last: `tests/harness/test_deckci.py` asserts it matches `lib.sh`, because these
numbers are how one script tells the next one what happened, and a silent disagreement between two
copies reclassifies a regression as an environment problem.

    0  ok
    1  FAIL       generic; a tool we shelled out to crashed
    2  ASSERT     the product is wrong. The ONLY code that means a regression.
    3  ENV        a precondition was missing. Never a regression.
    4  TRANSPORT  the wire broke. Retry this, and only this.
    5  USAGE      the script was invoked wrong.
"""

import argparse
import sys

EX_OK, EX_FAIL, EX_ASSERT, EX_ENV, EX_TRANSPORT, EX_USAGE = 0, 1, 2, 3, 4, 5


class Parser(argparse.ArgumentParser):
    """An ArgumentParser that exits 5 on a usage error, not 2.

    argparse's default is 2. In this repo 2 is EX_ASSERT — "the product is wrong" — the one code that
    fails a build and wakes someone up. A mistyped flag must never be indistinguishable from a
    conformance regression. `just cert conformance-test deck -- --freeze-vectors`, one `--` too many,
    reported itself to its caller as a failing test.
    """

    def error(self, message):
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        raise SystemExit(EX_USAGE)
