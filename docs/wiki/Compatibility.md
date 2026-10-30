# Compatibility

## Graphics cards

| GPU | Status |
|---|---|
| NVIDIA GTX 10 series (Pascal) | Tested on a GTX 1070; author reports extended gameplay without issues. |
| NVIDIA GTX 900, AMD RX 400/500, RX Vega and RX 5000 | Untested targets. They lack mesh shaders, but may encounter other hardware or driver limits. |
| GTX 16, RTX, RX 6000+ and Intel Arc | Have mesh shaders; this mod is not needed. |

The documented test setup is a GTX 1070, driver 580.97, Steam build 23447986, at 1440p,
66% render scale and Medium settings, at about 37 FPS. This is one setup, not a promise for
other areas or cards. Lower settings and render scale can help when GPU limited.

## Optional mods

| Mod | Setup |
|---|---|
| Legacy GPU Compatibility | Keep its filename `d3d12.dll`. |
| [ShaderInjector](https://github.com/frostbone25/ShaderInjector) | Recommended for visual adjustments. Follow its [installation guide](https://github.com/frostbone25/ShaderInjector/blob/main/INSTALL.md), using its own `dsound.dll` loader. |
| [OptiScaler](https://github.com/optiscaler/OptiScaler) | Use as `dxgi.dll` for alternative upscalers and optional frame generation. |
| Other `d3d12.dll` mods | Conflict with this mod's filename. Remove them before installation. |
| ReShade | Untested with this project. |

XeFG is an optional frame-generation output in OptiScaler where supported by the GPU and
installed tools. It is separate from XeSS upscaling. Follow the upstream
[OptiFG guide](https://github.com/optiscaler/OptiScaler/wiki/OptiFG) for input/output selection,
restart and activation. Frame generation improves displayed smoothness; it does not increase
the underlying game simulation rate.

Install and check each addition separately. Consult ShaderInjector's
[compatibility notes](https://github.com/frostbone25/ShaderInjector/blob/main/Solutions.md) for
issues when combining it with OptiScaler. These are separate downloads; the compatibility mod
does not bundle them or certify every combination and version.

## Known limitations

- Seven reflection compute shaders require unsupported half arithmetic and are skipped, so
  some reflection effects can differ. Four debug mesh shaders with line topology are also skipped.
- A sudden increase in geometry can briefly exceed the emulation buffers before they adapt,
  leaving some geometry missing for a frame.
- Rasterization order across mesh groups is approximate.
- Future engine changes may require converter updates even though game shader updates are
  handled automatically.
