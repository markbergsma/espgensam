# How GLM configures a speaker, and what espgensam sends

*Written 2026-09-21, from the captures listed below.*

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

Five logs, all under `captures/`, which is gitignored and therefore local to the machine that
recorded it:

| Log | Lines | What it caught |
|---|---|---|
| `glm_source_select_capture.log` | 13846 | source selection from the GLM app |
| `glm_scp_shelf_capture.log` | 7329 | Sound Character Profiler shelf edits |
| `glm_group_switch_capture.log` | 3764 | switching between the three groups of a GLM 5.2 setup |
| `glm_v5_no_wakeup_capture.log` | 936 | GLM v5 boot without a wake sequence |
| `glm_startup_capture.log` | 924 | OEM adapter startup |

The hardware is a Genelec 7350A subwoofer at address `0x02` and two 8330A two-way monitors at
`0x03` and `0x04`. The corresponding setup file is `captures/glm-config/Home Cinema.sam`; where
this document cites a `.sam` field, that is the file meant. Counts quoted as "across the
captures" are over all five logs.

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
| Sub extras | `3E 00 00`, `42 00 00`, `3C 00 00` | not sent |
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

Every opcode espgensam does not send has a payload that never varies, across all five logs:

| Opcode | Frames | Payload | Addressed to |
|---|---|---|---|
| `0x05` signal generator | 35 | `04 00 00 00 FF FF EA 00 01 90 00 02 DA`, always | every speaker |
| `0x2D` session preamble | 10 | `00`, always | broadcast, at app connect only |
| `0x3C` | 18 | `00 00`, always | `0x02` only |
| `0x3D` input sync | 53 | `00 00`, always | every speaker, and broadcast at app connect |
| `0x3E` | 19 | `00 00`, always | `0x02` only |
| `0x42` | 11 | `00 00`, always | `0x02` only |
| `0x10 01 09` | 28 | `00 00 00`, always | every speaker |

`0x10 01 09` is not a stray: there are exactly 28 `0x10 01 00` frames too, and each `09` follows
an `00` immediately. The pair travels together.

---

## 5. What the tail is

`0x2B` takes only two values in 159 frames: `03` and `04`. Read through espgensam's own bitfield
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

## 6. The subwoofer-only opcodes are probably the LFE quartet

`0x3C`, `0x3E` and `0x42` are addressed to `0x02` and nothing else, in 48 frames. Counting the
subwoofer-only fields of a `.sam` group node gives exactly eight. `Phase(degrees)` is already
mapped to `0x10 02`; `PhaseCalibratedWith`, `SubwooferGroupID` and `MultiChannel_AES_EBU` are
metadata or deliberately dropped. That leaves four:

| Opcode | Follows | Candidate field |
|---|---|---|
| `3C 00 00` | `3B` (2-byte big-endian crossover Hz) | `LFE_CrossoverFrequency(Hz)` — same shape, adjacent opcode |
| `3E 00 00` | `40 01` (subwoofer's second input routing) | `LFE_Channel` — a routing field beside a routing frame |
| `42 00 00` | `3E` | `LFE_Level`, perhaps with the `LFE_+10` boolean in the spare byte |

**The evidence has no variance, so this cannot be confirmed from what we have.** The captured
setup runs `LFE_Channel:0` and `LFE_Level:0` — LFE inactive — so every one of these frames is
`00 00` and there is nothing for the hypothesis to be tested against. `LFE_CrossoverFrequency(Hz)`
is 120 in that file, which would encode as `00 78`, not `00 00`; the mapping survives only if
GLM zeroes the whole quartet when no LFE channel is assigned. Plausible, unproven.

They are therefore not sent. A hardcoded `00 00` would be byte-correct for this setup and would
silently write "LFE off" over one that uses LFE. §8 has the experiment that settles it.

---

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
- **No flash commit.** Neither does GLM: there is no `0x15` anywhere in the group-switch
  capture, so a group switch is a RAM-only operation on both sides.

---

## 8. Open questions

**Does a group push actually land?** This is the question that decides whether any of the
unsent frames matter. Monitors ACK and Home Assistant reports success, but nothing has been
measured. *Experiment:* apply two groups whose calibration differs audibly — a large level
trim, or a deep notch — and measure, rather than trusting the ACKs.

**Are `0x3C` / `0x3E` / `0x42` the LFE quartet?** *Experiment:* build a GLM group that assigns
an LFE channel, a non-zero LFE level, and an LFE crossover different from the bass-management
crossover. Switch to it with espgensam listening only. If the three frames go non-zero and track
those fields, the mapping is settled and the encodings fall out with it. If they stay `00 00`,
the hypothesis is dead and they are something else.

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
