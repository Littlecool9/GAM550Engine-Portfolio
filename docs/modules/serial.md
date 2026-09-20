# serial

JSON documents on disk. It owns parsing, serialising, error reporting and the
read-modify-write cycle that lets two owners share one file. It knows nothing
about what any particular document means.

`mir::serial` · depends `mir::core`, `nlohmann_json`, both public · API `modules/serial/include/serial/` · maintainer Sumin Park · reviewed 2026-09-18

## Model

This module exists because of where `nlohmann_json` is allowed to live.

`JsonFile.h` returns and accepts `nlohmann::json`, so anything including it needs
that library's include path, which makes the dependency public. Putting these
functions in `core` would therefore have given the engine's most-depended module
a third public third-party dependency. `core` currently carries `glm` publicly and
`spdlog` privately, and keeping that list short is what a module in between buys.

The layering is one step per concern. `core/fs/FileIO.h` moves bytes and decides
nothing about them. `serial/JsonFile.h` decides those bytes are JSON. A module
above decides what the JSON means — `RenderSettings` reads a `"rendering"` key,
`InputMap` reads an `"input"` key, `SceneIO` reads a scene manifest.

```cpp
// read
const std::optional<nlohmann::json> root = ReadJsonFile(path);
if (!root || !root->is_object())
    return std::nullopt;

// write one section, leaving every other section on disk untouched
return UpdateJsonFile(path, [this](nlohmann::json &root)
                      { root["rendering"] = nlohmann::json{{"rayTracing", rayTracing}}; });
```

### Sharing one file

`project_settings.json` has two owners. `input` owns its `"input"` key and
`renderer` owns its `"rendering"` key, and neither knows the other exists.

`UpdateJsonFile` is what makes that work. It reads the current document, hands
the whole object to a callback, and writes the result back, so a caller assigns
its own key and every sibling key survives untouched. Without it each owner
would build a fresh document and drop the other's section on every save.

## API

| Function | Header | Role |
|---|---|---|
| `ParseJson` | `serial/JsonFile.h` | Text already in memory to `json`; `context` names the source in the log |
| `ReadJsonFile` | `serial/JsonFile.h` | A path to `json` |
| `WriteJsonFile` | `serial/JsonFile.h` | `json` to a path, atomically |
| `UpdateJsonFile` | `serial/JsonFile.h` | Read, edit through a callback, write back |

`ParseJson` exists separately from `ReadJsonFile` because some JSON never comes
from a path. `SceneResolve`'s mesh assets arrive as bytes out of an
`AssetBlobView`, so there is no filename to report; the caller passes the asset
id as `context` instead. `ReadJsonFile` is `ReadText` followed by `ParseJson`,
which is why the `try`/`catch` around `json::parse` appears once in the engine.

`WriteJsonFile` takes the indent that nlohmann's `dump()` takes, defaulting to 2.
Passing `-1` writes the document with no whitespace.

## Rules

**A missing file is quiet; a malformed file is logged.** `ReadJsonFile` tests for
existence before reading, and returns `nullopt` without logging when the file is
not there. A config that was never authored is a normal state — no game ships a
`project_settings.json` today — and logging it made every clean run look broken.
The smoke tests fail on any logged error, so this distinction is load-bearing
rather than cosmetic.

The cost is that each caller decides whether absence is an error for its own
file. `ReadDefaultGame` logs its own message because `engine.json` is required,
and `LoadGameProject` tests for `game.json` separately because a missing one
means the directory is not a game at all.

**`UpdateJsonFile` refuses to overwrite content it could not parse.** A file that
exists, holds something, and does not read as a JSON object makes the function
log and return `false`. It holds sections this caller does not own, and rewriting
it would delete them. Before this rule, one typo in a hand-edited
`project_settings.json` meant the editor's next Save silently dropped whichever
section the saver was not writing.

A blank file is the exception. Zero bytes and whitespace-only both start a fresh
object, because there is nothing there to protect, and refusing them left Project
Settings permanently unable to save.

**Writes are atomic.** `WriteJsonFile` goes through `WriteTextAtomic`, so a
reader sees either the whole previous document or the whole new one. An
interrupted save cannot leave a half-written config that fails to parse on the
next launch.

Atomic here means no torn document is ever visible. It does not mean the write
survives a power cut, because the flush reaches the operating system rather than
the disk.

**`json::value()` is not safe on an unchecked node.** It throws `type_error 306`
when the receiver is not an object, and engine code does not catch exceptions, so
that reaches `std::terminate` through `main`. Every caller checks `is_object()`
on a node before reading keys out of it. `InputMap::LoadFile` checks at three
levels — the document root, the `"input"` value, and each element of `"actions"`
and `"bindings"` — because a hand-edited file can put a scalar at any of them.

## State

*As of 2026-09-18.* Reading, writing, in-memory parsing and section-preserving
updates all work.

Callers today: `RenderSettings` and `InputMap` for `project_settings.json`,
`SceneIO` for a scene manifest and its per-entity files, `GameProject` for
`engine.json` and `game.json`, and `SceneResolve` for mesh assets through
`ParseJson`.

Tests in `modules/serial/tests/`: `JsonFile`, covering the round trip, the three
read outcomes, section preservation, the refusal on malformed content, and the
blank-file exception.

Missing: any schema or version handling. Every caller reads keys defensively with
`value()` defaults and a version field would have to be added per document type.

## Backlog

1. **A document-set write.** `SaveScene` writes a manifest plus one file per
   entity, and each file is atomic on its own while the set is not. A crash
   halfway leaves a manifest listing entities whose files were never written.
   The fix is to write the whole directory to a sibling and swap it.
2. **Durable writes.** `WriteTextAtomic` flushes to the operating system, not to
   the platter. A `FlushFileBuffers`/`fsync` shim in `core/platform/` would close
   that, and belongs there rather than here.
3. Undecided: whether a schema version belongs in each document or in a wrapper
   this module owns. Nothing has needed a migration yet.

## Changed

*2026-09-18* — **Module created.** Was: every caller hand-rolled parse, dump and
the read-merge-write cycle. `RenderSettings.cpp` and `InputMap.cpp` held the same
block copied twice, and `GameProject.cpp` and `SceneIO.cpp` went straight to
`std::ifstream` with `operator>>`. Now: one implementation here.
