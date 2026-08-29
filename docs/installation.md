# SA RenderStack Installation

## Supported Target

This guide applies to the `v0.1.0-alpha.1` split package for GTA San
Andreas 1.0.0.0 US, 32-bit. It is a manual archive installation. PowerShell,
MSBuild, LLVM-MinGW, Meson, Ninja, Git, and the source repository are not
required.

## Before Installing

1. Close `gta_sa.exe` and any mod loader, injector, or diagnostic process.
2. Confirm that the target is the GTA San Andreas game root containing
   `gta_sa.exe`.
3. Back up any existing files with the same paths as the package contents,
   especially `d3d9.dll`, `SA.RenderStack.ini`, `dxvk.conf`,
   `scripts/BridgeD3D9.ini`, and `backend/dxvk-gta/d3d9.dll`.
4. Keep the existing working installation unchanged until the new profile has
   passed your normal save-game and mod workload.

## Install

1. Open `SA-RenderStack-v0.1.0-alpha.1-split.zip`.
2. Extract its contents directly into the GTA game root.
3. Allow the archive folders to merge, preserving the package paths.
4. Do not rename either runtime DLL. The root proxy must remain
   `d3d9.dll`; the backend must remain
   `backend\dxvk-gta\d3d9.dll`.

The expected runtime layout is:

```text
<GTA root>\
  gta_sa.exe
  d3d9.dll
  SA.RenderStack.ini
  dxvk.conf
  backend\dxvk-gta\d3d9.dll
  scripts\BridgeD3D9.ini
```

The package also contains `docs\INSTALL.md`, `docs\README.md`, the project
license, the DXVK license, and `manifest.json`. The SDK and symbols archives
are for development and diagnosis; they are not needed in the game root.

## First Launch

Start the game normally and confirm that the menu and a save game load. For a
diagnostic run, inspect the generated `Diagnostics\DXVK\` directory and the
configured Bridge logs after exiting the game. Keep the first test workload
small enough to make rollback unambiguous.

The release profile selects the merged DXVK backend through:

```ini
[Backend]
Backend=DXVK
DxvkBackendDir=backend\dxvk-gta
```

The root `d3d9.dll` is the Bridge entry point. The backend DLL is not copied
over the root proxy.

## Optional Hash Verification

The release page publishes SHA-256 values for the ZIP archives. On a machine
with PowerShell, an optional check is:

```powershell
Get-FileHash .\SA-RenderStack-v0.1.0-alpha.1-split.zip -Algorithm SHA256
```

The generated `manifest.json` records the expected files and hashes inside the
split package. Hash verification is optional for installation and does not
require the build toolchain.

## Rollback

1. Close the game and all loader processes.
2. Restore each backed-up file to its original path.
3. If a package path did not exist before installation, remove that new file
   or directory only after checking that it contains no unrelated user files.
4. Start the previously working installation and preserve any new diagnostic
   logs for comparison.

Do not use `git` commands or the repository build scripts as a game rollback
mechanism. The release archive is independent of the source checkout.

## Scope

This alpha is a controlled split-DLL baseline. It does not bundle ReShade,
ENB, FLA++, OLA, Project2DFX, Urbanize, or other third-party mods. It makes no
universal FPS, texture-streaming, shader, or third-party proxy compatibility
claim without a corresponding fixed-workload result.
