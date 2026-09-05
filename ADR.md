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

**Amended by ADR-022.** A fourth view was added later: **D - Valuation self**, the same per-equity trailing-history fit as View B but over point-in-time valuation yields rather than embedding coordinates. It shares this ADR's premise exactly - it is another fit over the one shared feature store, not a second pipeline - which is why it cost a view rather than a stage.

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

---

### ADR-011 — Boundary estimators: robust Mahalanobis + kernel level set, always both

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

**Implementation note (M6 tear-veto fix).** The veto is implemented as: `gm-signals` tags every `spreads.parquet`/`baskets.parquet` row with the `tear_flag` of its date (never drops rows there, so `excursions.parquet` - built from the full, untruncated z-score series - always has a matching `spreads.parquet` row), and `gm-backtest` is the one that actually rejects a tear-flagged candidate from trading, into its own `rejected_tear_veto` counter, at entry time, in the same place and the same way View A's structural-change veto and View B's outside-boundary check are already enforced. An earlier version of this veto dropped rows directly in `gm-signals`, which silently desynced `spreads.parquet` from `excursions.parquet` and miscounted 819/7,376 real tear-vetoed excursions into `gm-backtest`'s `rejected_no_spread_data` - a counter documented as catching a genuine cross-stage data-integrity failure, not a policy decision (see `apps/gm-signals/main.cpp` and `apps/gm-backtest/main.cpp` for the full comment trail).

Scope: the veto is **entry-gated only** - it blocks a candidate whose excursion *starts* on a tear day, but does not force-close a position already open when a later day tears. Read literally, "mean-reversion signals from views B and C are suppressed" describes suppressing new signal emission, which is what entry-gating does; the ADR is silent on what should happen to a position already open when a tear occurs mid-holding. Entry-only gating is the reading implemented here, consistent with how View A and View B are *also* both entry-only checks in this same engine - not a special case invented for this veto. Roughly 41% of excursions in the real 16-year run have a holding window that contains a tear day without ever starting on one, and are therefore untouched by this veto; whether they *should* be is a real open question this ADR does not answer, left for a future decision rather than silently assumed either way.

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

**Amended (ADR-022 implementation).** That applies to *universe membership* and *delisted price history*, which remain the genuine gap. It does **not** extend to fundamentals: SEC XBRL carries real filing dates, so the fundamentals half of point-in-time discipline is already solved for free (§7.3, §6.6). The paid-source question is therefore narrower than this ADR originally implied — it is about prices and membership for names that left the index, not about knowing when a figure was published.

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

### ADR-022 — Relative valuation as a second geometry per equity (View D)

**Context.** Every coordinate in this system is derived from price co-movement. That makes two economically opposite situations look identical: a name can sit far from its usual place in the embedding because it got *cheap*, or because the business is *deteriorating*. Price geometry cannot distinguish them, and that distinction is exactly what ADR-013's reversion gate turns on — a divergence that reverts and one that keeps going are the same shape until something non-price is measured. §6.3's feature vector contains no valuation content at all.

**Decision.** Give every equity a **second geometric figure**, in valuation space, fitted with the same estimator as View B. Four commitments:

**1. The coordinates are yields, not multiples.** The three axes are earnings yield `E/P`, `EBITDA/EV`, and free-cash-flow yield `FCF/P`. Inverted deliberately. `P/E` diverges as `E → 0` and flips sign across zero earnings, which puts an unbounded, sign-flipping coordinate into a covariance estimate — the same near-degeneracy the FastMCD conditioning work (ADR-011) was about. `E/P` passes smoothly through zero and stays bounded. The fix belongs in the choice of coordinate, not in the estimator that has to swallow it.

   *Correction, recorded rather than quietly patched: that argument covers `E/P` and `FCF/P`, whose denominator is market capitalisation and therefore strictly positive. It does **not** cover `EBITDA/EV` — enterprise value is `mcap + debt − cash`, which goes negative for a company trading below its net cash, so the third coordinate can still flip sign. The two failure modes in that denominator get deliberately different treatment; see §6.6.*

**2. The figure is per-equity and self-referential — View D, the valuation analogue of View B.** Fit to that ticker's own trailing `L` days (default 756, matching View B) of `(E/P, EBITDA/EV, FCF/P)`, strictly causal. Depth answers "how cheap is this name *relative to its own history*", not "relative to the market". The cross-sectional analogue — a valuation View A — is deliberately **not** decided here; see Consequences.

**3. The numerators step, the denominators move daily.** Fundamentals restate four times a year; price and enterprise value move every session. So the valuation point moves *every day*, and a 756-day window holds 756 distinct points rather than 12 — which is what makes a robust ellipsoid a meaningful object over it at all. Known artifact: each earnings release puts a step in the cloud, so it carries roughly twelve shelves per window. That is a measurable property of the data, not a defect to suppress; whether the shelving distorts the ellipsoid enough to matter is an empirical question (§12).

**4. It is a new view, not a tenth stage.** View D lives inside `gm-boundaries`. It is the same *kind* of object as View B — a robust ellipsoid over one ticker's trailing cloud — so it reuses the same FastMCD call path and the same `scores.parquet` schema with a new `view` value. A separate executable would duplicate the fitting machinery to gain nothing. `gm-run`'s stage list is unchanged and ADR-006's partial-re-run property is preserved: changing a valuation parameter re-executes `gm-features` onward.

**Data flow.** Five touch points, in stage order:

| Stage | Change |
|---|---|
| `gm-ingest` | New `fundamentals.parquet`. Every row carries **two** dates: `period_end` (the fiscal period the figures describe) and `available_date` (the date they were published). |
| `gm-features` | `features.parquet` gains the three yield columns, computed as-of `D` under the rule below. |
| `gm-boundaries` | `scores.parquet` gains `view = "D"` rows. Same columns, same estimator. |
| `gm-signals` | A new optional condition (§6.5 condition 6). |
| `gm-view` | Renders both figures per equity. |

**Point-in-time is the load-bearing constraint, not a detail.** Any computation simulating day `D` may read only fundamentals rows where `available_date <= D`, per the rule documented in README.md. Until a paid point-in-time source exists, `available_date` is *estimated* — SEC filing dates from the Submissions API where retrievable, otherwise `period_end` plus a conservative lag — the manifest records `fundamentals_availability = "estimated" | "reported"`, and **no View D output may promote a trade while that flag reads `"estimated"`**. It is a research view until the data is right. This is the same phase-5 spend ADR-016 makes contingent on the gate.

**Consequences.**

- Marginal cost is small: one more per-ticker-per-day FastMCD fit, on an existing code path, over a cheaper coordinate space than the embedding.
- It is the **first non-price information in the system**. That is the point, and it is also the risk: everything about its value depends on data this project does not yet own.
- What it does not buy: a cross-sectional valuation geometry. Comparing yields *across* names requires a sector-normalization decision (a software company's steady-state `E/P` is not a utility's), and making that decision badly would produce a figure that looks informative and encodes only industry membership. Deferred until View D has been measured on its own.
- Fundamentals quality has no free two-source check. ADR-015's two-source price validation has no analogue here; SEC XBRL is authoritative but its tags are applied inconsistently across filers, so per-field coverage becomes a reported dataset statistic in the manner of ADR-016.
- With an estimated `available_date`, any edge measured from View D is **not evidence**. This ADR is explicitly build-the-machinery-now, buy-the-data-later; the machinery is testable without the data, the conclusions are not.

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
| Valuation | Earnings yield `E/P`; `EBITDA/EV`; free-cash-flow yield `FCF/P` (ADR-022 — point-in-time, and **not** cross-sectionally standardized; see §6.6) |

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
5. Liquidity/borrow feasibility on every leg;
6. *(ADR-022, OFF by default)* View D: the cheap leg is also cheap against its own valuation history. Separates "diverged because it got cheap" from "diverged because the business is deteriorating". Cannot be enabled while the run manifest reports `fundamentals_availability = "estimated"`.

Exit: `|z| < z_exit` (default 0.5), or horizon stop at 3× half-life, or hard adverse-excursion stop.

---

### 6.6 Valuation geometry (View D)

Fundamentals are a two-date table: `period_end` is the fiscal period the figures describe, `available_date` is when they were published. For a simulated day `D` and ticker `i`, let `F(i, D)` be the row with the greatest `period_end` among those satisfying `available_date <= D`. Rows are never read by `period_end` alone.

Valuation coordinates, all inverted so they stay bounded and sign-continuous through zero:

```
v(i, D) = ( E(i,D) / P(i,D),  EBITDA(i,D) / EV(i,D),  FCF(i,D) / P(i,D) )
```

with numerators from `F(i, D)` and denominators from day `D`'s price and enterprise value, so `v` moves every session even though `F` steps quarterly. `EV = market cap + total debt − cash & equivalents`, all balance-sheet terms from `F(i, D)`.

View D is then View B's construction on `v` instead of on `Ỹ(t)[i]`: per equity, FastMCD (ADR-011) over the trailing `L = 756` days of `v(i, ·)`, strictly causal, scored as signed normalized boundary distance with a chi-squared p-value. Depth is *cheapness relative to this name's own valuation history* — negative inside its normal range, positive outside it.

Unlike §6.3's feature vector, `v` is **not** cross-sectionally standardized. Standardizing it across names would convert it into a statement about industry membership (see ADR-022, Consequences).

**Enterprise value can cross zero, and the two ways it does are not the same problem.**

- `0 < EV ≪ mcap` — the coordinate becomes very large. **Left alone.** View D fits each ticker's own history with FastMCD, whose entire purpose is to ignore a minority of extreme points, so a brief episode of an enormous yield is precisely the contamination the estimator exists to absorb. Clamping the denominator to keep the number tidy would be the eigenvalue-floor mistake in a new location.
- `EV <= 0` — the coordinate flips **sign**, and robustness does not help: a sign-flipped value is not an outlier, it is a different quantity pointing the wrong way, and no breakdown point recovers from including it. That ticker-day carries **no valuation coordinate at all** — not floored, not imputed, not silently zero — and the exclusion is counted and published, the way ADR-016 already handles price retrievability. Per-status counts (`ok`, `no_fundamentals_available`, `non_finite_input`, `non_positive_market_cap`, `non_positive_enterprise_value`) go into the `gm-features` manifest so coverage is a reported number rather than something a reader infers from missing rows.

**Price basis: unadjusted close, never `adjclose`.** Market capitalisation is `close(i, D) × shares_outstanding(i, D)` using the *unadjusted* close. This is the easy mistake to make here, because every other feature in `gm-features` is built from `adjclose`: `adjclose` is back-adjusted for splits and dividends while a reported share count is not, so multiplying the two yields a market cap wrong by every split since the filing. Everything above is an aggregate dollar figure over an aggregate dollar figure — dollar earnings against dollar market cap — so no per-share adjustment basis has to be reconciled at all.

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
| **SEC XBRL Company Facts** | `data.sec.gov/api/xbrl/companyfacts/CIK##########.json` | Free | Authoritative fundamentals; descriptive `User-Agent` header required. **Carries a `filed` date on every fact**, so `available_date` is reported rather than estimated (ADR-022, §6.6) — measured, not assumed. Note it reports no Q4: `fp` runs Q1/Q2/Q3/FY, so trailing-twelve-month figures must be constructed by roll-forward, never by summing four quarterly entries. Parsed with nlohmann/json, matching the existing SEC readers in `gm-signals` and `gm-profiles` rather than introducing simdjson for one call site. |
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
  geometry.parquet     date, ticker, x, y, z, dim3..dim{k-1},
                       cluster_id, mst_degree   (x/y/z ARE dims 0/1/2;
                       columns past the third appear only when
                       geometry.embedding_dims > 3)
  edges.parquet        date, ticker_a, ticker_b, distance, in_mst
  fundamentals.parquet ticker, period_end, available_date, net_income_ttm,
                       ebitda_ttm, free_cash_flow_ttm, total_debt,
                       cash_and_equivalents, shares_outstanding
                       (gm-ingest; opt-in via ingest.fetch_fundamentals)
  valuation.parquet    date, ticker, earnings_yield, ebitda_ev_yield,
                       fcf_yield  (gm-features; each coordinate
                       independently present or absent - §6.6)
  surfaces/            boundary meshes (versioned binary, §8.3)
    {date}_A.gmmesh              View A: the market's envelope that date
    {date}_B_{ticker}.gmmesh     View B: one ticker's own tube
  scores.parquet       date, ticker, view, estimator, depth, pvalue, inside
                       view is "A" (market cross-section), "B" (one name's
                       own embedding history) or "D" (one name's own
                       valuation history, §6.6); depth is a SIGNED margin,
                       distance minus the critical distance, so negative
                       means inside. Which views a run scored is in the
                       manifest as views_scored.
  excursions.parquet   ticker, start, end, peak_depth, reverted, had_earnings
  spreads.parquet      date, ticker, z, half_life, basket weights
  regime.parquet       date, structural_change, market_eigenvalue_share, vix
  backtest/            trades.parquet, equity_curve.parquet, tearsheet.json
  meta/profiles.json   per-ticker description, SIC, links (learn panel)
  report.html          gm-report output
```

### 8.2.1 Accounting tag resolution, measured

EBITDA and enterprise value are not XBRL concepts. They are assembled from
tags that different filers use differently, and the assembly rules below
were derived by downloading companyfacts for 40 S&P 500 issuers sampled
across the alphabet and counting, not from reading the taxonomy.

Per-issuer derivability of each coordinate:

| Yield | Derivable | Blocked by |
|---|---|---|
| E/P | 40/40 | — |
| FCF/P | 39/40 | APA reports no capex tag |
| EBITDA/EV | 29/40 | 6 issuers have no operating-income subtotal (C, COP, DHI, EMR, FOX, STT), 5 no reachable long-term-debt tag (AKAM, BRK.B, DHI, GM, TTD), 1 no cash tag |

Three rules follow, and each exists because its absence produces a wrong
number rather than an error:

1. **Availability is per COORDINATE, not per row.** Six of the eleven
   EBITDA/EV misses are banks and similar, whose income statements do not
   have an operating-income subtotal at all - a structural fact, not a data
   gap. An all-or-nothing rule would discard their E/P and FCF/P too, for
   28% of the sample.
2. **A partial component sum is refused.** Where no depreciation-and-
   amortisation aggregate is reported, the two components are added
   together; if only one is present the concept is absent. Using
   `us-gaap:Depreciation` alone - which an earlier version of the chain did,
   for 3 of 12 issuers in the first real run - omits amortisation and
   understates EBITDA silently.
3. **Absence means zero only for genuinely optional concepts.** No
   `ShortTermInvestments` tag means the issuer holds none; no `cash` tag is
   a gap. Every substituted zero is counted, split between "this filer
   reports none" and "not published by this date yet".

Per-ticker-DAY coverage is lower than per-issuer coverage, because a
concept can resolve for an issuer and still not be published as of an early
date. Measured on a real run, among ticker-days that have a market
capitalisation on the full 98-issuer run: E/P 94.6%, FCF/P 77%,
EBITDA/EV 53%, plus 4431 days excluded for a non-positive enterprise
value.

Each field takes the most recent figure for its period **or any earlier
period** that was public by the row's own `available_date`. This introduces
no look-ahead - the availability cutoff is unchanged and only the period
relaxes, backwards - and it is what an analyst reads off the latest filing
to hand. It raised FCF/P from 64% to 94% of ticker-days and EBITDA/EV from
39% to 57%, by fixing an artifact rather than a shortage: net income is
re-reported as a comparative in nearly every later filing and so has many
more vintages than capex does, and those extra anchors previously produced
rows carrying net income and nothing else.

**Measured collinearity of the two default axes.** E/P and FCF/P share a
denominator, so between filings they are exactly proportional. Over 25806
real windows the median absolute correlation between them is **0.87**, the
90th percentile is **0.998**, and **17%** of windows exceed 0.99. The 754
windows (2.9%) that Mahalanobis and FastMCD both refuse to fit ARE that
collinearity - Mahalanobis names it, reporting a near-singular covariance
with points degenerate or collinear in some dimension. KDE, which inverts
nothing, scores all of them. So View D on its default axes is close to a
ONE-dimensional view, and a genuinely independent second axis means
EBITDA/EV - whose denominator is enterprise value rather than market cap -
at 53% coverage instead of 77%.

**View D therefore defaults to fitting in two dimensions**
(`earnings_yield`, `fcf_yield`). A boundary is fitted in one space, so
every point must carry every configured axis; adding EBITDA/EV does not
enrich the fit so much as shrink the cross-section it is fitted to. The
count of ticker-days that costs is published either way.

---

### 8.3 Surface naming and what each surface means

The two boundary views of §6.4 produce two different SHAPES, and pairing
either with the other's points would be a category error - so they are
named apart on disk and the viewer picks by what is currently drawn.

| File | Fitted to | Drawn when |
|---|---|---|
| `{date}_A.gmmesh` | every ticker present on `date` | the viewer is showing one date's whole market |
| `{date}_B_{ticker}.gmmesh` | that ticker's own trailing `view_b_lookback_days`, **excluding `date` itself** | the viewer is following that ticker through time |

**What shape View B comes out as depends on the lookback, and at the §6.4
default it is not a tube.** The measure is the mesh vertices' principal
extents, longest:middle - the ratio that separates a cigar from a
flattened disc. Longest:shortest does NOT separate them, because a
pancake scores just as high on it; an earlier version of this section
used that ratio and drew the wrong conclusion from it.

At the 756-day default, one AAPL surface measured 1.85 longest:middle
against View A's 2.06 on the same date - i.e. **less** elongated than the
market envelope it is being contrasted with.

At a 21-day lookback, across 69 dates spanning 2010-2026 (every 60th
exported surface):

| Ticker | median | p25 | p75 | max | fraction >= 2.0 |
|---|---|---|---|---|---|
| AAPL | 2.39 | 1.85 | 3.32 | 31.4 | 68% |
| NVDA | 2.42 | 1.95 | 3.22 | 12.4 | 70% |
| XOM  | 2.23 | 1.68 | 2.96 | 11.6 | 65% |

So roughly **two dates in three** produce a visibly elongated surface at a
one-month lookback, and one in three does not.

The reason is a fact about the data rather than about the code: a name
does not travel along a curve, it **wanders and revisits**. Over a month
in which it trended, the trailing cloud is nearly one-dimensional and its
envelope is a tube. Over a month in which it chopped sideways, the cloud
fills a region and so does the envelope. Over three years it always fills
a region. **The elongation is therefore itself a readout** - a tube means
the name has been moving directionally, a blob means it has been
oscillating - which is information the scores do not separately report.

Two further shapes show up and are not defects:

- **Disconnected lobes.** A KDE level set has no obligation to be one
  connected piece. Around the COVID crash AAPL's 21-day surface breaks
  into several, because its last month genuinely occupied several
  separate regions - pre-crash, in transit, post-crash. A single ellipsoid
  cannot represent that at all, which is the argument for the KDE
  estimator existing alongside the Mahalanobis one (ADR-007).
- **A current point on or outside the boundary.** The window excludes its
  own date, so this is the `inside` column being false, drawn.

Choosing `view_b_lookback_days` is choosing between two different
questions - "unlike its own last month" vs "unlike its own last cycle" -
not two resolutions of one.

Three properties follow from the definitions and are easy to misread as
faults:

1. **A View B tube's own date is not in its training set.** The current
   point can therefore sit *outside* its own tube, and when something
   interesting is happening it does. That is the finding, not a rendering
   error - it is the visual form of the `inside` column being false.
2. **View B surfaces are opt-in per ticker**
   (`boundaries.view_b_mesh_tickers`) and exported on a stride
   (`view_b_mesh_stride`). 81 names x 4129 dates is roughly a third of a
   million files, which is not a default. A View B mesh is also about 30x
   the work of a View A one at the same resolution, because it is fitted
   to ~756 training points rather than ~81.
3. **The viewer snaps backwards, never forwards.** Asked for a tube on a
   date that has none, it draws the newest one at or before that date and
   names the date it used. Snapping forward would put a surface fitted to
   not-yet-available data on screen - the look-ahead ADR-011 forbids in
   the scores, and no more acceptable in a picture.

At `embedding_dims > 3` a surface is necessarily the first-three-dimension
shadow of a k-dimensional fit, and is therefore **not** the boundary the
scores refer to; the manifest records `mesh_dims` alongside
`embedding_dims_scored` and carries a `mesh_projection_note` saying so.

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
2. Scoring dimension — display is 3; whether scoring uses 3/5/10 is a phase-3 sweep. **Partially informed:** the pipeline now genuinely scores in *k* dimensions rather than silently in 3, and a k=10 run over the full panel completes in the same order of time as k=3 (geometry 6.8s; boundaries scoring materially slower but tractable). One measured cost: at k=10, FastMCD declines to fit one frame in the whole 2010-2026 panel (2014-04-16, all 81 tickers), where at k=3 it declines none — the h-subset covariance is 10x10 and no subset of that frame yields a non-singular one. The other two estimators score it normally.
3. Within-sector vs cross-sector fits — likely different reversion characteristics; test both.
4. Short-side borrow feasibility — footnote in phase 1; becomes a constraint if the book is short-biased.
5. Market-mode removal for View C — help or hurt? Both `C*` and `C_res` retained until settled.
6. MSVC↔GCC bit-reproducibility — same platform reproduces bit-identically (guaranteed); cross-platform is tolerance-based; goldens are per-platform if needed.
7. Quarterly shelving in View D — **measured, and it matters more than "shelving" suggested.** The shelves are not merely steps: between filings *every* numerator is constant while the shared denominator (market cap) moves, so E/P and FCF/P are exactly proportional within a quarter and each shelf is a RAY through the origin rather than a plateau. A 756-day window is a fan of about twelve such rays, and it collapses toward a line whenever the cash-flow-to-earnings ratio is stable across them. Over 25806 real windows the median absolute correlation between the two axes is 0.87, the 90th percentile 0.998, and 17% exceed 0.99; 2.9% are singular enough that Mahalanobis and FastMCD both decline to fit while KDE, which inverts nothing, does not. So the answer is not a longer window — a longer window adds more nearly-parallel rays. It is a second axis with a DIFFERENT denominator, which means EBITDA/EV, at 53% ticker-day coverage against 77%. The run publishes the correlation so the choice is made on numbers.
8. Cross-sectional valuation geometry — a valuation View A needs a sector normalization that ADR-022 deliberately does not choose. Revisit once View D has been measured on its own.

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
