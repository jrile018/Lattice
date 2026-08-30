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

**Linux note on build tooling without root:** several ports (Thrift, for
Arrow's Parquet support; glfw3's X11 backend) need build-time tools that
are not always present and are easy to get wrong without root:

- `flex` + `bison` — if missing, `apt-get download flex bison libfl-dev
  libfl2 m4` and extract each with `dpkg -x <deb> <prefix>` (no root
  needed for download-only + local extraction). Put `<prefix>/usr/bin`
  on `PATH` **and** export `BISON_PKGDATADIR=<prefix>/usr/share/bison` —
  bison hardcodes `/usr/share/bison` at compile time and does not locate
  its data files relative to its own binary, so without the env var it
  fails with `m4sugar.m4: cannot open` partway through Thrift's build.
- `autoconf` + `automake` + `libtool` (+ `autoconf-archive`) — same
  `apt-get download` + `dpkg -x` approach for packages `autoconf
  automake libtool libtool-bin autoconf-archive autotools-dev`. Export
  `ACLOCAL_PATH=<prefix>/usr/share/aclocal:<prefix>/usr/share/aclocal-1.16`
  so `aclocal`/`autoreconf` find autoconf-archive's macros (e.g.
  `AX_CHECK_COMPILE_FLAG`, used by glfw3's `pthread-stubs` dependency) —
  without it, `autoreconf` fails with "possibly undefined macro" or
  `vcpkg_run_autoreconf` fails outright asking you to `apt install`
  packages that don't need installing, only extracting.

(This is exactly how the project's remote build box — no sudo — is set
up; see the M0 session notes in git history for the literal commands.)

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
