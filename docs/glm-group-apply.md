# How GLM configures a speaker, and what espgensam sends

*Written 2026-09-21, last revised 2026-09-22, from the captures listed below.*

This document is about one question: when GLM and espgensam are handed the same setup file and
told to apply the same group, do they put the same thing on the wire? It records GLM's two
configuration sequences frame by frame, the opcodes espgensam does not send, and — for each
difference — whether it is a deliberate choice or an open question.

It is not a comparison with any other project; for that see
[glm-protocol-comparison.md](glm-protocol-comparison.md).

---

## 1. Scope and method

Everything here is decoded from OEM GLM v5 adapter traffic captured with espgensam itself, in
the listen-only build (`espgensam-capture-local.yaml`, which sets `listen_only: true` so the
component decodes but never transmits):

```bash
esphome logs espgensam-capture-local.yaml | tee captures/glm_group_switch_capture.log
```

Eight logs, all under `captures/`, which is gitignored and therefore local to the machine that
recorded it:

| Log | Lines | Setup file | What it caught |
|---|---|---|---|
| `glm_source_select_capture.log` | 13846 | Home Cinema | source selection from the GLM app |
| `glm_scp_shelf_capture.log` | 7329 | Home Cinema | Sound Character Profiler shelf edits |
| `glm_group_switch_capture.log` | 3764 | Home Cinema | switching between three groups |
| `glm_lfe_capture.log` | 2186 | espgensam | an LFE group, `LFE_+10` set |
| `glm_lfe2_capture.log` | 1666 | espgensam | the same group, `LFE_+10` clear |
| `glm_lfe3_capture.log` | 1961 | espgensam | a second LFE group, feed on A instead of B |
| `glm_v5_no_wakeup_capture.log` | 936 | Home Cinema | GLM v5 boot without a wake sequence |
| `glm_startup_capture.log` | 924 | Home Cinema | OEM adapter startup |

The hardware throughout is a Genelec 7350A subwoofer and two 8330A two-way monitors. Leases are
reassigned per session, so the subwoofer is `0x02` in the first three logs and `0x05` in
`glm_lfe2_capture.log`; frames are quoted with whichever address they carried.

Two setup files. `captures/glm-config/Home Cinema.sam` is a calibrated 2.1 system, and is the
one meant wherever this document cites a `.sam` field without saying otherwise. The second was
built specifically to move fields the first held constant — a group trim, an LFE feed, a
different crossover — and lives outside the repository, in GLM's own directory; the two
configs that read it are `espgensam-lfe-local.yaml` and `espgensam-lfe-capture-local.yaml`.
Counts quoted as "across the captures" are over all eight logs.

A caveat that shapes everything below: **no acoustic measurement has been taken.** What is known
about espgensam's own pushes is that monitors ACK every frame and Home Assistant reports the
push complete. That is not evidence the DSP configuration lands. §8 says what would be.

---

## 2. GLM's two sequences

GLM has a full per-device configuration and a delta. Confusing one for the other is easy and
leads to wrong conclusions, so both are given here.

### 2.1 Full single-device configuration

From `glm_source_select_capture.log`, one 8330A being configured from scratch:

```
0x03  05  04 00 00 00 FF FF EA 00 01 90 00 02 DA   stop generator
0x03  17  01                                       prepare for configuration
0x03  10  0E 05 ...                                PEQ band 5
0x03  10  0E 06 ...                                PEQ band 6
0x03  3B  00 5A                                    crossover, 90 Hz
0x03  3D  00 00                                    input-select sync
0x03  40  00 01 02 00                              input routing
0x03  39                                           device info query
```

This is the sequence espgensam's group push actually resembles, and it is where `0x3B` goes out
to a **two-way** monitor.

### 2.2 Group switch: a delta over the above

From `glm_group_switch_capture.log`, one complete switch, 18:32:10.245 – 18:32:11.376:

```
FF  1F  00 00 08          duck, step 1
FF  1F  00 00 02          duck, step 2 (silence)
FF  3A  03 7F             wake
FF  3A  03 7F             wake
FF  3A  03 01             wake

0x02 3B 00 5A             -- routing pass, per speaker --
0x02 05 <13-byte const>
0x02 2B 04                   unmuted, LED off
0x02 40 00 01 02 00
0x02 40 01 01 01 00          subwoofer's second input
0x02 2B 04
0x03 05 <13-byte const>
0x03 2B 04
0x03 40 00 01 02 00
0x03 2B 04
0x04 ...                     same again
0x03 2B 04  0x04 2B 04  0x02 2B 04

FF   04                   -- DSP pass, per speaker --
0x03 17 01
FF   04
0x03 10 02 00 00 00 00       delay
0x03 10 01 00 7F FF FF       level compensation
0x03 10 01 09 00 00 00       unknown, always 000000
0x03 10 0E 00 ...            only the bands that changed
   ...
FF   04
0x02 17 01                   subwoofer
0x02 10 0E 00 ...
0x02 10 02 00 00 01 0B
0x02 10 01 00 6B 56 45
0x02 10 01 09 00 00 00
0x02 3E 00 00                subwoofer only
0x02 42 00 00                subwoofer only
0x02 40 00 01 02 00
0x02 3B 00 5A
0x02 3C 00 00                subwoofer only
0x02 10 0E ...
0x02 40 00 01 02 00          routing re-sent after the DSP block
0x02 40 01 01 01 00

FF   04                   -- tail --
0x03 2B 04  0x04 2B 04  0x02 2B 04
0x03 2B 04  0x04 2B 04  0x02 2B 04
0x03 3D 00 00
0x04 3D 00 00
0x02 3D 00 00

FF   1F  00 20 C4         restore x3
```

Because it is a delta, absence proves nothing. `0x3B` reaches only the subwoofer here — 11 of
11 crossover frames in this log go to `0x02` — not because two-ways have no crossover, but
because all three groups in this setup use 90 Hz, so it never changed. §2.1 shows two-ways
getting one when it does.

---

## 3. The envelope, beside ours

| Phase | GLM | espgensam |
|---|---|---|
| Duck | `FF 1F 00 00 08` → `FF 1F 00 00 02`, refreshed mid-push | `FF 1F 00 00 02` once |
| Wake | `FF 3A 03 7F` ×2, `FF 3A 03 01` | same, since 2026-09-21 |
| Routing pass, per speaker | `05`, `2B 04`, `40 00 …`, `40 01 …` (sub), `2B 04` | not sent; routing happens once, in the DSP block |
| DSP pass, per speaker | `FF 04`, `17 01`, `10 02`, `10 01 00`, `10 01 09`, `10 0E` ×changed | `FF 04`, `17 01`, `10 02`, `10 01 00`, `10 0E` ×20, `3B`, `40 00`, `40 01` |
| Sub extras | `3E` LFE level, `42 00 00`, `3C 00 00` | `3E` when the group has an LFE feed; `42` and `3C` not sent |
| Tail | `2B 04` sweep ×2, then `3D 00 00` per speaker | not sent |
| Restore | `FF 1F <vol>` ×3 | `FF 1F <vol>` once, preceded by `FF 04` |

espgensam's sequence is the step enum in `components/gensam/race.cpp`, dispatched by
`send_group_step_()` and driven one frame per `loop()` by `race_step_applying_group_()`.

### The wake burst is a second, distinct shape

GLM wakes the bus before **every** group switch, with the system already on, and the burst is
not the one it sends at app connect:

| | Sequence | Gaps |
|---|---|---|
| App connect | `7F` `01` `7F` `01` `7F` `01` | 5 / 20 / 3 / 2 / 1 ms |
| Before a group switch | `7F` `7F` `01` | ~22 / ~38 ms |

Six group switches out of six use the second shape. `GenSAMHub::send_wakeup()` already
implements the first, so the group push needed its own; see `GROUP_WAKE_SEQUENCE` in `race.cpp`.

---

## 4. Opcode evidence

Across all eight logs. Every opcode espgensam does not send has a payload that never varies —
which is exactly why `0x3E`, the one that does vary, turned out to be readable:

| Opcode | Frames | Payload | Addressed to | Sent by us |
|---|---|---|---|---|
| `0x05` signal generator | 75 | `04 00 00 00 FF FF EA 00 01 90 00 02 DA`, always | every speaker | no |
| `0x2D` session preamble | 12 | `00`, always | broadcast, at app connect only | no |
| `0x3C` | 41 | `00 00`, always | the subwoofer only | no |
| `0x3D` input sync | 105 | `00 00`, always | every speaker, and broadcast at app connect | no |
| `0x3E` LFE level | 38 | `00 00`, `00 06`, `00 FC` | the subwoofer only | **yes**, with an LFE feed |
| `0x42` | 25 | `00 00`, always | the subwoofer only | no |
| `0x10 01 09` | 62 | `00 00 00`, always | every speaker | no |

`0x10 01 09` is not a stray: there are exactly 62 `0x10 01 00` frames too, and each `09` follows
an `00` immediately. The pair travels together.

---

## 5. What the tail is

`0x2B` takes only two values in 389 frames: `03` and `04`. Read through espgensam's own bitfield
(`BYPASS_MUTE_MASK`, `BYPASS_LED_COLOR_MASK` in `const.h`), `04` is *unmuted, LED off* and `03`
is *muted, LED red* — byte for byte what `make_bypass(addr, false)` and `make_bypass(addr, true)`
already build.

So the tail is GLM **re-asserting the unmuted / LED-off state on every speaker** after
reconfiguring it, twice over, and then sending `3D 00 00` to each. `2B 04` appears only inside
group switches and never between them, so it is an artefact of reconfiguration rather than a
periodic refresh — plausibly defensive, on the assumption that a DSP reload can leave a speaker
muted or its LED in a calibration state.

`0x3D` has two positions, and the single-device one is more informative. Excluding replies and
polling traffic, it sits immediately *before* `0x40`: `3B`→`3D`→`40` three times and
`10`→`3D`→`40` four times. That reads as an **input-select preamble adjacent to routing** —
consistent with the `FF 2D 00` / `FF 3D 00 00` pair GLM sends at app connect — rather than a
latch applied after routing, which is what its position at the close of a group push suggests in
isolation.

---

## 6. LFE: what the subwoofer-only opcodes carry

The LFE channel is the discrete ".1" feed of a surround mix — a separate channel in the source
material, not the same thing as bass management, which redirects low frequencies *out of* the
main channels. A subwoofer reproduces both, and they sum inside it.

A `.sam` group node gives a subwoofer four LFE fields. Two captures of the same group differing
in exactly one of them settled where three of the four go.

### `0x3E` is the effective LFE level, in whole decibels

```
byte 2 = LFE_Level + 10 x LFE_+10,  as a signed 8-bit integer
```

| group | LFE_Level | LFE_+10 | expected | observed |
|---|---|---|---|---|
| Analog / AES3 | 0 | 0 | 0 | `00 00` |
| 2.1 LFE, capture 1 | −4 | 1 | +6 | `00 06` |
| 2.1 LFE, capture 2 | −4 | 0 | −4 | `00 FC` |

The two LFE-group captures are byte-identical on every other frame the subwoofer receives, so
the delta is attributable to `LFE_+10` alone — and it is **exactly 10**. That is what fixes the
unit at decibels and the flag's contribution at +10 dB, independently of how the absolute
values are read.

Note the field is *not* 16-bit two's complement: −4 dB goes out as `00 FC`, not `FF FC`. GLM
writes an int8 into the low byte without sign-extending into the pad.

Whole decibels is also all GLM's own LFE level control offers, confirmed in the UI rather than
inferred from the wire, so rounding to the field loses nothing. Its *range* has not been swept,
which is why espgensam validates against the encoding's limits rather than a narrower guess.

### `LFE_Channel` is routing, and needs no opcode of its own

It is the sub-channel byte of the subwoofer's **second** `0x40` frame. The two `0x40` frames a
subwoofer gets are two different feeds, not one setting sent twice:

| | input 0 | input 1 |
|---|---|---|
| no LFE | program, `40 00 02 00 03` | `40 01 02 00 00` — nothing |
| LFE on B | program, `40 00 02 00 01` | `40 01 02 00 02` — the LFE feed |
| LFE on A | program, `40 00 02 00 02` | `40 01 02 00 01` — the LFE feed |

All three groups carry `Input:3` in the setup file, yet the program input goes out as the A+B
**sum** only in the first. GLM narrows it off the LFE channel, which it has to: summing both
would fold the LFE content into the bass-managed path on top of its own feed.

**It narrows to the channel the LFE is not on**, not to A. A third capture switched between
two groups differing only in which channel carries the LFE, and the program input followed it
the other way — the two rows above are mirror images. That rules out "always A", which the
first LFE capture could not distinguish on its own.

### `LFE_CrossoverFrequency(Hz)` is never transmitted

It is 120 Hz — the standard LFE bandwidth limit — in every group of every capture, and GLM's
UI offers no way to change it. No frame carries 120 (`0x78`) anywhere.

### `0x3C` and `0x42` are still unknown

Both are subwoofer-only and `00 00` in all eight captures, and neither is an LFE field:

- `0x42` stayed `00 00` across a controlled change of `LFE_Level`, which is now known to live
  in `0x3E`.
- `0x3C` stayed `00 00` while `LFE_CrossoverFrequency(Hz)` was 120, which would be `00 78`.

An earlier revision of this document argued `0x3C` was "omitted when LFE is on, which is
backwards for an LFE parameter". That was wrong: it is absent from the LFE switch in the first
capture and present in the second, so its absence was GLM's delta suppression and carries no
meaning.

## 6a. `10 01 00` carries two trims summed, not one

A sixth capture (`glm_lfe_capture.log`, five switches over a three-group setup) settled what
the level word actually contains. One group carries `Group_Sensitivity:-0.3` at group level,
copied into each device node, over `Level_Sensitivity:0` everywhere; the other two carry 0.

GLM sent `10 01 00 7B A7 8D` to **all three speakers** in that group and `10 01 00 7F FF FF` in
the other two. And:

```
round(10^((Level_Sensitivity + Group_Sensitivity) / 20) x 8388607) == 0x7BA78D
```

exactly, for every device in every group. So the field is the sum of the device's own
calibration trim and the group's offset, not the per-device trim alone — which is all the
earlier captures could show, because their `Group_Sensitivity` was zero throughout.

`sam_import.py` previously listed `Group_Sensitivity` among the dropped fields, so applying
such a group played the whole system 0.3 dB loud. It now adds the two. The `-999`
"not calibrated" sentinel is applied first, deliberately: an uncalibrated device contributes
0 dB of its own and still takes the group's offset. Covered by
`test_group_sensitivity_is_summed_into_the_level()` in `tests/test_sam_import.py`, and by the
verbatim frame in `tests/test_dsp_frame.cpp`.

## 7. Where espgensam differs on purpose

Each of these is a choice, not an oversight.

- **All 20 PEQ slots, every time.** GLM sends only the bands that changed. espgensam transmits
  unconfigured slots as the bypass vector, because the monitor otherwise holds whatever the
  previous group left in them, and a stale filter is worse than an unnecessary frame. A push is
  a few hundred milliseconds either way.
- **`0x3B` to every enabled device.** This matches GLM's *full* configuration (§2.1). The
  delta-only group switch sending it to the subwoofer alone is not a counter-example.
- **One pass, with `3B` and `40` at the end.** GLM runs a routing pass across all speakers
  before the DSP pass and re-sends `40` afterwards. espgensam does a single pass per device.
  Whether GLM's ordering matters is unknown; nothing observed suggests it does.
- **One duck frame instead of a ramp, and no mid-push refresh.** GLM keeps polling during a
  push, so its own volume heartbeat would reassert the real volume partway through — hence the
  refresh. espgensam suspends polling for the duration, so a single frame holds.
- **No `2B 04` re-assert.** espgensam never touches mute for an enabled device, so it inherits
  whatever state was there rather than re-asserting one.
- **No `3E 00 00` when there is no LFE feed.** GLM sends one to every subwoofer regardless.
  The frame only restates a level that applies to nothing, and the two opcodes bracketing it
  are still unexplained, so a group without LFE is left alone rather than half-matching.
- **No LFE on a hand-picked input change.** Selecting an input from Home Assistant sends the
  subwoofer's input-1 frame with an empty channel: that path has no group behind it, so there
  is nothing to say what the LFE routing should be. A group push sets it properly.
- **No flash commit.** Neither does GLM: there is no `0x15` anywhere in the group-switch
  capture, so a group switch is a RAM-only operation on both sides.

---

## 8. Open questions

**Does a group push actually land?** This is the question that decides whether any of the
unsent frames matter. Monitors ACK and Home Assistant reports success, but nothing has been
measured. *Experiment:* apply two groups whose calibration differs audibly — a large level
trim, or a deep notch — and measure, rather than trusting the ACKs.

**What are `0x3C` and `0x42`?** Subwoofer-only, `00 00` in all eight logs, and now known not
to be LFE fields (§6). Nothing in either setup file has moved them. *Experiment:* none
obvious — they need a GLM feature nobody has exercised yet, so the next lead is likelier to
come from a capture taken for some other purpose.

**Do DSP writes stick when the monitors are in standby?** `race_step_configuring_()` calls
`finish_temporary_wake_()` — which commands standby — *before* `start_group_apply_()`, so a
group pushed during a standby-recovery discovery goes to monitors that have just been told to
sleep. If those writes are lost the next wake rediscovers and re-pushes, so it self-heals, but
GLM's wake-then-configure ordering hints that they may be. *Experiment:* capture our own traffic
across a standby cycle and check whether the monitors' reported level and crossover match the
group afterwards.

**Does the tail do anything observable?** The `2B 04` re-assert and the `3D 00 00` sweep are
cheap and constant, so they could be added at any time. Worth knowing first whether a speaker
ever comes out of a DSP reload muted, which is the behaviour they would be guarding against.

**What is `0x10 01 09`?** Constant `000000` on every device, paired 1:1 with the level frame.
Not decoded here, deliberately: read as a level it would mean −130 dB and mute the speaker.
