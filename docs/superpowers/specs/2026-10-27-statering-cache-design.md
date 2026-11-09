# StateRing idle capture and interface validation optimization

The user requested optimizing the two measured paths after Grove Street capture
59128/1: always-on full StateRing snapshots and repeated device API validation.
Preserve Fading/RW/D3D9/stencil restoration, picture quality, public API1-7,
F7/F8 collection, and all existing dirty work. Build candidates only; no game
launch, deployment, commits or changes to streaming/ProperShaders quality.

Use the existing guard/ring files. StateRing Enable arms the existing F10 key;
idle guards perform no full diagnostic snapshots. A key edge opens a bounded
one-second detailed window and exports it at the next owner polling point after
expiry. Holding/repeating the key cannot extend/restart that window. Snapshot
admission is independent of the mandatory Fading state correction. Stop on
shutdown/reconfiguration, and reject records started in a previous window.
This deliberately changes F10 from an immediate historical dump to an on-demand
capture; document that change. F7/F8 timing and output stay unchanged.

Cache resolved D3D9 method pointers per calling thread, keyed by actual device,
vtable, relevant method pointers and a lifecycle generation. Never cache render
state values. A warm matching resolution avoids repeated VirtualQuery. New
devices/vtables, in-place method changes, reset, final release, configuration,
shutdown and failed guarded accesses require fresh validation. Retain cold-path
readability/executability checks, getter-slot validation and SEH failure paths.
Release-method lookup may cache validated vtable/function identity; each object
still receives its matching Release, with no retained COM references.

Alternatives: turning off the entire guard risks the known loading/Fading bug;
leaving detailed capture continuous retains measured cost. An on-demand ring and
lifetime-bound pointer cache address the observed paths without changing draws.
The user has delegated implementation details and explicitly selected this work;
continue without another design/permission round.

Tests must first fail on production behavior: idle full-snapshot calls, unbounded
manual window and repeated warm validation. Exercise actual system D3D9 and
DXVK devices, state restoration/pixels, COM reference balance, vtable mutation,
device replacement/reset and invalid pointers. Forwarding counters may observe
real APIs; do not replace their behavior. Preserve the existing actual x86
Fading gateway regression. Repeated A/B compares compiled old/current production
guard/ring sources with identical native fixtures; report units and limits.
After focused checks, build All, run the full pipeline, obtain one final scoped
review and verify installed hashes remain unchanged.
