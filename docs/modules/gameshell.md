# gameshell

The screens every game gets from the engine: a HUD while it plays and a pause
menu over it, driven by the gamepad, the mouse or the keyboard. It is built on
`gameui`'s public API alone, as a game's own screens would be, and decides
nothing about the game: it reports what the player asked, and `App` carries it
out.

`mir::gameshell` · depends `mir::gameui` (public) · API `modules/gameshell/include/gameshell/` · maintainer Quy Dong Ta · reviewed 2026-10-02

## Model

`GameShell` is drawn every frame inside the game UI's frame, and returns a
`GameShellAction`: `Pause`, `Resume`, `Restart`, `Quit` or nothing. `App`
turns those into `TogglePause`, `StopScene` and `PlayScene`, after the UI's
frame has ended, the way it handles the editor's toolbar.

```cpp
// App::Run
shellAction = mGameShell.Draw(mGameUI, IsPlaying(mWorld), dt);
mGameUI.EndFrame();
ApplyShellAction(shellAction);
```

Outside Play it draws nothing and an open menu closes. In Play it draws the
HUD, then the menu over it.

**The HUD** sits in the top-right corner of the safe area: the play time and a
Pause button carrying the `UIPause` prompt for the active device, `Start` on a
pad and `Escape` on the keyboard. The button is `mPointerOnly`. While the game
runs, the stick and the confirm button are the game's, so no `UINavigate` press
can land on it; the mouse alone still pauses by clicking it. The clock stops
while the menu is open and starts from zero on Restart and on each Play.

**The pause menu** opens on `UIPause` (Escape, Start) or the HUD's button, and
closes on `UIPause`, `UICancel` (Escape, B, the right mouse button) or Resume.
It dims the whole area, centres a panel in the safe area, and puts focus on
Resume. Resume, Restart and Quit are a column linked up and down with wrap,
with Select and Back prompts under them. Quit leaves Play in the editor and
closes the window in a game build.

While it is open it calls `UIContext::HoldInput()`, so gameplay reads every
action at rest through `InputState::ForGame`. The menu also asks `App` to
pause the simulation, but that is not what keeps the game's input from it:
`gameui`'s own state does, and the same call keeps a menu that pauses nothing,
an inventory say, from leaking input to the game.

Each device does the whole round trip alone:

| | Open | Navigate | Close |
|---|---|---|---|
| Gamepad | Start | D-pad, left stick | B, Start, or A on Resume |
| Keyboard | Escape | Arrow keys | Escape, or Enter on Resume |
| Mouse | Click the HUD's Pause | Hover | Click Resume, or the right button |

## API

| Type | Header | Role |
|---|---|---|
| `GameShell`, `GameShellAction` | `gameshell/GameShell.h` | The HUD and the menu together, the play clock, and what the player asked this frame |
| `PauseMenu`, `PauseMenuAction` | `gameshell/PauseMenu.h` | Opens, closes, holds the input; `Open` for a button elsewhere |
| `GameHud` | `gameshell/GameHud.h` | The clock and the pointer-only Pause button; true on the frame it is clicked |

`src/gameshell/ShellPrompts.h` draws a prompt, a key cap or pad pill with the
binding's label and then what it does, for both screens.

## Rules

**Use `gameui`'s public API and nothing else.** No ImGui, no `UIState`
internals and no device: the screens read `UIPause`, `UICancel` and the rest by
name through `UIContext`, and the device appears only in tests, which feed raw
input through `InputSystem`. This module is the proof that `gameui` serves
someone building a game, so reaching past its API would prove nothing.

**Report, do not act.** `EngineModeState` is `App`'s to change, through
`PlayScene`, `StopScene` and `TogglePause`. The shell returns what the player
asked and leaves the game alone, which is also what lets it be tested without
a world.

**Close on the frame after the action.** The frame the menu closes still holds
the input, so the press that closed it never reaches the game as well.

## State

*As of 2026-10-02.* `App` draws it every frame unless `--ui-demo` is given. It
works in the editor's scene viewport, in Play, and in a game build, which with
`--no-editor`, or in a build without the editor at all, starts in Play.

`tests/GameShell.tests.cpp` feeds raw keys, pad buttons and the mouse through
`InputSystem` and the default bindings into a headless ImGui context, as `App`
does, and covers the menu opened, navigated and closed by each device alone
(the mouse in an offset area, as in the editor's viewport), game input held
while it is open without the capture flags, the stick and confirm not reaching
the HUD in play, nothing drawn outside Play and Stop closing the menu, both
screens inside the viewport and fullscreen at any safe area, and the clock.
`smoke.game-mode` runs a game build for 120 frames on the GPU. Whether it looks
right is checked by running a game: Play in the editor, or `--no-editor`.

## Backlog

1. **Content from the game.** The shell is the engine's. A game will want its
   own HUD and its own options in the menu, through Lua or data, which is
   `gameui`'s content backlog item.
2. **Settings.** No options screen: volume, bindings and the safe-area inset
   would live behind a fourth button.
3. **Confirm before Quit.** Quit and Restart act at once; a game with progress
   to lose wants a yes or no first, which wants a modal over the menu and
   `gameui`'s focus scopes.
4. **The pause press itself.** The game UI runs before `InputSystem` in a frame,
   so the game reads the Escape or Start that opens the menu on the frame
   before it opens. A game that binds those keys for itself sees them once.

## Changed

*2026-10-02* — **Created.** The pause menu and the HUD, the first screens
built on `gameui` from outside it.
