# Writing engines for the MFX Pedal

This is a practical guide to adding your own effect to USER LAB, or building a
whole new engine. It assumes you have read the sector-8 note in the main README.

## Clean-room rule

Write DSP from an understanding of **what an effect does** — its frequency
response, its distortion curve, its dynamic behaviour — and implement it yourself.
Do not transcribe or translate decompiled firmware from any device. Algorithms
(a comb filter, a frequency shifter, a resonant band-pass) are fair game; another
vendor's specific code is not.

## The engine ABI

The firmware keeps a registry of 20 engines. Each engine is a struct (`mfx_engine_t`
in `sdk/mfx_fw.h`) with function pointers and a parameter array. The ones you set:

| Field | Meaning |
|-------|---------|
| `name[16]`, `category[12]` | shown in the program list |
| `process(e, in, out, n)` | audio callback; `in`/`out` are interleaved stereo `int16`, `n` = total int16 count |
| `activate(e)` / `deactivate(e)` | called when the program is selected / left |
| `tap(e, period, flag)` | TAP footswitch and MIDI clock arrive here; `period` = beat interval in ms |
| `params[...]` | registered with `mfx_add_param(e, idx, type, name, default, min, max, cb)` |
| `state` | your per-instance state pointer (point it at CCM RAM) |

Register parameters in your constructor, then `mfx_add_presets(...)` for the preset
list. `mfx_add_param` types include `LEVEL` (0..0xFFFF), `ENUM` (uses a `label()`
function you supply), `MIX`, etc. See `engine_ctor()` in `engines/lab/lab.c`.

## State: no writable globals

The linker script (`sdk/link.ld`) **forbids `.data` and `.bss`**, because the
bootloader does not set them up for code in sector 8. So:

- Put all mutable state in a `state_t` struct that lives in **CCM RAM**
  (`0x10000000`, 64 KB), reached through the `ST` macro.
- Constant tables (`static const`) are fine — they live in flash.
- Audio delay lines use the firmware's **shared delay buffer** (`MFX_DELAYBUF`,
  42000 × `int16`). Clear it when your mode starts (see `reset_mode`), or stale
  audio from the previous program will feed back.

## Adding a mode to USER LAB

1. Add an `M_YOURMODE` entry to the mode `enum`.
2. Add its name to `mode_names[]` and its four parameter names to `pnames[]`
   (same index order as the enum).
3. Write `static void m_yourmode(state_t*s, float il, float ir, float*ol, float*or_,
   float wet, float dry)`. Read your four smoothed params from `s->sm[0..3]`.
   Output `*ol = dry*il + wet*your_wet_left;` likewise for the right channel.
4. Add a `case M_YOURMODE:` to the dispatch `switch` in `lab_process`.
5. Optionally add presets (`PR(M_YOURMODE, p1,p2,p3,p4, mix)`).
6. If the mode is tempo-based, read `s->clocked` / `s->tempo` / `s->clkmul`
   (already computed each block) like NITRO/FLANGER/MURF do.

## DSP helpers already in `lab.c`

- `svf(...)` — TPT state-variable filter (low/high/band outputs), stable and resonant.
- `soft(x)` — smooth saturator (bounded, ~unity for small `x`).
- `rd(buf, len, pos)` — fractional (interpolated) read from a delay buffer.
- `expmap(x, lo, hi)` — exponential 0..1 → `lo..hi` (good for frequencies/times).
- `tsin(s, phase)` — table sine; `onepole`, `frand`, `exp2_approx`, `clampf`, etc.

## Budget

Keep the whole engine under the sector-8 window (~14 KB). `build.py` prints the
size and aborts if it overflows. Prefer compact hand-written DSP over large lookup
tables. Audition on the PC harness (`host/render_lab.c`) before every flash.
