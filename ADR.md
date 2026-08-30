# ADR: Geometric Market Manifold — Equity Relationship Geometry for Statistical Arbitrage (C++ Edition)

- **Status:** Proposed (awaiting approval before implementation)
- **Date:** 2026-08-29 (remodeled same day: full C++ implementation)
- **Owner:** johnp
- **Scope:** ~100 most-liquid US large-cap equities, daily bars, 2010–present
- **Language:** C++20, single codebase from ingestion to visualization

---

## 1. Purpose

Build a system that represents a universe of equities as points in a geometric space derived from how they actually behave, wraps a surface around the region of **normal** behavior, and detects when an individual equity leaves that region.

The system answers three questions:

1. **Where is this equity right now, relative to everything else?** — its coordinates in relationship space.
2. **Is it behaving normally?** — is its point inside or outside the learned normality surface, and by how much.
3. **Has the space itself changed?** — the surface is refit on a rolling window, so it deforms over time. A sudden deformation is a regime change, and is itself a signal.

The trading thesis is mean reversion: an equity that exits its normal region without a fundamental cause tends to be pulled back inside. The excursion is the entry, the re-entry is the exit.

A secondary, non-trading goal is **comprehension**: the same interface lets you click any point and learn what the company does, who it is economically wired to, and why it sits where it sits.

---

## 2. The core idea in plain language

Two stocks that move together get placed near each other. Two that move differently get placed far apart. Do that for all ~100 names and you get a cloud of points floating in space — a map of the market where distance means "unrelatedness."

Now draw a surface around the region where behavior is normal.

- A point **inside** the surface is doing what it usually does. Ignore it.
- A point that **pokes outside** has left its normal operating range. That is the dislocation.
- The **surface itself breathes** — expanding when stocks act independently, clenching when everything moves as one, and tearing when the market's structure genuinely breaks.

Three different surfaces answer three different questions, all fit over one shared feature store (ADR-008).

---

## 3. Engineering principles

These govern every decision below. They are the priorities of a production quant codebase, in order:

1. **Correctness is provable, not assumed.** Every numerical routine has reference test vectors — analytic cases, published examples, or synthetic data with known answers. No routine ships on "it looks right."
2. **Determinism.** Same inputs + same config + same binary ⇒ bit-identical outputs. No wall-clock, no unseeded RNG, no `-ffast-math`, no order-dependent parallel reductions in scored paths.
3. **One-way data flow.** Stages communicate only through immutable, versioned artifacts on disk. No stage reaches backward; the viewer never computes.
4. **The hot path is boring.** Exceptions and allocation are fine at setup; the per-frame loops are allocation-free, exception-free, and profiled.
5. **Research artifacts are production artifacts.** The backtest engine and the (eventual) live engine consume the same feature and signal code. There is no "research version" of any formula.
6. **Everything is replayable.** A manifest pins config, git commit, compiler, flags, and input-data hashes for every run.

---

## 4. Glossary

| Term | Meaning |
|---|---|
| **Universe** | The ~100 tickers under study at a point in time. Point-in-time, never today's list applied to history. |
| **Frame** | One trading day's complete geometric state: correlation matrix, embedding, boundaries, scores. |
| **Feature vector** | The row `x(i, t)` describing equity `i` on day `t`. The "point." |
| **Manifold / normality surface** | The fitted boundary enclosing the region of normal behavior. Not a manifold in the strict differential-geometry sense; the name is kept because it matches how the concept is used. |
| **Excursion** | An episode where a point sits outside its surface: a start, a peak depth, an end. |
| **Excursion depth** | Signed normalized distance from the boundary. Negative = inside, positive = outside. |
| **Peer basket** | The synthetic hedge portfolio built from an equity's nearest neighbours in relationship space. |
| **Spread** | Log-price of the equity minus weighted log-price of its peer basket. The tradable residual. |
| **Stage** | One pipeline executable consuming and producing artifacts (ADR-006). |
| **Run** | One full pipeline execution under a fixed config, written immutably to `runs/<run_id>/`. |

---

## 5. Decision records

### ADR-001 — Universe: top ~100 by liquidity, point-in-time

**Context.** "Most popular equities" is ambiguous; retail-attention feeds are no longer publicly available at useful fidelity. Liquidity is the durable proxy for popularity and the property that matters for tradability.

**Decision.** Top 100 names by trailing 60-day median dollar volume, drawn from `S&P 500 ∪ Nasdaq-100 ∪ small manual high-attention list`, reconstituted annually with **point-in-time membership**.

**Consequences.** Avoids the "AI winners only" distortion of projecting today's list onto 2010. Requires reconstructing historical index membership (§7.1). Delisted names remain a partial gap (ADR-016).

**Rejected.** Hand-picked 30 names (too thin for geometry). Full S&P 500 in phase 1 (kills iteration speed while designing; the C++ engine makes scaling to it later a config change, not a rewrite).

---

### ADR-002 — Daily bars, 2010–present, free data sources

**Decision.** Daily adjusted closes, 2010-01-01 → present, from free sources (§7). No paid feed in phase 1.

**Consequences.** Holding periods of days-to-weeks. ~15 years spans the 2011 euro crisis, 2015 vol shock, 2018 Q4, COVID, the 2022 rate regime, and the 2023+ AI concentration regime — enough regimes to fit on some and validate on others. Free-data quality risk is handled explicitly (ADR-015).

**Rejected.** Intraday (paid, ~2 orders of magnitude more data; revisit only if daily proves the concept). Paid daily (revisit specifically to fix survivorship, ADR-016).

---

### ADR-003 — Language: C++20 across the entire system

**Context.** The original design was Python notebooks + a web viewer. The remodel directive: everything in C++, planned as a senior quantitative developer would.

**Decision.** C++20 (not 23 — MSVC/GCC parity on the remote box is cleaner at 20) for every component: ingestion, validation, feature computation, geometry, boundary fitting, signals, backtest, artifact export, and the interactive viewer. No Python anywhere in the build or runtime.

**What this buys.**
- **One codebase to production.** The formula that scored the backtest is the object file that would score live data. Research→production rewrite risk — the classic quant-shop failure mode — is eliminated by construction.
- **Determinism** is achievable in a way interpreted stacks fight against: pinned toolchain, controlled floating-point, no dependency drift under the run.
- **Throughput** where it matters: parameter sweeps (thousands of full-history replays), persistent homology across ~3,800 frames, and headroom to run the full S&P 500 without architectural change.
- **A native viewer** with sub-millisecond frame scrubbing across 15 years of geometry.

**What this costs — stated honestly.**
- Development velocity: expect **2–3× the effort** of the Python design, concentrated in the data layer and the viewer, not the math.
- Loss of the scientific-Python ecosystem: robust covariance, one-class SVMs, and MDS arrive via a C++ library or get implemented and tested by hand (ADR-011, ADR-012).
- Ad-hoc exploration is slower. Mitigated by a fast artifact→viewer loop and a report generator (ADR-007), not by shell escapes into other languages.

**Consequences.** The dependency set, testing burden, and milestone plan below are all sized for this decision.

**Rejected.** Python/hybrid (directive); Rust (weaker numeric/sci-viz ecosystem for this workload, no team familiarity signal); C++23 (toolchain parity risk between MSVC on the dev box and GCC on the remote box).

---

### ADR-004 — Toolchain: CMake + vcpkg, pinned; Windows (MSVC) + Linux (GCC) first-class

**Decision.**
- **Build:** CMake ≥ 3.27 with `CMakePresets.json` defining `windows-msvc-release`, `windows-msvc-debug`, `linux-gcc-release`, `linux-gcc-asan`. One-command configure+build on both machines.
- **Dependencies:** vcpkg in **manifest mode** (`vcpkg.json` with a pinned baseline commit). Builds are reproducible from a clean clone; no system-installed libraries.
- **Compilers:** MSVC 19.4x on the dev box; GCC 13+ on the remote box (`john-riley@192.168.0.136`). Both build in CI-style scripts from day one, because the sweep engine runs on Linux and "works on my machine" is not a milestone.
- **Floating point:** strict conformance (`/fp:precise`, no `-ffast-math`). Double precision throughout scored paths.
- **Warnings:** `/W4` / `-Wall -Wextra -Wconversion`, warnings-as-errors in CI scripts.
- **Sanitizers:** ASan+UBSan preset on Linux; the full test suite passes under both before any milestone closes.

**Consequences.** Slower first build (vcpkg compiles Arrow once); every subsequent build is cached. Cross-platform discipline from day one is what makes remote sweeps free later.

**Rejected.** Conan (fine, but vcpkg's MSVC integration is smoother on this dev box). Header-only-everything (Arrow and the viewer stack make that impossible anyway). Bazel (overkill for one developer).

---

### ADR-005 — Third-party dependency policy: small, boring, pinned

**Context.** Every dependency is a supply-chain and maintenance liability. A senior codebase buys leverage, not variety.

**Decision.** The approved set, by role (vcpkg names in parentheses):

| Role | Library | Why this one |
|---|---|---|
| Linear algebra | **Eigen 3** (`eigen3`) | Header-only, the C++ standard for dense linalg; self-adjoint eigensolvers and SVD cover MDS, RMT, Procrustes. |
| Columnar storage | **Apache Arrow / Parquet** (`arrow`) | The artifact format (ADR-017); language-agnostic on-disk contract. |
| HTTP client | **cpr** (`cpr`, wraps libcurl) | Boring, portable TLS fetching for Stooq/SEC/FRED. |
| JSON | **simdjson** (`simdjson`) parse, **nlohmann-json** (`nlohmann-json`) write | Fast parse of large SEC files; ergonomic manifest writing. |
| Config | **toml++** (`tomlplusplus`) | Typed, comment-friendly configs; TOML over YAML to avoid YAML's type-coercion traps. |
| Logging | **spdlog** (`spdlog`) | Structured, fast, boring. |
| CLI | **CLI11** (`cli11`) | Declarative stage interfaces. |
| Dates | **Howard Hinnant date** (`date`) | Civil-date arithmetic; NYSE calendar built on top in-house (ADR-010 note). |
| Tests | **Catch2** (`catch2`) | Sections + matchers suit numeric testing. |
| Benchmarks | **google-benchmark** (`benchmark`) | Regression-guard the hot loops. |
| Small QP (basket weights) | **OSQP** (`osqp`) | Tiny constrained least-squares problems (k≈8); a tested solver beats a hand-rolled active set. |
| Special functions | **Boost.Math** (`boost-math`) | Normal/chi-squared CDFs for p-values and the Deflated Sharpe Ratio. |
| Parallelism | **oneTBB** (`onetbb`) | `parallel_for` over frames and sweep cells; deterministic reduction patterns where scores are produced. |
| Persistent homology | **Ripser** (vendored single header) | The reference TDA implementation *is already C++*; phase-3 lens costs no FFI. |
| Viewer | **Dear ImGui + ImPlot + GLFW + glad** (`imgui[glfw-binding,opengl3-binding]`, `implot`, `glfw3`, `glad`) | The standard in-house-trading-tool stack; immediate-mode UI + raw OpenGL for the 3D cloud/mesh. |

**Explicitly implemented in-house** (with reference tests, ADR-020): Ledoit–Wolf shrinkage, Marchenko–Pastur clipping, classical MDS, orthogonal Procrustes, weighted KDE boundary, FastMCD robust covariance (ADR-011), OU fitting, walk-forward engine, Deflated Sharpe, NYSE trading calendar, CSV parsing (formats are known and fixed; a hand-rolled RFC-4180 reader with tests beats a dependency).

**Rejected.** dlib/mlpack/Shark for the ML pieces (each drags a large surface for one or two functions; the functions we need are individually small and testable). Qt and VTK for the viewer (capability is real, but dependency weight and API surface are disproportionate to four tabs; ImGui+GL is the trading-desk idiom). xtensor (Eigen suffices).

---

### ADR-006 — Pipeline = staged CLI executables with artifact handoff (replaces notebooks)

**Context.** The original design used notebooks as orchestration. Notebooks do not exist in a C++ world, and their real value — inspectable intermediate state — must be preserved by other means.

**Decision.** The pipeline is a chain of small executables, each with a single responsibility, communicating **only** through Parquet/JSON artifacts:

```
gm-universe   → universe.parquet                (point-in-time membership)
gm-ingest     → prices.parquet + quality report (fetch, cache, validate)
gm-features   → features.parquet               (returns, betas, momentum, …)
gm-geometry   → geometry/, edges.parquet       (corr → distance → MDS → Procrustes)
gm-boundaries → surfaces/, scores.parquet      (ellipsoid + kernel fits, all views)
gm-signals    → spreads.parquet, excursions.parquet, signals.parquet
gm-backtest   → backtest/                      (walk-forward, costs, DSR)
gm-report     → report.html                    (static self-contained diagnostics)
gm-sweep      → orchestrates cells of the above across a parameter grid
gm-view       → interactive viewer (read-only, ADR-018)
```

Each stage: reads one TOML config + upstream artifacts, validates schema versions on load, writes its outputs plus a stage manifest, and is idempotent (re-running with identical inputs is a no-op or byte-identical rewrite). `gm-run` drives the whole chain and assembles the run manifest.

**Consequences.** Intermediate state is *more* inspectable than notebooks (every hand-off is a typed file on disk, viewable in `gm-view` or any Parquet reader). Partial re-runs are free: a boundary-parameter change re-executes only `gm-boundaries` onward. The "exploration loop" becomes: edit config → run affected stages (seconds at N=100) → look in viewer.

**Rejected.** One monolithic binary with flags (loses partial re-run and blast-radius isolation). Embedded scripting (Lua/cling) for exploration (violates the single-language directive and adds a soft second language).

---

### ADR-007 — Reporting: `gm-report` emits static, self-contained HTML

**Context.** Notebooks also served as the *record* of an analysis. That role needs a C++-native replacement.

**Decision.** `gm-report` renders each run's diagnostics — data-quality tables, eigenvalue spectra, alignment residuals, excursion/reversion statistics, backtest tearsheet — to a single static HTML file with inline SVG charts generated by our own small plotting module (axes, lines, scatters, heatmaps; ~1kLOC, tested). No JS frameworks, no external assets, no server.

**Consequences.** Every run is accompanied by a human-readable artifact that can be archived or shared. The plotting module is deliberately minimal: publication charts are not the goal, decision-grade diagnostics are.

**Rejected.** Generating Plotly/vega HTML (embeds a JS stack — against the spirit of the directive and adds an untested rendering dependency).

---

### ADR-008 — One shared feature store, three boundary fits

**Context.** Three plausible definitions of "the object" — market cross-section, own history, peer-relative residual — look like competing designs but share all expensive computation.

**Decision.** Compute one feature table `X[equity, day, feature]` once (`gm-features` + `gm-geometry`). Fit three boundaries over it (`gm-boundaries`):

| View | Fit to | Answers | Role |
|---|---|---|---|
| **A — Market** | All equities at time t | "Is this name structurally odd vs the market's current shape?" | Visual centerpiece; regime detector |
| **B — Self** | One equity's trailing history | "Is this name outside *its own* normal range?" | Per-name normality with an honest base rate |
| **C — Peer-relative** | Equity-vs-peer-basket spread | "Has the tradable residual stretched?" | The actual trade signal |

**Consequences.** Marginal cost of all three over any one ≈ 15%. Cross-validation for free: a signal confirmed by B and C while A shows a stable (non-tearing) shape is materially higher-confidence than any single view.

---

### ADR-009 — Correlation estimated with shrinkage + RMT denoising, never raw

**Context.** With N=100 and window W=60, q = N/W ≈ 1.67: the sample correlation matrix is rank-deficient and its small eigenvalues are estimation noise. Geometry built on it jitters randomly day to day.

**Decision.** Every correlation matrix passes through (a) Ledoit–Wolf shrinkage toward a structured target, then (b) Marchenko–Pastur eigenvalue clipping (noise bulk replaced by its mean, diagonal re-normalized). Both in-house on Eigen, both reference-tested. RMT-clipped is the default for geometry; raw is retained for diagnostics only. The top eigenvector (market mode) is separated and optionally removed for the peer-relative view.

**Rejected.** Raw Pearson (unstable). EWMA-only (kept as an alternative estimator flag, insufficient alone at this q).

---

### ADR-010 — Embeddings: classical MDS, Procrustes-aligned across frames

**Decision (two coupled choices).**
- **Classical MDS** (eigendecomposition of the double-centered squared-distance matrix, Eigen `SelfAdjointEigenSolver`) is the embedding. Deterministic, linear, preserves global distances — which is what "how far outside" depends on.
- **Every frame is orthogonally Procrustes-aligned** to the previous frame (SVD of the cross-covariance), periodically re-anchored to a long-window reference to stop drift. Without this, rotation/reflection invariance makes the animation thrash while nothing real changes. The post-alignment residual — change that rotation *cannot* explain — is retained as the **structural change metric**, a first-class regime series.

A tested in-house **NYSE trading calendar** (holidays + half-days, 2010→present, from published exchange history) underlies all windowing; day-count bugs are the quietest way to corrupt every downstream number.

**Rejected.** UMAP/t-SNE (stochastic, globally distorting, frame-unstable — and no C++ implementation worth trusting for scored output).

---### ADR-011 — Boundary estimators: robust Mahalanobis + kernel level set, always both

**Context.** An ellipsoid is interpretable but convex-only; a kernel boundary is shape-flexible but statistically mute. In C++ neither arrives for free.

**Decision.** Two estimators per view, both surfaced:
- **Robust Mahalanobis ellipsoid.** Phase 1 ships shrunk-covariance Mahalanobis with MAD-standardized inputs (simple, testable). Phase 2 upgrades to **FastMCD** (Rousseeuw & Van Driessen), implemented in-house and validated against published reference results — MCD is the deliberately-scheduled hard numerical deliverable of the project. Depth is reported in sigmas with a chi-squared p-value.
- **Kernel level-set boundary.** Weighted Gaussian KDE with Scott/Silverman bandwidth; the surface is the density contour containing (1−α) of training mass. Rendered via marching cubes (in-house, ~500 LOC, tested on analytic shapes) for the viewer mesh.

Disagreement between the two is a first-class output: it marks genuinely non-elliptical regions of "normal."

**Rejected.** OCSVM (needs an SMO solver — a large dependency or a large in-house effort for marginal benefit over a KDE level set at these dimensions). Isolation Forest / autoencoders (score without geometry: nothing to render, no "inside").

---

### ADR-012 — Topology (persistent homology) as a phase-3 lens, via Ripser

**Decision.** Vendor Ripser (single C++ header, the reference implementation) to compute H0/H1 persistence per frame. Outputs: persistence summary features joined to the feature store, and the Wasserstein distance between consecutive diagrams as a "shape change rate." Primary role is a **veto**: when the shape is tearing, mean-reversion signals from views B and C are suppressed — the definition of normal is actively invalid.

**Consequences.** This is the one place the C++ directive is a pure win: the best tool was already C++, and running it across all ~3,800 frames is a `parallel_for`.

---

### ADR-013 — Reversion must be verified before it is traded (the gate)

**Context.** "Outside the normal region" is an anomaly detector. Anomalies split into noise (reverts → profit) and news (the surface was wrong → falling knife). No anomaly strategy survives without separating them.

**Decision.** `gm-signals` + `gm-backtest` must report, out-of-sample, before anything is traded:
- `P(point returns inside within H days | exit depth ≥ d)` — the empirical reversion base rate;
- the same, **conditioned on** an earnings date or 8-K inside the window;
- fitted OU half-life distribution of the spreads;
- performance net of realistic transaction and borrow costs.

If excursions do not revert materially better than the unconditional base rate, the geometry ships as a visualization tool and **is not traded**. That outcome is explicitly acceptable.

---

### ADR-014 — Walk-forward only, Deflated Sharpe mandatory

**Decision.** Boundaries are fit on trailing data and scored strictly out-of-sample (View B's fit never contains today's point). Every reported Sharpe is accompanied by the **Deflated Sharpe Ratio**; `gm-sweep` counts trials automatically into the run manifest — the number of configurations tried is recorded by machinery, not recalled by memory.

**Consequences.** Headline numbers look worse and are believable.

---

### ADR-015 — Two-source price validation

**Decision.** Prices fetched from the primary source are validated against an independent-lineage secondary (§7.2) on a rotating sample. Automated screens flag: |return| > 50% unmatched by a known corporate action, zero-volume days, gaps > 3 trading days, and series that change retroactively between fetches. All raw pulls are cached with fetch timestamps; a run reproduces even after upstream data drifts.

**Consequences.** In an anomaly-detection system, a single bad tick *is* a fake signal. This is the defense.

---

### ADR-016 — Survivorship bias: bounded and reported, not fully solved in phase 1

**Decision.** Point-in-time index membership is reconstructed from published index-change history, so names that left are included while they were in — to the extent their prices remain retrievable. Per-year retrievability coverage is a **reported dataset statistic**. Full resolution needs a paid point-in-time source (Sharadar/Norgate/CRSP) — a phase-5 spend, contingent on the gate (ADR-013) passing.

---

### ADR-017 — Artifacts: Parquet + JSON manifests, immutable, schema-versioned

**Decision.** Tables are Parquet (Arrow C++); boundary meshes are a compact versioned binary (header + vertex/index buffers); manifests are JSON carrying `schema_version`, full config, git commit, compiler + flags, library versions, input hashes, and timings. `runs/<run_id>/` is immutable — a changed parameter is a new run. Every consumer validates `schema_version` and refuses what it doesn't understand.

**Consequences.** Any figure traces to the exact binary and data that produced it. Parquet keeps the artifacts readable by anything, which future-proofs the data even against this ADR's own language decision.

---

### ADR-018 — Viewer: native Dear ImGui + OpenGL, strictly read-only

**Decision.** `gm-view` is a native desktop application: GLFW window, OpenGL 3.3 core, Dear ImGui panels, ImPlot 2D strips, in-house orbit-camera renderer for the point cloud, MST edges, trails, and the marching-cubes surface mesh (solid + wireframe). It memory-maps run artifacts, holds an LRU cache of decoded frames, and targets < 1 ms frame decode so the 15-year time scrubber is instant. It computes **nothing** financial — it draws what `gm-boundaries` wrote (ADR-006).

**Consequences.** The viewer is real engineering work (~a third of phase-1 effort) but becomes the primary research instrument: scrubbing 3,800 frames at 60 fps is exploration Python never offered.

**Rejected.** C++ HTTP server + three.js (JavaScript, against the directive). Qt/VTK (per ADR-005).

---

### ADR-019 — Error handling, logging, and numeric hygiene

**Decision.**
- **Errors:** `tl::expected<T, Error>`-style returns (vendored single header) across module boundaries; exceptions permitted only during startup/config; hot loops are `noexcept`.
- **Logging:** spdlog, structured; every stage logs its config hash, input hashes, row counts, and timing at INFO — a run's console output is itself an audit trail.
- **Numerics:** `double` everywhere in scored paths; no fast-math; parallel reductions over scored quantities use fixed-order or compensated summation; every eigendecomposition checks convergence status; NaN is never a sentinel (explicit validity masks in tables).
- **IDs:** strong types (`TickerId`, `Date`, `FrameIndex`) — no bare `int`/`string` crossing interfaces; the compiler enforces what code review would otherwise have to catch.

---

### ADR-020 — Testing strategy: reference vectors, golden files, property tests, benchmarks

**Decision.** Four layers, all in CI scripts on both platforms, ASan/UBSan clean:
1. **Unit + reference tests.** Every in-house numeric routine validated against published or analytically-derived answers: Ledoit–Wolf on a fixture with a precomputed shrinkage intensity; MP clipping on synthetic random matrices; MDS recovering planted coordinates from their own distance matrix; Procrustes recovering a known rotation; FastMCD against the published Rousseeuw examples; OU parameters recovered from simulated paths; DSR against the paper's worked example; NYSE calendar against exchange-published holiday lists.
2. **Golden pipeline tests.** A frozen 10-ticker, 2-year fixture dataset runs the entire chain; outputs are byte-compared to committed goldens. Any numeric drift is a reviewed, deliberate golden update.
3. **Property tests.** Correlation matrices PSD after every transform; distances satisfy the triangle inequality; alignment never changes inter-point distances beyond tolerance; View B scores are causal (recomputing with future data truncated changes nothing).
4. **Benchmarks.** google-benchmark on the frame loop (corr→MDS→align→fit→score) and on viewer frame decode; regressions >10% fail the milestone.

---

### ADR-021 — Parallelism and the remote box

**Decision.** oneTBB `parallel_for` over independent frames (geometry, boundaries, homology) and over sweep cells; determinism preserved because frames are independent and per-frame work is sequential. `gm-sweep` shards a TOML-defined grid across cores; on the remote box (`john-riley@192.168.0.136`, 8 cores/30 GB, Linux+GCC preset) sweeps run detached under `tmux` with logged output per the standing remote-compute convention; results rsync back as ordinary run directories. Same binaries, same artifacts, different machine.

---

## 6. Mathematical specification

Unchanged in substance from the original design; restated with implementation bindings.

### 6.1 Returns and correlation

Daily log returns from adjusted close: `r_i(t) = ln(P_i(t)/P_i(t−1))`, on the in-house NYSE calendar. Rolling window `W` (default 60, swept {40, 60, 90, 120}) → sample correlation `C(t)`, then:

1. **Ledoit–Wolf shrinkage** toward a structured target → `C_LW(t)`.
2. **MP clipping**: eigenvalues inside the Marchenko–Pastur bulk for q = N/W replaced by their mean; re-normalize to unit diagonal → `C*(t)`. (Eigen `SelfAdjointEigenSolver`.)
3. **Market mode**: top eigenvector retained separately; removing it yields `C_res(t)` for View C. Both kept — whether removal helps is an open empirical question (§12).

### 6.2 Distance, embedding, alignment

Mantegna metric `d_ij = sqrt(2(1−ρ_ij))` ∈ [0, 2]. Classical MDS on `D(t)` → `Y(t) ∈ R^{N×k}` (k=3 display, up to 10 for scoring). Orthogonal Procrustes per frame: `R* = argmin_R ‖Y(t)R − Ỹ(t−1)‖_F, R'R=I`, solved by SVD (Eigen `BDCSVD`); `Ỹ(t) = Y(t)R*`. The scale-normalized residual `‖Ỹ(t) − Ỹ(t−1)‖_F` is the **structural change metric**.

### 6.3 Feature vector `x(i,t)`

| Group | Features |
|---|---|
| Position | Aligned coordinates `Ỹ(t)[i]` |
| Centrality | Distance to cloud centroid; to own cluster centroid; MST degree and betweenness |
| Risk | Beta to market eigenvector; idiosyncratic vol; idio/total variance ratio |
| Momentum | 5/21/63-day cumulative return, cross-sectionally standardized |
| Peer-relative | Spread z; spread velocity; OU half-life |
| Flow *(ph. 4)* | ETF co-membership centrality; short-interest percentile |
| Topological *(ph. 3)* | H0/H1 persistence summaries |

Cross-sectionally standardized per day (MAD-based) before any boundary fit.

### 6.4 Boundaries

- **View A:** fit both estimators to `{x(i,t) : all i}` per frame; score = signed normalized boundary distance; ellipsoid p-value from chi-squared (Boost.Math).
- **View B:** per equity, fit to trailing `L` days (default 756), strictly causal.
- **View C:** k nearest neighbours under `D(t)` (default k=8); basket weights by constrained ridge (`w ≥ 0, Σw = 1` — a tiny QP via OSQP); spread `s_i = ln P_i − Σ w_j ln P_j`; OU fit via exact AR(1) MLE mapping → half-life `ln 2/θ` and z-score `z = (s−μ)/(σ/√(2θ))`; boundary is a level set on `(z, ż)`.

### 6.5 Signal (all conditions required)

1. View C: `|z| > z_entry` (default 2.0) and half-life in the tradable band (3–30 days);
2. View B: outside its own surface — unusual *for this name*, not merely cross-sectionally;
3. View A: structural change metric below veto threshold (the shape is not tearing);
4. No scheduled earnings inside the expected holding horizon;
5. Liquidity/borrow feasibility on every leg.

Exit: `|z| < z_exit` (default 0.5), or horizon stop at 3× half-life, or hard adverse-excursion stop.

---

## 7. Data sources (all phase-1 needs are free)

### 7.1 Universe membership (point-in-time)

| Source | Access | Cost | Notes |
|---|---|---|---|
| S&P 500 current constituents + join date | `en.wikipedia.org/wiki/List_of_S%26P_500_companies` | Free | **Amended during M1 (2026-08-30).** The ADR originally assumed this page carries a separate "changes" table (additions/removals with dates) sufficient for full point-in-time reconstruction. As of the live page fetched during M1, that table is no longer present — verified by direct fetch, not assumed from memory (`data/raw/sp500_wikipedia.html`, retrieved 2026-08-30). What the page *does* still provide, and what M1 actually uses: a 503-row current-constituent table with a `Date added` column per member, snapshotted to `data/reference/sp500_constituents.csv`. This answers "was ticker X a member on date D" correctly for any name still in the index today (`D >= date_added`). It cannot answer that question for a name that was **removed** from the index before today — that gap is real, but it is not new: it is the same survivorship gap ADR-016 already scoped and already assigned to a paid point-in-time source (Sharadar/Norgate/CRSP) as a later-phase decision. This finding sharpens ADR-016's estimate rather than contradicting it. |
| Nasdaq-100 membership | ~~Wikipedia~~ deferred | Free (source TBD) | **Amended during M1.** Same live-fetch check found no components table on the current Nasdaq-100 Wikipedia page either (no dedicated "List of Nasdaq-100 companies" article exists as a fallback). Rather than force a fragile scrape against a page that clearly reorganizes over time, this is deferred: **M1's base pool is S&P 500 current constituents only**, explicitly narrower than ADR-001's original `S&P 500 ∪ Nasdaq-100 ∪ manual list`. Nasdaq's own official listings page is the more likely durable free source and is the next thing to try when this is revisited, not another Wikipedia scrape. |
| Ticker → CIK map | `www.sec.gov/files/company_tickers.json` | Free | Official; handles ticker changes; joins prices to filings. |
| Liquidity ranking | Computed | Free | Trailing 60-day median dollar volume from the price panel itself. |

### 7.2 Prices — **C++-driven source ordering (a real remodel change, amended again during M1)**

The Python design used `yfinance` (Yahoo primary). `yfinance` exists to negotiate Yahoo's unofficial crumb/cookie/session dance; reimplementing that dance in C++ is fragile maintenance against an undocumented target. A senior call: **make the trivially-fetchable source primary.** At the C++ remodel, that meant demoting Yahoo below Stooq.

**Amended during M1 (2026-08-30), verified by direct fetch, not assumed:** Stooq's `/q/d/l/` CSV endpoint now serves a client-side JavaScript proof-of-work challenge (a SHA-256 hashcash script) before returning any content. This blocks every plain HTTP client uninformly — `curl`, and identically `cpr::Get` from our own C++ code — not a `cpr`-specific gap. There is no lightweight fix: solving it requires executing JavaScript, which means a real or headless browser, which is out of scope for a compiled HTTP client. Stooq is therefore **not usable as the primary source** as designed, full stop, regardless of implementation language.

Meanwhile, Yahoo's unofficial chart JSON endpoint — the one explicitly demoted to "tertiary spot-checks only" specifically because it was expected to be the fragile one — was fetched directly and works cleanly with a plain `curl`/`cpr::Get`: no auth, no cookies, no JS challenge. A single request with `period1`/`period2` query params returned 4,189 daily bars for AAPL spanning 2010-01-04 through 2026-08-28 (`open`/`high`/`low`/`close`/`volume` plus a separate `adjclose` series), which is the entire history this project needs in one call per ticker.

**Revised ordering for M1 onward:**

| Source | Access | Cost | Role |
|---|---|---|---|
| **Yahoo chart endpoint** | `query1.finance.yahoo.com/v8/finance/chart/{ticker}?period1=…&period2=…&interval=1d&events=div,splits` — JSON, no auth | Free | **Primary**, promoted from tertiary. Verified working via plain HTTP; one request per ticker covers the full 2010-present range. Unofficial and undocumented, so gm-ingest's ADR-015 validation screens are the safety net, not a nice-to-have — this is exactly the kind of source that can break without notice. |
| Stooq | `stooq.com/q/d/l/` CSV | Free, but **currently blocked** by a JS proof-of-work challenge | **Demoted, not removed.** gm-io's CSV parser (already built) is ready for it the moment this is resolved (a different endpoint, a solved-challenge proxy, or Stooq relaxing the check) or for any other CSV-shaped free source found later. Not load-bearing for M1. |
| **Tiingo** | REST + free API key, documented JSON | Free tier | **Secondary/validator**, unchanged in role — still the intended independent-lineage cross-check against Yahoo (ADR-015) once a free API key is obtained. Not yet wired into M1's ingest run; the two-source validation requirement is real and open, tracked as an M1 follow-up rather than silently dropped. |
| Alpaca | REST, free tier (IEX feed) | Free | Only relevant if this ever goes live for execution. |
| Polygon.io | REST | Paid — verify pricing | The intraday upgrade path, if ever. |
| Sharadar SEP (Nasdaq Data Link) | REST | Paid — verify pricing | The survivorship fix (ADR-016), phase 5. |

This is the second live-fetch-contradicts-the-plan finding in the same M1 session (the first being §7.1's Wikipedia changes table). Both are recorded here rather than silently worked around, per the project's own engineering principles (§3): a design doc that quietly stops matching reality is worse than one that gets corrected in the open.

### 7.3 Fundamentals & identity — the "learn about it" panel

| Source | Access | Cost | Notes |
|---|---|---|---|
| **SEC XBRL Company Facts** | `data.sec.gov/api/xbrl/companyfacts/CIK##########.json` | Free | Authoritative fundamentals; descriptive `User-Agent` header required; simdjson parses these large files comfortably. |
| SEC Submissions | `data.sec.gov/submissions/CIK##########.json` | Free | Filing history incl. 8-K dates — **required** by ADR-013 to tag news-driven excursions. |
| SEC Financial Statement Data Sets | Quarterly bulk ZIPs | Free | Bulk alternative to per-company calls. |
| SIC code | In the submissions JSON | Free | Official, stable; adequate for sector coloring with the profile text. |
| Company profile text | SEC 10-K Item 1 / Wikipedia intro | Free | Business description for the panel. GICS (paid) not required. |

### 7.4 Economic relationships — the supply-chain layer (descriptive in phase 1)

| Source | Access | Cost | Notes |
|---|---|---|---|
| **ETF co-membership** | Issuer holdings CSVs (iShares/SPDR/Invesco product pages) | Free | **Recommended first layer.** Stocks held by the same ETFs are mechanically co-traded by flows; daily CSVs, trivial C++ ingestion. |
| 10-K major-customer disclosures | EDGAR full-text + parsing | Free | ASC 280 mandates disclosure of >10%-of-revenue customers → a genuine customer/supplier graph. Noisy text extraction; highest-value free option; phase 4. |
| Compustat Customer Segment | WRDS (academic) | Institutional | The Cohen–Frazzini dataset, if ever accessible. |
| FactSet Revere / Bloomberg SPLC | Enterprise | Expensive | Out of scope; listed for completeness. |
| Wikidata | API | Free | Ownership/subsidiaries for the panel; never systematic. |

### 7.5 Context & regime

| Source | Access | Cost | Notes |
|---|---|---|---|
| **FRED** | `api.stlouisfed.org` (free key) | Free | VIX, 10y–2y, credit spreads — overlaid on the Evolution tab's strips. |
| FINRA short interest | Bulk downloads | Free | Bi-weekly; crowded shorts explain a distinct excursion class. |
| Earnings dates | SEC 8-K dates (primary), Yahoo calendar (spot-check) | Free | **Required** by ADR-013 — an unmasked earnings gap is a beautiful, untradeable "dislocation." |

---

## 8. System architecture

```
 FREE SOURCES              STAGED C++ PIPELINE (each box = one executable)         VIEWER
 ------------              --------------------------------------------           ------

 Stooq, Tiingo ─┐   ┌────────────┐  ┌────────────┐  ┌────────────┐
 SEC EDGAR      ├──►│ gm-universe│─►│ gm-ingest  │─►│ gm-features│─┐
 FRED, FINRA    │   └────────────┘  └────────────┘  └────────────┘ │
 ETF holdings  ─┘         artifacts (Parquet + manifests) flow →   ▼
                    ┌────────────┐  ┌──────────────┐  ┌────────────┐  ┌────────────┐
                    │ gm-geometry│─►│ gm-boundaries│─►│ gm-signals │─►│ gm-backtest│
                    └────────────┘  └──────────────┘  └────────────┘  └────────────┘
                                          │                  │               │
                                          ▼                  ▼               ▼
                                   ┌──────────────────────────────────────────────┐
                                   │            runs/<run_id>/  (immutable)       │
                                   └──────────────┬───────────────────┬───────────┘
                                                  │ read-only         │
                                            ┌─────▼─────┐       ┌─────▼─────┐
                                            │  gm-view  │       │ gm-report │
                                            │ ImGui/GL  │       │ HTML+SVG  │
                                            └───────────┘       └───────────┘
```

### 8.1 Repository layout

```
equities/
  ADR.md                          <- this document
  README.md
  CMakeLists.txt  CMakePresets.json  vcpkg.json
  config/
    universe.toml  params.toml  sweeps/*.toml
  data/
    raw/          immutable timestamped source pulls
    reference/    CIK map, index change history, NYSE calendar table
  libs/
    gm-core/      strong types, calendar, errors, config, manifest
    gm-io/        parquet read/write, mesh format, csv, http+cache
    gm-data/      loaders, validation (ADR-015), universe logic
    gm-features/  returns, betas, momentum, standardization
    gm-geometry/  shrinkage, RMT, distance, MDS, procrustes, MST
    gm-boundaries/ mahalanobis, fastmcd, kde, marching cubes
    gm-topology/  ripser wrapper, persistence features   (phase 3)
    gm-signals/   baskets (OSQP), OU, entry/exit rules
    gm-backtest/  walk-forward, costs, DSR
    gm-plot/      minimal SVG charting for gm-report
  apps/
    gm-universe/ gm-ingest/ gm-features/ gm-geometry/ gm-boundaries/
    gm-signals/ gm-backtest/ gm-report/ gm-sweep/ gm-run/ gm-view/
  third_party/
    ripser/  tl-expected/            (vendored, pinned, licensed)
  tests/        unit/  golden/  property/  fixtures/
  benchmarks/
  runs/<run_id>/                    immutable outputs (ADR-017)
```

### 8.2 Run artifact contract

```
runs/2026-08-29__w60_k3_mds_rmt/
  manifest.json        config, git commit, compiler+flags, lib versions,
                       input hashes, trial count, timings
  universe.parquet     date, ticker, in_universe, dollar_volume_rank
  prices.parquet       validated adjusted OHLCV panel
  features.parquet     the feature store  (§6.3)
  geometry.parquet     date, ticker, x, y, z, cluster_id, mst_degree
  edges.parquet        date, ticker_a, ticker_b, distance, in_mst
  surfaces/*.gmmesh    per-frame boundary meshes (versioned binary)
  scores.parquet       date, ticker, view, estimator, depth, pvalue, inside
  excursions.parquet   ticker, start, end, peak_depth, reverted, had_earnings
  spreads.parquet      date, ticker, z, half_life, basket weights
  regime.parquet       date, structural_change, market_eigenvalue_share, vix
  backtest/            trades.parquet, equity_curve.parquet, tearsheet.json
  meta/profiles.json   per-ticker description, SIC, links (learn panel)
  report.html          gm-report output
```

---

## 9. The viewer (`gm-view`)

Four tabs in increasing abstraction, plus a persistent learn panel:

- **2D Pairs** — pick two names: return scatter, rolling correlation, spread with bands, excursion history. The legible baseline every exotic claim gets checked against.
- **3D Sectors** — the cloud colored by sector with cluster hulls. Answers "did the geometry rediscover sectors?" — the cheapest possible detector of a broken pipeline.
- **Manifold** — the centerpiece: point cloud inside the wireframe/solid normality surface; inside points dim, outside points lit and labeled with depth; toggles for MST edges, ellipsoid vs kernel surface, color-by (sector/depth/momentum/short interest), and trailing per-point paths.
- **Evolution** — time made explicit: a play/scrub control across the full history over ImPlot strips of the structural change metric, market eigenvalue share, VIX, and a per-ticker inside/outside ribbon.
- **Learn panel** (click any point, any tab): what the company does, SIC/sector, position and depth in all three views, current peer basket with weights, economic links, spread chart, excursion history with reversion outcomes. This is the "learn about the different equities" requirement — and the judgment-building tool for knowing when to distrust the signal.

Performance contract: < 1 ms frame decode from the mapped run directory; 60 fps scrub across ~3,800 frames × 100 names.

---

## 10. Validation protocol

1. **Geometry sanity** — clusters correspond to real sectors; the object visibly clenches in March 2020. If not, stop and debug.
2. **Alignment quality** — stable animation; structural change metric spikes at known events (Aug 2015, Feb 2018, Mar 2020, Jan 2021, Sep 2022).
3. **Reversion study — the gate (ADR-013)** — out-of-sample `P(revert within H | depth ≥ d)`, split by earnings/8-K. **No lift over base rate ⇒ the project ships as a visualization tool.**
4. **Walk-forward backtest** — expanding fits, out-of-sample trades only, realistic costs (spread, commission, borrow, impact at traded size).
5. **Deflated Sharpe** with machine-counted trials (ADR-014).
6. **Robustness** — results must survive W, k, and estimator perturbations. A result that only works at W=60, k=8 is an artifact.
7. **Engineering acceptance (C++-specific)** — full test suite green on MSVC and GCC; ASan/UBSan clean; golden pipeline byte-stable; benchmarks within budget; a run reproduces bit-identically from a clean clone + cached raw data.

---

## 11. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Anomaly is not an edge — excursions may not revert | **Highest** | ADR-013 is an explicit go/no-go gate before any trading work |
| C++ dev velocity: data layer + viewer are real effort | High | Staged milestones (§13); math library is the easy part and lands early |
| Correlation noise at q≈1.67 | High | ADR-009 mandatory shrinkage + RMT |
| Embedding instability fakes "movement" | High | ADR-010 Procrustes; residual becomes a feature |
| Hand-rolled numerics harbor silent bugs | High | ADR-020 reference vectors + goldens + properties + dual-platform CI |
| Survivorship bias inflates backtests | High | ADR-016: point-in-time membership; residual bias measured and reported |
| Overfitting the parameter space | High | ADR-014 walk-forward + DSR + automatic trial counting |
| Bad tick fabricates a dislocation | Medium | ADR-015 two-source validation + screens + timestamped cache |
| Source breakage (esp. anything unofficial) | Medium-High | Yahoo chart endpoint is primary and unofficial (§7.2, revised M1) — the ADR-015 validation screens are the real defense here, not source diversity, since Stooq (the intended primary) is currently blocked outright and Tiingo isn't wired in yet |
| FastMCD implementation difficulty | Medium | Phase-1 stand-in (shrunk covariance + MAD); MCD is its own milestone with published reference tests |
| Crowding — residual reversion is well-trodden | Medium | Thin-margin expectation; costs modeled from day one |
| Viewer scope creep | Medium | Performance contract + four fixed tabs; anything else is phase 4+ |

**The largest risk is still conceptual, not technical.** A rotating manifold with a red point outside it is persuasive independent of whether the trade makes money — and a native 60 fps viewer makes it *more* persuasive. ADR-013 exists so that persuasiveness never substitutes for evidence.

---

## 12. Open questions

1. Universe reconstitution frequency — annual assumed; quarterly tracks liquidity better but adds geometric turnover noise.
2. Scoring dimension — display is 3; whether scoring uses 3/5/10 is a phase-3 sweep.
3. Within-sector vs cross-sector fits — likely different reversion characteristics; test both.
4. Short-side borrow feasibility — footnote in phase 1; becomes a constraint if the book is short-biased.
5. Market-mode removal for View C — help or hurt? Both `C*` and `C_res` retained until settled.
6. MSVC↔GCC bit-reproducibility — same platform reproduces bit-identically (guaranteed); cross-platform is tolerance-based; goldens are per-platform if needed.

---

## 13. Milestones (each closes only with tests green on both platforms, sanitizers clean)

- **M0 — Skeleton (foundation).** Repo, CMake presets, vcpkg manifest, gm-core (strong types, errors, config, manifest), NYSE calendar + tests, CI scripts local+remote, empty stage binaries wired end-to-end passing a trivial artifact. *Exit: `gm-run` executes the whole chain on a stub fixture on both machines.*
- **M1 — Data layer.** gm-io (Parquet, HTTP+cache, CSV), gm-universe (point-in-time), gm-ingest with the ADR-015 validation screens and quality report; the 10-ticker golden fixture frozen. *Exit: 15-year, 100-name validated price panel builds locally; coverage stats reported.*
- **M2 — Geometry.** Shrinkage, RMT, distance, MDS, Procrustes, MST; reference tests for each; gm-report v1 (eigenvalue spectra, alignment residual). *Exit: geometry artifacts for full history in < 60 s locally; structural change metric spikes at known events.*
- **M3 — Boundaries + Viewer alpha.** Phase-1 Mahalanobis + KDE level set + marching cubes; scores for views A and B; gm-view with Manifold + Evolution tabs against real artifacts. *Exit: the object visibly clenches in March 2020, on screen, scrubbed live.*
- **M4 — Signals + the gate.** Peer baskets (OSQP), OU fitting, excursion tracking, earnings/8-K tagging; the ADR-013 reversion study in gm-report. *Exit: a defensible out-of-sample answer to "do excursions revert?" — the go/no-go.*
- **M5 — Backtest + sweeps.** Walk-forward engine, cost model, DSR, gm-sweep sharding on the remote box. *Exit: deflated, cost-netted walk-forward results with machine-counted trials.*
- **M6 — Depth (contingent on M4 passing).** FastMCD, Ripser lens + tear-veto, remaining viewer tabs (2D Pairs, 3D Sectors), learn panel with SEC profiles, ETF co-membership layer. *Exit: each lens either improves the reversion statistics or is documented as not doing so.*
- **M7 — Hardening (contingent on M5 promise).** 10-K customer-graph parsing, paid point-in-time data decision (ADR-016), full-S&P-500 scale test, benchmark budget review.

---

## 14. References

- Mantegna (1999), *Hierarchical structure in financial markets* — correlation distance, MST.
- Ledoit & Wolf (2004), *A well-conditioned estimator for large-dimensional covariance matrices*.
- Laloux, Cizeau, Bouchaud & Potters (1999), *Noise dressing of financial correlation matrices* — the RMT case for ADR-009.
- Avellaneda & Lee (2010), *Statistical arbitrage in the US equities market* — residual reversion, OU, s-scores; direct ancestor of View C.
- Rousseeuw & Van Driessen (1999), *A fast algorithm for the minimum covariance determinant estimator* — the ADR-011 phase-2 deliverable.
- Cohen & Frazzini (2008), *Economic links and predictable returns* — supply-chain layer justification.
- Gidea & Katz (2018), *Topological data analysis of financial time series: landscapes of crashes* — ADR-012.
- Bailey & López de Prado (2014), *The deflated Sharpe ratio* — ADR-014.
- Bauer (2021), *Ripser: efficient computation of Vietoris–Rips persistence barcodes* — the vendored TDA engine.
