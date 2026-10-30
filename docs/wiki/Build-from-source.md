# Build from source

The repository contains the DLL source, conversion tools and synthetic test shaders. A build
does not require private extracts, generated files from the author's checkout, or a game install.
Converting the actual game shaders requires your own installed copy of the game.

## Prerequisites

- Windows 10/11.
- Visual Studio 2022 or newer, or Build Tools, with the MSVC x64 compiler and a Windows SDK.
- CMake, Ninja and Python 3.11+ on `PATH`.
- Internet access for downloaded dependencies.

Clone or download the [source repository](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat),
then run these commands from its root:

```powershell
python -m pip install -r requirements.txt
.\proxy\build.cmd
python proxy\test\run_tests.py
```

The build downloads the pinned DXC release with a SHA-256 check and builds `dxilasm` from
its tracked source. It generates the test shader fixtures, `proxy\build\d3d12.dll`,
`proxy\build\legacygpu.ini` and `proxy\build\proxytest.exe`. Tests run on the local GPU.

The ignored `proxy/build`, `tools/dxc-*` and `tools/downloads` folders are generated outputs or
download caches. Nothing from the private `work` folder is required.

## Make a player package

```powershell
python tools\package_release.py <version>
```

This downloads pinned Python and lz4 packages as needed, then creates
`dist\LegacyGPUCompat-<version>.zip` and its `.zip.sha256` sidecar. The ZIP includes the DLL,
default config, conversion tools and dependency license files. It contains no game shaders.
Install it using the [installation guide](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Installation).

## Manual shader conversion

```powershell
python tools\build_shaders.py "<game folder>" "<game folder>\End\Binaries\Win64\legacygpu_shaders"
```

`<game folder>` contains `End`. Copy the built DLL and INI to `End\Binaries\Win64` too.
Without `legacygpu_tools` beside the DLL, it uses the shader folder as supplied and does not
perform automatic conversion after updates. Run the command again when conversion is needed.

For the runtime architecture and converter details, read
[How it works](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/blob/main/docs/how-it-works.md).
