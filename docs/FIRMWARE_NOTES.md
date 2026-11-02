# ALM MFX pedal v118 — RE notes

- UF2 family 0x57755a58, app @ 0x08010000..0x0809c500 (574720 B). Rebuild with scripts/mkuf2.py is byte-identical.
- MCU: STM32F405/407 class (98 vectors, SP 0x20020000, >=1MB flash). Free flash after 0x0809c500 (~400 KB).
- ABI: softfp (floats in core regs), FPU fpv4-sp-d16. Code compiled ~-O0.
- RAM: .data 0x20000000-0x20001ae8 (init @ flash 0x0809a9b4), .bss to 0x2001f338 -> ~3 KB left for stack.
- Shared delay buffer: 0x20008428, 42000 int16 (len var @ 0x2000004c, write ptr @ 0x2001cc48). CCM 0x10000000 (64 KB) used as ring buffers by some engines.
- Engine registry @ 0x20002218: 20 slots x 0x4e4. count byte +0x61d4, active ptr +0x61d0, mute/xfade counter +0x61d8. 19 used -> 1 free slot.
- Registration: FUN_08025adc (calls 19 ctors; last FUN_0803b15c UTILITIES).
- Switch engine: FUN_080262b8(id). Lookup FUN_08026198.
- Engine struct: +0x00 name[16], +0x10 category[12], +0x1c params[16] x 0x48, +0x49c nparams, +0x49d order[], +0x4ad order count,
  +0x4b0 desc string, +0x4bc process(eng, int16* in, int16* out, short n) interleaved stereo, n = int16 count,
  +0x4c0 activate(eng), +0x4c4 deactivate(eng), +0x4c8 tap/clock(eng, period, flag), +0x4cc poll-changed() ,
  +0x4d0, +0x4d4(eng, x), +0x4d8, +0x4e0 state ptr.
- add_param FUN_0802666c(eng, idx, type, name, default, min, max, cb(param, idx, val)).
  param+0x3c / +0x40 / +0x44 setters FUN_08026c34/50/6c (formatters), +0x2e flag FUN_08026a68. get value FUN_080269f0.
  FUN_080267e8(eng, idx, desc, n) = description/"info" param.
- FUN_08029b20(eng, paramcb) sets global current param handler (called in ctor + activate).
- printf = FUN_0801ec24, snprintf = FUN_0801ecf8.
- Audio dispatch FUN_08025874(in, out, n) (pointer @ 0x801ff98), called from DMA path FUN_0801fe1c.

Engines (slot: name / category):
0 DIGI-PCM/ECHO 1 TAPE-TECH/ECHO 2 CRUMBULAR/ECHO 3 URSA MINOR/ECHOVERB 4 POCKET PL8/REVERB 5 ALMICON/REVERB
6 QUAIDRA/REVERB 7 YETTI/REVERB 8 SLINKY/REVERB 9 TH-30/DISTORTION 10 TY-50/DYNAMICS 11 2051 BIT/CORRUPTER
12 MODULATING/PANNER 13 RING+FREQ/MODULATOR 14 ENSEMBLES/ENSEMBLE 15 MULTI/PHASER 16 SQUOVAL/REZONATOR
17 AKEMIE/POLY VOICE 18 UTILITIES
