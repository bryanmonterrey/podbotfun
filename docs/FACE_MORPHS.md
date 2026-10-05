# Face morphs — the eyes as the dynamic island (task spec, 2026-08-27)

**Status: task for later.** Owner's ask: "how the eyes are going to morph into
certain things — a music player is necessary too." This doc fixes the vocabulary
and the rules so the work can start in any session without re-deciding them.
Doctrine: the eyes ARE the UI (`docs/ROADMAP.md`, memory
`device-interaction-doctrine`); every morph obeys `.claude/skills/lilguy-motion`
(house/pop springs, two-beat launches, authored exits, hysteresis, no shadows on
dark, exactly one picture).

## What exists

- `main/include/eyes/audio_bars.hpp` — the four-bar EQ takeover while the
  device speaks or plays audio. Platform-free, allocation-free, deterministic,
  golden-hashed. The simulator drives it from the speaking edge (`U` previews).
- Status-glyph takeover (below speaking) in `host/simulator.cpp`.
- Camera app's morphing SDF (circle → rounded square) and liquid-glass shutter —
  the reference for "one shape becoming another" on this rasterizer.

## The rule that makes it one system

**Every morph is the two eye pills becoming the thing, and the thing becoming
the two eye pills again.** Nothing fades in over the face; nothing pops from
nowhere. The eyes are the source material: they split, merge, stretch, bend or
multiply into the state's shape (pop spring, two-beat: ooze ~35 % then fire),
hold, then collapse back on an authored exit (anticipate 7–10 % the wrong way,
collapse 70–90 ms) into two pills that blink once on arrival. Silhouette
continuity every frame (rule 8: exactly one picture) — the SDF/morph engine
should interpolate *shapes*, never crossfade two renders.

Geometry budget: the face is a 466 px circle; morph content lives inside the
centre ~300 × 220 px "island" so the rim stays clean. Colors never spring
(rule 5): shape moves, palette snaps.

## The vocabulary (build in this order)

| # | State | Trigger | Shape the eyes become | Exit |
|---|-------|---------|------------------------|------|
| 1 | **Listening** | stream opens / `UserStartedSpeaking` | the two pills lean in: taller, closer, a slow 1 Hz breathe; on the user's voice energy they widen ~6 % (hysteresis, rise 55 ms / fall 140 ms) | back to rest on house spring |
| 2 | **Thinking** | `AgentThinking` / turn ended | pills merge into one wide pill that wanders (slow lissajous), like eyes looking up-left | splits back into two |
| 3 | **Speaking** | first PCM frame | **exists**: four EQ bars (`audio_bars.hpp`) | bars shrink to two pills on `AgentAudioDone` + drained |
| 4 | **Success check** | tool/order/payment confirmed | pills swing into a ✓ (left pill = short stroke, right pill = long stroke), splat on landing (1.18/0.84 @ 70 ms) | hold 900 ms, anticipate, collapse |
| 5 | **Notification glance** | message/event arrives | one pill slides to the top edge and becomes a short capsule with a glyph; the other eye keeps looking at you | capsule dives back into the eye |
| 6 | **Pairing code** | mirror pairing | pills stack into 4 digit slots; digits set in the DIGIT font, one slot per house-spring beat | slots fold back into pills |
| 7 | **Wallet approval** | payment needs a spoken yes | pills become a wide pill showing amount + token; a thin ring around the face counts the 10 s | ring completes → check (4) or collapses |
| 8 | **Timer / countdown** | "set a timer" | one ring eye, one digit pill | — |
| 9 | **Music player** | play/pause/track | see below | — |

## The music player (required)

The eyes become the player; nothing else is drawn.

- **At rest / playing:** the two pills widen into **four EQ bars** (state 3,
  reused — one visualizer for voice and music) with the **track title** riding
  as a slow marquee in a thin capsule *under* the bars (12 px cap height, one
  line, ellipsis when idle, marquee only while playing).
- **Progress:** a hairline **arc around the island** (not the rim), 270°, house
  spring on seek, colour = palette accent, never white on the dark panel.
- **Pause:** the four bars collapse to two *short* pills (the eyes, half-lidded)
  — the face literally rests. Play = two-beat launch back to bars.
- **Skip:** bars slide out the edge of the island as the next track's bars slide
  in (30–50 ms phase lag between the two, rule 3), title capsule swaps with a
  dip-to-black (out 0.22 s ease-in, in 0.34 s ease-out).
- **Volume:** crown (Superheavy) or "louder/quieter" — the arc brightens for
  500 ms and a thin volume tick rides it; no separate slider.
- **Barge-in:** speech ducks the bars to 30 % energy (state 1 layered on the
  bars' floor) so a question doesn't kill the song; the reply plays over
  ducked music, then energy returns.
- Source is a stream (see `docs/VOICE_AGENT.md` for the transport doctrine): a
  ring-buffered PCM path, the same one the voice reply uses, with the visualizer
  fed by the ring's RMS — never a decoded file "played when ready".
- Voice only: "play …", "pause", "next", "what's this". No touch player UI
  (touch = pause/play at most).

## Where it goes

- Core: a `face_morph.hpp` next to `audio_bars.hpp` — a platform-free morph
  engine: N source shapes (the two pills) → target shape list, pop/house
  springs, phase-lag, all as pure functions of (t, params). Golden-hash tests
  in `host/test_main.cpp`; simulator keys to preview each state (extend the
  `U` takeover).
- Device: the LVGL task only reads state; the morph never allocates; band
  writes stay in TE windows (CLAUDE.md invariants).
- Site: the hero's Lanyard eyes (`site/components/Lanyard.tsx`, `mood`) should
  mirror states 1–4 so the website face and the device face are one animal.

## Definition of done

Each state: enters from pills, exits to pills, silhouette-continuous, obeys
the spring constants, deterministic under test, previewable in the simulator,
and the music player works end-to-end on the sim against a real stream.
