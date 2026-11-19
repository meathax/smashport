# Smash TV for Windows

![Main menu with Custom Widescreen 16:9 selected](screenshots/menu.png)

Play the retail L8 twin-stick arcade game in its original 4:3 arena or in shared-screen 16:9. The Windows player compiles the arcade main and sound programs from your own ROM set. Two players can share a keyboard, use XInput controllers, or play over LAN.

## Play

1. On Windows x64, install the Visual Studio C++ x64 build tools, CMake, Ninja and Python 3.11 or newer.
2. Open **Play Smash TV.cmd** and choose your retail L8 ROM ZIP. If you use a split set, keep the parent and clone ZIPs together. First launch validates the chips and builds the game locally.
3. Select **Game** to choose Arcade Original or Widescreen, then **Start**. Press **5** to insert a coin and **1** to start. Player two joins with **6**, then **2**.

| Action | Player one | Player two |
| --- | --- | --- |
| Move | W A S D | I J K L |
| Fire | Arrow keys | Numpad 8 4 5 6 |

On a controller, move with the left stick and fire with the right. **Esc** opens the pause menu. **F11** toggles fullscreen.

## Enhanced effects

Blast direction and distance determine how far debris travels. Flesh and metal fragments skid across the floor and strike walls. Persistent blood, debris and smoke stay visible across floor panels and grates. Tune the effects in the enhancements menu.

![Cleared arena with the Map One overlay and an open exit](screenshots/arena.png)

## Build from PowerShell

```powershell
.\build.ps1 -Rom 'C:\path\to\smashtv.zip'
.\test.ps1
```

The local executable is written to `build/rom-player/SmashTV.exe`. This development build requires the tools listed above. A standalone installer must generate the game on the player's PC before a binary release can ship.

The Windows frontend incorporates work from [tmnt-port](https://github.com/cheviman/tmnt-port) and [arcade-lan](https://github.com/cheviman/arcade-lan). See the [third-party notices](THIRD_PARTY_NOTICES.md) for credits and licenses.
