# Courier Descriptions

A SkyrimNet plugin
that gives the vanilla courier something to say about **who handed him your mail**.

When he delivers an item he has no "who sent it" voiceline for, he says one extra
line — generated at delivery time, spoken in his own voice, in the per-item slot
between *"Let's see here..."* and *"Looks like that's it. Got to go."*

> *"Redguard barmaid from the Mare gave me this, sounding desperate."*
>
> *"Jarl Balgruuf handed me this himself, looking like he had something heavy on his mind."*
>
> *"Someone with a very hard stare handed this to me, and wouldn't say another word."*

**He never reads your mail.** The letter's text is used only to work out what sort
of person sent it. The courier describes *them* — how they looked, how they paid,
how they seemed — and never says what the letter is about.

Deliveries the courier already has a voiced line for (Calcelmo's note, the Dark
Brotherhood note, an inheritance, and so on) are left completely alone.

---

## How it works

```
a quest queues a delivery   ->   WICourierItemCount rises
  arm (10s poll)   snapshot the courier's container; per item, read the letter,
                   work out who signed it, and pre-generate a one-line description
  armed (1.5s)     rescan for late arrivals; publish each finished line into a
                   dialogue INFO via the SKSE plugin
  delivery         SkyrimNet voices those INFOs in the courier's own voice, with
                   lip sync and subtitles, and the vanilla closing line still plays
```

Two parts ship together as one mod:

- **`CDN_CourierDescriptions.esp`** — the quest, the four dialogue lines the
  courier speaks into, and the silent placeholder audio that holds the slot open
  while speech is generated.
- **`CDNBookText.dll`** — an SKSE plugin that reads a book's body text (which
  Papyrus cannot), resolves a signature or title to a real NPC, and swaps the
  generated text into a dialogue line at runtime.

Because the line is real dialogue rather than narration, it gets the courier's
actual voice type, spatial audio, lip sync and subtitles — and does not suppress
the vanilla lines around it.

### Sender recognition

When the letter names its sender — in the title (`Letter from <Name>`) or in the
sign-off — the mod resolves that name to an actual NPC reference and feeds the
character's SkyrimNet bio into the generation, so the courier describes the real
person. How familiar he sounds scales with how publicly known they are: a Jarl he
recognises on sight and may name; an innkeeper he can place by trade; a private
citizen stays a stranger he can only describe.

Where no sender can be identified, he describes a stranger and deliberately
**invents no physical details** — he takes what he says from the handover itself
(how they paid, where he met them, that they would not give a name), which nothing
in the game can later contradict.

## Requirements

| | |
|---|---|
| Skyrim SE/AE with [SKSE](https://skse.silverlock.org/) | the platform |
| [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) | needed by `CDNBookText.dll` (VR: [58101](https://www.nexusmods.com/skyrimspecialedition/mods/58101)) |
| [Fuz Ro D-oh](https://www.nexusmods.com/skyrimspecialedition/mods/15109) | supplies timing for unvoiced dialogue |

All three are required, on top of SkyrimNet itself — **0.25 (beta 25) or
later**, since the content ships in the beta 25 plugin format. See
`INSTALL.txt` for what each one failing looks like.

> **SkyrimNet's "Enable TTS For Silent NPCs" must be turned on**
> (WebUI → Settings → Voice).
>
> The courier's line is delivered as an *unvoiced* dialogue line, and that
> setting is what makes SkyrimNet speak one. With it off, everything else works
> — the sender is resolved, the line is generated and written into the dialogue
> — and the courier stands there saying nothing.

## Installing

Grab the archive from [Releases](../../releases) and install it with a mod
manager. The plugin is ESL-flagged, so it costs no load order slot. Safe to add
to an existing save; removing it returns the courier to his vanilla lines.

The SkyrimNet content ships as an **external plugin layer**
(`SKSE/Plugins/SkyrimNet/external/oldcustard.courier-descriptions/`); SkyrimNet
registers it at start-up and lists it under **Plugins → Installed Plugins**
with an *External* badge — that list is the quick check that it loaded.

Configuration lives in the SkyrimNet WebUI under **Courier Descriptions** —
debug notifications, whether to use the LLM at all, and which LLM variant.

## Compatibility

**Courier replacers are supported.** The courier's line needs a placeholder
audio file under his voice type to hold the dialogue slot open, so a replacer
that gives him a different voice needs that voice listed here. Shipped:

| Mod | Voice type(s) |
|---|---|
| *(vanilla)* | `MaleYoungEager` |
| [Lilly — Female Courier Replacer](https://www.nexusmods.com/skyrimspecialedition/mods/4595) | `PrettyCourierVoice` |
| [The Courier Crew](https://www.nexusmods.com/skyrimspecialedition/mods/61400) | `FemaleCoward`, `FemaleCondescending`, `MaleEvenToned`, `MaleKhajiit` |

Using a replacer that isn't listed? The courier still speaks his line, but the
timing goes wrong and his closing *"Looks like that's it"* can get cut off. Open
an issue with the mod's name and it's a one-line fix.

Otherwise this mod adds four dialogue lines to `WICourierDeliveries` and
overrides one Papyrus fragment (`TIF__00039F81`), so it coexists with anything
that only *adds* courier letters — Hearthfires, Dragonborn, 3DNPC and the rest
are handled automatically, and letters the courier already has a recorded line
for are left alone.

## Known issue: the pause around the line

The courier's extra line sits in a dialogue slot of a **fixed 13 seconds**, held
open by a silent placeholder audio file. Expect a pause before the line and
usually some quiet after it.

This is a SkyrimNet limitation rather than a tuning mistake, and it has two
halves:

- **Before.** SkyrimNet generates the speech on demand, when the dialogue line
  fires — there is no way to pre-warm it. The engine does not wait, so the
  courier stands there until the audio exists. That is typically 4–5 seconds on
  a cloud TTS provider, and near-instant on a fast local voice.
- **After.** The placeholder *is* the slot length. When speech finishes early
  the remainder is silence, and shortening the line does not shorten the wait —
  it just moves the silence around.

13 seconds is chosen to fit generation plus speech with a few seconds spare. On
a slow provider that margin can be eaten, and the courier's own *"Looks like
that's it"* may start while his generated line is still speaking. A fast TTS
setup will not run into this.

A delivery carrying several described items spends this budget per item, so a
four-item delivery is a noticeably long conversation. Most deliveries carry one.

## Repository layout

| Path | What it is |
|---|---|
| `Plugin/` | the authored `.esp` and its `.seq` — binary, edited with xEdit/houseCARL |
| `Scripts/Source/` | Papyrus sources: the quest script and the courier closing-line override |
| `_imports/` | compile-time stubs (`SkyrimNetApi`, `CDN_BookText`, `WICourierScript`) |
| `External/oldcustard.courier-descriptions/` | the SkyrimNet beta 25 content plugin, shipped as an external layer: `manifest.json`, `prompts/courier_sender_line.prompt`, and `settings/CourierDescriptions.yaml` (the WebUI settings schema, read as `Plugin_CourierDescriptions`) |
| `BookTextPlugin/` | the SKSE plugin: C++ source, CMake, vcpkg manifest |
| `_build/` | the silent `.fuz` masters that hold the dialogue slot open |
| `tools/` | `make_silent_fuz.py` (builds those) and `package.ps1` (builds a release) |
| `docs/` | notes reported upstream to SkyrimNet |
| `INSTALL.txt` | ships inside the release archive |

Build output is **not** committed — no `.pex`, no `.dll`, no archives. The
release archive is the only place those are distributed.

## Building

### The SKSE plugin

CommonLibSSE-NG is a submodule, so clone recursively (or
`git submodule update --init --recursive` after the fact):

```bash
git clone --recursive <repo-url>
cd BookTextPlugin
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake \
                    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build --config Release
```

Output: `BookTextPlugin/build/CDNBookText/SKSE/Plugins/Release/CDNBookText.dll`.

**Bump `project(CDNBookText VERSION …)` in `BookTextPlugin/CMakeLists.txt` (and
`vcpkg.json`) for a release.** It becomes the plugin's SKSE version and the first
line of `CDNBookText.log`, which is how a bug report's log gets matched to a
build. CMake echoes `Parsing commonlibsse_plugin_version <x>` when you configure,
so it is easy to confirm.

The **static triplet is load-bearing** — the DLL has to ship self-contained, or
SKSE silently unloads it when it cannot resolve `fmt.dll`/`spdlog.dll`. Verify
with `dumpbin /imports`: only Windows DLLs and the VC++ runtime should remain.

### The Papyrus scripts

Compile `Scripts/Source/*.psc` with `_imports/` **first** on the import path,
then SkyrimNet's own header set. Order matters: the local stubs must win, and
SkyrimNet's headers supply the SKSE-extended `ObjectReference` members the quest
script uses.

### A release archive

```powershell
tools\package.ps1 -ScriptsDir <folder with the three .pex> `
                  -Dll BookTextPlugin\build\CDNBookText\SKSE\Plugins\Release\CDNBookText.dll `
                  -Version 1.1.0
```

It assembles the exact shipping layout from tracked sources plus those two build
inputs, copies the silent `.fuz` to one name per dialogue line per courier voice
type, and writes `Courier Descriptions-<version>.zip`. It refuses to run if
anything is missing, so a release cannot be built half-empty.

## License

MIT — see [LICENSE](LICENSE). CommonLibSSE-NG is licensed separately by its own
authors.
