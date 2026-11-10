# MetaphorCPU: what it changes and what it saves

MetaphorCPU is an ASI mod for **Metaphor: ReFantazio** (the April 2025 Steam build). It lowers the CPU and GPU load of the game, mainly for **Android emulation** (Winlator / GameNative: Wine + FEX + DXVK). One build and one INI serve every device: `[General] Profile=auto` picks the Android profile under Wine and the PC profile on Windows. A Steam Deck, recognised by its processor or its board, gets a profile of its own: the picture changes of the Android column and the CPU side of the PC column. It has not been tested on a Steam Deck; for a Deck that is not recognised, the release has an INI with `Profile=steamdeck` in its `Steam Deck` folder.

**Test device:** RedMagic Astra tablet (Snapdragon 8 Elite, Adreno 830), Balanced mode, 1600×900, in-game 30 FPS cap, the Sunlumeo street in Grand Trad (the heaviest common scene). **PC:** i7-10750H laptop, for visual checks and correctness checks.

## The whole mod (tablet, preset Q21)

One 17-minute session under GameNative (Wine 10.0), in the Sunlumeo street, standing still, switching every 60 s between the game as-is, the mod's Android defaults, and the defaults with `SteadyImage=on`. The first 3 minutes are left out; 20–25 windows of 10 s per variant, with the drift over time fitted out. Battery power and energy per frame come from the last 10 s of the slots instead, where power has nearly settled.

| | Game as-is | MetaphorCPU (defaults) | Defaults + `SteadyImage` |
|---|---|---|---|
| Frame rate | 29.6 FPS (28.6 once throttled to 607 MHz) | **29.9 FPS** | 29.9 FPS |
| Battery power | 9.9 W | **7.5 W (−2.46 W, −25%)** | 7.8 W (−21%) |
| Energy per frame | 340 mJ | **250 mJ (−26%)** | 262 mJ (−23%) |
| GPU clock / busy | 832 MHz, 87% busy (607 MHz, 99% once throttled) | **443 MHz, 82% busy** | 525 MHz, 76% busy |
| Game CPU | 247% of a core | **193% (−22%)** | 202% |
| Main thread | 14.3 ms/frame | 12.8 ms/frame (−11%) | 13.0 ms/frame |

* **The game as-is needs the GPU's highest clocks:** it holds 30 FPS at 832 MHz until the tablet throttles it to 607 MHz, where it is 99% busy and falls below 30 FPS. With the mod the 30 FPS cap holds at the 443 MHz clock step.
* **Battery −2.46 W (−25%):** the whole device's draw, screen included, so about a third longer play on a charge in this scene.
* **Why the last 10 s:** battery power takes about 40 s to settle after a switch, and each variant here ran for 60 s. Averaged over whole slots, with the game as-is still rising and the mod still falling, the saving reads −1.84 ± 0.18 W (−19%; ± is two standard errors). Fitted with the lag, it reads −2.8 W (−28%).
* Understated, if anything: the job semaphore replacement only takes effect at startup, so the game-as-is variant had it too.
* **`SteadyImage`:** the frame rate holds. The GPU runs one clock step higher, for about 2 ms more GPU work a frame (busy time scaled to 600 MHz) and 0.35 W more in the last 10 s of the slots; about 0.5 W once settled (the last column below).
* Standing still, every variant was smooth (frame-time spread 0.1–0.3 ms) once loading had finished.
* In this run the SMAA blending-weights shader was not yet patched (fixed since): SMAA High and Medium kept the game's diagonal search there.

## At a glance

| Change | Visible? | Android | PC | Android: GPU time · power |
|---|---|---|---|---|
| Shadow-caster culling (static + animated casters) | No | on | on | −4.4 ms · −0.9 W |
| CPU fixes (job semaphore, spin / wait, no core pinning) | No | on | off | 0 ms · −0.3 W |
| Simpler far meshes (incl. shadow casters by texel) | Hardly | on | off | −2.4 ms · −0.4 W |
| Far shadows every 2nd frame | Hardly | on | on | −0.8 ms · −0.3 W |
| SMAA High (instead of Ultra) | Hardly | on | on | −0.4 ms · −0.1 W |
| Character outlines within 40 m | Hardly | on | off | −0.2 ms · −0.2 W |
| No toon rim light | Yes, less shimmer | on | off | −0.2 ms · −0.1 W |
| Glare off (runs in the capital only) | A faint night glow is gone | on | off | −0.4 ms · −0.2 W |
| Shadows up to 100 m | No shadows beyond 100 m | on | off | −0.6 ms · −0.4 W |
| Far NPCs animated every 2nd frame (beyond 50 m) | Hardly | on | off | −1.1 ms · −0.3 W |
| NPC stand-ins from 60 m, adaptive down to 40 m | Yes, NPCs at the edge change model | on | off | −1.5 ms · −0.2 W |
| **Steadier image: one switch** (`SteadyImage=on`: the four rows below + SMAA Medium) | Yes, much less shimmer | off | off | **+2.4 ms · +0.5 W** |
| Temporal stabilisation, characters (40%) | Yes, steadier characters | off | off | +0.7 ms · +0.3 W |
| Temporal stabilisation, rest of the image (50%) | Yes, steadier edges and railings | off | off | +0.6 ms · +0.1 W |
| ↳ World clamped to 4 neighbours | No | on | on | −0.5 ms · 0 W |
| Characters' motion vectors | Yes, moving characters stay sharp | off | off | +1.3 ms · +0.1 W |

The "↳" rows only act when the row above them is on. Pictures of each change, in this order: **[VisualComparison.md](VisualComparison.md)**.

**The last column** comes from one session of about 26 minutes on the tablet, in the same street, standing still (preset `Q52_features_tablet.ini`, GameNative, Wine 10.0): 20 variants of 30 s in turn, after a 4-minute hold. **−** is a saving and **+** a cost against the defaults, which take 20.3 ms and 7.4 W. Each change was taken back on its own with all the others on. The stabilisation was built up part by part, so each of its rows is the step from the variant before; its first row is the whole switch, measured with the limbs' motion limited to 20 m, which made no measurable difference (see the options below).

* **GPU time** is per frame and scaled to a fixed 600 MHz, because the GPU's clock moves with the load (at their usual 443 MHz the defaults take about 27 ms of the 33 ms frame). Accuracy: ±0.3 ms, ±0.4 ms for the stabilisation's parts.
* **Power** is the whole device's battery draw, screen included, once settled. Accuracy: about ±0.3 W, from the difference between the rounds, so **GPU time is the firmer comparison**: a row under 0.3 W can't be told from no change on its own. Together they hold up: the 11 rows that are on by default add up to −12 ms and −3.3 W, against −2.5 to −2.8 W for the whole mod.
* **CPU fixes** without the job semaphore, which only takes effect at startup. The game's CPU use falls by about 9% of a core.

## How it is tested

| Method | What it gives |
|---|---|
| **In-session A/B cycles** (`[ABTest] Compare=…`) | One game session switches between variants every 30–60 s while the player stands still. `tools\summarize_variants.py` fits every metric against variant + time, so heating and battery drift are factored out. The ± values are two standard errors. Battery power follows a switch with a lag of about 18 s: `--power-lag 18` fits it with that lag and gives the settled values. |
| **GPU timestamp queries** (`[GpuProbe] Timing=1`) | GPU time per render pass, grouped by target size and format: shows which pass a change acts on. |
| **Device telemetry** (Android) | Battery power (W), GPU clock and busy %, CPU clusters and per-thread CPU time, read from `/sys` and `/proc`. |
| **Correctness checks on PC** (`[ShadowCull] Verify=1`, preset Q51) | Every draw the culling would skip is still drawn, inside an occlusion query. About a million of them every 10 s, animated characters included: **0 samples passed**, so the picture is unchanged. |
| **Compare shots on PC** (`[GpuProbe] CompareShots`, F10; preset `Q53_feature_shots_pc.ini` for every change) | The same moment saved once per variant at full size, then compared side by side and as zoomed sheets. Camera drift between shots is removed by a per-pixel fit before differences are judged. |
| **Play checks with F7** | The temporal stabilisation is switched on and off while playing (a still image shows only part of it). `[GpuProbe] MotionVectorView` colours the characters' motion. |
| **Offline analysis** | Frame capture (every pass's output and shader), mesh census (which meshes make the far triangles) and simplification tests on those meshes (`tools\simplify_eval.c`) before anything was built into the mod. |
| **Unit tests** | About 15 test programs, about 250 checks (`build.ps1 -RunTests`), including the D3D11 parts on a WARP device: culling geometry, shader patching (DXBC checksum), mesh read-back and simplification, the adaptive controller on simulated time, the stabiliser and the motion vectors. |

## 1. Clear improvements: no visible change

| Change | What it does | Visuals | Default (Android / PC) |
|---|---|---|---|
| **Shadow-caster culling** (`[ShadowCull]`) | The game draws every shadow caster into all 3 shadow cascades. Each cascade now skips the static casters that cannot reach any of its texels. | None: 0 samples from any skipped draw | on / on |
| **Animated-caster culling** (`AnimatedMarginPercent=10`) | The same for skinned casters (characters), bounded by their bone palette with a 10% margin. | None: 0 pixels, also while walking and running | on / on |
| **Job semaphore replacement** (`[JobSync] FastSemaphore`) | The engine's job semaphore woke every worker for each job and blocked the poster until all had woken. The replacement wakes one worker and never blocks. | None (CPU) | on / off |
| **Spin and wait fixes** (`[Yield]`, `[SpinLock]`, `[Timer]`, `[CriticalSection]`) | The engine spinlock (1500 spins without `PAUSE`) spins briefly and then waits; `Sleep(0)` loops yield adaptively; one timer resolution instead of 100–200 changes per second. | None (CPU) | on / off |
| **No hard core pinning** (`[Affinity] Mode=1`) | The game pins threads to cores. The mod blocks that and lets the OS place threads. | None (CPU) | on / off |

## 2. Light visual changes (not noticeable in play)

| Change | What it does | Visuals | Default (Android / PC) |
|---|---|---|---|
| **Simpler far meshes** (`[Quality] FarMeshes=seams`, `FarMeshSeamsFrom=150`) | The game has no working switch to simpler world models. The mod reads each far world mesh back once and simplifies it in the background (meshoptimizer). Draws then use versions whose shape error stays under 0.5 px on screen. Across texture seams only from 150 m; nearer, the seams stay. Versions that would lose more than 25% of a mesh's surface (foliage, bushes) are left out. | Same as the game in compare shots. Across seams at every distance, columns got stripes; hence the 150 m limit. | on / off |
| ↳ **Shadow casters by shadow-map texel** (`FarMeshShadowTexels=1`) | Casters take their simpler version by the shadow map's texel size (about 10 cm in the far cascade), not by screen pixels. | No visible change (tree shadows, facades) | on / off (far meshes off) |
| **Far shadows every 2nd frame** (`FarShadowRefresh=2`) | The far shadow cascade is rendered every 2nd frame and keeps its map in between. | Far shadows of moving things update at half the rate; indistinguishable on PC | on / on |
| **SMAA High** (`[Quality] Smaa=auto`: High, Medium with `SteadyImage`) | The game runs SMAA Ultra. The mod patches the game's own SMAA shaders to the High and Medium presets at creation. | Medium looks like Ultra in zoomed compare shots (Off is clearly jagged). High keeps a diagonal search: less shimmer on diagonal edges in play | on / on |
| **Character outlines within 40 m** (`[Quality] OutlineDistance=40`) | The characters' ink outlines are left out beyond 40 m (speech bubbles and 3D texts keep theirs). | The change at 40 m is hardly noticeable | on / off |
| **No toon rim light** (`[Quality] RimLight=off`) | Characters' and toon props' material shaders get a copy at creation in which the rim light (a bright 1–2 px band just inside the outline) reads 0. | Less shimmer: the bright dashed edges are gone and nothing else changes | on / off |
| **Glare off** (`[Quality] Glare`) | Leaves out the glare chain (streaks and halo around bright lights). It only runs in the capital. | A faint glow around lit windows and lamps at night is gone; nothing visible by day | on / off |
| **Shadows up to 100 m** (`[Quality] ShadowDistance=100`) | Caps the shadow distance (the game: 200 m). All three cascades fit inside it, so the near shadows also get sharper. | No shadows beyond 100 m (60 m looked worse) | on / off |
| **Far NPCs animated every 2nd frame** (`FarCharacterAnimation=2`) | NPCs beyond 50 m from the camera are posed every 2nd frame and hold their pose in between. Each pose is one GPU skinning job of the game, so about half of the far ones are skipped each frame. | Looks smooth | on / off |

## 3. Visible changes, chosen deliberately

| Change | What it does | Visuals | Default (Android / PC) |
|---|---|---|---|
| **NPC stand-ins from 60 m** (`CharacterLodDistance=60`) | Moves the game's own switch to its simple NPC models from 100 m to 60 m from the hero. | NPCs beyond 60 m use the game's simple models (as with MetaphorFix's LOD option) | on / off |
| ↳ **Adaptive NPC distance** (`AdaptiveLod`) | Glides in 2 m per second (down to 40 m) while the last 3 s average under 25 FPS. Glides out 1 m per second (up to 60 m) while the last 10 s hold 29 FPS. | Only the NPCs right at the edge change model, a few at a time | on / off |

## 4. A steadier image (costs GPU time; off by default)

The broken, shimmering lines on characters (laces, stitches, hair strands) are thinner than a pixel: geometric aliasing, which SMAA cannot fix. Only more samples per pixel do: MSAA (not feasible in this renderer), supersampling, or temporal accumulation. The mod adds its own temporal stabilisation after the game's colour grading, just before SMAA. F7 switches it while playing.

**One switch:** `[Quality] SteadyImage=on` turns on the rows below (characters 40%, the rest 50%, the characters' motion vectors) and SMAA Medium. The parts' own keys are `auto` in the INI and follow the switch; a value of their own wins.

| Change | What it does | Visuals | Default (Android / PC) |
|---|---|---|---|
| **Temporal stabilisation of characters** (`[Quality] TemporalStabilise=40`) | Each character pixel keeps 40% of its previous value. The previous value follows the camera (reprojected exactly from the game's view matrices) and is clamped to the current neighbourhood, so moved features leave no trail. | Steadier characters, little smearing. Without motion vectors, moving characters look a little soft. | off / off |
| **Temporal stabilisation of the rest** (`TemporalStabiliseWorld=50`) | The same for every other pixel at 50%, reprojected with the camera. A pixel whose previous position was off screen starts afresh. | Steadier railings, ropes, roof edges and far windows while the camera moves | off / off |
| ↳ **4 neighbours** (`TemporalStabiliseWorldTaps=4`) | The world's pixels are clamped to their 4 direct neighbours instead of the 3×3 (half the reads); the characters keep the 3×3. | No difference seen in play | on / on |
| **Characters' motion vectors** (`CharacterMotionVectors=on`, within `MotionVectorDistance=40` m) | After the characters' pass, the mod draws each character within 40 m again into a motion target, from its previous transform and a copy of its previous skinned vertices. The stabilisation then follows the characters' own movements. | Moving characters stay sharp | off / off |

## Options, off by default

| Option | Effect | Why off |
|---|---|---|
| `VolumetricFog=0` | Less GPU work, capital only | It tints the capital's distant air; kept |
| `Smaa=off` / `medium` | Less GPU work than High (Medium: about −0.4 ms with the stabilisation on) | Off is visibly jagged; Medium shimmers more on diagonal edges than High, unless the stabilisation is on (`SteadyImage` sets Medium) |
| `FarMeshes=safe` | Less GPU work than the game's meshes | `seams` from 150 m saves more with the same look |
| `FarMeshSeamsFrom=0` (across seams everywhere) | A little less GPU work | Stripes on columns and uneven stone corners at 50–150 m |
| `FarShadowRefresh=3` | A little less GPU work | Every 2nd frame looked the same and saves almost as much |
| `OutlineDistance=all` (no character outlines) | Less GPU work | Near characters look flatter. A play preset sets it |
| `WorldTextureBias=0.5` / `1` (the world's textures smaller) | No saving found | Softer paving, for nothing |
| `InnerOutlines` (the outline lines drawn over a character's own body: hair, sleeves, collars) | Fewer lines | It removes thick lines that look good (arms, lapels) and leaves most of the thin broken ones (hair). Not used |
| `OutlineDepthOffset` (the outlines pushed back by a share of their distance) | Fewer streaks in hair | It does not touch the broken lines inside some characters' hair and clothes (their material). Not judged in play yet |
| `LineFix=redraw` / `thick` (the characters' thin line meshes drawn 4 / 8 times, shifted by a third of a pixel) | The dashes close | The laces still shimmer as the character sways (geometric aliasing). The temporal stabilisation is the cure |
| `MotionVectorSkinDistance=20` (with the characters' motion vectors, in a crowd of more than `MotionVectorSkinCrowd=12`: characters beyond 20 m get the motion of their walking and turning alone) | Copies less: 3.3 MB a pass instead of 5 MB in the test street | No saving measured in GPU time or power, so every character keeps its limbs' motion |

## Tested and dropped

| Idea | Result |
|---|---|
| Lower texture detail for far objects | No saving (far objects already sample small mips); flags looked worse |
| Global texture mip bias | Blurry, also the 3D UI |
| Characters' textures softer (`CharacterTextureBias`) | No visible gain on the thin dark lines (they are outline lines, not texture); painted stitches stay |
| Culling tiny objects / tiny shadow casters | No saving: almost nothing in this game is that small |
| Anisotropic filtering clamp | No saving: very few of the game's textures are filtered anisotropically |
| View-aware cascade culling | Too little left to remove after the shadow-caster culling |
| Render-pass load/store tuning for the tiler | Almost nothing avoidable: not worth it |
| Hiding far NPCs / far characters | Popping was jarring; the stand-ins replace it |
| Shadow distance 60 m | Looked worse than 100 m |
| The game's own world-model LOD switch | Exists in the data only; nothing reads it at run time (static analysis) |
| MSAA | Not feasible: the deferred renderer reads its single-sample targets texel by texel; every pass would need replacement shaders, and 4× MSAA would multiply the G-buffer's bandwidth by 4 |
| Full TAA with a sub-pixel jitter (what Luma does on PC) | Weeks of work (jitter-aware outlines and screen effects, motion for everything). The mod's stabilisation with the characters' motion vectors covers most of it without jitter |
| Luma's FSR 3 on the tablet (ReShade add-on under Wine / DXVK) | It runs, and the antialiasing is much better (no shimmer, no broken lines). But from 75% it costs too much battery; from 50% it is too blurry, worse than native |
| Far NPCs animated more rarely (every 3rd frame beyond 50 m, or every 2nd from 30 m) | Little to gain, for choppier middle-distance characters |
| Simpler far meshes on PC | No gain on PC (GPU not limited by triangles; small CPU cost) |
| CPU fixes on PC | No gain on PC (frame-capped or GPU-bound) |
| A larger shape error for far meshes; simplifying near world and characters | Little to gain, with more visual risk |
| Per-shader costs: particles, the two heavy forward shaders, emissive | Left out one at a time on the tablet: none of them costs a measurable amount |
| The painterly background (wobble + edge darkening) | Part of the game's art style; it also writes the background, so it cannot just be left out |
| Occlusion culling with GPU queries | Hidden objects hold a large share of the triangles, but on the tablet's tiled GPU the queries alone cost several times what a culling could save |

## Outside the mod (settings that matter as much)

| Setting | Effect |
|---|---|
| Winlator / GameNative **Processor Affinity: all cores ticked** | Otherwise the game can end up on only 2 cores |
| In-game **30 FPS cap** | All results above are at 30 FPS |
| In-game **ambient occlusion off** (tablet) | Saves GPU time; AO is costly here |
| MetaphorFix `[LOD] Distance = 10` | Keeps the grass distance; the NPC switch is set by this mod |
| MetaphorFix **camera sway off** (tablet) | The sway drifts the view a little every frame, so thin edges crawl. Off, the picture shimmers less; the stabilisation also has less to do |
