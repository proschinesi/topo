# Contributing to topo

## Reporting chips and bugs

- **Chip reports** are the most useful contribution: if your chip or macOS version is not in the
  "Tested chips" table of the README, open a [chip report](https://github.com/proschinesi/topo/issues/new?template=chip_report.md)
  with the output of `topo --dump-soc --count 3` and a screenshot.
- **Bugs**: open a [bug report](https://github.com/proschinesi/topo/issues/new?template=bug_report.md) with your chip,
  `sw_vers`, `topo --version` and the `--dump-soc` output. Search the existing issues first.
- Problems that also happen in btop (process list, disks, network, themes) should be reported to
  [btop](https://github.com/aristocratos/btop/issues) as well.

## Build and test

Requirements: Apple Silicon, macOS 26 or later, Apple clang 19 or later, CMake.

```bash
cmake -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI runs the same steps on `macos-26`. Hardware readings cannot be tested in CI (the runner is a VM), so changes to
the SoC collection need a note with the chips and macOS versions they were tried on, and possibly a unit test for
the pure parts (see `tests/apple_soc.cpp`).

## Pull requests

- Explain why a change is needed; for larger features open an issue first.
- Keep unrelated changes in separate pull requests.
- Do not show invented values: when a reading is missing, show `—`; mark estimates with `~`.
- Keep the cost low: topo should stay around 1% of one core at 1000 ms (see "Overhead" in the README).

## Code style

Follow the style of btop:

- tabs for indentation (tab size 4);
- alternative operators `and`, `or`, `not`;
- opening braces at the end of the same line;
- `fmt::format` for string formatting, RAII, standard algorithms, no new dependencies when the existing ones suffice;
- files changed for topo carry a notice in their header (Apache-2.0 section 4).

For everything else see the [btop contributing guidelines](https://github.com/aristocratos/btop/blob/main/CONTRIBUTING.md).
