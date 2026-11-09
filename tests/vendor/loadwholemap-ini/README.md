# Test-only INI consumer provenance

`IniReader.h`, `ini_parser.hpp` and `LICENSE.txt` are unmodified copies from
[JuniorDjjr/LoadWholeMap](https://github.com/JuniorDjjr/LoadWholeMap), commit
`acf56ce9968aadfe169d01ab67ff3fb7a23f0af7`, directory
`PC/LoadWholeMap/IniReader/`. They are used only by the offline native test
probe, never linked into either RenderStack DLL. Keep the original notices.

| File | SHA256 of imported bytes |
| --- | --- |
| IniReader.h | 09AD30491544CBF2643DB6FEE8149E0796F2471C073C8739BA383AB88EBC31D1 |
| ini_parser.hpp | AD2A875E6D05606A5B7ACE45E027CA472C19B8FEF42AFE203F07504F259CD7AD |
| LICENSE.txt | 72496FED468FA5CBE1FDC71BFD925CA03858A8066934B59B68014B7DB3FF83B8 |

The public PC plugin source at this commit identifies itself as v1.0. It is
**not** a full source match for the installed v1.1 ASI. The regression combines
this real reader with separately audited v1.1 call-site evidence: the installed
binary passes `Settings`, `RemoveUnusedInterval` and default `60`. It does not
assert equivalence of every parser detail or all streaming behavior between
those versions. In particular, do not copy the v1.0 reaper policy into v1.1.

The tested installed ASI SHA256 is
`D7BD005EDE20F00B358B75C4D4844618024F2A74AAF60957553EBD964BEE527C`.
Its key is at RVA `0x47D20`, default push at `0x73BB`, reader call at `0x73E9`.
The native probe executes the following captured 25-byte decision fragment
from RVA `0x7F4B` (no ASI/DllMain is loaded):

```text
A1 84 CB B7 00 2B 05 D0 08 05 10 3B 05 E4 08 05 10
77 06 66 0F 2F C8 76 32
```

Three absolute address operands at offsets `1`, `7`, `13` are relocated to
probe-owned current-time, last-reclaim-time and interval DWORDs. The final
`jbe` displacement at offset `24` changes from `0x32` to `0x06`, so the two
outcomes return `true`/`false` instead of entering the original callees or
epilogue. The comparisons, unsigned timer subtraction, strict `>` branch and
RAM bypass are unchanged. RAM and cap enter as `xmm1` and `xmm0` respectively.

This covers the actual interval decision fragment, not the outer pressure
test, model removal, conversion, I/O, the main-thread game call chain, or
resource residency. The deterministic A/B counts assume the pressure gate is
already entered at each simulated callback. They are not game FPS results.
