# MFX USER LAB

An unofficial SDK and multi-effect program for the **ALM Busy Circuits "MFX Pedal"**
(firmware **v118**). It adds one new program — **USER LAB / FX VIBES** — into the
pedal's single free engine slot (#20), without touching any of the 19 factory programs.

> **Personal use / interoperability.** This repository contains **only original
> code**. It ships **no ALM firmware** — neither the original `v118` nor a patched
> build. You supply your own `v118`, and `build.py` patches it locally on your
> machine. Please do not redistribute ALM's firmware (original or patched).
>
> **Clean-room effects.** The modes below are **original DSP** written from a
> behavioural understanding of how each effect *sounds and responds* — not ports
> or decompilations of anyone's code. Mode names are evocative; they are not
> trademarks of the gear that inspired them.

## The program

USER LAB is one program with **seven selectable modes** (the `MODE` parameter),
plus two global clock controls. Every mode exposes four parameters mapped to the
A/B knobs and the value encoder; names and values show in clear on the display.

| Mode | Character | P1 | P2 | P3 | P4 |
|------|-----------|----|----|----|----|
| **NITRO** | resonant band-pass "scream" swept by an LFO | FREQ (centre) | SPAN (sweep width) | RATE (LFO speed) | DEPTH (amount) |
| **CHORD** | three pitched voices over the dry root (chord generator) | HARMONICS (chord) | TIMBRE (inversion) | MORPH (detune) | ROOT (transpose) |
| **FLANGER** | flanger blended with a spring tail | RATE | DEPTH | REGEN | SPRING |
| **TERMINAL** | CRT/8-bit degrader: bit-crush + sample-rate reduce + glitch | BITS | RATE | GLITCH | FILTER |
| **PHONE** | "voice down a phone line": telephony band-pass + grit + line noise | DRIVE | BAND | CRUSH | NOISE |
| **MURF** | 8 resonant band-pass filters animated by a step sequencer | PATTERN | RATE | ENV | FREQ (band shift) |
| **HAZE** | long, modulated dream-pop reverb wash with tremolo | DECAY | TONE | MOD | TREM |

### Global clock (all tempo-based modes)

Two extra parameters drive the rate of NITRO, FLANGER and MURF:

- **CLOCK** — `INT` / `MIDI`. The program syncs whenever a tempo arrives, from the
  **TAP** footswitch or from **MIDI clock**.
- **CLK MUL** — clock multiplier, 27 steps:
  `0.1 · 0.25 · 0.3 · 0.37 · 0.46 · 0.5 · 0.75 · 0.87 · 0.9 · 0.93 · 1 · 1.1 · 1.3 ·
  1.5 · 1.7 · 1.8 · 2 · 2.6 · 3 · 3.5 · 3.7 · 4 · 5 · 6 · 7 · 7.5 · 8` (default `1`).

**MIX** is the firmware's standard wet/dry control (auto-hidden in insert mode). The
A/B knob assignment bug (offset stuck at 153 %) is worked around automatically.

### CHORD types (HARMONICS)

`OCT · 5 · SUS4 · m · m7 · m9 · m11 · 69 · M9 · M7 · M` — three voices over the dry
root. TIMBRE lifts the upper voices (inversion feel), ROOT transposes ±12 semitones,
MORPH detunes/animates the voices.

### MURF patterns

Twelve original 8-step animation patterns (all-on, staircases, rolling pairs,
odd/even, converge-diverge, fill/empty, bounce, interlock, sparse pulses,
call-and-response). PATTERN selects; RATE (or the clock) sets the step speed; ENV
sets each gate's attack/release (snappy → smooth); FREQ shifts the 8 bands ±1 octave.

## Why only sector 8 (the ~14 KB limit)

The pedal's USB bootloader only reliably erases and programs flash **up to the end of
sector 8 (`0x080A0000`)**. The factory firmware ends at `0x0809C500`, leaving about
**14 KB** of safe space — that is where USER LAB lives.

- Writing inside that window flashes fine.
- Writing past `0x080A0000` (sectors 9+) makes the upload stop partway and the USB
  disk disconnect — the bootloader never erases those sectors.

`build.py` aborts if the engine exceeds the window. USER LAB (seven modes) is ~12 KB.
This is also why a full large granular/reverb engine (tens to hundreds of KB of
tables) does not fit here — the DSP has to be hand-written and compact.

## How the patch works

Of the entire firmware, exactly **one instruction** changes: the `BL` that registers
the last factory engine is redirected to a small hook, which first calls the original
constructor and then registers USER LAB in the free slot. All engine code and state
live in the free tail of sector 8 and in CCM RAM; there are **no writable globals**
(the linker forbids `.data`/`.bss`), so nothing collides with the firmware.

## Flashing

1. Keep your original `mfx-pedal-v118.fw` at hand.
2. With the pedal **unpowered**, connect USB-C — it mounts as a USB disk (Update Mode).
3. Copy the `.fw` you built to the drive root. Wait for "Update Complete".
4. Browse to **USER LAB** at the end of the program list.
5. To revert, copy the original `v118` back the same way. The pedal cannot be bricked
   this way — the bootloader always lives in a protected sector.

## Build

You need `arm-none-eabi-gcc` and Python 3, plus **your own** `mfx-pedal-v118.fw`.

```sh
python3 build.py  mfx-pedal-v118.fw  mfx-pedal-v118-userlab.fw  engines/lab
```

`build.py` verifies the SHA-256 of the stock v118, compiles the engine for the
STM32F405 (Cortex-M4F, softfp), checks it fits sector 8, applies the one-instruction
hook, and writes the patched `.fw`.

## Audition on your computer (no pedal needed)

The same DSP source compiles on a PC so you can render WAVs before flashing:

```sh
cd host
gcc -O2 -I ../sdk -I ../engines/lab render_lab.c -lm -o render_lab && ./render_lab
gcc -O2 -I ../sdk -I ../engines/lab test_hw.c   -lm -o test_hw    && ./test_hw
```

`render_lab` writes one WAV per mode; `test_hw` runs behavioural checks (feedback
decay, LFO motion, band-pass response, bit-crush, reverb tail length).

## Writing your own mode

See [`docs/WRITING_ENGINES.md`](docs/WRITING_ENGINES.md) for the engine ABI, the
state/DSP conventions, and a walkthrough of adding a mode. The reverse-engineering
notes for the firmware interface are in [`docs/FIRMWARE_NOTES.md`](docs/FIRMWARE_NOTES.md).

## Layout

```
build.py                 patch/flash tool (checks SHA, compiles, hooks, writes .fw)
sdk/mfx_fw.h             reverse-engineered firmware interface (addresses, structs) — no ALM code
sdk/link.ld             linker script (places code in sector 8, forbids .data/.bss)
engines/lab/lab.c       USER LAB: the seven-mode effect program
host/render_lab.c       PC render harness (WAV per mode)
host/test_hw.c          PC behavioural tests
docs/                   firmware RE notes, Ghidra scripts, engine-writing guide
examples/user_template  minimal engine template to start from
```

## License

Original code in this repository is released under the MIT License (see `LICENSE`).
This project is not affiliated with or endorsed by ALM Busy Circuits, Teenage
Engineering, Moog Music, or any other manufacturer referenced for descriptive purposes.
