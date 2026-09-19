# audio

Owns the Wwise sound engine's lifetime and its per-frame pump, and keeps the AK
SDK out of the rest of the engine. 

`mir::audio` · depends `mir::core`, `Wwise` (private) · API `modules/audio/include/audio/` · maintainer Dong Ta · reviewed 2026-09-18

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

Banks arrive as build artifacts, and they belong to a game rather than to the
engine. The Wwise authoring project does not live in this repository; generated
`.bnk` files are committed to `games/<name>/audio/SoundBanks/`, ride the game
directory copy next to the executable, and the runtime resolves them from
`GameProject::mSoundBankDir`, so a moved build folder still runs, matching how
`assets`, `fonts` and `branding` already behave.

`game.json`'s `soundBanksRoot` names that directory and defaults to
`audio/SoundBanks`. It sits outside `assetsRoot`, so `AssetCooker` never walks
it — and the cooker skips `.bnk` and `.wem` outright, so a bank dropped under a
cook root by mistake is passed over rather than buried in a blob it cannot be
loaded from.

Today this is a harness, not an audio system. One hardcoded listener and one
hardcoded emitter exist so that playback can be proven end to end; there is no
positioning, no entity mapping and no component.

```cpp
audio::Init(mProject.mSoundBankDir);        // App::Initialize
audio::PostEvent("Play_chargeDown");        // after the window is shown

audio::Update();                            // once per frame, after mScheduler.Update

audio::Terminate();                         // App::Shutdown
```

## API

| Name | Header | Role |
|---|---|---|
| `Init` | `audio/AudioSystem.h` | Brings up all four Wwise stages and loads a game's banks; false on any failure, and on a game that has none |
| `Update` | `audio/AudioSystem.h` | One `RenderAudio()`; nothing is mixed without it |
| `Terminate` | `audio/AudioSystem.h` | Reverse-order teardown, safe whether or not `Init` succeeded |
| `PostEvent` | `audio/AudioSystem.h` | Fires an event by name on the fixed global game object |
| `SetRTPCValue` | `audio/AudioSystem.h` | Writes a game parameter on one object, or globally with a `gameObject` of 0 |
| `SetSwitch` | `audio/AudioSystem.h` | Writes a switch on one object; Wwise has no global switch to fall back on |
| `SetState` | `audio/AudioSystem.h` | Writes a state group, which Wwise scopes globally - no object takes part |
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

**`Terminate()` runs on every `Init()` failure path, not just at shutdown.** Each
stage guards on its own module's state rather than on a single "did init finish"
flag, so a partially-initialised engine tears down correctly. Debug Wwise asserts
at `MemoryMgr::Term` if anything is still allocated, which is what makes a clean
Debug exit evidence rather than luck.

**`Init.bnk` loads before any other bank.** It carries the bus layout every other
bank resolves against, and loading a content bank first fails.

**The bank path is resolved from the executable, never the working directory.**
`GameProject::mSoundBankDir` is built from `mir::ExecutableDir()`, and `Init`
takes it as given rather than deriving a path of its own. `ctest` runs from the
build directory while a VS Code F5 launch typically runs from the repository
root, so a CWD-relative path passes the smoke tests and silently finds nothing
under the debugger.

**A game with no SoundBanks is not an error.** `Init` checks the directory
before any Wwise stage comes up, logs `Info` and returns false. The smoke tests
fail on any logged Error, so treating a deliberately silent game as a failure
would turn every game without audio red.

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

**A game parameter, switch or state is addressed by name, never by ID.**
`AK::SoundEngine` hashes the string at the call, so the banks need no string
table and nothing here has to carry generated IDs. The cost is that a misspelled
name is not an error anywhere: Wwise takes the write and it reaches nothing, so
`SetRTPCValue` returning true means "the call was made", not "something heard
it".

**Only the RTPC has a global fallback.** A game parameter with no per-object
value falls back to the global one, which is what `gameObject = 0` writes, and
reaching one object alone additionally needs the parameter scoped to Game Object
in the authoring project. A switch is per object with no fallback, and a state
is global with no per-object form - hence the three different signatures.

**There is no `AkMusicEngine` in 2025.1.** It was folded into `AkSoundEngine`, so
there is no separate init call to add, and older integration guides that list one
are describing a version this project does not use.

## State

*As of 2026-09-18.* Working: the full init and teardown sequence in Debug and
Release, and both of `HelloWorld`'s banks loading, confirmed on hardware. The
Wwise Profiler connects to Debug and Profile builds.

Banks are per game. `HelloWorld` carries `Init.bnk` and `Test.bnk`; `EmptyScene`
has no `audio/` directory and runs silent through the rule above.

Everything about *what* plays is a placeholder — listener id 2 and emitter id 1
are registered at init and nothing posts to them. `PostEvent` has no caller
outside this module today; audible output was proven with a temporary call and
that call was not kept. No positioning, no distance attenuation, no entity
mapping.

There is no `modules/audio/tests/`. Coverage comes from `smoke.HelloWorld`, which
runs the engine for 120 frames and fails on any logged Error or Critical; since
every failure path in `Init` logs, a passing smoke test means the engine came up
and both banks loaded. It does not and cannot assert that sound reached the
speakers.

`SetRTPCValue`, `SetSwitch` and `SetState` are wired through to Lua (see
`docs/modules/script.md`) but nothing audible has been proven through them yet.
`HelloWorld`'s `Init.bnk` declares ten game parameters, and because banks carry
IDs rather than names, none of their names can be recovered from this repository
- whoever owns the authoring project has to supply one, or author an RTPC mapped
to volume or pitch, before `games/HelloWorld/assets/scripts/audio_rtpc.lua` can
be heard doing anything.

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
2. **Per-object unregistration.** `Terminate` calls `UnregisterAllGameObj`, which is
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
6. **Settle where the authoring project lives.** It was removed from this
   repository, so `games/HelloWorld/audio/SoundBanks/` is currently a pair of
   binaries nobody can regenerate without rebuilding the Wwise project from
   scratch. Its own repository, shared storage, or back here once the content
   settles are all defensible; what would decide it is whether anyone outside
   the engine programmers edits audio. Per-game banks raise the stakes a little:
   each game that gains audio needs its own generation target, and its own
   `Init.bnk`.
