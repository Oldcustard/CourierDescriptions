# CDNBookText — SKSE plugin

A tiny SKSE plugin that exposes one native Papyrus function the game doesn't:
**`CDN_BookText.GetBookText(Book) -> String`** — returns a book/letter's body text
(the `DESC` field) at runtime. Used by the *Courier Descriptions* SkyrimNet plugin
so the LLM can read what a delivered letter actually says (then have the courier
describe the *sender*, never the contents).

Builds **one DLL for Skyrim SE / AE / VR** (Address Library, struct-independent)
on CommonLibSSE-NG (alandtse/CommonLibVR branch `ng`).

## Build

Requirements: VS 2022+ Build Tools (C++23 MSVC), CMake ≥ 3.21, vcpkg, Git.

From a **VS Developer Command Prompt** (x64):

```bat
git clone <this-repo> CDNBookText
cd CDNBookText
git submodule add -b ng https://github.com/alandtse/CommonLibVR.git lib/commonlibsse-ng
git submodule update --init --recursive

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build --config Release
```

The DLL (+ pdb) lands in `build/CDNBookText/SKSE/Plugins/CDNBookText.dll`.

**The static triplet is load-bearing.** SKSE plugins must ship as one
self-contained DLL: link CommonLib's deps (`spdlog`, `fmt`, …) statically so the
DLL has no runtime dependency on `fmt.dll`/`spdlog.dll` — otherwise the game can't
resolve those imports and SKSE silently unloads the plugin at launch. The
companion `vcpkg-configuration.json` pins `x64-windows-static-md` (static libs,
dynamic CRT) as the default triplet so a plain `cmake -B build -S .` with the
vcpkg toolchain also produces a self-contained DLL; the `-DVCPKG_TARGET_TRIPLET`
flag above is belt-and-suspenders.

First build compiles CommonLibSSE from source (~10–15 min, once) and builds the
vcpkg deps statically (~5–10 min, once). The vcpkg manifest (`vcpkg.json`)
supplies CommonLib's header deps (spdlog, fmt, directxtk, directxmath, rapidcsv,
xbyak).

Verify the result is self-contained — the only load-time imports should be
Windows system DLLs + the VC++ runtime (no `fmt.dll`/`spdlog.dll`):

```bat
dumpbin /imports build\CDNBookText\SKSE\Plugins\Release\CDNBookText.dll
```

## Distribute

Ship a Data-shaped mod folder:

```
CDNBookText/
├── SKSE/Plugins/CDNBookText.dll
└── Scripts/CDN_BookText.pex          # compiled from Scripts/Source/CDN_BookText.psc
```

**Do not** also ship `fmt.dll`/`spdlog.dll` — with the static triplet they are
linked into `CDNBookText.dll`, so the single DLL is fully self-contained (its
only load-time imports are Windows system DLLs + the VC++ runtime, which the game
already loads). Shipping the dynamic copies would be a leftover from a non-static
build.

Users install it like any mod. The Papyrus `CDN_BookText.psc` ships alongside so
dependent plugins compile against it.

## Runtime dependency (user's game)

Users need the **Address Library** database for their EXE version
(Nexus 32444 for SE/AE; 58101 for VR). If missing, SKSE logs
"Failed to locate an appropriate address library…" and disables the plugin.

## How it works

`TESObjectBOOK` inherits `TESDescription`; `TESDescription::GetDescription(BSString&,
TESForm*, fieldType='CSED')` fills the buffer with the `DESC` text (the book body).
The native just calls that and returns it as a `BSFixedString` (Papyrus `String`).
