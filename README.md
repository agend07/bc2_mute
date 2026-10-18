# BC2 Client Chat Filter (`msimg32.dll`)

Client-side chat filter and player mute list for **Battlefield: Bad Company 2** (compatible with Project Rome / retail `BFBC2Game.exe`).

It works entirely on the client side using non-destructive instance vtable hooks in memory:
- **Does not modify `BFBC2Game.exe` or Project Rome's `dinput8.dll`** on disk.
- **Does not patch `.text` code instructions** in memory.
- Supports **in-game chat slash commands** (`/mute`, `/unmute`, `/mutelist`) and hot-reloading of `muted_players.txt`.

---

## Installation

1. Download `bc2-chat-filter-windows.zip` from Releases (or build it with `./build.ps1`).
2. Extract **`msimg32.dll`** (and optionally `muted_players.txt`) directly into your Battlefield Bad Company 2 game folder — next to `BFBC2Game.exe` and `dinput8.dll` (e.g. `C:\Program Files\EA Games\Battlefield Bad Company 2\`).

> **How it loads:** `BFBC2Game.exe` imports `msimg32.dll` at startup. This proxy DLL forwards all standard Windows `msimg32.dll` calls (`AlphaBlend`, etc.) to `C:\Windows\System32\msimg32.dll` while filtering chat events in the background.

### Uninstallation
Delete `msimg32.dll`, `muted_players.txt`, and `chat_filter.log` from your game folder.

---

## Usage

### 1. In-Game Chat Commands
While playing on a server, open the chat box (`J` = All, `K` = Team, `L` = Squad) and type:

- **`/mute <player_or_keyword>`** — mutes any player whose name (or message) contains `<player_or_keyword>` (case-insensitive) and saves it to `muted_players.txt`.
- **`/unmute <player_or_keyword>`** — removes matching entry from the mute list.
- **`/mutelist`** — displays your current mute list locally in the chat window.

*Note: Slash commands handled by the filter are intercepted locally and are **never** sent to the game server.*

### 2. Editing `muted_players.txt`
You can also edit `muted_players.txt` using any text editor:
- One player name (or substring/keyword) per line.
- Matching is **case-insensitive** and matches substrings.
- Lines starting with `#` or `;` are ignored.
- Changes saved to `muted_players.txt` are automatically reloaded within 2 seconds without restarting the game.
- **Note on `C:\Program Files\` installations:** If BC2 is installed under `C:\Program Files\...` and run without Administrator privileges, Windows UAC File Virtualization stores `muted_players.txt` and `chat_filter.log` under `%LOCALAPPDATA%\VirtualStore\Program Files\EA Games\Battlefield Bad Company 2\`.

---

## Building from Source

Requires Visual Studio 2019 or 2022 with C++ x86 build tools (`cl.exe`):

```powershell
./build.ps1
```

Outputs are placed in `dist/`:
- `dist/msimg32.dll`
- `dist/bc2-chat-filter-windows.zip`
- `dist/SHA256.txt`
