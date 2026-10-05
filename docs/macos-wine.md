# SimCity 3000 Unlimited on macOS (and Linux) with Wine

This is the setup GTA Mode was developed on: an Apple Silicon Mac with Homebrew's Wine 11 running the Windows
game through Rosetta, and cnc-ddraw replacing DirectDraw. Linux works the same way with your distribution's Wine.

Pick a folder for everything, for example `~/Games/SimCity3000`, and call it `$SC3` below:

```bash
export SC3=~/Games/SimCity3000
```

## 1. Wine

```bash
brew install --cask wine-stable      # macOS; on Linux install wine from your package manager
```

## 2. The game files

The Steam release (app 2741560) is Windows-only, but the Mac Steam client can still download it:

1. In Steam, logged into the account that owns the game, run `open steam://open/console`.
2. In the console, type `download_depot 2741560 2741561`.
3. The files (about 392 MB) land in
   `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_2741560/depot_2741561`.

With a CD or another copy, use its `SimCity 3000 Unlimited` folder instead. GTA Mode supports the game DLLs of the
Steam release; on other builds it switches itself off and says so in `gta_mode.log`.

## 3. A Wine prefix with the game in it

```bash
export WINEPREFIX="$SC3/prefix"
wineboot -i
ditto "<the depot folder from step 2>" "$WINEPREFIX/drive_c/Games/SimCity 3000 Unlimited"
```

Save this as `sc3u.reg` and import it with `wine regedit sc3u.reg`. The keys mirror what the Steam installer
writes:

```
REGEDIT4

[HKEY_LOCAL_MACHINE\Software\Wow6432Node\Electronic Arts\Maxis\SimCity 3000 Unlimited]
"language"=dword:00000409
"InstalledPath"="C:\\Games\\SimCity 3000 Unlimited\\Apps"
"ScenarioCreator"="C:\\Games\\SimCity 3000 Unlimited\\Apps\\ScenarioCreator\\SimCity Scenario Creator 97.mde"

[HKEY_LOCAL_MACHINE\Software\Electronic Arts\Maxis\SimCity 3000 Unlimited]
"language"=dword:00000409
"InstalledPath"="C:\\Games\\SimCity 3000 Unlimited\\Apps"
"ScenarioCreator"="C:\\Games\\SimCity 3000 Unlimited\\Apps\\ScenarioCreator\\SimCity Scenario Creator 97.mde"

[HKEY_CURRENT_USER\Software\Wine\DllOverrides]
"ddraw"="native,builtin"
```

## 4. cnc-ddraw

Wine's own DirectDraw flickers badly with this game. Download `cnc-ddraw.zip` from
[cnc-ddraw releases](https://github.com/FunkyFr3sh/cnc-ddraw/releases), copy `ddraw.dll` and `ddraw.ini` into the
game's `Apps` folder, and set these in the `[ddraw]` section of `ddraw.ini`:

```ini
windowed=true
maintas=true
vsync=true
renderer=opengl
width=1200
height=900
```

`renderer=opengl` is required for GTA Mode's HUD. Keep the window small enough to fit your screen: if macOS
shrinks it, the mouse pointer stops lining up with the game. Edit `ddraw.ini` with the game closed, because
cnc-ddraw rewrites the size on exit.

## 5. GTA Mode

Copy `gta_mode.dll` into the same `Apps` folder.

## 6. Play

```bash
export WINEPREFIX="$SC3/prefix" WINEDEBUG=-all
cd "$WINEPREFIX/drive_c/Games/SimCity 3000 Unlimited/Apps"
wine SC3U.exe -intro:off
```

cnc-ddraw locks the mouse inside the window when you click in it. Ctrl+Tab releases it (so does Cmd+Tab to another
app).

## Problems we hit

| Symptom | Fix |
|---|---|
| Heavy flicker | Use cnc-ddraw (step 4) |
| Black window | `wine reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d gdi` |
| Starting through `explorer /desktop=...` exits at once | Start `SC3U.exe` directly, without a Wine virtual desktop |
| `regedit` hangs after moving the prefix | Kill the stale Wine processes (`wineserver -k`), then retry |
| Mouse pointer out of sync with the game | The window didn't fit the screen: lower `width`/`height` in `ddraw.ini` |
