# audio

Owns the Wwise sound engine's lifetime and its per-frame pump, and keeps the AK
SDK out of the rest of the engine. 

`mir::audio` · depends `mir::core`, `Wwise` (private) · API `modules/audio/include/audio/` · maintainer Dong Ta · reviewed 2026-09-15

## Model

Audiokinetic ships Wwise through its own launcher rather than vcpkg, so this is
the one dependency in the engine that is not a `find_package(... CONFIG)` away.
`cmake/FindWwise.cmake` resolves an SDK root from `WWISE_SDK_ROOT` or the
`WWISESDK` environment variable, picks the arch and toolset tree, and exposes
everything as one `Wwise::Wwise` imported target. Nothing outside that module
names a `.lib`.

The sound engine comes up in four stages that must happen in order — memory
manager, stream manager, low-level I/O, sound engine — and go down in exactly the
reverse. That ordering is the whole reason this module exists as a unit rather
than as calls scattered through `app`.

Banks are authored in `modules/audio/WwiseProject/` and generated to
`GeneratedSoundBanks/Windows/`. The build copies them next to the executable and
the runtime resolves them from `mir::ExecutableDir()`, so a moved build folder
still runs, matching how `assets`, `fonts` and `branding` already behave.

Today this is a harness, not an audio system. One hardcoded listener and one
hardcoded emitter exist so that playback can be proven end to end; there is no
positioning, no entity mapping and no component.

```cpp
audio::Init();                              // App::Initialize
audio::PostEvent("Play_chargeDown");        // after the window is shown

audio::Update();                            // once per frame, after mScheduler.Update

audio::Term();                              // App::Shutdown
```

## API

| Name | Header | Role |
|---|---|---|
| `Init` | `audio/AudioSystem.h` | Brings up all four Wwise stages and loads the banks; false on any failure |
| `Update` | `audio/AudioSystem.h` | One `RenderAudio()`; nothing is mixed without it |
| `Term` | `audio/AudioSystem.h` | Reverse-order teardown, safe whether or not `Init` succeeded |
| `PostEvent` | `audio/AudioSystem.h` | Fires an event by name on the fixed global game object |
| `WwiseSdkVersion` | `audio/Wwise.h` | The SDK version this build compiled against |
| `IsSoundEngineInitialized` | `audio/Wwise.h` | Safe before init, where it returns false |

`audio/Wwise.h` exists from the build-wiring work that preceded any runtime code.
It is the smallest thing that proves the headers compile and the import libraries
link, and it is worth keeping for exactly that.

## Rules

**No AK type appears in a public header.** The surface is `const char*` and
`bool`. Wwise's headers need `UNICODE` defined and two sample directories on the
include path, and a leaked `AkGameObjectID` would push both onto every consumer
of this module.

**A newly registered game object has no listener.** Register the listener, call
`SetDefaultListeners`, and only then register the emitter — the default set
applies to *subsequent* registrations, so an emitter registered first keeps an
empty listener set. Get this backwards and `PostEvent` still returns a valid
playing ID while nothing is audible, with no error logged anywhere.

**`Term()` runs on every `Init()` failure path, not just at shutdown.** Each
stage guards on its own module's state rather than on a single "did init finish"
flag, so a partially-initialised engine tears down correctly. Debug Wwise asserts
at `MemoryMgr::Term` if anything is still allocated, which is what makes a clean
Debug exit evidence rather than luck.

**`Init.bnk` loads before any other bank.** It carries the bus layout every other
bank resolves against, and loading a content bank first fails.

**The bank path comes from `mir::ExecutableDir()`, never the working directory.**
`ctest` runs from the build directory while a VS Code F5 launch typically runs
from the repository root, so a CWD-relative path passes the smoke tests and
silently finds nothing under the debugger.

**`RenderAudio()` runs once per frame, after `mScheduler.Update`.** Calls queued
before it are processed that frame; anything posted after waits for the next one.
It sits outside `App::Run`'s window-size guard so audio keeps running while the
window is minimised.

**`AK_OPTIMIZED` is set by the imported target, never by hand.** It has to match
how the Wwise libraries themselves were built, and the same switch drops
`CommunicationCentral` and `ws2_32` from the Release link line. Defining it in
one place keeps the two from drifting.

**The low-level I/O is Wwise sample source compiled into this target**, not a
shipped library — `AkDefaultIOHookDeferred.cpp` plus two files from `Common/`.
2025.1 ships only the deferred hook; the blocking one that most tutorials use was
removed. Those sources call the wide Win32 APIs, which is why this target defines
`UNICODE`/`_UNICODE` while the rest of the engine does not.

**`PostEvent` returns a playing ID, not an `AKRESULT`.** It is checked against
`AK_INVALID_PLAYING_ID`; running it through the usual `Check()` helper would
compare against the wrong type of failure and report success for every bad event
name.

**There is no `AkMusicEngine` in 2025.1.** It was folded into `AkSoundEngine`, so
there is no separate init call to add, and older integration guides that list one
are describing a version this project does not use.

## State

*As of 2026-09-15.* Working: the full init and teardown sequence in Debug and
Release, both banks loading, and one event producing audible output, confirmed on
hardware. The Wwise Profiler connects to Debug and Profile builds.

Everything about *what* plays is a placeholder — listener id 2, emitter id 1, one
event name passed as a literal from `App::Initialize`. No positioning, no
distance attenuation, no entity mapping.

There is no `modules/audio/tests/`. Coverage comes from `smoke.HelloWorld`, which
runs the engine for 120 frames and fails on any logged Error or Critical; since
every failure path in `Init` logs, a passing smoke test means the engine came up,
both banks loaded and the event was accepted. It does not and cannot assert that
sound reached the speakers.

The test bank's media is PCM, so no codec library is linked. `Profile` maps to
CMake's `RelWithDebInfo` and there is no preset for it, so that branch of the
config mapping has never been exercised.

## Backlog

1. **An object and listener API**, then `AudioListener` and `AudioEmitter`
   components with a system in `mScheduler` that syncs transforms and posts
   events. Doing the wrapper first is deliberate: it is a mechanical refactor of
   code already proven, while the component design still has open questions —
   whether emitters post on demand or on a trigger, how 2D UI sounds bypass
   positioning, and what happens to a playing voice when its entity is destroyed.
2. **Per-object unregistration.** `Term` calls `UnregisterAllGameObj`, which is
   enough while the object set is fixed. Once entities come and go during play,
   destruction needs to unregister, or the session leaks game objects.
3. **Bank lifecycle.** Everything loads at init and unloads at term. A real
   project wants banks per level or per scene.
4. **Exercise the Profile configuration**, either through a preset or a one-off
   `RelWithDebInfo` configure. It is the only config path in `FindWwise.cmake`
   that has never been built.
5. **A codec library** once any sound leaves PCM conversion. Wwise defaults to
   Vorbis, and a Vorbis bank plays silently with no error when
   `AkVorbisDecoder` is not linked.
6. **Trim the authoring project.** `WwiseProject/Originals/` carries about 6.7 MB
   of iZotope factory impulse responses that the project template copied in and
   nothing references, and `.cache/` regenerates from `Originals/` so it does not
   belong in version control.
