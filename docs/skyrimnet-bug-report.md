# SkyrimNet bug report — ready to paste

Target: https://github.com/MinLL/SkyrimNet-GamePlugin/issues/new?template=bug_report.yml

Each heading below is one field in the issue form. Dropdown fields give the exact
option to pick.

---

## What's happening?

When an NPC starts a conversation on their own — a forced greeting, a quest
delivery, any `InvisibleContinue` chain — and one of the lines is unvoiced,
`voiceSilentNPCs` generates the TTS correctly but nothing keeps the conversation
in step with it. The NPC stands silent for ~4–5 seconds, then starts speaking,
and the game has already moved on to the next line by the time he finishes.

The damaging part is what happens to that next line. It is a **normal, fully
voiced vanilla line**, and SkyrimNet suppresses it — its own log says so:

```
Skipping silent dialogue for <NPC> - actor silenced (TTS playback in progress)
(Dialogue) <NPC>: Looks like that's it. Got to go. (Talking to Player)
```

The subtitle is printed, the line is registered as spoken, and no audio plays.
So installing an unvoiced line into a conversation silently deletes the vanilla
line that follows it. The player hears my line, then the conversation just ends.

The generated line itself is perfect — right voice type (`maleyoungeager`,
resolved from the actor, not a generic), spatial audio, real lip sync, subtitles,
correct place in the conversation. It is purely that nothing waits for it.

This looks like a bug rather than a missing feature because **SkyrimNet already
solves it on the other path**: for a player-clicked topic,
`skynet_Library.psc::OnPlayDBVOTopic` blocks on `while !IsNPCDialogueReady()` and
then releases the dialogue menu's topic timer. A forced greeting has no menu and
no player click, so none of that runs. Same flag, same feature, one of the two
entry points synchronised and the other not.

## Steps to reproduce

1. `dbvo.enabled: true`, `dbvo.voiceSilentNPCs: true`, `DBVO.esp` not installed.
2. Add one **unvoiced** dialogue INFO (no `.fuz`) to a conversation the NPC
   starts by himself. I used the vanilla courier: an INFO added to
   `WICourierDeliveries` (`039F6C`), reached through the forced greeting
   `039F79`, conditioned to fire during a delivery and ordered (via
   `PreviousDialog`) to land before the closing line `039F81`.
3. Trigger the conversation — for the courier, queue any delivery and wait for
   him to walk up.
4. Watch/listen to the whole exchange, and check the log for
   `actor silenced (TTS playback in progress)`.

Happens on every delivery. Reproduced across separate runs with different line
text, so it is not a cached-audio fluke — only the size of the overlap changed.

## How often does this happen?

`Every time (100% reproducible)`

## Expected behavior

The conversation should stay in step with the generated speech, the way it
already does for player-clicked topics: the NPC speaks his generated line, it
finishes, and *then* the next line plays — with its own audio intact.

At minimum, SkyrimNet should never suppress a **voiced vanilla line** because its
own TTS is still playing. Losing base-game audio is worse than the two lines
overlapping.

## What area is affected?

`Voice / TTS (no audio, wrong voice, audio glitches)`

## TTS Provider

`Inworld`

(Latency is a cloud round-trip, ~3.5 s to generate plus ~1 s before playback
starts. A faster local engine would narrow the window but not close it — the
engine still isn't waiting on anything.)

## LLM Model

`google/gemma-4-26b-a4b-it` — not relevant to this issue; the line's text is
settled ~60 seconds before the NPC speaks it, and the failure is entirely in the
TTS/dialogue timing after that.

## STT Provider (if relevant)

`Not relevant to this issue`

## Game Version

`AE (1.6.1170+)`

## Game Version (if Other)

*(leave blank)*

## Modlist

custom (Journals of Jyggalag)

## SkyrimNet version

beta22 (`SkyrimNet 0-22-0-0`, `Git: dev @ 7e34c215709a`,
`GamePlugin: main @ be4f27521ad1`)

## Did this work before?

`Not sure / first time trying`

## What changed? (if it used to work)

*(leave blank)*

## Log file

Trimmed to the one conversation. The NPC is the courier; the line at 21:46:50 is
mine (unvoiced), the line at 21:46:58 is vanilla and voiced.

```
21:46:50.405  Added audio file with speech for <NPC>: ...\WICourier_WICourierDeliver_0003DF63_1.fuz
21:46:50.411  Skipping silent audio file for <NPC>: Sound\Voice\Fuz Ro Doh\Stock_8.fuz
21:46:50.484  On-demand: Generating TTS for silent response: '<my line>' (generation 1, scene: false)
21:46:50.491  (Dialogue) <NPC>: <my line> (Talking to Player)
21:46:53.993  GenerateTTS: Successfully generated 442808 bytes of audio          <- 3.5 s
21:46:53.995  Cached TTS for voiceID 'maleyoungeager': <my line> (442808 bytes)
21:46:54.933  PlayAudioFromMemory: Starting (speaker: <NPC>)                     <- 4.45 s after the line fired
21:46:58.738  DialogueItemCtorHook: adding 3 paths to timed suppression
21:46:58.740  Added audio file with speech for <NPC>: ...\WICourier_WICourierDeliver_00039F81_1.wav (text length: 0)
21:46:58.741  Skipping silent dialogue for <NPC> - actor silenced (TTS playback in progress)
21:46:58.741  (Dialogue) <NPC>: Looks like that's it. Got to go. (Talking to Player)
21:46:59.169  Menu 'Dialogue Menu' is closing
```

The dialogue slot lasted 8.26 s and needed 9.47 s (4.45 s latency + 5.02 s of
speech). The vanilla closing line fired 0.8 s before mine finished and was
dropped.

## Anything else?

**Second, related defect: the Papyrus speech-state API can't see this audio at
all**, so a mod can't even build its own workaround. Through a whole delivery —
including while the generated line was audibly playing — `GetSpeechQueueSize()`
returned `0` on every call, and `GetTimeSinceLastAudioEnded()` climbed straight
through the playback without ever resetting:

```
22:25:59.746  Queueing speech for speaker: <NPC> -> player: <my line> (... 0 total tasks in queue)
22:26:02.771  Cached TTS for voiceID 'maleyoungeager': <my line> (423404 bytes)
22:26:03.679  PlayAudioFromMemory: Starting (speaker: <NPC>)
22:26:04.088  GetSpeechQueueSize: Current speech queue size: 0      <- audio is playing
22:26:04.089  GetTimeSinceLastAudioEnded: Time since last audio ended: 266222ms
22:26:07.094  GetSpeechQueueSize: Current speech queue size: 0
22:26:07.094  GetTimeSinceLastAudioEnded: Time since last audio ended: 269228ms
```

`GetSpeechQueueSize()` never returned anything but `0` anywhere in a 4 MB log.
Silent-INFO TTS appears to run through `SilentDialogueManager` /
`TopicInfoEventHandler` rather than whatever queue those two functions report, so
from Papyrus there is no way to tell that an NPC is currently speaking a
generated line — which rules out a mod-side hold as well. Either function
reflecting silent-dialogue playback would be enough to work around the main
issue.

**Where the generation side works fine.** `TopicInfoEventHandler::ProcessEvent`
hooks `TESTopicInfoEvent`, so forced greetings *are* in scope and the line comes
out in the NPC's own voice with full lip sync. Only the pacing is unsynchronised.

**Three ways I can see to fix it, best first — any one would do.**

1. **Pre-generate for NPC-initiated conversations.** `PrepareNPCDialogue`
   already does exactly the right work — scans a topic's TopicInfos, evaluates
   conditions, finds silent responses, generates their TTS ahead of time
   (`PrepareNPCDialogue: Found silent response: '{}'`). Its trigger just looks
   scoped to the menu's selected topic. If a `DialogueContext` could also be
   built when an NPC-initiated conversation begins — `DialogueItemCtorHook`
   already receives `TESQuest*, TESTopic*, TESTopicInfo*, TESObjectREFR*`, or the
   first `TESTopicInfoEvent` would do — the same scan could run over the upcoming
   lines. That removes the delay rather than hiding it.

2. **Hold the line while its TTS is still generating.** SkyrimNet already
   intercepts voice resource loading (`BSResourceIDGenerateFromPathHook:
   Suppressing voice resource '{}' (within time window)`) and already registers
   paths for timed suppression from `DialogueItemCtorHook`. If a silent line's
   audio isn't ready, that hook could supply a silence buffer covering generation
   plus playback, so the engine's own duration clock waits it out. Mods can only
   do this crudely with a fixed-length silent `.fuz`; SkyrimNet is the only
   component that knows how long the generated audio will actually be.

3. **Smallest version — let modders warm the cache.**
   `SilentDialogueManager::EnsureTTSCached(...)` already exists and already
   caches by voice ID and text. Exposing it to Papyrus would be enough:

   ```papyrus
   ; Pre-generate and cache TTS for a line an NPC is about to speak.
   ; Returns 0 on success, 1 on error.
   int function EnsureTTSCached(String text, Actor speaker) Global Native
   ```

   A mod that knows its line in advance could warm the cache itself and the
   existing on-demand path would find it already cached. In my case that alone
   would fully solve it.

**Workarounds I tried.**

- `SkyrimNetApi.PrepareNPCDialogue("")` from Papyrus when the NPC starts talking
  — doesn't work. Logs `Base game dialogue detected, no VAD needed` and returns
  without scanning (no `SelectedTopic`, no `Found silent response`), presumably
  because there's no menu selection to build a `DialogueContext` from.
- Lowering Fuz Ro D-oh's `WordsPerSecondSilence` so its placeholder holds longer
  — works, but it's a global setting affecting every unvoiced line from every
  mod, and its placeholder is capped at 10 s (`Stock_10`), barely above latency
  plus speech.
- Shipping a silent `.fuz` at my own INFO's voice path, sized to cover
  generation plus speech. Works, but the duration has to be guessed against
  variable TTS latency, and it forces a word limit on generated lines so speech
  can't outrun the fixed slot. Too long and the NPC stands mute; too short and
  the next line is eaten.
- A looping spacer INFO gated on a global that Papyrus clears once
  `GetSpeechQueueSize()` drains and `GetTimeSinceLastAudioEnded()` goes quiet.
  Built it, and it **cannot work** — see the second defect above: neither
  function ever observes this audio, so the "speech finished" condition never
  arrives and the hold can only end on a timeout. It also needs several failsafes
  or a stalled script traps the player in a dialogue loop.
