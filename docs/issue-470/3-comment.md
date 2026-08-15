## Field capture on beta 23.1 — the suppression window measured, and corrections to my comment above

I offered above to rebuild with a deliberately short placeholder to give you a fresh capture. That turned out to be unnecessary: a user hit this on their own machine running my shipping build, and their logs are a better capture than a rigged test would have been. Different machine, different modlist, different language (Russian), stock **beta 23.1** (`0-23-1-0`, `hotfix/beta23.1 @ d3fc789ec95a`, Build Date Aug 10 2026), runtime 1.6.1170, Inworld.

The user's game is Russian, so quoted dialogue is theirs as logged. `<courier>` stands in for the actor's name; everything else in these excerpts is verbatim.

### Correction 1: the suppression is not "while audio is playing" — it outlasts playback by seconds

I described the drop as binary: *"if any generated audio is still playing at the moment the next INFO fires, that line is discarded outright"*. That is not what happens. A line is also discarded when it fires **seconds after** the generated audio has already finished.

Clearest case. My second unvoiced INFO is refused 2.69 s after SkyrimNet itself reported the audio complete:

```
23:37:05.314  PlayAudioThroughEngine: sound 4862 finished after 6.24s (estimated 6.10s)
23:37:05.314  ActionManager: Speech ended for actor <courier>
23:37:05.590  GetTimeSinceLastAudioEnded: Time since last audio ended: 275ms
23:37:07.588  GetTimeSinceLastAudioEnded: Time since last audio ended: 2273ms
23:37:08.000  Skipping silent dialogue for <courier> - actor silenced (TTS playback in progress)
```

I checked the whole window either side of this: there is no queue or playback activity for this actor at all between 23:37:00 and 23:37:08. Nothing was playing and nothing was pending. `TTS playback in progress` is printed 2.7 s after playback demonstrably ended, by SkyrimNet's own accounting on two independent subsystems.

Across five deliveries, sorted by the gap between `ActionManager: Speech ended` and the next INFO firing:

| TTS latency | speech | gap after speech ended | next line | source |
|---|---|---|---|---|
| 4.24 s | 4.87 s | **4.09 s** | spoken | my machine, log on hand |
| 4.59 s | 5.62 s | **3.13 s** | spoken | *my notes only — see caveat* |
| 1.18 s | 6.24 s | **2.69 s** | dropped | user's machine |
| 5.20 s | 5.73 s | **2.26 s** | dropped | my machine, log on hand |
| 4.43 s | 6.17 s | **1.60 s** | dropped | user's machine |

From logs I can still produce, the window is **greater than 2.69 s and no more than 4.09 s**. The 3.13 s row would put it at ~3 s, but that session's log has rotated out and I am only citing it as indicative — I got caught relying on notes once already in this thread and would rather flag it than repeat that.

Either the flag is not cleared when playback completes, or there is a deliberate trailing window. In both readings the log message contradicts the behaviour, which is what sent me looking in the wrong place for a long time.

It also moves the target for anyone working around this. My mitigation is a fixed-length silent `.fuz` sized to cover latency plus speech; it actually has to cover latency plus speech **plus a multi-second quiet period**, on every provider, at every line length.

### Correction 2: my `GetSpeechQueueSize()` claim was too absolute

I said above it is *"always 0 ... never returns anything else anywhere in the file"*. That was true of the session I had and is **not** true generally — in this user's log it returns 1, 2, 3 and 4 across nineteen calls. The narrower point stands: it reads `0` on every call throughout silent-INFO playback (7 calls while my line was audibly playing), so it does not appear to observe this path.

### The mod events I asked for already exist — they just skip this path

My "small addition to the fix list" above asked for a `SkyrimNet_SpeechComplete` / `SkyrimNet_AudioEnded` mod event. **Those already exist on beta 23.1** and fire correctly — I withdraw the request as written. The real gap is narrower: they are not raised for silent-INFO (DBVO) playback.

Same actor, same session, ~15 seconds apart. My unvoiced INFO:

```
23:35:14.911  Sent 'SkyrimNet_SpeechStarted' (sender=0x00039FB7)   <- at GENERATION start
23:35:19.282  ActionManager: Speech started for actor <courier>
23:35:25.500  ActionManager: Speech ended   for actor <courier>
   ... no AudioStarted, no AudioEnded, no SpeechComplete for this line ...
```

The same courier's ordinary lines, minutes later in the same conversation:

```
23:35:30.052  ActionManager: Speech started for actor <courier>
23:35:30.056  Sent 'SkyrimNet_AudioStarted' (sender=0x00039FB7)
23:35:32.139  ActionManager: Speech ended   for actor <courier>
23:35:32.141  Sent 'SkyrimNet_AudioEnded'   (sender=0x00039FB7)
```

(`0x00039FB7` is the courier's reference — confirmed by those events pairing to within 4 ms of his `ActionManager` transitions, not assumed.)

So the channel, the sender plumbing and the internal signal are all present and working, and `ActionManager: Speech started/ended` fires correctly **even for the silent-INFO line** — it simply is not forwarded to the mod-event channel there. For my line the only event raised is `SpeechStarted`, at generation start, 4.4 s before the first word.

**Raising the existing `AudioStarted`/`AudioEnded` for silent-dialogue playback would let a mod build a correct adaptive hold** — the workaround I abandoned as impossible in the original report. That looks like a much smaller change than what I originally asked for.

One caveat: an event at playback-end is still short of when the actor stops being silenced (correction 1). A hold built on `AudioEnded` alone would still fire too early unless the flag is cleared with the audio, or the window is queryable.

### Consequence: chained unvoiced INFOs collide

Because the window outlasts the audio, consecutive unvoiced INFOs cannot chain. My mod uses up to four, because a courier delivery can carry several items and each needs its own line — which is how vanilla structures that topic. In both multi-item deliveries in this capture, line 1 spoke and line 2 was refused, at gaps of 1.60 s and 2.69 s after line 1's audio ended.

This is the same mechanism as the vanilla-line case in the original report, so I now treat it as one bug rather than two — and it is the part that no amount of placeholder tuning on my side can fix, since every line I add extends the window that eats the next one.

### A question rather than a claim

There is a top-level `shouldSilenceActors` setting (WebUI *"Should Silence Actors"*, described as *"Enable or disable automatic actor voice silencing during AI-generated speech"*). I have **not** tested whether it governs this specific check, so I am not putting it forward as a workaround — but if it does, that would at least give affected users something to try. Worth a word either way.

### What would fix it, updated

1. **Pre-generation for NPC-initiated dialogue** (fix #1 in the body) — still the one that removes the problem rather than managing it.
2. **Clear the silence flag when playback ends**, or make the trailing window explicit and queryable. As it stands the log message asserts playback is in progress when it demonstrably is not.
3. **Raise `SkyrimNet_AudioStarted`/`AudioEnded` for silent-dialogue playback**, so mods can hold correctly instead of guessing a fixed duration.

Happy to supply the full logs (Papyrus + SkyrimNet, ~21 MB) or any trimmed window on request.
