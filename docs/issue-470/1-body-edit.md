# Issue #470 — body edit

Replace the **last two bullets** of the "Workarounds I tried" section.

---

## FIND (current text — delete these two bullets)

```
- Shipping a silent `.fuz` at my own INFO's voice path, sized to cover
  generation plus speech. Works, but the duration has to be guessed against
  variable TTS latency, and it forces a word limit on generated lines so speech
  can't outrun the fixed slot. Too long and the NPC stands mute; too short and
  the next line is eaten.
- A looping spacer INFO gated on a global that Papyrus clears once
  `GetSpeechQueueSize()` drains and `GetTimeSinceLastAudioEnded()` goes quiet.
  Genuinely adaptive and it's what I shipped, but it needs several failsafes or a
  stalled script traps the player in a dialogue loop — not something every mod
  author should have to reinvent.
```

## REPLACE WITH

```
- **Shipping a silent `.fuz` at my own INFO's voice path**, sized to cover
  generation plus speech. This is what I ended up shipping, and it is a guess.
  The duration is fixed at authoring time; TTS latency is not. And because the
  engine holds the slot for the file's full length regardless of when the audio
  actually ends, a longer file trades dead air for safety margin one-for-one —
  there is no setting that is right for both a fast local voice and a slow cloud
  one. It also forces a word cap on generated lines so speech can't outrun the
  slot.

- **A looping spacer INFO** gated on a global that Papyrus clears when the NPC
  stops speaking. Genuinely adaptive and the right shape — but I built it, tested
  it, and had to remove it, because **there is no signal available to drive it**:

  - `GetSpeechQueueSize()` returns `0` on every call, including while generated
    audio is audibly playing. In my most recent log it is `0` across all 99
    calls. It never reports a non-zero queue, so it cannot "drain".
  - `GetTimeSinceLastAudioEnded()` counted straight through playback without
    ever resetting (see the beta 23 update in the comments — this one is now
    fixed).

  With neither signal, the hold can only ever end on a timeout, and a
  self-relinking INFO whose release depends on a timeout risks trapping the
  player in a dialogue loop. The spacer never fired once in testing.
```

---

## Why this edit matters

The current text says the adaptive spacer is "genuinely adaptive and it's what I
shipped". Both halves are wrong:

1. **It was not shipped.** It was built, tested, and removed. The spacer INFO
   never fired once — there is no trace of the record in any log. What actually
   shipped is the silent `.fuz`.
2. **The mechanism described was impossible, not merely fiddly.** Both Papyrus
   signals it depends on were blind.

As written, a maintainer reasonably concludes the reporter already has a working
adaptive solution and this is a polish request. The truth is the opposite: every
adaptive route was blocked by SkyrimNet's own signals, leaving only a
fixed-length guess. That is a considerably stronger case for fixing it.
