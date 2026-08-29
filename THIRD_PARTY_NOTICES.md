# Third-Party Notices

SA RenderStack is an aggregate distribution. The root `LICENSE` applies only
to SA RenderStack-specific files authored for this repository. Vendored code,
headers, static runtime components, and other third-party material keep their
own copyright and license terms.

Do not use the root license as a replacement for any license listed below.
When a source file or subtree contains its own notice, that notice controls
that material.

## DXVK

- Project: [DXVK](https://github.com/doitsujin/dxvk)
- Upstream tag: `v3.0.1`
- Upstream commit: `c850747f1df24180ce97b7a9094603f39da1251d`
- Source scope: `backend/dxvk/`, except for the separately listed dependency
  subtrees and headers
- License text: `backend/dxvk/LICENSE`

The GTA San Andreas compatibility overlay is a modified DXVK source version.
The D3D9 backend combines the D3D9 and DXGI entry points for the x86 release
profile. The original DXVK license and provenance records must remain with
the modified source and binary distribution.

## DXVK Dependencies

The pinned source commits and import paths are recorded in
`backend/dxvk/SA_RENDERSTACK_DEPENDENCIES.toml`.

| Component | Source path | License or notice |
| --- | --- | --- |
| MinGW DirectX headers | `backend/dxvk/include/native/directx` | `COPYING.MinGW-w64.txt` |
| Vulkan-Headers | `backend/dxvk/include/vulkan` | `LICENSE.md` (Apache-2.0 and MIT) |
| SPIR-V-Headers | `backend/dxvk/include/spirv` | `LICENSE` and `LICENSES/` |
| OpenVR headers | `backend/dxvk/include/openvr` | `LICENSE` (BSD-3-Clause style) |
| libdisplay-info | `backend/dxvk/subprojects/libdisplay-info` | `LICENSE` (MIT) |
| dxbc-spirv | `backend/dxvk/subprojects/dxbc-spirv` | `LICENSE` (MIT) |
| nested SPIR-V headers | `backend/dxvk/subprojects/dxbc-spirv/submodules/spirv_headers` | `LICENSE` and `LICENSES/` |

The corresponding license files remain in the source tree. The runtime and
SDK packages copy the relevant notices into their `docs/` directory so the
binary archive is not dependent on a checkout of the source repository.

## Static MinGW-w64 Runtime

The x86 DXVK link uses `-static`, `-static-libgcc`, and
`-static-libstdc++`. The compiler itself is not redistributed, but the
resulting binary may contain statically linked MinGW-w64 runtime components.
The exact notices from the build toolchain are preserved under
`third_party/licenses/mingw-w64/`:

- `COPYING.MinGW-w64-runtime.txt`
- `COPYING.winpthreads.txt`
- `COPYING.winstorecompat.txt`

The MinGW-w64 DirectX header notice is preserved at
`backend/dxvk/include/native/directx/COPYING.MinGW-w64.txt`.

## Distribution Rules

- Keep this file with source and binary distributions.
- Keep `backend/dxvk/LICENSE` unchanged as the DXVK license text.
- Keep all dependency copyright notices and license texts in their original
  source paths.
- Mark changes to vendored source as modified; do not present the overlay as
  official upstream DXVK.
- Do not imply endorsement by DXVK, Khronos, Valve, MinGW-w64, or any other
  upstream project.
