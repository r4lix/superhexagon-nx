# superhexagon-nx

<img width="4060" height="1732" alt="1793289575237" src="https://github.com/user-attachments/assets/c8fa9f00-cab3-4c25-bc36-abbe63b5d419" />

Super Hexagon (Terry Cavanagh) running natively on the Nintendo Switch.

**No game content is included.** You need your own copy of the game (the Steam version).

## Disclaimer

This project was created **entirely with the help of AI coding tools**.

I am **not a developer**, so the code may contain bugs, security vulnerabilities, mistakes, or unfinished features. superhexagon-nx is a personal project that I decided to share with others who may find it useful.

Use it at your own risk. I cannot guarantee support, stability, compatibility, security, or regular updates.

## Install

1. Download `superhexagon-nx.zip` from the [latest release](https://github.com/r4lix/superhexagon-nx/releases/latest) and extract it to the root of your SD card. This creates `sdmc:/switch/superhexagon/`.
2. Extract the music, sounds, font and beat tables from your Steam copy (needs Python 3, `tools/bake.py` and `tools/pe.py` from this repo):
   ```
   python tools/bake.py "<Steam>/steamapps/common/Super Hexagon" data
   ```
3. Copy the resulting `data` folder to `sdmc:/switch/superhexagon/data`.
4. Launch **Super Hexagon** from the homebrew menu.

Saves, local high scores and settings are written next to the `.nro`.

## Controls

| Button | Action |
|---|---|
| A or + | confirm / start |
| B | back; quits from the main menu |
| + (in a run) | back to the menu |
| D-pad / stick / L / R / touch | move |
| Touch the "HIGH SCORES" bar | local high scores |

## Secret menu

Enter **Up Up Down Down Left Right Left Right B A** (or hold **L + R + ZL + ZR** for a second). Navigate with touch, D-pad or stick, close with **+**.

Cheats are **off by default**: tick "Enable cheats" in the Cheats tab (it resets every time the game starts). Every cheat can be bound to a button: god mode, no walls, autopilot, game speed, slow-mo, pause / frame step, rewind, save states, rotation tricks and more. Also settings, a frame-time graph and debug tools.

## Credits

Game by Terry Cavanagh, music by Chipzel.
