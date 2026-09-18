# OpenGL 4.6 development validation

The `gl46-dev` branch is development work, not a stable SDK or Khronos
conformance claim. Its pinned CTS discovery inventory currently accounts for
all **19,714** cases: **14,580 Pass**, **5,133 NotSupported**, and one
compatibility warning.

## NotSupported audit

`tools/audit-gl46-notsupported.py` matched every NotSupported case to its QPA
diagnostic; none lacked evidence. The result is an implementation triage, not
an acceptance waiver.

| Classification | Cases | Interpretation |
| --- | ---: | --- |
| Optional extension | 4,161 | Outside OpenGL 4.6 Core by itself |
| Profile-inapplicable | 2 | Test does not apply to this context |
| Format/sample limit | 86 | Legal advertised limit requires review with the case |
| Core capability shortfall | 583 | Implementation work, dominated by pre-raster SSBO limits |
| Target capability shortfall | 192 | Unsupported texture targets require implementation review |
| Extension-gated review | 12 | Verify whether the case is optional or exposes a core dependency |
| Manual review | 97 | Diagnostic is not sufficient for automatic disposition |

The large optional-extension count is dominated by sparse textures and
fragment shading rate. It must not hide the **884** capability/review cases in
the final four rows. In particular, tessellation-control and
tessellation-evaluation shader-storage limits currently report zero in many
constant-expression cases.

Reproduce the audit from retained local QPA results:

```sh
python3 tools/test_gl46_notsupported_audit.py
python3 tools/audit-gl46-notsupported.py \
  --plan .local/opengl46/queue-final-verified-20260917/plan.json \
  --results results --json not-supported.json --markdown NOT-SUPPORTED.md
```

## Native stress and Piglit subset

Commit `71adf81a0ff2cf6364dfb639d419e7361880b526` combines six existing or
focused public-API oracles in one native title and repeats the complete set
twice. The frozen eboot SHA-256 was
`9b2056d267d9f3e6f3832cb7b1ed3dfaf6651985f06e3531294494aa038ca011`.

All **12/12** checks passed on the firmware-6.02 console, followed by exact-title
close, runtime-layer release, healthy FTP/klog/loader services, and exact lock
release. The checks cover:

- persistent/coherent immutable buffers;
- compute SSBOs, images, atomics and indirect dispatch;
- compute-to-graphics synchronization;
- indirect graphics draws;
- SPIR-V load, specialization, link and draw; and
- Piglit's ordered six-dispatch SSBO/atomic-counter sequence, including the
  final 256-word payload.

The focused Piglit mapping, upstream pin, attribution and license are under
[`tests/piglit`](../tests/piglit/README.md). This adapts selected edge cases to
the native title harness; it does not port or claim to run Piglit's desktop
runner.
