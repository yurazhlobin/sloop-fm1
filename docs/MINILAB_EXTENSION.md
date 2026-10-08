# MiniLab extension v0.1: external MIDI scale/chord

## Baseline and scope

Inspected baseline: SLOOP 2.4.1, commit
`a1c5d68767ae10fafb6821dc63b9b1fc490342d2`. The fork's `upstream` remote
points to `https://github.com/isod89/sloop-fm1.git`; the baseline is pinned
by commit, not by a moving branch. No upstream synchronization or device
installation was performed.

Only v0.1 is implemented. There is no knob/fader parameter CC routing,
MiniLab template, MIDI learn, or v0.2 implementation. CC120/123 are handled
only as note-release safety messages.

## Phase 1: verified source inventory

Locations below refer to the implementation in this branch unless marked
as baseline.

| Surface | Location | Verified behavior |
| --- | --- | --- |
| DIN/TRS parser | `firmware/src/midi_uart.c:40-101` | UART1 DMA ring, running status, velocity-zero packets, realtime clock; `uart_midi_poll -> um_drain -> um_byte -> midi_in_q`. |
| USB MIDI parser | `firmware/src/usb.c:660-694` | `midi_in_event` validates USB-MIDI packets and appends channel messages to the same queue. USB and TRS note messages share channel identity. |
| Local playable keys | `firmware/src/seq.c:1352-1506,1545` | `keyboard_block -> key_down/key_up`; stored `kb_kind/kb_trk/kb_nt` determine releases, not current settings. UI layers are handled before musical playback. |
| KEY | `firmware/src/ui_layers.c:342-346`; `firmware/src/params.c:64` | SCL + key sets `P_ROOT` on **all three** synth tracks. The root remains a per-track parameter, including editor/project changes. |
| SCALE / KEYS | `firmware/src/params.c:11-15,64-67`; `firmware/src/seq.c:22-40,145-197` | Sixteen masks. `P_QUANT` is OFF=0, SNAP=1, WHITE=2. |
| CHORD / CHORD+ | `firmware/src/seq.c:199-335` | TRIAD, 7TH, 9TH, SUS4, POWER; CHR with CHORD uses the minor mask. Shared `chord_play_notes` also implements modifiers and VLEAD. |
| Synth dispatcher / recording | `firmware/src/seq.c:1124-1157,438-495` | `input_on` calls recording, arp or synth voices; `input_off` ends recording holds, arp holds and delayed/sounding notes. Synth steps retain at most four pitches (mono modes retain one). |
| Voices / strum | `firmware/src/voice.c:179-226,439-532` | Shared eight-voice budget, pitch-based note-off, MONO/LEGATO/UNISON, engine caps; strum note-off cancels pending notes. |
| External routing | `firmware/src/seq.c:2003-2010,2209-2226` | Configured drum channel has priority; otherwise channels 1-3 address tracks 1-3; other channels address the selected track, including the drum track when selected. |
| Playback | `firmware/src/seq.c:1800-1823,1917` | Stored pitches go straight to `trk_note_on/trk_note_chord`, never through keyboard mapping. |
| Storage and restore | `firmware/src/project.c:31-96,517-527,665-697` | Fixed project/global counts and fixed settings object sizes are used by loading and backup/restore. They are unchanged. |
| New ownership / switch | `firmware/src/midi_keys.c:4-192`; `firmware/src/ui_menu.c:9-14,44,177-179` | External input snapshots plus runtime-only menu switch. |

**Original bypass confirmed:** at baseline `seq.c:2214-2239`,
`events_block -> midi_route -> input_on(t, d1, velocity)` passed the incoming
pitch unchanged. It did not call `kb_map` or any chord generator. Only
selected-track channels had a stored destination (`midi_sel_on`); releases
on fixed/drum channels used the current routing decision. The unmodified
`scale_test` passed before implementation.

Local keyboard musical call graph:

```text
keyboard_block -> key_down -> kb_map -> synth_key_map
                          -> chord_play_notes (when CHORD enabled)
                          -> input_on -> rec_note / arp_add / trk_note_chord
               -> key_up -> input_off (stored destination and pitches)
```

External musical call graph after this change:

```text
UART um_byte / USB midi_in_event -> midi_in_q -> events_block
    -> midi_keys_event -> midi_track
        RAW: incoming pitch
        FOLLOW SCL: synth_key_map -> chord_play_notes
    -> input_on -> existing recording / arp / synth dispatcher
Note Off / velocity zero -> stored destination + stored output pitches
CC120/123 -> release that channel's mappings
track panic -> clear its mappings and existing voice/arp state
```

## Exact musical behavior

RAW preserves chromatic pitches, velocity and normal channel/drum operation,
irrespective of KEY, SCALE, KEYS, CHORD, transpose or FM-1 octave.

FOLLOW SCL treats MIDI note 60 as the onboard C4 key; notes 53-79 match the
27 onboard keys. Mapping extends to all MIDI pitches without wrapping the
keyboard index:

- OFF: chromatic pitch plus FM-1 octave and track transpose. KEY does not
  transpose OFF mode.
- SNAP: add octave/transpose, then round **down**, not nearest, to the
  destination track's scale relative to KEY.
- WHITE: successive physical white keys walk successive scale degrees
  around C4=root. Black keys are silent. A five-note scale does **not**
  reset at each physical octave: C5 is degree 7, not degree 5.
- CHORD overrides OFF/SNAP with WHITE mapping. F# flips the third, G# adds
  seventh, A# makes sus4, C# adds ninth, D# inverts. These physical black-key
  modifiers work in every incoming octave and revoice held external chords.
  Modifiers are scoped to the original destination track, independent of
  onboard modifiers/UI layers. STRUM and VLEAD reuse existing logic.
- The drum track, synth SAMPLE GM kit, and optional SLICE selectors bypass
  scale/chord conversion. External GM/slice pitches retain their established
  meaning; onboard engine-specific layouts are not transplanted onto MIDI.

The external chord list removes duplicate tones and tones above MIDI 127
that upstream CHORD+ can produce at range boundaries. Local behavior is
not changed by that external-only normalization. The shared input boundary
also ignores generated onboard tones above 127 rather than indexing MIDI
ownership state outside its bounds.

## Ownership and recording

Each of the 16 channels and 128 input pitches has a snapshot containing
destination, kind, root, output list and velocity, including silent keys
and modifiers. Note-off uses the snapshot even after selection, drum
channel, KEY/SCALE/CHORD, octave, transpose or RAW/FOLLOW changes.

A repeated Note On for the **same channel/input pitch** replaces its prior
mapping (retrigger policy, not FIFO stacked note-ons). One Note Off releases
the replacement; surplus Note Off messages do nothing. Distinct input
pitches/channels sharing a resulting pitch are reference-counted; only the
last live owner (external or onboard) releases it. Poly notes may retrigger with new velocity;
the arp keeps one hold per resulting pitch. These ownership safeguards also
apply in RAW mode, fixing ambiguous legacy overlapping/retrigger releases.

The second review reproduced premature release in both onboard/external
directions and between live input and sequence/arp playback. All live inputs
now share per-track/pitch reference counts. The synth dispatcher also tracks
independent INPUT, SEQ, ARP and four ROLL-slot ownership bits; it releases a
pitch only after its last source ends. STRUM entries retain their source so
canceling a delayed note does not release another source's sounding note.
The arp counts distinct held input pitches, and an ownership bit replaces
the ambiguous `arp_note != 0` test, including MIDI pitch 0.

Track all-off, including scene/panic paths, invalidates live snapshots,
modifier caches, pending revoices and onboard note/modifier state. Arp and
roll bookkeeping and their MIDI-out holds are cleared too. A stale external
Note Off cannot consume a subsequently acquired input reference. The host
initialization helper resets the same state between regression cases.

CC123 and CC120 release the channel's tracked synth notes and pending strum,
and end a latched arp when no physical holds remain. They do not kill
other channels' owned pitches or change one-shot drums. Both use normal
envelope release (CC120 is not an immediate audio hard-mute).
Upstream has no MIDI sustain pedal implementation; CC64 remains ignored.

LIVE REC goes through the existing `input_on` path and stores **resulting
sounding pitches**, including chords, rather than controller key numbers.
With ARP enabled, the arp records its actual output, as with onboard keys.
Playback is never remapped. Existing step capacity, record arming,
quantization, strum recording and mono restrictions remain in effect.

Stress testing exposed an existing allocator edge case: retriggering a
stolen voice during its stage-4 fade could revive it without checking the
shared budget. The small `voice_alloc` fix treats such slots as requiring
room before reuse. A deterministic test reproduces that case; all 105
existing golden renders remain unchanged.

## Configuration and persistence decision

Hold HOME to open MENU, SELECT to SYSTEM, then set **EXTERNAL MIDI** with
KNOB 1 (right=FOLLOW SCL, left=RAW) or OCT+ (toggle).
KNOB 2 is now HARDWARE CALIBRATION; KNOB 3 is ABOUT.

The user approved a runtime-only setting to avoid persistence migration:
after every restart the mode is **RAW**. Closing the menu, saving/loading
projects or presets, and full backup/restore do not save or replace it.
`G_COUNT`, `P_COUNT`, project versions, `persist_t`, and packed lighting
settings are untouched. A persisted setting would require a separately
reviewed compatible migration plus restore/editor tests; no spare storage
bits are silently repurposed here.

## Validation and build status

Host environment: GCC 13 in Ubuntu WSL2, Windows Python 3/Pillow for asset
generation, and Linux Node.js 22.12.0 for the web tests. The Windows web
runner fails at its existing absolute ESM import; Linux runs it successfully.

Results:

- Baseline and final `scale_test`: pass (all scales, roots, octave/transpose
  ranges, local key lifecycle, arp, recording and MIDI out).
- `midi_keys_test`: pass, including all MIDI pitches, all scale/root/mode
  combinations at representative octave/transpose boundaries, all chord
  types and 32 modifier combinations, revoice, collisions, routing,
  mode/parameter changes, velocity zero, repeated notes, voice stealing,
  panic, strum, VLEAD, recording/playback and actual USB/TRS parser input.
- The same suite under AddressSanitizer + UndefinedBehaviorSanitizer:
  pass, including a 6000-event lifecycle/budget stress and voice-mode changes.
- Existing MIDI parser, sequencer, project format, real UI/menu (including
  its 20000-frame fuzz), and 40000-frame stress tests: pass.
- Existing DSP regression: 105 golden renders, **0 changed**, 0 health
  failures, 0 voice/routing failures, 0 crashes. Host CPU results are timed,
  not target instruction measurements.
- New checks cover symmetric onboard/external and sequence/live overlaps,
  arp overlap and pitch 0, independent rolls, delayed STRUM ownership,
  stale all-off snapshots, every onboard scale/chord/modifier lifecycle,
  all 2048 input slots, cached duplicate modifiers, canceled/replaced work,
  continuous modifier changes without starvation, four revoice visits per
  block, and eight queued MIDI packets per block.
- The complete standard runner was executed, including its ten-minute
  simulated soak, 40000-frame stress, 15000-frame sanitized stress, UI and
  MIDI sanitizers, loader/package, storage/recovery, installer simulations,
  all engine/project tests and Linux web tests.
- **The full runner is not green:** its unchanged target-cost check reports
  `fm1_alnk0_irq = 268` against a stored budget of `174` (+54%, limit +10%).
  An isolated archive of the original baseline
  `a1c5d68767ae10fafb6821dc63b9b1fc490342d2`, built using exactly the same
  compiler/SDK/generated assets, also reports **268**. All budgeted engine
  render checks pass; FM6 functions have no stored budgets in the existing
  file. Golden files and CPU/target budgets were **not updated**.

Focused host invocation after generating `build/gen`:

```sh
cc -O2 -w -Ibuild/gen -Ifirmware/src \
  -o build/host/midi_keys_test tests/midi_keys_test.c -lm
build/host/midi_keys_test
```

The standard runner now includes the MIDI suite under ASan/UBSan as well.
The actual pi32v2 app and update loader compile/link and the package builder
produces **`build/felucca.fwsc` (610019 bytes, identity `FM-1_900`)**. Loader
image: 8140 bytes; compressed OTA: 6863 bytes. Build checks verify RAM-only
flash routines, entry point, MMIO isolation and memory limits; simulated
update/package tests pass. The tracked `docs/firmware/sloop-2.4.1.fwsc` is
still the original stock release, not this extension. **No device was flashed.**

Final package SHA256:
`5b4451e562ac6775f5337d8aa732eae7802a64a82cdf3b4d9cea0cb788e8d03f`.

The original 18432-byte snapshot allocation did **not** fit: target linking
overflowed `.pool` by **8928 bytes**, even before the required 8192-byte
reserve. Snapshots now occupy **14336 bytes in `.bss`** (2048 x 7 bytes,
compile-time size assertion); packed kind/track/count fields are runtime
state only, never serialized. Active/pending work bitmaps use 1024 pool
bytes. The STRUM owner tag shares its existing flag byte and its queue
entries remain eight bytes.

| Target region | Actual usage | Capacity | Free |
| --- | ---: | ---: | ---: |
| RAM `.data + .bss` | 96436 B | 98304 B | 1868 B |
| `.pool` | 335584 B | 344064 B | 8480 B |

Pool headroom exceeds the build's 8192-byte requirement by only **288 bytes**.
The linker regions, stack/guard reservations and checks were not enlarged
or weakened. This fits the default build, not a guarantee for optional
feature combinations. Hardware stack high-water and IRQ deadlines remain
unmeasured. Dependency provenance and the verified Windows/WSL2 build-only
command are in BUILDING.md.

## Execution-cost review

Originally a modifier edge scanned 2048 snapshots to reconstruct its mask
and another 2048 to find chords, potentially regenerating every held chord.
A repeated modifier Note On could do that twice. The audio ISR drained the
entire MIDI queue, multiplying those scans and chord generations in a burst.
VLEAD adds up to twelve candidate voicings per chord, each with at most four
tones; STRUM and voice allocation add further work. This is a real deadline
risk, not something host golden renders or average DSP timing can rule out.

`midi_key_mods()` now reads a cached mask in O(1); five per-track counters
maintain duplicate modifier ownership. `midi_key_revoice()` only marks a
track dirty in O(1). Once per 32-sample control block:

- Coalesced jobs copy at most three fixed 64-word bitmaps.
- A round-robin worker visits at most **four** pending snapshots, hence at
  most four chord regenerations, irrespective of the held-input count.
  Empty-word walks total at most 192 words and bit selection at most
  4 x 32 probes in a block. Released/replaced inputs cancel their work.
- A running pass is not restarted by additional modifiers. It uses the
  latest mask, then schedules a follow-up pass if necessary, avoiding
  starvation of high-index inputs.
- The MIDI queue processes at most **eight packets**, including clock/
  transport packets, per control block. Large USB bursts preserve FIFO order
  but are deferred rather than monopolizing the ISR.

At 44.1 kHz each control block is approximately 0.726 ms; the DMA half-buffer
deadline is approximately 5.80 ms (eight control blocks). Typical single-track
MiniLab chord changes span a few blocks. A pathological 2048-input bitmap
on three tracks can require 1536 processing blocks (~1.11 s), plus
scheduling/completion blocks, for one pass and an additional pass after
intervening changes. This is intentionally
bounded CPU work, **not** an instantaneous response guarantee.

Safety CC120/123 still synchronously scan a channel's 128 snapshots and
release its held outputs; explicit track panic/scene reset still scans all
2048 snapshots to forget ownership, without regenerating chords. These
emergency/reset paths require hardware timing checks too. No host-time
measurement is represented as FM-1 cycle or WCET evidence.

The current compiler also inlined the entire DSP/control block into the
DMA ISR, inflating the static ISR-loop metric to 33843. `mix_block()` is now
explicitly out-of-line, matching the baseline's compiled call boundary and
bringing that metric back to **268**, without changing DSP algorithms or
hiding helper work behind a rewritten budget. Its standalone control work
still needs hardware profiling; the existing stale 174 budget remains a
visible blocker for a fully green runner.

## Known limitations

- No persistent mode setting, sustain, MIDI CC sound controls or MIDI echo.
- USB and TRS share `(channel, pitch)` ownership, as upstream; use different
  channels if two controllers play concurrently. Same-input repeats replace,
  rather than stack, held notes.
- Synth ownership protects gates by source/pitch, not by independent voice
  IDs. A shared pitch can still retrigger with the most recent velocity;
  independent articulation needs separate tracks.
- Voice stealing and synth/step/arp caps remain intentional; not every
  requested chord tone can sound when the shared budget is exhausted.
- MIDI input queue overflow or a disconnected controller losing Note Off
  is not repaired automatically. Send CC123 or use a track panic/restart.
- Target linking now proves static memory fit. Hardware IRQ latency,
  stack high-water, long USB bursts and safety-panic cost remain unverified;
  the baseline-reproduced target-budget failure is still open.
- Extreme CHORD+ output is normalized only on the external path; hardware
  parity at pitch boundaries should be checked separately.

## Hardware validation, backup and rollback

No hardware validation has been performed.

1. Before any manual installation, take a Web Editor **full backup** and
   retain the official SLOOP 2.4.1 firmware for rollback. Preserve the
   baseline commit and the exact package you build. Never use the installer
   automatically as part of a test run.
2. Once a new package has actually built and passed target checks, install
   it manually using the documented installer. MiniLab DIN OUT connects
   to FM-1 TRS MIDI IN with a **Type A** adapter; supply MiniLab power
   separately as needed. Start at a safe monitoring volume.
3. With RAW after restart, synth keys on channel 4 must play chromatically
   on the selected synth track despite WHITE/CHORD settings. Pads on
   channel 10 must retain kick/snare/hat mapping and velocity levels.
4. Select FOLLOW SCL. On C minor / WHITE, play C4 D4 E4 F4 G4 A4 B4:
   expect C D Eb F G Ab Bb. Black keys alone are silent. Compare pitches
   against onboard keys. Check octaves above and below C4, and a pentatonic
   scale across B4/C5. Check SNAP rounds E4 down to Eb4.
5. Test every CHORD type, then press/release F#, G#, A#, C#, D# before and
   during a held white key. Confirm revoicing, STRUM cancellation on early
   release and VLEAD. Watch for dropouts with several held chords.
6. Hold notes/chords, then change selected track, key, scale, transpose,
   FM-1 octave, CHORD and RAW/FOLLOW. Releasing the original input must end
   its saved output on its original track. Repeat a pitch; overlap two
   SNAP inputs or chords sharing tones, and release in both orders.
   Also overlap onboard, external, sequence, arp and roll notes at the same
   pitch. Release each source in both orders; the other must retain its gate.
   Include STRUM, arp pitch 0 and a scene/panic followed by new notes and
   stale Note Off messages. Profile worst-case MIDI/modifier/panic bursts
   with the heaviest DSP mix and watch for audio underflows.
7. Check channels 1/2/3 address fixed synth tracks, 4/16 the selected track,
   10 drums. Check configured drum-channel priority and selected drum-track
   routing. Verify MIDI clock/transport, CLOCK-only input and CC123 recovery.
8. Arm LIVE REC, record WHITE notes/chords and arp output. Disable recording,
   change KEY/SCALE/CHORD, and play back: recorded pitches must not change
   or acquire an extra chord transformation. Repeat with drum pads.
9. Restart: mode must return to RAW; projects/presets/backups must load
   normally. If validation fails, reinstall the official baseline firmware
   manually and restore the full backup. If boot recovery is required,
   follow BUILDING.md's FM-1-transporter reference instead of retrying blind
   installs.

For future upstream updates, rebase deliberately from the pinned baseline,
review storage/editor format changes, rebuild and repeat these regressions
before device installation. v0.2 remains a separate branch/PR after v0.1
hardware validation.
