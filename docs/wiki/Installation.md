# Installation

## Requirements

- FINAL FANTASY VII REBIRTH, Steam version, installed on Windows 10/11.
- A graphics card without mesh shaders; see [Compatibility](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Compatibility).
- About 150 MB free in the game folder for the tools and converted shaders.

Players do not need to install Python, Visual Studio or shader tools separately.

## Install the release

1. Close the game. Download `LegacyGPUCompat-<version>.zip` from
   [Releases](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/releases).
   Choose the release asset, not GitHub's source-code ZIP.
2. In Steam, right-click the game > **Manage > Browse local files**. This opens the folder
   containing `End`.
3. Before extracting, remove any other mod's `d3d12.dll` from `End\Binaries\Win64`, such as a
   feature-level spoof. Legacy GPU Compatibility already handles that check.
4. Extract the entire ZIP into the folder containing `End`, merging the folders. Do not extract
   into `Win64` or leave the contents inside an extra ZIP-named folder.
5. Check this layout, then launch the game normally:

**In Steam:** right-click FINAL FANTASY VII REBIRTH, then select **Manage → Browse local files**.

![Steam's game context menu with Manage expanded and Browse local files visible.](assets/steam-browse-local-files.png)

**Extract into this folder:** it contains `End`, `Engine` and `ff7rebirth.exe`.
Merge the ZIP's `End` folder with the existing one. Your drive and installation path may differ.

![The FF7 Rebirth root folder in File Explorer, showing End, Engine and ff7rebirth.exe.](assets/game-root-folder.png)

```text
<game folder>/
  End/
    Binaries/
      Win64/
        d3d12.dll
        legacygpu.ini
        legacygpu_README.txt
        legacygpu_LICENSE.txt
        legacygpu_tools/
```

## First start

The mod converts the game's shaders before the game appears. Allow a few minutes, depending
on your CPU. A progress window appears if conversion takes longer than six seconds. Afterward,
the game may also compile its own shaders. Let both finish.

![Shader-conversion progress window with converted shader count and estimated time remaining.](assets/shader-conversion.png)

*Example conversion window. Shader counts and time estimates vary by game build and CPU.*

The mod creates `legacygpu_shaders` and writes `legacygpu.log` beside the DLL. Keep the default
INI settings for normal play; diagnostics are disabled by default.

## Updates

After a game update, the mod detects changes on the next start and converts new or changed
shaders. It reuses existing outputs and removes obsolete ones. An engine change can still need
a mod update.

To update the mod, close the game and extract the new release into the same game folder,
replacing its files. Back up `legacygpu.ini` first if you customized it; compare your changes
with the new defaults before restoring them.

## Uninstall

Close the game. Delete `d3d12.dll`, `legacygpu.ini`, `legacygpu.log`, `legacygpu_README.txt`,
`legacygpu_LICENSE.txt`, `legacygpu_tools` and `legacygpu_shaders` from `End\Binaries\Win64`.
The game will again require its normal GPU features.
