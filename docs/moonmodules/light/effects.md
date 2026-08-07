# Effects

Every effect, one block each: its preview, what it does, and what each control means — together. An effect writes per-pixel color into its [Layer](moxygen/Layer.md)'s buffer each tick; [modifiers](modifiers.md) reshape the result and a [driver](moxygen/PreviewDriver.md) sends it out. Effects that name an index color read the global palette (the `palette` control on [Drivers](moxygen/Drivers.md)) via `colorFromPalette`. Each block's emoji are its `tags()` (origin/creator/audio — see the [tag emoji legend](../../architecture.md#tag-emoji-legend)); **Dim** is its native axes ([Layer](moxygen/Layer.md) extrudes a lower-dim effect onto a bigger grid). Effects are grouped into sections by origin, and each block carries that effect's preview, behaviour, and control descriptions together. (For how this page maps to the source/asset folders, see the [folder-structure decision](../../adr/0015-library-is-a-tag-not-a-folder.md).)

Effects are built from the shared [power functions](power-functions.md) — the drawing, field and motion routines every effect composes; that page lists each one with its callers.

**Jump to:** [MoonLight](#moonlight-effects) · [MoonModules](#moonmodules-effects) · [WLED](#wled-effects) · [FastLED](#fastled-effects) · [projectMM-native](#projectmm-native-effects)

**Migrating an effect — behaviour is the spec.** A ported effect must reproduce the original's **exact** visual behaviour: end users have relied on these for years, so a port that looks different is a regression, not an improvement. Don't get creative with defaults, oscillator math, color mapping, or geometry, and don't silently drop a parameter that *is* the mechanism (the PaintBrush straight-vs-curved-lines bug was a dropped partial-line `length`; Game of Life was wrong the first time by not porting the real algorithm). Study the source for the algorithm, defaults, and visual result; pin it with unit + scenario tests; then write our **own** implementation against `EffectBase`/our primitives — carry the behaviour forward, don't trace or copy the structure (see [*Industry standards, our own code*](../../../CLAUDE.md#principles)). Credit the origin as prior art in the block below.

> Some WLED-origin effects show a preview gif from [WLED-Utils](https://github.com/scottrbailey/WLED-Utils) by scottrbailey (the canonical WLED effect gif set, cross-linked with credit); these show WLED's rendering. Effects with a local `../../assets/…` gif show our own output.

## MoonLight effects

<a id="distortionwaves"></a>

### DistortionWaves 💫 · 2D

Two interfering sine waves beat against each other into a moiré color field.

- `freq_x` / `freq_y` — horizontal/vertical wave frequency (1–8).
- `speed` — animation rate (0 = frozen).

Origin: WLED · by ldirko & blazoncek (WLED port) · [gallery](https://editor.soulmatelights.com/gallery/1089-distorsion-waves) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/DistortionWavesEffect.md)

[Tests](../../tests/unit-tests.md#distortionwaveseffect)

<a id="fixedrectangle"></a>

### FixedRectangle 💫 · 3D

A solid color filling a positioned box within the grid, with an optional alternating-white checker on the box's pixels.

- `red` / `green` / `blue` / `white` — the box color.
- `X position` / `Y position` / `Z position` — the box's origin corner.
- `Rectangle width` / `Rectangle height` / `Rectangle depth` — the box extent on each axis.
- `alternateWhite` — alternate box pixels to white in a checker pattern.

Origin: MoonLight · by [limpkin](https://github.com/limpkin) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/FixedRectangleEffect.md)

[Tests](../../tests/unit-tests.md#fixedrectangleeffect)

<a id="freqsaws"></a>

### FreqSaws 💫📊 · 2D

Audio-reactive sawtooth waves: each column maps to a frequency band whose magnitude drives a per-band oscillator speed, so louder bands sweep their sawtooth up the column faster, with three phase methods.

- `fade` — background decay per frame.
- `increaser` — how fast a band's speed ramps up with its magnitude.
- `decreaser` — how fast a silent band's speed decays.
- `bpmMax` — ceiling on a band's oscillation speed.
- `invert` — flip alternate columns vertically.
- `keepOn` — keep oscillating even when a band is silent.
- `method` — phase model (`Chaos`, `Chaos fix`, `BandPhases`).

Origin: MoonLight (audio) · by [@TroyHacks](https://github.com/troyhacks) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/FreqSawsEffect.md)

[Tests](../../tests/unit-tests.md#freqsawseffect)

<a id="lavalamp"></a>

### LavaLamp 💫🦅 · 2D

<img src="../../assets/light/effects/LavaLampEffect.gif" width="300" alt="LavaLamp effect preview">

Three slow blobs through a black→red→orange→yellow→white ramp — atmospheric lava look.

- `bpm` — blob drift speed.
- `radius` — blob influence radius.
- `intensity` — field gain into the black→red→orange→yellow→white ramp.

Origin: projectMM original (metaball lava lamp)

Detail: [technical](moxygen/LavaLampEffect.md)

[Tests](../../tests/unit-tests.md#spiraleffect)

<a id="lines"></a>

### Lines 💫 · —

<img src="../../assets/light/effects/LinesEffect.gif" width="300" alt="Lines effect preview">

Sweeps axis-aligned planes in sync; red/green/blue name the X/Y/Z axis — a preview-orientation test pattern.

- `speed` — sweep BPM.
- `axis` — which plane sweeps (`all`, `x (red)`, `y (green)`, `z (blue)`).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/LinesEffect.md)

<a id="metaballs"></a>

### Metaballs 💫🦅 · 2D

<img src="../../assets/light/effects/MetaballsEffect.gif" width="300" alt="Metaballs effect preview">

`count` blobs orbit via integer sin/cos; metaball field per pixel — bright HSV merge/split.

- `bpm` — orbit speed.
- `radius` — blob influence radius.
- `count` — number of orbiting balls (1–8).
- `hue_shift` — rotate the palette index.

Origin: projectMM original (metaballs)

Detail: [technical](moxygen/MetaballsEffect.md)

[Tests](../../tests/unit-tests.md#metaballseffect)

<a id="particles"></a>

### Particles 💫🦅 · 2D

<img src="../../assets/light/effects/ParticlesEffect.gif" width="300" alt="Particles effect preview">

A swarm of drifting particles with persistent fading trails.

- `count` — number of particles (1–255).
- `speed` — drift velocity.
- `fade` — trail persistence (higher = longer tails).
- `hue_shift` — rotate every particle's hue.

Origin: MoonLight · by WildCats08 / [@Brandon502](https://github.com/Brandon502) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/ParticlesEffect.md)

[Tests](../../tests/unit-tests.md#particleseffect)

<a id="plasma"></a>

### Plasma 💫🦅 · 2D/3D

<img src="../../assets/light/effects/PlasmaEffect.gif" width="300" alt="Plasma effect preview">

Summed sine waves on orthogonal + diagonal axes; large rolling blobs (3D on volumetric layouts).

- `bpm` — roll speed.
- `scale_x` / `scale_y` — blob size on each axis (larger = bigger, calmer blobs, lower spatial frequency).
- `hue_shift` — rotate the palette index.

Origin: FastLED / WLED lineage (classic plasma)

Detail: [technical](moxygen/PlasmaEffect.md)

[Tests](../../tests/unit-tests.md#plasmaeffect)

<a id="praxis"></a>

### Praxis 💫 · 2D

An algorithmic palette pattern driven by two beat oscillators (a macro and a micro mutator) whose frequencies and ranges reshape the hue field over time.

- `macroMutatorFreq` / `macroMutatorMin` / `macroMutatorMax` — the coarse mutator's beat frequency and its oscillation range.
- `microMutatorFreq` / `microMutatorMin` / `microMutatorMax` — the fine mutator's beat frequency and range.

Origin: MoonLight · by MONSOONO / @Flavourdynamics · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/PraxisEffect.md)

[Tests](../../tests/unit-tests.md#praxiseffect)

<a id="rainbow"></a>

### Rainbow 💫 · 2D

<img src="../../assets/light/effects/RainbowEffect.gif" width="300" alt="Rainbow effect preview">

Diagonal animated rainbow — always-visible default/test effect.

- `speed` — animation BPM (one full hue cycle per beat).

Origin: FastLED · Mark Kriegsman (rainbow) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_FastLED.h)

Detail: [technical](moxygen/RainbowEffect.md)

[Tests](../../tests/unit-tests.md#rainboweffect)

<a id="random"></a>

### Random 💫 · 3D

Lights one random light per frame in a random palette color over a fading background — a sparse, palette-tinted sparkle.

- `fade` — how fast prior sparkles fade to black.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/RandomEffect.md)

[Tests](../../tests/unit-tests.md#randomeffect)

<a id="rings"></a>

### Rings 💫🦅 · 2D

<img src="../../assets/light/effects/RingsEffect.gif" width="300" alt="Rings effect preview">

Expanding concentric rings from random centres, additive overlap (calm defaults).

- `count` — number of concentric rings (1–255).
- `speed` — expansion rate.
- `thickness` — ring band width.
- `hue_shift` — rotate every ring's hue.

Origin: projectMM original (concentric rings)

Detail: [technical](moxygen/RingsEffect.md)

[Tests](../../tests/unit-tests.md#spiraleffect)

<a id="ripples"></a>

### Ripples 💫🟦🦅 · 3D

<img src="../../assets/light/effects/RipplesEffect.gif" width="300" alt="Ripples effect preview">

Distance-from-centre sets a per-column wave phase; the lit surface ripples like water.

- `speed` — wave animation rate (0 = frozen, 99 = fast).
- `interval` — wavefront spacing (low = tight rings, high = wide).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/RipplesEffect.md)

[Tests](../../tests/unit-tests.md#spiraleffect)

<a id="rubikscube"></a>

### RubiksCube 💫🧊 · 3D

A 3D Rubik's Cube projected onto the volume: it scrambles, then plays its solution back one turn at a time, the six faces in their standard colors.

- `turnsPerSecond` — how fast the cube turns.
- `cubeSize` — the cube order (2×2 up to 8×8).
- `randomTurning` — turn endlessly at random instead of scramble-then-solve.
- `usePalette` — color the six faces from the system-wide palette instead of the classic Rubik's colors.

Origin: MoonLight · by WildCats08 / [@Brandon502](https://github.com/Brandon502) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/RubiksCubeEffect.md)

[Tests](../../tests/unit-tests.md#rubikscubeeffect)

<a id="fireworks"></a>

### Fireworks 🔬 · 2D

Shells rise, stall, and burst into sparks that arc over and fall. Every stage is a particle-kernel call: spawn, gravity, angleEmit, drag, age. Nothing schedules the apex — the shell decelerates under gravity and bursts when its vertical velocity crosses zero, so a faster launch bursts higher without a second control.

- `launchRate` — how often a new shell goes up.
- `launchSpeed` — how hard it is thrown, and so how high it bursts.
- `gravity` — how fast everything falls, per 60 Hz of simulated time.
- `sparks` — sparks per burst.
- `sparkLife` — how long a spark survives.
- `drag` — air resistance flattening the arc.
- `fade` — trail length (the Layer's decay, not the pool's).

Physics is driven by elapsed time, not frame count, so the same settings behave identically on a desktop at thousands of fps and an ESP32 at a few hundred ([architecture § tick rate](../../architecture.md#effects)).

Origin: projectMM original, on the WLED Particle System's firework family (@Brandon502 / WildCats08)

<a id="ballpit"></a>

### Ballpit 🔬 · 2D

Falling balls that pile up and shove each other aside. The heap is emergent: gravity pulls, the floor stops, and contact between neighbours produces the shape. `tilt` turns the pit into a slope and the whole pile slides and re-settles.

- `balls` — how many share the pit.
- `gravity` — how hard they fall.
- `size` — contact radius in pixels: how far apart balls sit when touching.
- `bounce` — restitution: how much speed a contact keeps.
- `tilt` — sideways force, turning the pit into a slope.
- `drag` — damping, so the heap settles instead of sloshing.

Exercises the half of the particle kernel [Fireworks](#fireworks) leaves untouched: sparks never notice each other, these do. Collisions are the one non-linear part of the kernel, so the pool is deliberately small.

Origin: projectMM original, on the WLED Particle System's ballpit family (@Brandon502 / WildCats08)

<a id="dissolve"></a>

### Dissolve 🔬 · 2D

Two color fields trade places pixel by pixel in an order that looks random but is computed, so the transition needs no per-pixel state and no shuffled index list. Two devices rendering the same frame dissolve identically without exchanging anything.

- `bpm` — how fast one transition completes.
- `spread` — how much of the transition pixels spend mid-flight; 0 gives a hard edge.
- `eased` — ease the progress instead of sweeping linearly.
- `scatter` — random order; off gives a positional wipe from the same code.

Origin: projectMM original, on the classic dissolve transition in its position-addressed (shader) form

<a id="echo"></a>

### Echo 🔬 · 2D

The previous frame fed back through a zoom and rotation, dimmed, with a bright source drawn on top — trails that spiral away from themselves, like a camera pointed at its own monitor.

- `bpm` — how fast the source orbits.
- `zoom` — how much the feedback grows each frame.
- `rotate` — rotation per frame, which turns the trail into a spiral.
- `decay` — how fast the echo fades; higher is a shorter trail.
- `size` — radius of the bright source.

Shows that feedback is not a primitive: once the grid can be read as a texture (`sampleWrap`), the whole family of trails, zoom blur and smear is a few lines.

Origin: projectMM original, on video feedback and the standard texture-feedback shader shape

<a id="spectrum"></a>

### Spectrum 🔬📊 · 2D

An audio analyser with real meter ballistics: bars rise fast enough to catch a transient and fall slowly enough to read, and a peak dot marks the recent maximum and drifts down.

- `attack` — how fast a bar rises toward a new level.
- `release` — how fast it falls back.
- `peakDecay` — how fast the peak dot drifts down.
- `showPeaks` — draw the floating peak dots.
- `colorByColumn` — color per band instead of by height.

The asymmetry is the whole point; a symmetric follower either misses the hit or flickers.

Origin: projectMM original, on standard VU/PPM meter ballistics and WLED's GEQ band mapping

<a id="truchet"></a>

### Truchet 🔬 · 2D

A maze of interlocking arcs that never repeats, drawn without storing a single tile. Randomly-turned tiles with arcs at their edges join into continuous winding paths across the whole surface — the pattern looks designed, and nothing designed it.

- `bpm` — how fast the pattern drifts.
- `scale` — tiles across the short side.
- `thickness` — how fat the arcs are.
- `softness` — edge softness: the anti-aliasing width.
- `shuffle` — reshuffles which way the tiles face.
- `drift` — slide the pattern instead of holding still.

**The representative 2D shader**, and a better introduction to the form than [Raymarch](#raymarch): no 3D, no rays, no float, cheap on any target. It shows the three moves most shader effects are built from — folding space so one tile becomes hundreds (`repeat`), deciding each tile's orientation from its position alone (`hashInt`, so no array remembers it and two devices agree without exchanging anything), and turning a distance into a soft edge (`smoothstep`).

Origin: projectMM original, on Sébastien Truchet's 1704 tiling and the standard shader fract/hash/smoothstep idiom

<a id="tunnel"></a>

### Tunnel 🔬 · 2D

A texture mapped onto the inside of an infinite tube, so the viewer appears to fly down it forever. Nothing is 3D: the angle around the centre is one texture coordinate and the reciprocal of the distance is the other, which is perspective for the price of a divide.

- `bpm` — how fast the tunnel flies past.
- `depth` — texture scale along the tunnel; higher is finer rings.
- `twist` — rotation per unit depth, so the tunnel corkscrews.
- `segments` — kaleidoscope the wall; 1 leaves it plain.
- `octaves` — wall texture detail, and the cost knob.
- `vignette` — darken toward the vanishing point so it reads as receding.

Origin: projectMM original, on the standard demoscene tunnel

<a id="waterripple"></a>

### WaterRipple 🔬 · 2D

A propagating wave simulation: drops land, their rings spread outward, reflect off the edges and interfere where they cross. The crossing is what a closed-form ripple cannot fake, because two rings meeting have to add and cancel.

- `speed` — simulation steps per second: how fast the water itself moves, independent of the framerate.
- `dropRate` — how often drops land, in time rather than per frame.
- `damping` — how fast waves lose energy; higher is calmer water.
- `strength` — how hard a drop hits.
- `colorByHeight` — color the surface by height so crests and troughs read differently.
- `hueBase` / `hueSpread` — where in the palette the still surface sits, and how far a crest and a trough reach from it.

Distinct from [Ripples](#ripples), which draws expanding rings from a closed-form radius: that one is cheaper and always looks like clean concentric circles, this one behaves like water. Costs two int16 buffers sized to the grid.

Origin: projectMM original, on Hugo Elias's water surface algorithm

<a id="raymarch"></a>

### Raymarch 🔬 · 2D

A lit 3D scene rendered by marching a ray through a distance field, one ray per pixel. Nothing draws a sphere: the scene is a function returning the distance to the nearest surface, and the spheres emerge because each ray stops where that function says a surface is. The lighting is derived too — the surface normal is the gradient of the distance field.

- `bpm` — how fast the scene animates.
- `steps` — ray marching steps: the quality and cost knob.
- `blend` — how much the two spheres melt into each other.
- `cameraY` — camera height above the floor.
- `showFloor` — include the ground plane.

**Compiled only where the SoC declares a hardware FPU** (`SOC_CPU_HAS_FPU`, which every ESP32 variant and the desktop satisfy). This is the one stated exception to the integer-only render-path rule, and it is gated rather than assumed. The cost is per *pixel*, not per chip — measured at 0.30 ms/frame for 32×32 on desktop, and 1.64 ms for 4096 lights on an ESP32-S3 while still holding 409 fps. What limits it is pixel count; `steps` trades quality for cost. Frames also stream over NetworkSend, so a desktop can drive a fixture that could never compute this locally.

Origin: projectMM original, on Iñigo Quilez's raymarching and distance-function articles

<a id="polarnoise"></a>

### PolarNoise 🔬 · 2D

A warped noise field addressed by angle and radius, folded into a kaleidoscope. The field turns and breathes around the centre rather than scrolling past it.

- `bpm` — how fast the field drifts.
- `scale` — noise cells across the grid: low is broad shapes, high is fine detail.
- `segments` — kaleidoscope wedges; 1 disables the fold.
- `warp` — domain-warp strength; 0 gives a plain field.
- `octaves` — fbm octaves, and the main cost knob.
- `twist` — how much the radius shears the angle, setting the spiral.

Cost scales with `octaves` and `warp`: at `warp` > 0 and `octaves` 2 it is roughly 4 noise samples per pixel. On a large wall set `octaves` to 1 or `warp` to 0, which degrades to a plain polar noise that still reads well.

Origin: projectMM original, after Stefan Petrick's polar/noise vocabulary and Iñigo Quilez's domain warping

<a id="sdfshapes"></a>

### SdfShapes 🔬 · 2D

A circle and a box orbit and melt into each other, drawn as signed distance fields rather than rasterized outlines. One distance per pixel yields three looks at once: an anti-aliased fill, an outline (`|d| - width`), and a glow that falls off into the surrounding field.

- `bpm` — orbit speed.
- `radius` — circle radius, as a fraction of the short side.
- `boxSize` — box half-extent, same scale.
- `blend` — melt radius; 0 unions the shapes hard.
- `outline` — 0 fills the shape; higher draws an outline of that width.
- `glow` — tint the field around the shape by distance.

Measured on an ESP32-S3 at 128×128: 20 fps, 728 cycles/pixel using the true-distance form, alongside StarSky (692) and Metaballs (647) at the same size.

Origin: projectMM original, after Iñigo Quilez's distance-function catalogue and polynomial smooth-minimum (iquilezles.org)

<a id="solid"></a>

### Solid 💫 · 3D

A flat fill with five color modes: a plain RGB(W) color, the active palette spread across the lights, an RMS-averaged single palette color, or the palette banded along the grid's rows or columns.

- `red` / `green` / `blue` / `white` — the flat color in `RGB(W)` mode (ignored in the palette modes).
- `brightness` — scales the flat and palette-spread output.
- `colorMode` — `RGB(W)`, `Palette` (spread across the lights), `Palette avg` (RMS mean of the palette), `Palette rows`, `Palette cols` (palette banded along that axis).
- `minRGB` — in the band modes, drops palette entries whose every channel is below this floor.
- `randomColors` — in the band modes, deterministically shuffles the surviving palette entries.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/SolidEffect.md)

[Tests](../../tests/unit-tests.md#solideffect)

<a id="spheremove"></a>

### SphereMove 💫🧊 · 3D

A hollow spherical shell that bounces through the 3D volume, its surface colored from the palette, leaving no trail.

- `speed` — how fast the sphere moves through the volume.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/SphereMoveEffect.md)

[Tests](../../tests/unit-tests.md#spheremoveeffect)

<a id="spiral"></a>

### Spiral 💫🦅 · 2D

<img src="../../assets/light/effects/SpiralEffect.gif" width="300" alt="Spiral effect preview">

Rotating spiral from angle + distance (`atan2_8`/`dist8`).

- `bpm` — rotation speed.
- `twist` — how tightly the arm winds (hue gain per unit of distance).
- `hue_shift` — rotate the palette index.

Origin: projectMM original (rotating spiral)

Detail: [technical](moxygen/SpiralEffect.md)

[Tests](../../tests/unit-tests.md#spiraleffect)

<a id="starfield"></a>

### StarField 💫 · 2D

A perspective starfield: stars approach the viewer from a vanishing point, brightening as they near, then respawn at depth.

- `speed` — how fast stars approach (frame throttle).
- `numStars` — how many stars are active.
- `blur` — motion-trail fade per frame.
- `usePalette` — color the stars from the palette instead of white.

Origin: MoonLight · by [@Brandon502](https://github.com/Brandon502), inspired by Daniel Shiffman / [Coding Train](https://www.youtube.com/watch?v=17WoOqgXsRM) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/StarFieldEffect.md)

[Tests](../../tests/unit-tests.md#starfieldeffect)

<a id="starsky"></a>

### StarSky 💫 · 3D

<img src="../../assets/light/effects/StarSkyEffect.gif" width="300" alt="StarSky effect preview">

Twinkling stars at random light positions, each fading in and out independently over a dark background.

- `speed` — fade rate per frame (how fast each star brightens/dims).
- `star_fill_ratio` — how many stars (as a fraction of the light count).
- `usePalette` — color the stars from the active palette instead of white.

Origin: MoonLight · by [limpkin](https://github.com/limpkin) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/StarSkyEffect.md)

[Tests](../../tests/unit-tests.md#starskyeffect)

<a id="text"></a>

### Text 💫 · 2D

Renders a multi-line string in a bitmap font. Static by default (laid out top-left, each newline dropping one font-height, clipped where it runs off the grid); turn on `scroll` to march the whole block leftwards as a wrapping marquee. Text color comes from the active palette.

- `text` — the string to show; a **multi-line text area** (each line renders on its own row).
- `scroll` — off (default) = static; on = horizontal marquee.
- `font` — glyph size (`4x6` compact, `6x8` larger).
- `speed` — marquee speed (only used when `scroll` is on).
- `hue` — palette index for the text color.

Origin: projectMM original, on MoonLight's Scrolling Text · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/TextEffect.md)

[Tests](../../tests/unit-tests.md#texteffect)

## MoonModules effects

<a id="gameoflife"></a>

### GameOfLife 💫🌙 · 2D/3D

Conway's cellular automaton generalised to 2D/3D: selectable rulesets (+ custom `B#/S#`), cells that inherit a neighbour's palette color on birth, optional green→red age coloring, a dead-cell blur fading toward the background color, toroidal `wrap`, a 1.5 s settle pause, and 3-CRC stasis self-respawn (R-pentomino/glider) when the board goes static.

- `backgroundColorR` / `backgroundColorG` / `backgroundColorB` — the color dead cells fade toward (0–255 each).
- `ruleset` — the birth/survive rule (Conway, HighLife, InverseLife, Maze, Mazecentric, DrighLife, or Custom).
- `customRuleString` — a custom `B#/S#` rule, read only when `ruleset` = Custom.
- `GameSpeed (FPS)` — generation rate (0–100, 100 = uncapped).
- `startingLifeDensity` — % of cells alive at start (10–90).
- `mutationChance` — % chance a newborn gets a random color (0–100).
- `wrap` — toroidal edges (cells wrap around).
- `disablePause` — skip the 1.5 s settle pause between boards.
- `colorByAge` — green→red aging instead of inheriting a neighbour's palette color.
- `infinite` — respawn on stasis (R-pentomino/glider) instead of resetting.
- `blur` — dead-cell fade strength toward the background color.

Origin: MoonModules · by Ewoud Wijma (2022), mods by Brandon Butler / [@Brandon502](https://github.com/Brandon502) · [natureofcode](https://natureofcode.com/book/chapter-7-cellular-automata/) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonModules.h)

Detail: [technical](moxygen/GameOfLifeEffect.md)

[Tests](../../tests/unit-tests.md#gameoflifeeffect)

<a id="geq"></a>

### GEQ 💫🐙📊 · 2D

<img src="https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_139.gif" width="300" alt="GEQ effect preview" title="WLED effect preview — WLED-Utils by scottrbailey"> <!-- preview: WLED-Utils (scottrbailey), WLED FX 139; replace with our own capture once bench-verified -->

A flat graphic equaliser: the 16 audio bands rise as vertical bars from the bottom, with optional smoothing between bars, per-bar palette coloring, and falling peak markers.

- `fadeOut` — how fast bars fade each frame.
- `ripple` — falling-peak marker decay.
- `colorBars` — color each bar from the palette by band instead of by row.
- `smoothBars` — blend neighbouring bands for smoother bar heights.

Origin: WLED (audio) · by Andrew Tuline (WLED-SR) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/GEQEffect.md)

[Tests](../../tests/unit-tests.md#geqeffect)

<a id="geq3d"></a>

### GEQ3D 💫🌙📊 · 2D

A 3D-perspective graphic equaliser: audio bands rise as bars with faked depth, their side/top lines drawn toward a "projector" vanishing point (sweeping left↔right) and shortened by `depth`. Bands left of the projector are painted right-to-left, bands right of it left-to-right; per-face darkening (side/top/front) and optional `borders`.

- `speed` — projector sweep rate (1–10, higher = faster).
- `frontFill` — bar front-face fill strength (0–255).
- `horizon` — vanishing-point row the projector sits on.
- `depth` — how far the side/top perspective lines reach toward the projector.
- `numBands` — bands shown (2–16, fewer = wider bars).
- `borders` — outline each bar.

Origin: MoonModules (audio) · by [@TroyHacks](https://github.com/troyhacks) (GPLv3) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonModules.h)

Detail: [technical](moxygen/GEQ3DEffect.md)

[Tests](../../tests/unit-tests.md#geq3deffect)

<a id="noise2d"></a>

### Noise2D 💫🌙🐙 · 2D

A smoothly drifting value-noise field: each pixel samples 3D noise (grid position × `scale`, time on the Z axis) and indexes the palette directly, giving an organic plasma wash that morphs over time.

- `speed` — how fast the field morphs (time-flow rate).
- `scale` — noise zoom (higher = finer, more detailed).

Origin: WLED · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/Noise2DEffect.md)

[Tests](../../tests/unit-tests.md#noise2deffect)

<a id="paintbrush"></a>

### PaintBrush 💫🌙📊 · 3D

Audio-reactive brush strokes: lines whose 3D endpoints oscillate on the beat (`beatsin8`, audio-band timebase), each stroke shortened to a band-magnitude length so the moving tip sweeps a curve over the fading field.

- `oscillatorOffset` — phase-spread between the oscillating endpoints (0–16).
- `numLines` — parallel animated strokes (2–255).
- `fadeRate` — background decay per frame (0–128, higher = shorter strokes).
- `minLength` — a stroke draws only if longer than this, so quiet bands stay dark.
- `color_chaos` — per-line random hue vs a per-band gradient.
- `phase_chaos` — random per-frame phase jitter.

Origin: MoonModules (audio) · by [@TroyHacks](https://github.com/troyhacks) (GPLv3) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonModules.h)

Detail: [technical](moxygen/PaintBrushEffect.md)

[Tests](../../tests/unit-tests.md#paintbrusheffect)

<a id="tetrix"></a>

### Tetrix 💫🌙 · 2D

Falling Tetris-style blocks: each column drops a brick that lands on the growing stack, fills the column, then clears and restarts.

- `speed` — fall speed (0 = randomised per brick).
- `width` — brick height (0 = randomised).
- `oneColor` — one advancing palette color for all bricks instead of random per-brick colors.

Origin: WLED · by Andrew Tuline (WLED-SR) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/TetrixEffect.md)

[Tests](../../tests/unit-tests.md#tetrixeffect)

## WLED effects

<a id="blurz"></a>

### Blurz 🐙📊 · 2D

<img src="https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_163.gif" width="300" alt="Blurz effect preview" title="WLED effect preview — WLED-Utils by scottrbailey"> <!-- preview: WLED-Utils (scottrbailey), WLED FX 163; replace with our own capture once bench-verified -->

Audio-reactive blurred dots: one frequency band per frame lights a dot whose position maps to that band (or to the major-peak frequency), then the whole frame is blurred for soft trails.

- `fadeRate` — background decay per frame.
- `blur` — blur strength applied each frame.
- `freqMap` — place the dot by the major-peak frequency instead of scanning bands.
- `geqScanner` — scan the dot across the strip in a GEQ-like sweep.

Origin: WLED (audio) · by Andrew Tuline (WLED-SR), enhancements by [@softhack007](https://github.com/softhack007) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/BlurzEffect.md)

[Tests](../../tests/unit-tests.md#blurzeffect)

<a id="bouncingballs"></a>

### BouncingBalls 🐙 · 2D

<img src="https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_091.gif" width="300" alt="BouncingBalls effect preview" title="WLED effect preview — WLED-Utils by scottrbailey"> <!-- preview: WLED-Utils (scottrbailey), WLED FX 91; replace with our own capture once bench-verified -->

A row of balls per column bounce under gravity, each losing energy on impact and relaunching when it stops, palette-colored by ball index over a fading background.

- `grav` — gravity strength (higher = faster fall, snappier bounce).
- `numBalls` — balls per column (1–16).

Origin: WLED · by Andrew Tuline (WLED-SR) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/BouncingBallsEffect.md)

[Tests](../../tests/unit-tests.md#bouncingballseffect)

<a id="freqmatrix"></a>

### FreqMatrix 🐙📊 · 1D

<img src="https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_138.gif" width="300" alt="FreqMatrix effect preview" title="WLED effect preview — WLED-Utils by scottrbailey"> <!-- preview: WLED-Utils (scottrbailey), WLED FX 138; replace with our own capture once bench-verified -->

A 1D scrolling frequency display: each frame shifts the strip and injects a new pixel at one end whose hue comes from the dominant frequency and whose brightness from the volume.

- `speed` — scroll rate.
- `fx` — sound-effect intensity (scales the injected brightness).
- `lowBin` / `highBin` — the frequency window mapped across the hue range.
- `sensitivity` — input gain (10–100).
- `audioSpeed` — let the volume modulate the scroll speed.

Origin: WLED (audio) · by Andrew Tuline (WLED-SR) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/FreqMatrixEffect.md)

[Tests](../../tests/unit-tests.md#freqmatrixeffect)

<a id="lissajous"></a>

### Lissajous 🐙 · 2D

<img src="https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_176.gif" width="300" alt="Lissajous effect preview" title="WLED effect preview — WLED-Utils by scottrbailey"> <!-- preview: WLED-Utils (scottrbailey), WLED FX 176; replace with our own capture once bench-verified -->

A Lissajous curve traced across the grid from two phase-shifted `sin8`/`cos8` sweeps, palette-colored along its length, with a fading trail.

- `xFrequency` — the x-axis sweep frequency (sets the curve's lobe count).
- `fadeRate` — trail fade per frame.
- `speed` — how fast the curve's phase advances.

Origin: WLED · by Andrew Tuline (WLED-SR) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/LissajousEffect.md)

[Tests](../../tests/unit-tests.md#lissajouseffect)

<a id="noisemeter"></a>

### NoiseMeter 🐙📊 · 3D

<img src="https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_136.gif" width="300" alt="NoiseMeter effect preview" title="WLED effect preview — WLED-Utils by scottrbailey"> <!-- preview: WLED-Utils (scottrbailey), WLED FX 136; replace with our own capture once bench-verified -->

An audio VU meter rendered as a noise bar: the volume sets how many rows light from the bottom, each row colored by drifting Perlin noise, filling the full width and depth.

- `fadeRate` — trail decay per frame (200–254).
- `width` — how strongly the volume drives the bar height.

Origin: WLED (audio) · by Andrew Tuline (WLED-SR) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/NoiseMeterEffect.md)

[Tests](../../tests/unit-tests.md#noisemetereffect)

<a id="wave"></a>

### Wave 🌊 · 2D

An oscilloscope waveform scrolls across the grid with a fading trail; six selectable shapes.

- `bpm` — travel speed (phase advance per minute).
- `fade` — trail fade per frame (0 = instant clear, 255 = long tail).
- `type` — waveform shape (`Sawtooth`, `Triangle`, `Sine`, `Square`, `Sin3`, `Noise`).

Origin: MoonLight · by Ewoud Wijma · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/WaveEffect.md)

[Tests](../../tests/unit-tests.md#waveeffect)

## FastLED effects

<a id="fire"></a>

### Fire ⚡️🦅 · 2D

<img src="../../assets/light/effects/FireEffect.gif" width="300" alt="Fire effect preview">

Fire2012-style heat field — sparks at the base rise and cool through the active palette (heat = palette index, cold at the low end, hottest at the high end); spark count scales with width.

- `cooling` — how fast heat dissipates as it rises (higher = shorter flames).
- `sparking` — chance of a new spark at the base each frame (higher = livelier fire).

The flame color comes from the **active palette**. For the classic fire look pick the **Lava** palette (black→red→orange→yellow→white — the recommended default); any palette works, so an Ocean or Forest palette turns the flame blue or green.

Origin: FastLED / MoonLight · Mark Kriegsman's Fire2012; MoonLight adapts [MatrixFireFast](https://github.com/toggledbits/MatrixFireFast) (toggledbits)

Detail: [technical](moxygen/FireEffect.md)

[Tests](../../tests/unit-tests.md#fireeffect)

<a id="noise"></a>

### Noise ⚡️ · 2D/3D

<img src="../../assets/light/effects/NoiseEffect.gif" width="300" alt="Noise effect preview">

Smooth animated value noise; true 3D field on volumetric layouts.

- `scale` — spatial frequency of the field (1–32, higher = finer detail).
- `bpm` — scroll speed (8 noise cells per beat).

Origin: FastLED · inoise field (Mark Kriegsman)

Detail: [technical](moxygen/NoiseEffect.md)

[Tests](../../tests/unit-tests.md#noiseeffect)

## projectMM-native effects

<a id="audiospectrum"></a>

### AudioSpectrum 📊

The 16 mic frequency bands spread across X, each column lit bottom-up by its magnitude.

- `colorMode` — bar coloring: `height` (green base → red top, the VU look) or `per-band` (each column its own hue, the rainbow analyser look).

Origin: projectMM original, on the WLED-SR GEQ / spectrum concept (Andrew Tuline) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h)

Detail: [technical](moxygen/AudioSpectrumEffect.md)

[Tests](../../tests/unit-tests.md#audioservice)

<a id="audiovolume"></a>

### AudioVolume 🔊

A whole-grid VU meter: every light pulses with the mic level, color indexing the palette by loudness.

- `brightness` — overall brightness ceiling for the VU pulse (1–255).

Origin: projectMM original (VU meter)

Detail: [technical](moxygen/AudioVolumeEffect.md)

[Tests](../../tests/unit-tests.md#audioservice)

<a id="demoreel"></a>

### DemoReel 🎬 · 3D

A demo reel: plays every other registered effect in turn, auto-advancing on a timer, so one Layer cycles the whole library hands-free — the showcase/test tool for everything. It hosts a single live effect at a time (created from the effect registry, rendered into this Layer) and swaps to the next when the interval elapses — new effects are picked up automatically. It can also pick a fresh palette each cycle and overlay the playing effect's name. The `status` line shows which effect is playing (e.g. `playing: Plasma (3/20)`). It never hosts itself, and it plays effects in sequence rather than compositing them (layering is the [Layer](moxygen/Layer.md) stack's job).

- `interval` — seconds each effect plays before advancing (1–120).
- `shuffle` — jump to a random next effect instead of registry order.
- `randomPalette` — pick a random palette on each cycle (showcases the palette set); default on.
- `showName` — overlay the playing effect's name in a small font; default on.

Origin: FastLED · Mark Kriegsman's [DemoReel100](https://github.com/FastLED/FastLED/blob/master/examples/DemoReel100/DemoReel100.ino); projectMM reel

Detail: [technical](moxygen/DemoReelEffect.md)

[Tests](../../tests/unit-tests.md#demoreeleffect)

<a id="networkreceive"></a>

### NetworkReceive 📡🌙

Receives lights-over-UDP (Art-Net, E1.31/sACN, DDP) and writes it into the layer — the receive side for Resolume/Madrix/xLights/LedFx.

- `universe_start` — the first incoming universe to map onto the layer (mirrors the sender).
- `channels_per_universe` — bytes each universe maps to (510 = whole RGB lights per universe, the xLights/Falcon convention; 512 for Madrix-style senders that pack pixels across universe boundaries).

Origin: projectMM original (E1.31 / Art-Net receive)

Detail: [technical](moxygen/NetworkReceiveEffect.md)

[Tests](../../tests/unit-tests.md#networkreceiveeffect)

**Wire contract:** listens for [Art-Net](https://art-net.org.uk/downloads/art-net.pdf), [E1.31 / sACN](https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf), and [DDP](http://www.3waylabs.com/ddp/) simultaneously; `universe_start` + `channels_per_universe` map incoming universes onto the layer buffer. The end-to-end pair with [NetworkSendDriver](moxygen/NetworkSendDriver.md).

<a id="sine"></a>

### Sine 🌀 · 3D

R/G/B each follow a sine along one axis at 120° phase offset — a glowing, scrolling color box.

- `frequency` — spatial frequency, waves across the box (1–20).
- `amplitude` — peak brightness (0–255, 255 = full).
- `bpm` — scroll speed.

Origin: MoonLight (Sinus, AI-generated) · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h)

Detail: [technical](moxygen/SineEffect.md)

[Tests](../../tests/unit-tests.md#sineeffect)
