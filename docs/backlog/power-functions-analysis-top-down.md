# Power functions — top-down build spec

> **Forward-looking design document — exception to CLAUDE.md present-tense rule.** Stage 2 of the power-functions work: turns the [bottom-up catalog](power-functions-analysis-bottom-up.md) into an implementable spec — homes, types, signatures, migration order, tests, budgets. Written 2026-08-06 against the nine product-owner decisions recorded there. Where this document makes a NEW decision it is marked **(proposal)** and listed in § Decisions for sign-off. Companion boundary: the [livescripts top-down](livescripts-analysis-top-down.md) owns the MoonLive *engine* (grammar, IR, codegen); this document owns the *builtin surface* the engine calls into.

## TL;DR

- **The set lands in the existing homes, grown — not a new parallel library.** `core/math16.h` (new: the 16-bit contract tier), `core/noise.h` (grows fbm/warp/16-bit sampling), `light/draw.h` (grows splat, AA line, circle, rect/bar, scroll, the SDF trio), `light/particles.h` (new: the particle kernel), `light/polar.h` (new: the polar/kaleido LUT, modifier-first), `light/Palette.h` (grows cosine palettes + gamma). One style: same free-function shape `draw::` already has, same fixed-point vocabulary everywhere **(proposal)**.
- **Three shared types carry the whole contract:** `pos_t` = `int32_t` positions in **24.8 sub-pixel fixed point** (±8M pixels — covers a 16K-light strip where WLED-PS's int16 cannot; one word on every 32-bit target); `angle16` = `uint16_t`, 65536 = full turn; `frac16` = `uint16_t` 0..65535 fractions. Velocities are `int16_t` 8.8 per frame. The 8-bit tier (`math8.h`) stays as the internal fast path and for inherently mod-256 domains (palette index, hue) **(proposal)**.
- **The 22-effect boilerplate dies with one struct:** `draw::Canvas{buf, dims, cpl}`, returned by `EffectBase::canvas()`. Taken as `const Canvas&` (measured: a non-const reference costs ~3% more instructions in a tight per-pixel loop, because the extents become memory re-loads the compiler cannot hoist past a possible alias with the buffer; passing dims by value avoids that today). The gain is **correctness, not speed**: buffer and dims are currently two independent arguments nothing checks for agreement, and the pairing becomes unrepresentable-if-wrong — plus the 16 `depthDim()` copies are deleted rather than centralised, and `splat`/SDF-coverage/projection get a home for their context instead of adding loose parameters at every call site. The existing `(Buffer&, dims)` overloads remain during migration and their removal is **mandatory, not aspirational** — a permanent two-API window is worse than either option alone **(proposal)**.
- **`particles` is a pool the effect owns, not a module:** SoA arrays in ScratchBuffer, allocated at `prepare()`, semi-implicit Euler `step()`, the named forces (`gravity/force/drag/bounce/attract`), two emitters, optional binned collisions, rendered through the sub-pixel `splat`. Sized by the effect; zero static RAM when unused.
- **Noise: keep value noise, widen it — gradient noise is a swap-in upgrade, not a blocker.** `noise16(x,y,z)` returns full-range 16-bit (our existing value noise rescaled and interpolated up); the name promises the *field*, not the algorithm, so Perlin gradient noise can replace the core later without touching any caller **(proposal)**.
- **Migration order is by leverage, cheapest risk first:** ① `beatPhase` + `map16` + `Canvas` (mechanical, pixel-identical, kills the three biggest hand-roll counts) → ② geometry + bars (4 audio effects) → ③ `splat` + `particles`, converging the five particle-shaped effects (bench-judged, the PS-replaces-twin decision) → ④ fields + polar (LavaLamp/Metaballs/Rings/Spiral) → ⑤ hidden-modifier extraction as encountered (FreqSaws `invert` first). Each pixel-identical claim is pinned by a **golden-frame test** (fixed seed, fixed time, byte-compare) — a new, small test harness capability.
- **MoonLive exposure is stage 3 and states only its requirements here:** a builtin table of ≥ 64 entries, typed multi-arg host calls (up to 6 args + return), the symbols `x/y/z/w/h/d/time` (already threaded to the runtime, unexposed), and a per-frame entry point alongside the per-pixel one — the bottom-up's feasibility math says scripts *compose* kernels per frame; they do not interpret per pixel on large surfaces. The calling convention itself belongs to the livescripts engine work.
- **Budgets are stated per family and gated:** the render loop's ceiling stays the bottom-up's 293 cycles/pixel at 128×128@50; the particle budget is ~40 cycles/particle/frame (2048 particles ≈ 0.34 ms at 240 MHz); every function gets a host micro-benchmark and the migrations ride the existing `collect_kpi` gate. Zero static RAM for everything unused (`check_footprint`).

## 1. Homes and style (proposal)

CLAUDE.md's rule is extend-don't-duplicate, and the PO's one-codebase decision demands a single style. Both are satisfied by growing the existing homes with one consistent convention rather than opening a parallel `fx::` library:

| Home | Gains | Notes |
|---|---|---|
| `core/math16.h` **(new)** | `sin16/cos16` — 130-byte quarter-wave 16-bit table + lerp (0.031% error; the zero-table variant was tried and rejected — see §6), `triwave16/quadwave16/cubicwave16`, `ease16InOutQuad/Cubic`, `map16/map32` (fencepost-safe), `isqrt32`, `dist16`, `scale16`, `BeatPhase` (the stateful uint64 accumulator, `phase(bpm, ms)` → `angle16`), `beatsin16` rebuilt on the LUT+lerp sine | The contract tier. `math8.h` is unchanged and becomes internal/domain-specific (palette index, hue). |
| `core/noise.h` | `noise16(x[,y[,z]])` full-range, `fbm16(p, octaves)`, `warp16` (the one composition rule) | Same 16.0 fixed coordinate convention it already has. |
| `light/draw.h` | `Canvas`, `splat` (24.8 sub-pixel Wu write, the PS/WLED weight math with the inverse-gamma note), `lineAA` (Wu 1991), `circle/fillCircle` (midpoint), `rect/fillRect/bar`, `scroll(axis, delta, wrap)`, `sdCircle/sdBox/sdSegment + smin` + `coverage(d)` AA helper | Free functions, `Canvas&` first arg — the `draw::` shape it already has. |
| `light/particles.h` **(new)** | `particles::Pool` (SoA over ScratchBuffer), `step`, `gravity/force/drag/bounce/attract`, `spray/angleEmit`, `collide` (x-binned, optional), `render(Canvas&, palette)` | The industry name (Reeves 1983). Fire2012-style heat, Elias ripple, and the CA step are siblings in the same header — stateful field kernels. |
| `light/polar.h` **(new)** | `PolarLut` (per-pixel r,θ baked at prepare), `kaleido(n)` fold | Modifier-first per the PO decision; effects may consume the LUT read-only. |
| `light/Palette.h` / `core/color.h` | `cosPalette` (Quilez 12-constant, baked to the existing 16-entry `Palette` on change), `gamma8` LUT | `colorFromPalette` stays the one hot-path seam. |

Every function's doc block names its canonical source — the convention the effects already follow.

## 2. Types (proposal)

- **`pos_t = int32_t`, 24.8 fixed point.** One pixel = 256 sub-units. Chosen over WLED-PS's int16+6-bit (±512 px — too small for a 16K 1D strip) and our ParticlesEffect's 12.4 (±2048 px — same problem). int32 is single-word on every target; `>>8` decodes; the sign-corrected shift idiom from the bottom-up applies (never bare `>>` on negatives).
- **`angle16 = uint16_t`**, 65536 = full turn; overflow is the free 2π wrap. The 8-bit angle survives only inside `math8.h`.
- **`frac16 = uint16_t`** 0..65535 for interpolation/easing inputs and outputs.
- **Velocity `int16_t` 8.8 per frame**; forces via the 3.4 accumulator (the WLED-PS smooth-sub-unit trick, reimplemented fresh).
- **Time**: `elapsed()` ms as today; `BeatPhase` owns the uint64 numerator-divide-late idiom the nine effects hand-roll.
- **Dimension-generic rule**: every geometry/field function takes `Coord3D`; 1D/2D degenerate by extent (the `draw::blur` model — one call, every axis with extent > 1).
- **Dimension audit (verified against the dimension-generic decision):** fully generic by construction — frame ops, pixel ops (`splat` = 2/4/8 corners for 1D/2D/3D), fields (`noise16` has all three arities), time/color/random, and the particle kernel (SoA per axis — one system where WLED-PS maintains two; 3D collisions correct, x-binning just less selective). The SDF trio is the strongest case: `|p|−r` IS two points / circle / sphere, one formula. Five named 2D-primary items, each with its path: `lineAA` (3D = splat along the 3D Bresenham line — falls out of the generic splat), `text` (glyphs are 2D; renders a z-slice on 3D fixtures, meaningless in 1D), `PolarLut`/`kaleido` (cylindrical/spherical variants wait for a consumer), `angleEmit` (3D needs the spherical two-angle form), `ripple`/fire (volumetric variants wait for a consumer). None is a blocker: the pipeline already lifts lower-dim output via `Layer::extrude()`, so a 2D-primary function stays usable on every fixture, like today's 2D effects.
- **Fixed point is the default and invisible (standard approach, PO decision):** an effect writer works in `pos_t`/`angle16`/`frac16` and the power functions, and never chooses a width or a representation per case — the vocabulary IS fixed point. The only per-case judgment left is effect-private math outside the power functions, already governed by the existing rule: per-frame float allowed, per-light float not ([coding-standards § numeric types](../coding-standards.md)).

## 3. The particle kernel

```
particles::Pool pool;                    // POD view over ScratchBuffer arrays
pool.init(scratch, count);               // at prepare(): SoA x/y/z, vx/vy/vz, ttl, hue — no allocation later
pool.gravity(g);                         // one dv per frame, applied to all (branch costs more than work)
pool.force(i, fx, fy);                   // 3.4 accumulator per particle
pool.drag(k);                            // v *= (256-k)/256
pool.step();                             // semi-implicit Euler: v += a; x += v  (Fiedler)
pool.bounce(e, roughness);               // reflect at walls, v = -(v*e)>>8; roughness scatters
pool.attract(p, strength);               // inverse-square, near-field clamped
pool.spray(emitter); pool.angleEmit(emitter, angle16, speed);
pool.collide();                          // optional; x-binned broad phase, impulse response
pool.render(canvas, palette);            // sub-pixel splat per live particle; ttl fades brightness
```

**Defaults (standard approach, PO decision):** `render()` composites **additively with saturation** (light adds; hue-preserving rescale on overflow, never clip-to-white) through the **sub-pixel splat** — the effect writer gets both without deciding. Case-by-case is opt-OUT: `RenderStyle::Hard` for single-pixel retro rendering, nothing else to choose. Trails are deliberately NOT a pool feature: decay stays the one existing mechanism (the collected `fadeToBlackBy`), so the system has a single decay path rather than a second one hidden inside particles.

Costs (from the bottom-up's measured prior art): step ≈ 6 ops/axis, splat ≈ 4 mul + 4 saturating adds, collide only when enabled. Budget ~40 cycles/particle/frame without collisions. Pool size is the effect's choice against its ScratchBuffer — the pay-for-what-you-use rule; nothing static.

The five converging effects and what each pins: Particles (12.4 → 24.8, wall bounce), BouncingBalls (analytic float → Euler + restitution — the named non-identical case, bench-judged), StarField (perspective divide stays effect-side; the pool carries state), StarSky (SoA aging = ttl), Tetrix (state machine keeps its logic, positions ride `pos_t`).

## 4. MoonLive requirements (stage 3 — stated, not built here)

What the builtin surface needs from the engine, recorded for the livescripts work:

1. Builtin table ≥ 64 entries (today 16).
2. Typed multi-arg host calls, ≤ 6 args + optional return (today: one `uint32_t` in, one out — `drawLine` is inexpressible).
3. Script symbols `x/y/z/w/h/d/time` — already threaded to the runtime entry point, needs only grammar exposure.
4. **Two entry shapes:** `frame()` (compose kernels — the scalable path per the 293-cycles/pixel math) and `pixel(x,y,z)` (the PixelBlaze-ergonomics path, honest ceiling ~32×32 interpreted). Scripts choose; large fixtures use `frame()`.
5. Stateful objects (a `Pool`, a `BeatPhase`) exposed as *handles* — script-declared, arena-allocated at compile, passed as an opaque first arg. No script-side memory management.

Until the ABI lands, stages 1–2 proceed compiled-side; nothing here blocks on the engine.

## 5. Migration plan (stage 1) and example effects (stage 2)

Order by leverage, cheapest risk first; every batch lands with its tests and the branch stays under ~100 files:

1. **Foundations** — `math16.h`, `Canvas`, `BeatPhase`, `map16`: mechanical replacement in the 9 phase-accumulator effects, the 6 `imap` copies, the 22 preambles, the 16 `depthDim()`s. Pixel-identical (same arithmetic, one home) → golden-frame pinned.
2. **Geometry** — `bar/rect` into the 4 audio meters; `scroll` into FreqMatrix; `splat` lands with its unit tests.
3. **`particles`** — the kernel + the five convergences, one effect per commit, bench-judged (PS-replaces-twin decision); the old private representations deleted.
4. **Fields + polar** — the shared blob oscillator (LavaLamp ≡ Metaballs) onto `sin16`+`splat`; Rings/Spiral onto `PolarLut`; `noise16` under Noise2D with a rescale note.
5. **Hidden-modifier extraction** — FreqSaws `invert` → MirrorModifier; audit the rest as they migrate (the effects-vs-modifiers decision).

**Golden-frame harness (new, small):** render N frames at fixed seed/fixed `elapsed()` into a buffer, hash, compare against a checked-in golden. Only for effects claiming pixel-identical; a deliberate divergence replaces the golden in the same commit with the bench note. Lives beside the existing effect tests.

### Stage 2 — new showcase effects

The goal is **beautiful effects, not conditioned ones** (PO decision): each showcase leans on the toolbox for its heavy lifting — the named power functions carry the effect's core mechanic — and is otherwise free to add any effect-local code that makes it better. That is the coverage proof (the library did the hard part) and the reference value (a writer sees the functions in real use), without a purity rule that would make an effect worse to keep a list clean. New showcases exist only where stage 1's migrations do not already exercise a family; everything else is proven by the rewrites themselves.

| Effect | Showcases | Power functions exercised |
|---|---|---|
| `FireworksEffect` | **particles** — the full kernel in one look | `Pool`, `spray`/`angleEmit`, `gravity`, `drag`, ttl fade, sub-pixel `splat`, additive default |
| `BallpitEffect` | **particles collisions** — the piece Fireworks leaves off | `collide` (binned, impulse), `bounce` + wall roughness, `force` tilt via controls |
| `SdfShapesEffect` | **the shader look** — anti-aliased morphing shapes | `sdCircle/sdBox/sdSegment`, `smin`, `coverage` AA, `cosPalette`, `beatPhase` |
| `PolarNoiseEffect` | **the Petrick idiom** — the named coverage target | `PolarLut`, `fbm16`, `warp16`, `colorFromPalette`, per-target headroom |
| `WaterRippleEffect` | **the field kernels** — a true propagating simulation (the existing `RipplesEffect` is closed-form) | `ripple` (Elias two-buffer), `splat` drops, `blur` |
| `RaymarchEffect` *(desktop + small-fixture tier)* | **the ceiling clause made visible** — a raymarched 3D SDF scene (rotating smooth-min blobs, soft shadows, the Quilez canon) | the SDF *concepts* in 3D (raymarch loop is effect-local float, as any showcase may be); `cosPalette`; gated on a `hasHeavyCompute` platform constant (the `hasNetwork` pattern), and reachable on a 16×16 ESP32 panel too at 15,600 cycles/pixel — streamable to a real wall via NetworkSend |
| `TunnelEffect` | **the gather primitive** — the structural gap the canon survey found; one effect proves the whole texture-mapping third of the canon | `sampleWrap` (G1), `mat23` (G3) for the per-frame rotation, `PolarLut`, ping-pong buffers, `cosPalette` |
| `EchoEffect` | **feedback composition** — that feedback is 3 lines once gather exists, not a primitive (the survey's own argument, made visible) | `sampleWrap` + `fade` + `combine` (G2, screen/max op), ping-pong swap convention |
| `VectorBallsEffect` | **projection + filled geometry** — a rotating 3D object, the classic demoscene proof | `project` (family 9), `depthSort`, `fillTriangle` (G6), `lineAA`, `circle/fillCircle`, `mat23` |
| `SpectrumEffect` | **the audio primitives** — replaces GEQ's hand-rolled meter machinery with the real ballistics | asymmetric envelope (G4), `peakHold`, `smoothFollow`, `map8_to_16`, `bar`; beat-locked motion via the audio service's onset/PLL (G5) |
| `DissolveEffect` | **stateless randomness + dithering** — a transition carrying zero per-pixel state | `hashInt` (position-addressable), `bayerDither` (G7), `easeInOutQuad` (Penner), `gamma8` |

Attached to the effects above rather than earning their own: `worley` (G8) is a palette-mapped field in `PolarNoiseEffect` alongside `fbm16`; `attract` joins `BallpitEffect` (an attractor well the balls fall into); `kaleido` joins `TunnelEffect` (the same polar LUT, folded); `quadwave/cubicwave` and `isqrt/dist16` are used wherever they are the cheaper shape, not showcased for their own sake.

Families with no new effect, deliberately: frame ops, geometry bars, time/motion and color are exercised by the stage-1 migrations (the audio meters, the nine `beatPhase` conversions, the 27 palette users); the CA kernel already has `GameOfLifeEffect`; `text` has `TextEffect`. A showcase that duplicates a migration would not earn its place.

**How far each type goes (coverage vs limits, stated up front):**

- **Particles**: everything the WLED-PS canon expresses (32 effects' worth of emitters/forces/collisions/fire) is expressible. Ceilings: *count* — thousands on ESP32 (~40 cycles, ~16 B each; 2048 ≈ 0.34 ms/frame), far more on desktop, never GPU-class millions; *deferred families* — constraint chains (Verlet/Jakobsen rope-cloth) and boids wait for a consuming effect; WLED-PS's per-particle size/wobble renderer is covered by SDF-circle glow instead of a second render path.
- **Petrick idiom**: fully expressible (polar + layered warped noise + palette). The limit is per-pixel budget, not vocabulary: 5–10 field samples/pixel is full-rate on ≤32×32 classic, medium sizes on S3, uncapped on desktop — but a 128×128@50 wall affords ~1 sample/pixel. Animartrix itself is FPU-bound to Teensy/S3-class at moderate sizes; the escape hatches are half-resolution field + upscale (the virtual-layer downscale lever), a field rate below the render rate, or desktop headroom.
- **Shader look**: anti-aliased shapes, outlines, glow, smooth-min morphing — yes, everywhere; general Shadertoy — never via GLSL (it is composition of our kernels, not a transpiler), and on ESP32 **it depends on the fixture size, not on the chip**. The budget is per pixel, so it scales with pixel count (240 MHz, measured): **16×16@60 = 15,600 cycles/pixel** (raymarching, fractals and feedback all reachable — a small panel is a legitimate shader target), **32×32@60 = 3,900** (rich multi-sample fields), **64×64@50 = 1,170** (a few samples), **128×128@50 = 292** (one field sample + palette + blend). So an advanced shader effect is not "desktop-only" — it is *small-fixture-and-desktop*, and the same effect simply needs a bigger machine as the wall grows. An effect that wants both can scale its own sample count from `nrOfLights()`. Three SDFs ship (circle/box/segment); more of Quilez's catalog only with a consuming effect. **On desktop the ceiling clause applies**: thousands of cycles per pixel make raymarching, fractals and feedback genuinely reachable — `RaymarchEffect` is the named showcase, gated on a `hasHeavyCompute` platform constant, and desktop frames stream to physical fixtures over NetworkSend, so the heavy tier lights real walls, not just the preview.

## 6. Resource accounting (the minimalism audit)

Verified against CLAUDE.md § Principles and [architecture.md § Hot path discipline / § Core and light domain](../architecture.md). What the set costs, what it removes, and the gates that keep the balance visible:

- **Flash:** the 16-bit tier costs **130 bytes of table** plus code. The zero-table variant (interpolating the existing 8-bit `sin8_lut`) was implemented first and **rejected on measurement**: rounding the endpoints to 8 bits distorts the segments the interpolation runs between, giving 1.1% of amplitude — worse than the 0.69% it was supposed to beat. Measured against FastLED **master** (b2a1344): classic `lib8tion sin16` 0.69%; **ours 0.031%** (130 B); master's `fl::sin32` near-exact but 1040 B plus two int64 multiplies per call. 130 bytes for a 22x improvement over lib8tion is the minimalism call — and the estimate-then-verify order is the lesson: the first design's headline number was an unmeasured guess. New kernels (particles, geometry, SDF) add low-single-digit KB; the migrations *delete* the nine phase accumulators, six `imap`s, sixteen `depthDim`s, five private particle representations and the local `plot`/`triangle8` re-implementations, and PS-replaces-twin removes whole effect bodies (WLED's same move saved ~12 KB). **Gate: the per-target flash table in repo-health is read per migration batch; a batch that grows flash needs its reason in the commit.**
- **RAM:** everything sized is `prepare()`-time ScratchBuffer/`platform::alloc` (PSRAM-preferred), zero static — `check_footprint` enforces. Two honest costs, stated rather than hidden: a 2D particle at `pos_t` is ~16 B vs WLED-PS's 10 B (the price of addressing a 16K strip WLED's int16 cannot; pools are effect-sized, so small fixtures pay small); `PolarLut` defaults to **8-bit r,θ (2 B/pixel — 24 KB on 48×256)**, with the 16-bit variant (4 B/pixel) as an explicit opt-in — large fixtures require PSRAM already (`nrOfLightsType` gates on it).
- **Cycles:** per-light work is integer throughout (the fixed-point-default decision); budgets in § Testing; the KPI tick gate catches a regression at its cadence.
- **Repo:** golden-frame tests store **hashes, never frame blobs** — repo-health's size trend stays flat.
- **Boundary:** `math16`/`noise` are domain-neutral core (no light knowledge — "core primitives, not one-offs": each has many callers by construction); `draw`/`particles`/`polar` are light domain. No new mixing.
- **Complete construct, real consumer:** per architecture.md's surviving rule, each power function is built as the cleanest complete version (no crippled subsets) — and lands in the same PR as its first real consumer, so nothing ships speculatively: `beatPhase` is *extracted from* the nine effects that prove it.
- **Subtraction closes the loop:** after stage 1, `math8.h` keeps only entries with remaining callers (palette/hue and internal fast paths); superseded 8-bit forms and the temporary `(Buffer&, dims)` overloads are removed, and the five converged effects' private state code is deleted, not deprecated.

## 7. Testing and budgets

- **Unit**: every power function gets behavior-named tests (bounds, wrap, saturation, the fencepost cases the effects documented); the particle kernel gets the WLED-PS-derived edge list (tunneling lookahead, zero-distance pairs, sticky pile-up) implemented as behaviors, not ported assertions.
- **Golden frames** as above; **scenario**: one new scenario driving a particles effect through its controls (the live equivalent, same shape as the existing effect scenarios).
- **Perf**: host micro-bench per function (a small `bench_powerfunctions` target, numbers into performance.md); on-device via the existing `collect_kpi` gate per migration batch. Ceilings: 293 cycles/pixel composite at 128×128@50; ~40 cycles/particle; `sin16` ≤ 12 cycles; `splat` ≤ 30.
- **Footprint**: `check_footprint` zero-static for every family; pools and LUTs are ScratchBuffer/prepare-time only.
- **The final gate is the wall**: stage-1 batches 3–5 get judged on the big fixture, per the pixel-identical divergence clause.

## 8. Decisions for sign-off (new in this document)

1. Homes: grow existing headers + `math16.h`/`particles.h`/`polar.h`; no umbrella namespace (§1).
2. Types: `pos_t` int32 24.8, `angle16`, `frac16`, velocity 8.8 (§2).
3. `Canvas` + `EffectBase::canvas()`; old overloads subtracted after migration (§1, §2).
4. Noise: widen value noise to `noise16` now; gradient noise is a swap-in later (§ TL;DR).
5. Migration order and the golden-frame harness (§5, §7).
6. MoonLive requirement list handed to the livescripts work; stages 1–2 do not block on it (§4).
7. The resource accounting and its gates: flash read per batch, PolarLut 8-bit default, goldens as hashes, complete-construct-with-real-consumer, the math8 subtraction pass (§6).

Carried unchanged from the bottom-up: dimension-generic; one set everywhere without capping desktop; demo effects/pixel-identical-by-default; effects-vs-modifiers; Petrick coverage target; one codebase; `particles` naming; PS-replaces-twin; the 16-bit contract. Added on review (PO, 2026-08-06): **particle blending and fixed point are defaults, not per-effect decisions** — additive+splat rendering out of the box with a single opt-out, and the fixed-point vocabulary invisible to the writer (§2, §3).

## Out of scope (deferred to implementation)

Exact per-function signatures beyond §3's shapes (the PR is the spec); the MoonLive grammar/ABI design (livescripts work); GPU acceleration of the contract on desktop; boids/Verlet-constraints/Porter-Duff (below-the-cut list stands); palette-system changes beyond `cosPalette`; a public "effect SDK" doc page (falls out of the migrated effects + catalog when stage 2 lands).
