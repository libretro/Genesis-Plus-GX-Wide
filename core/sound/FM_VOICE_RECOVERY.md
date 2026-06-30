# FM voice-steal recovery (design note)

Opt-in, off-by-default recovery of FM (YM2612/YM3438) notes that the sound
driver silences when it "steals" a channel for a sound effect. Distinct from
the PSG `voice_recovery` option: different detection, different state, different
failure modes. Core option: `<core>_fm_voice_recovery` = Off / Conservative /
Aggressive.

## Why this is FM, and why detection is tractable

Empirically (Streets of Rage attract demo, instrumented build):

- Gameplay audio on many Mega Drive titles is entirely FM (+PCM); PSG is unused
  in-game. So PSG recovery cannot help; the dropped notes are FM channels.
- A steal is a full operator-patch reload on a sounding channel. Counting
  structural operator writes (regs 0x30/0x50/0x60/0x70/0x80/0x90/0xB0) between
  notes gives a sharply **bimodal** distribution: melodic note changes write 0-1
  (patch reused), steals write ~22-30 (full 4-operator reload). The 2-21 band is
  essentially empty, so a threshold (>=12) separates them cleanly.
- SoR reserves a fixed channel (ch2) for SFX; other drivers rotate 2-3.

## Proven by proof-of-concept

Snapshotting the full `FM_CH` (4 operators' phase/envelope/TL/etc.), calling
`setup_connection(&shadow, spare)` to re-point its output to a spare `out_fm`
slot, rendering it each sample via the stock `chan_calc`, and mixing with the
captured pan masks **works**: a stolen note audibly sustains, no crash, DC stays
zero, deterministic, bit-exact when off. The two unknowns (can the state be
captured+re-rendered faithfully; does a note actually sustain) are retired.

## Build plan

1. **Groundwork (mechanical, build-verifiable).**
   - Shadow pool: `FM_CH fm_shadow[N]` + per-slot {active, src_ch, pan[2], age,
     retiring}; extend `out_fm` to `out_fm[6+N]`.
   - Extract single-channel envelope helpers from `advance_eg_channels` /
     `update_ssg_eg_channels` so a standalone shadow can be advanced.
   - Render hook in `YM2612Update`: after the real `chan_calc(&CH[0],6)`, for
     each active shadow clear its slot, `chan_calc(&shadow,1)`, advance its EG on
     the eg tick, clip, then add into `lt/rt` with the captured pan.

2. **Capture trigger.**
   - Speculative snapshot on the first structural write to an *audible* channel
     (carrier `vol_out < ENV_QUIET`, covering key-on and key-off-but-releasing —
     the PoC's key-on-only test missed key-off-first steals).
   - **Commit** only once the structural-write count crosses the threshold (real
     patch reload); discard if the next key-on is the same instrument (a musical
     articulation, not a steal).

3. **Envelope continuation + retire.**
   - Advance the shadow's envelope (don't freeze it as the PoC did) so sustains
     hold and decays ring down naturally.
   - Retire by issuing `FM_KEYOFF` on the shadow so it releases on its own RR
     (graceful tail, reuses the chip's release path). Trigger: the music driver
     reclaims the source channel (next key-on on that original channel). Plus a
     hard lifetime cap (~2-3s) and natural EG_OFF retirement.

4. **Voice pool:** N per level (Conservative 1-2, Aggressive 4-6); evict
   quietest/oldest when full.

5. **Determinism / savestates:** integer + pure function of the write stream =
   deterministic, netplay-safe. Do **not** serialize the shadow pool (transient,
   self-heals); ensure a state load clears it. State format unchanged.

6. **Validation (via headless harness):** each iteration render Off vs On on the
   SoR demo + other FM titles; check difference spectrogram (recovered notes),
   DC offset, click metric, and Off==stock bit-exactness. Conservative tier gates
   on note-age to keep false positives to brief tails. Final quality is by ear:
   target "fewer dropped notes than stock, no new artifacts," not perfection.

7. **Performance:** each active shadow ~= one extra FM channel/sample at ~53 kHz
   (N shadows ~ N/6 added FM cost). Cap N at 1-2 on arm32 (Miyoo); off-by-default
   keeps the default path free.

## Notes / caveats

- The detector will occasionally fire on a legitimate in-music instrument change;
  retire-on-reclaim bounds the cost to a brief extra tail.
- Own branch, own commit series — this adds a voice allocator, an EG-advance
  refactor, and retire logic to the most performance-sensitive file in the core.

## Empirical verdict (corrected)

> **Correction.** An earlier draft concluded the feature could not ship because it
> "over-triggered 5-7x louder than the recovery" and that steals were inseparable
> from instrument changes. **That was wrong** -- an artifact of the headless test
> harness, not the core. The harness did not null `retro_variable.value` for option
> keys it doesn't answer, and the stock `_ym2612` handler reads `var.value` without
> resetting it, relying on the preceding option block having left it NULL. Inserting
> the answered `_fm_voice_recovery` key just before `_ym2612` leaked the level string
> into the YM2612 core-variant selector, so on/off runs used *different YM2612 cores*.
> The huge measured difference was that core swap, not the feature. A real frontend
> (RetroArch) answers `_ym2612` and overwrites the value, so the core was always
> correct; only the test rig was broken.

With the harness fixed (null unanswered keys, like a real frontend), validated on
three FM titles -- Streets of Rage 1, Bare Knuckle III (SoR3), Sonic 1:

- **Off is bit-exact vs stock on all three games.**
- The feature fires sparingly and recovered events are localized and musical:
  difference spectrograms show individual harmonic notes, not a pervasive layer.
  Bare Knuckle III barely triggers (its driver keys notes off before reloading) --
  a clean no-op, not a failure.
- No crash, no DC offset, no click artefacts; deterministic.

## How the recovered note decays: three attempts

**v1 -- hold keyed-on.** The first version spawned the shadow voice keyed on,
copying the stolen note mid-sustain and holding it at sustain level until a
lifetime cap keyed it off and cut it. For a note in sustain that produces a
**flat tone with a hard cut** -- a beep, not a tail. Confirmed on the SoR1 ~102 s
steal: a steady ~3.3 kHz tone, envelope min/max 0.25, held for the full cap
(0.60 s) then chopped.

**v2 -- key off at spawn (ring down).** Keying the shadow off at the moment of the
steal makes it follow the instrument's own release envelope. No beep, but
self-selecting in a limiting way: fast-release notes die almost immediately, so
only slow-release notes (pads, bass) recover audibly. Coverage is narrow -- on
SoR1 it drops the 44 s and 102 s recoveries entirely (added RMS 132 -> 46).

**v3 -- synthetic fade (shipping).** Keep the note keyed on so even fast-release
notes are recovered, but apply a smooth quadratic output fade over the lifetime
window (gain = ((life-age)/life)^2) instead of a hard cut. This decays every
recovered note to silence without a flat hold or an abrupt edge, and recovers the
notes v2 dropped. On SoR1 aggressive: added RMS 46 -> 67, samples changed
0.65% -> 1.71%; the 44 s and 102 s notes come back as decaying tones (flatness
~0.20, tail falling to ~22% of peak) rather than a flat beep. Listener-confirmed
free of objectionable artefacts on SoR1, Bare Knuckle III and Sonic 1.

The tradeoff v3 makes explicitly: the fade is *synthetic*, not the chip's own
release, so a recovered note's decay shape is imposed rather than hardware-faithful.
For a best-effort "recovery" feature this is a reasonable choice, and it is the
only part of the signal path that is not bit-exact-faithful -- and only ever on
the *added* shadow voices, never on the six real channels. Off remains bit-exact.

## Coverage: dropping the age gate

An early version gated recovery on note age (only steals of notes older than
~0.8-1.5 s were recovered). Instrumenting the capture path showed this rejected
the overwhelming majority of real steals: SoR1 had 122 steal candidates but only 7
passed the gate (114 were notes shorter than 0.5 s); Sonic 1 had 167 candidates,
only 9 passed. Most stolen background notes are simply short, so a high gate
discards them and the background line still drops out under sound effects -- the
exact symptom this feature is meant to fix. Since the synthetic fade keeps a
mistaken recovery to a gentle brief tail, the gate is not worth its cost.
Aggressive now uses no age gate; conservative keeps a light ~0.25 s floor.
Effect on SoR1 aggressive: recoveries 7 -> 122, samples changed 1.7% -> 12.4%,
added RMS barely moves (67 -> 78). Listener-validated clean, including in
music-dense passages. Off remains bit-exact.

Status: shippable as an experimental, off-by-default option. Off is bit-exact on
all three test games; on, it recovers stolen background notes as smoothly-decaying
tones and never alters or removes any real channel.

## Validation method note

The harness `var.value` fix is essential: without it, any per-option bit-exactness
test on this core is unreliable depending on option ordering, because the stock
option-read loop relies on the previous block having left `var.value` NULL.
