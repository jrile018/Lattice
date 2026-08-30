# geomarket

Geometric market manifold — equity relationship geometry for statistical
arbitrage. See [ADR.md](ADR.md) for the full design (data sources, math,
architecture, milestones). This file is the quickstart.

## Prerequisites

| | Windows | Linux |
|---|---|---|
| Compiler | MSVC (Visual Studio 2022, C++ workload) | GCC 13+ |
| CMake | ≥ 3.27 | ≥ 3.27 |
| Generator | Ninja | Unix Makefiles (see note) |
| Git | required | required |

vcpkg itself is **not** a prerequisite — it is bootstrapped into
`tools/vcpkg/` by the steps below and pinned to the exact commit in
`vcpkg.json`'s `builtin-baseline`, so every clone builds the same
dependency versions.

**Linux note:** the `linux-gcc-*` CMake presets use the "Unix Makefiles"
generator, not Ninja, so a from-scratch box needs no extra package
beyond what's in the table above. If `ninja` happens to be installed,
nothing stops you from adding a Ninja preset locally.

**Linux note on flex/bison:** Arrow's Parquet support depends on Thrift,
which needs `flex` and `bison` to build from source. If they are not
installed system-wide and you don't have root, they can be fetched
without root via `apt-get download flex bison libfl-dev libfl2 m4` and
extracted locally with `dpkg -x <deb> <prefix>`. Put `<prefix>/usr/bin`
on `PATH` **and** export `BISON_PKGDATADIR=<prefix>/usr/share/bison`
before running `vcpkg install` — bison hardcodes `/usr/share/bison` at
compile time and does not locate its own data files relative to its
binary, so without the env var it fails with `m4sugar.m4: cannot open`
partway through Thrift's build. (This is exactly how the project's
remote build box — no sudo — is set up; see the M0 session notes in git
history if you need the literal commands.)

## Building

```bash
git clone <this repo>
cd equities
git clone https://github.com/microsoft/vcpkg.git tools/vcpkg
cd tools/vcpkg && git checkout $(grep '"builtin-baseline"' ../../vcpkg.json | sed -E 's/.*"([0-9a-f]{40})".*/\1/') && cd ../..
./tools/vcpkg/bootstrap-vcpkg.sh -disableMetrics   # or bootstrap-vcpkg.bat on Windows

cmake --preset windows-msvc-release   # or: linux-gcc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

The first configure builds every pinned dependency from source (Arrow is
the long pole — expect 30-90 minutes on the first run, cached
thereafter). Subsequent configures are fast.

## Repository layout

See ADR.md §8.1 for the full annotated layout. Short version:

- `libs/gm-core/` — strong types, error handling, config, manifest, the
  NYSE trading calendar. No financial math lives here yet (that starts
  in `libs/gm-geometry/` etc. from M1 onward).
- `apps/gm-*/` — one CLI executable per pipeline stage (ADR-006). Each
  reads a TOML config plus upstream artifacts and writes its own
  artifacts and manifest under `runs/<run_id>/<stage>/`.
- `apps/gm-run/` — orchestrates the full chain end to end.
- `runs/` — immutable, run_id-keyed output (gitignored; regenerate
  rather than diff).
- `tests/golden/` — end-to-end pipeline tests that actually invoke the
  built binaries and check their artifacts.

## Status

M0 (repo skeleton, build plumbing, `gm-core`, NYSE calendar, and the
full 8-stage pipeline wired end-to-end as stubs) — see ADR.md §13 for
what "done" means at each milestone.
