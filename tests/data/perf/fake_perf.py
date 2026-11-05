#!/usr/bin/env python3
"""Fake `perf` for deterministic tests（docs/35）.

Real `perf record` needs `perf_event_paranoid`/CAP_PERFMON, which most CI containers do not
have. This shim implements just enough of the CLI to exercise maque's profiling lifecycle
end-to-end:

  perf --version
  perf record -o <file> -p <pid> ...        # writes a fake blob, sleeps until SIGINT
  perf report --stdio -i <file> ...         # prints a synthetic report
  perf script -i <file> ...                 # prints synthetic samples (used by tools)

The report text intentionally includes the same columns as real perf (`Overhead`, optional
`Samples`, `Command`, `Shared Object`, `[.] Symbol`) so the production parser is tested.
"""
import signal
import sys
import time

REPORT = """\
# To display the perf.data header info, please use --header/--header-only options.
#
# Samples: 400  of event 'cpu-clock'
#
# Overhead  Samples  Command       Shared Object      Symbol
# ........  .......  ............  .................  ..............................
#
    62.50%      250  maque_player  maque_player  [.] maque::rt::Engine::step_frame()
    20.00%       80  maque_player  maque_player  [.] maque::rt::Scene::visit()
    10.00%       40  maque_player  libc.so.6     [.] memcpy
     7.50%       30  maque_player  maque_player  [.] maque::profile::parse_report_text(std::string const&)
"""

SCRIPT = """\
7f0012345678 maque::rt::Engine::step_frame() (maque_player) /repo/modules/runtime/src/engine/frame.cpp:16
7f0012349999 maque::rt::Scene::visit() (maque_player) /repo/modules/runtime/src/scene/tree.cpp:120
7f001234aaaa memcpy (libc.so.6) /build/glibc/string/memcpy.c:30
7f001234bbbb maque::profile::parse_report_text(std::string const&) (maque_player) /repo/modules/core/profile/src/profile.cpp:300
"""


def find_arg(args, flag):
    try:
        return args[args.index(flag) + 1]
    except (ValueError, IndexError):
        return None


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("--version", "-v"):
        print("perf version 6.99.fake (maque tests)")
        return 0

    cmd = args[0]
    if cmd == "record":
        out = find_arg(args, "-o") or "/tmp/fake-perf.data"
        try:
            with open(out, "wb") as f:
                f.write(b"FAKE PERF DATA\n" * 64)
        except OSError as exc:
            print(f"fake perf: cannot write {out}: {exc}", file=sys.stderr)
            return 1
        # Probe form (`perf record ... -- true`) has no -p: finish immediately.
        if "-p" not in args:
            return 0
        stop = {"v": False}

        def on_signal(_sig, _frm):
            stop["v"] = True

        signal.signal(signal.SIGINT, on_signal)
        signal.signal(signal.SIGTERM, on_signal)
        while not stop["v"]:
            time.sleep(0.02)
        return 0

    if cmd == "report":
        sys.stdout.write(REPORT)
        return 0

    if cmd == "script":
        sys.stdout.write(SCRIPT)
        return 0

    if cmd in ("stat", "--help", "-h"):
        return 0

    print(f"fake perf: unsupported command {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
