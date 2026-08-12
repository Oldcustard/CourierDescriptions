## Update for beta 23.1 — one signal fixed, and a much simpler ask

Retested on **beta 23.1** (`0-23-1-0`), AE 1.6.1170, Inworld. I've also corrected
a mistake in the original report — see the end.

### Still true on beta 23.1

`PrepareNPCDialogue` still declines to scan for NPC-initiated dialogue:

```
09:58:20.055  Papyrus::PrepareNPCDialogue called with:
09:58:20.055  PrepareNPCDialogue: Base game dialogue detected, no VAD needed
```

Latency is unchanged. Measured end to end on a forced greeting:

```
09:58:25.331  my unvoiced INFO fires, silent placeholder opens the slot
09:58:25.391  TopicInfoEventHandler: On-demand: Generating TTS for silent response
09:58:28.908  Cached TTS for voiceID 'maleyoungeager' (488672 bytes)
09:58:29.918  AudioManager: Started engine playback          <- 4.587 s after the INFO fired
09:58:35.541  ActionManager: Speech ended                    <- 5.623 s of speech
```

### Fixed in beta 23 — thank you

**`GetTimeSinceLastAudioEnded()` now works.** It reset from `144236ms` to `933ms`
within a second of speech ending. Under beta 22 it counted straight through
playback and never reset. I assume this falls out of routing TTS through the game's
audio engine (`EngineSoundPlayer`, `AudioManager: Started engine playback`) — either
way it is the first usable "the NPC has stopped talking" signal from Papyrus.

### Still missing

- **`GetSpeechQueueSize()` is still always `0`** — including every second of
  playback. `0` across all 99 calls in this session's log; it never returns
  anything else anywhere in the file.
- **No completion event.** Only `SkyrimNet_SpeechStarted` fires, and it fires at
  TTS *generation* start (09:58:25.405), 4.5 s before playback begins — so it
  means "a line is coming", not "he is speaking now".

### A small addition to the fix list: fire a speech-complete mod event

Both ends of this already exist. The internal signal is computed **with the actor**:

```
ActionManager.cpp:87   Speech ended for actor <NPC> (7336273823387584939)
```

and the mod-event channel already exists and already fires for the start:

```
ModEventSender.cpp:47  Sent 'SkyrimNet_SpeechStarted' (numArg=0.0, sender=0x00039FB7)
```

A matching `SkyrimNet_SpeechComplete` / `SkyrimNet_AudioEnded` would let any mod
build a correct adaptive hold instead of guessing a fixed duration. This looks
like plumbing rather than new machinery.

### But fix #1 (pre-generation) would make all of this unnecessary

This is the part I under-argued originally, and it is the reason I'd rank
pre-generation well above the other options.

**The latency offset is the entire problem.** Fuz Ro D-oh's placeholder grows at
~0.5 s per word while speech consumes ~0.35 s per word (measured: 5.623 s for 16
words = 2.85 words/sec). So its slot *already* outgrows the speech it has to
cover — the margin is positive at every sensible line length. The only reason it
fails today is that ~4.6 s of TTS latency has to be paid out of the same slot
before the first word is spoken:

| Words | FRD slot | Today (4.6 s latency) | With pre-generation |
|---|---|---|---|
| 12 | 6 s | −2.8 s | **+1.8 s** |
| 16 | 8 s | −2.2 s | **+2.4 s** |
| 20 | 10 s | −1.6 s | **+3.0 s** |
| 24 | 10 s (cap) | −3.0 s | **+1.6 s** |

Remove the latency and **stock Fuz Ro D-oh handling just works**, up to roughly
28 words, with no mod-side workaround at all: no shipped silent `.fuz`, no word
cap on generated lines, no spacer INFO, no new Papyrus signals. Every workaround
in the original report exists solely to absorb that one offset.

One practical note on timing: it needs to start early enough. In this run the
first `TESTopicInfoEvent` (the forced greeting, `039F79`) fired at 09:58:23.480 —
**1.85 s** before my INFO at 09:58:25.331. Against a ~3.5 s generation that leaves
~1.65 s of residual latency rather than zero, which is still comfortably positive
(16 words → 7.26 s needed against an 8 s slot). Hooking anything earlier than the
first topic-info event — the greeting package, or `DialogueItemCtorHook`, which
already receives `TESQuest*, TESTopic*, TESTopicInfo*, TESObjectREFR*` — makes it
comfortable rather than marginal.

### Correction to the original report

The last workaround bullet said the adaptive spacer INFO was "genuinely adaptive
and it's what I shipped". That was wrong on both counts and I've edited the body.
It was built, tested and **removed** — the spacer never fired once — and the
mechanism it describes was not achievable, because both signals it relies on
(`GetSpeechQueueSize()`, and `GetTimeSinceLastAudioEnded()` as it behaved in beta
22) were blind to the audio. What actually shipped is the fixed-length silent
`.fuz`. Apologies for the confusion — the corrected version makes the case
better anyway.

### Confirmed: the vanilla line is still dropped on beta 23

The suppression itself is unchanged, and beta 23 is more explicit about it than
beta 22 was. Where beta 22 printed the subtitle and silently played nothing, beta
23 names the line it discards:

```
TopicInfoEventHandler.cpp:641   ... actor silenced (TTS playback in progress)
TopicInfoEventHandler.cpp:358   Dropping suppressed line from <NPC>: Looks like that's it. Got to go.
```

That is base-game audio being deleted because SkyrimNet's own generated audio is
still playing.

The property that makes this hard to work around is that **the drop is binary,
not proportional**. If any generated audio is still playing at the moment the
next INFO fires, that line is discarded outright — a 0.2 s overlap costs the
whole line exactly as a 4 s overlap does. So "make the placeholder a little
longer" is not a partial improvement; nothing short of *zero* overlap preserves
the vanilla line. That is what drove me to a 13 second placeholder for a 5.6
second line, and it is why the margin table above matters: the target isn't
"mostly fits", it's "always fits, on every provider, at every line length".

(The two log lines above are quoted from my analysis notes — that session's log
has since rotated out, and my current build no longer triggers the path because
the 13 s placeholder ends the generated line 3.1 s before the closing line fires.
Say the word and I'll rebuild with a deliberately short placeholder to give you a
fresh capture with timestamps.)
