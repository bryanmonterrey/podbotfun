# Recreate the simulator device (for the Blender / 3D agent)

The desktop simulator has **no mesh file** — its casing is drawn procedurally
in C++ (`host/simulator.cpp`). This is the exact geometry and material it
draws, so a 3D artist/agent can rebuild it faithfully as a `.glb`. All lengths
are millimetres; the device is a round puck, symmetric about its centre axis.

## Silhouette (concentric, one centre)
- Metal shell body: Ø **55.00** (widest point).
- Cover glass / black bezel: Ø **48.96**, sits slightly proud of the shell.
- Active AMOLED circle (the screen): Ø **43.76** — a flat black disc; the
  creature face is drawn here (leave a `Screen` node/material to swap a texture).
- Thickness: **15.05** (front glass face to back). The rim rolls a quarter
  turn from the glass edge to the outer silhouette (rounded edge, not a sharp
  cylinder — see "roll" below).

## Side keys (2, on ONE edge)
- Two keys (BOOT and PWR) seated in machined pockets in the rim.
- Angular position: **±0.82 rad** from the +X axis (about ±47°), on the same
  side; +Y points down in the source.
- Each key: **7.0** long along the rim (arc), pocket depth **1.9** into the rim.
  The shell overlaps the key edges (key reads as part of the body, seated in
  the pocket wall).

## Mic ports (2, on the adjacent/left edge)
- Two holes, Ø **1.1** each, ~**30 mm** apart, at angles **±2.46 rad** (~±141°).
- Small dark cylindrical bores, not through-holes visually.
- (USB-C at the bottom edge and a rear speaker port exist on the real board;
  add if modelling the back.)

## Material — powder-coated aluminium alloy (NOT chrome)
The look is a matte powder coat, from `shell_color()` in the source:
- Neutral grey with the faintest **cool** cast: RGB roughly balanced with blue
  ~3.5% higher than red (`r, r*1.005, r*1.035`).
- **Matte / rough**: diffuse-dominated, wrapped diffuse (stays lit ~0.45 past
  the terminator), a broad soft environment sheen (power 6, weight 0.12) — no
  tight specular pinpoint.
- **Orange-peel grain**: fine value-noise (two fine octaves + one coarse) that
  perturbs the surface normal ~10% — reads as roughness, not painted noise. In
  Blender: a low-strength noise/bump on the shell, Principled BSDF roughness
  ~0.6–0.75, metalness ~0.15, clearcoat 0.
- Rim edge darkens slightly (~34%) near the outer silhouette.
- Cover glass: near-black (#060608-ish), low roughness (~0.12), slightly proud.

## Deliverable
- One `.glb`, Y-up, real-world scale in **metres** (so Ø55 mm = 0.055 m), or
  note the unit. Named nodes: `Shell`, `Glass`, `Screen`, `KeyBoot`, `KeyPwr`,
  `Mic1`, `Mic2`. Keep `Screen` a separate flat disc mesh with its own material
  so the site swaps in the animated face texture.
- Reference the exact numbers above; the store already expects `Screen` as the
  face node (see `site/components/DeviceModel.tsx`).

## Source of truth
`host/simulator.cpp`: constants near line 379 (`kDisplayMm`/`kGlassMm`/
`kBodyMm`/`kButtonArcMm`/`kButtonPocketMm`/`kMicHoleMm`, button/mic angles) and
`shell_color()` (~line 474) for the finish. `docs/MECHANICAL.md` has the
Waveshare-verified outline.
