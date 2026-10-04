# Contributing

## Most useful right now: a Windows hardware report

If you have an SP410, SP410BT or SP420 and a Windows PC, the checklist is in
[docs/HARDWARE.md](docs/HARDWARE.md#reporting-your-printer). Reports that
answer an [open question](docs/REVERSE_ENGINEERING.md#open-questions) are
especially welcome.

## Clean-room rules

Read [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) first. In
short: no vendor code or binaries, findings in your own words, and never
commit vendor material.

## Code

- C99, no dependencies beyond the C library, Winsock, SetupAPI and WinSpool
  on Windows, and pthreads on POSIX. 2-space indent, braces on their own line
  (match the surrounding code).
- `src/core/dither.c` and `src/core/tspl.c` are shared with
  [sp410-cups-driver](https://github.com/rightiswrong/sp410-cups-driver).
  Change them in both repositories, and regenerate the fixtures
  (`tests/fixtures/*.linux.prn`) from the Linux filter if the output changes on purpose.
- Never call `setlocale()`: TSPL needs `.` as the decimal separator.
- The build must stay warning-free with `-Werror` under GCC, MinGW-w64 and clang.
- Every behaviour change needs a test in `tests/run_tests.py`. Prefer exact
  dot comparisons (`assert_dots`) where the input allows.

## Checks before a pull request

```sh
make check                                   # native tests
make CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS=-fsanitize=address,undefined check
make CFLAGS="-O1 -g -fsanitize=thread" LDFLAGS=-fsanitize=thread check
make windows CFLAGS="-O2 -Werror"            # MinGW-w64 cross build
```

(Run `rm -rf build/native` between sanitizer builds.) CI repeats all of this
and also installs the package on a Windows runner and prints through the
IPP Class Driver.

Update `CHANGELOG.md`, and `MANIFEST.md` if you add, move or remove files.
