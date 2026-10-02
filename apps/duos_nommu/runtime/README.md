# C++ and OpenCV NOMMU probes

These are opt-in boot profiles, not production decoder offload. Hardware
startup, TLS, self-relocation and timing must be verified on the board.
The existing kernel heartbeat and warm-reset DT fix are retained.

## Build (inside duodocker)

Place the read-only OpenCV reference checkout's source into
`apps/duos_nommu/build/opencv-source` first; do not run its mutating build script
in the reference directory. The tested source checkout is
`opencv-mobile-metalv` c959763. Exclude its `.git`, build and install directories.

```sh
bash apps/duos_nommu/runtime/build.sh
NOMMU_CXX_PROBE=runtime_probe bash apps/duos_nommu/package.sh
NOMMU_CXX_PROBE=opencv_probe bash apps/duos_nommu/package.sh
```

The build pins musl 1.2.5 by SHA256, compiles it as RV64IMAC/LP64 PIC, and uses
SDK musl GCC 10.2's matching soft-float C++/libgcc/libatomic libraries. The C++
headers come from that SDK toolchain. OpenCV `core`/`imgproc` are static PIC,
with thread, filesystem, environment, vector/intrinsic and OpenCL paths off.
No fast-math flag is enabled. Executables use musl's self-relocating `rcrt1.o`;
the explicit linker arguments are necessary because the SDK compiler specs
ignore or misinterpret normal `-static-pie` combinations. `verify_elf.py`
requires RV64 soft-float ET_DYN, no interpreter and no shared dependencies.
Archive symbols are hidden to convert their address relocations to RELATIVE.
`normalize_static_pie.py` resolves SDK PIC local-dynamic TLS module IDs to 1,
validating that this is the sole static module, then removes those relocations.
It rejects any remaining relocation except NONE/RELATIVE: musl rcrt1 does not
perform arbitrary dynamic symbol or TLS relocation. This is a specialized static
executable build, not a general shared-library loader.
A 512 KiB ELF stack request accommodates C++/OpenCV bring-up.

Outputs, under `apps/duos_nommu/build/`:

- `duos-nommu-runtime_probe.tar.gz`: allocator, C++ vector/string, software math,
  exception, monotonic clock and sleep probe as /init.
- `duos-nommu-opencv_probe.tar.gz`: local threshold validation and timing as /init.

Each contains `bundle/` with matched `fip-nommu.bin`/`boot-nommu.sd`, log reader,
config, ELF inspection, checksums and the verified shell rollback FIP plus
compatible main boot FIT. The regular SDK SD image and shell bundle are unchanged.

## Test on the board

First test **runtime_probe**. Extract it into its own directory:

```sh
mkdir -p /mnt/data/duos-nommu-runtime-probe
tar -xzf /mnt/data/duos-nommu-runtime_probe.tar.gz -C /mnt/data/duos-nommu-runtime-probe
cd /mnt/data/duos-nommu-runtime-probe/bundle
sha256sum -c SHA256SUMS
cp fip-nommu.bin /boot/fip.bin
cp boot-nommu.sd /boot/boot.sd
sync
```

Power-cycle, then collect on A53:

```sh
/mnt/data/duos-nommu-runtime-probe/bundle/read_nommu_log > /mnt/data/nommu-runtime.txt
cat /mnt/data/nommu-runtime.txt
```

Expected: `duos-cxx: PASS pid=1`, repeat `alive runtime=PASS`, and increasing
kernel heartbeat with zero fatal trap fields. Missing PASS is a startup failure,
not a timing result. A startup trap may also appear in the kernel log. Test warm
reboot after collecting the result. Do not interpret ELF build checks as proof
that Linux NOMMU supports every libc service.

Only after that passes, extract **opencv_probe** into
`/mnt/data/duos-nommu-opencv-probe`, install its matched boot pair, and power-cycle.
Read its log periodically with the bundle's `read_nommu_log`.
Validation covers 1890 cases: isolated strided crops, tiny dimensions, six
kernels, seven thresholds and five patterns. It prints progress after each
size. Allow several minutes for large soft-float cases. Timings are emitted
only after validation passes; mismatches print their exact case and stop.
OpenCV-version rounding differences must be resolved before using timings.

The timing stage uses 100 samples plus five warmups per mode/size, checks every
output, and reports integer nanoseconds (divide by 1000 for microseconds).
These are local C906L compute timings, not mailbox/ION offload timings. The
input generator and operations mirror the A53 threshold benchmark, using
boxFilter, saturating subtraction and threshold. No full ArUco Nano decoder,
shared image job transport, live application changes or Rust runtime are
included yet. The diagnostic shell is replaced by the probe while this profile
is installed; the main A53 shell remains available.

Rollback with both `fip-shell.bin` -> `/boot/fip.bin` and `boot-shell.sd` ->
`/boot/boot.sd`, `sync`, then power-cycle. The older standalone `fip-restore.bin`
and `boot-restore.sd` return to the regular SDK firmware pair.

## Built and deployed on 2026-10-02

- Both executable probes passed static ELF/ABI/dependency/relocation checks.
- Minimal OpenCV `core` and `imgproc` compiled successfully.
- Runtime boot Image: 1,776,028 bytes, 1,882,336 including BSS.
- OpenCV boot Image: 3,365,276 bytes, 3,471,584 including BSS.
- Both archives were SCP'd to `/mnt/data/` and read back with matching SHA256.
- The active boot pair was not replaced. Runtime startup, threshold parity and
  C906L OpenCV timings await the board test; no performance conclusion yet.

The OpenCV profile adds `printk.devkmsg=on` only to its small-kernel command
line so bursts of validation/timing rows are not silently rate-limited.

## First hardware results and rounding correction

The user tested `runtime_probe` on 2026-10-02: `duos-cxx: PASS pid=1` at
0.069223 s, repeated alive/PASS through 20.101290 s, kernel heartbeat 22 and
zero trap fields. This verifies the exercised C++ allocation, software math,
exception and clock operations, including static PIE/TLS startup on the board.
Other libc facilities and threads remain untested.

OpenCV 4.12 also started successfully, but the first validation stopped at
`2x3 k=15 t=1 pattern=3 x=1 y=0`: OpenCV=255, scalar=0. No timings were produced.
The scalar reference used OpenCV 3.2's fixed-point SHIFT=16; OpenCV 4.12's
`ColumnSum<ushort, uchar>` uses SHIFT=23. The generated pixels were
`[[125,125],[125,131],[131,126]]`. Replicated 15x15 sum at (1,0) is 28473:
SHIFT=16 rounds the mean to 126, SHIFT=23 to 127; only the latter passes the
strict `(mean - input) > 1` threshold.

The shared header now permits an explicit compile-time normalization precision,
defaulting to 16 for existing production and A53/scalar benchmarks. Only the
OpenCV 4.12 NOMMU probe selects 23. Validation remains byte-exact and mandatory;
this does not ignore differing pixels or change the live decoder. The corrected
probe awaits a complete hardware validation run. Cross-version decoder parity
with the existing A53 OpenCV 3.2 library is a separate check: native OpenCV 4.12
rounding is not guaranteed byte-identical to 3.2.

Host regression checks after the precision change passed: the exact 2x3 case
with both shifts, output-stride guards, and all 1176 existing portable threshold
cases under the unchanged default SHIFT=16. The full 1890 OpenCV validation
still needs to pass on C906L; these checks are not a substitute for that run.

## Corrected hardware run: complete PASS

The refreshed board log confirms full validation at **135.716572 s** and
**DONE PASS at 185.528411 s** after boot. Later `alive result=PASS` lines mean
both validation and all timing runs completed. The original 21.9-second file
was an early snapshot; reading it again did not refresh the result.

Preserved evidence:
[raw board log](../../../docs/benchmarks/duo-s-threshold-nommu-opencv-log.txt)
and [timing TSV](../../../docs/benchmarks/duo-s-threshold-nommu-opencv.tsv).
Several long timing lines are truncated in the captured log: all means are
complete, but unavailable p50/p95 fields are left empty, not inferred.
The source now splits timing identification and statistics into shorter lines
for future builds. The tested board bundle still uses the original long lines.

Mean local computation, 100 samples, milliseconds:

| ROI | C906L OpenCV 4.12 | C906L scalar (SHIFT=23) |
| --- | ---: | ---: |
| 32x32 | 0.235 | 0.083 |
| 64x64 | 0.601 | 0.299 |
| 128x128 | 1.501 | 1.166 |
| 200x200 | 3.308 | 2.796 |
| 320x240 | 5.915 | 5.398 |
| 640x360 | 17.074 | 16.052 |
| 1280x800 | 73.566 | 71.360 |

This establishes a working soft-float Linux NOMMU C++/OpenCV path with exact
local validation. It does not provide a threshold speed advantage over the
portable scalar kernel: OpenCV is slower at every tested size. At 640x360 it
is roughly 4.7x the earlier A53 OpenCV OD mean (~3.67 ms). That comparison uses
different OpenCV versions and builds, and the current log does not independently
capture CPU clocks; it is not a controlled CPU architecture comparison.
No mailbox, image-copy or ownership-handover cost is included here. The results
do not justify synchronous threshold offload for lower latency. Full ArUco
Nano performance remains unmeasured, and floating-point context support remains
a separate milestone before replacing software floating point.

## Hardware floating-point probe (next board test)

Build `fp_probe` with `runtime/link.sh fp_probe runtime/fp_probe.cc` (absolute
source path or path relative to the caller), then package with
`NOMMU_CXX_PROBE=fp_probe bash apps/duos_nommu/package.sh` inside duodocker.
`runtime/build.sh` also builds this executable alongside the other probes.
Output: `build/duos-nommu-fp_probe.tar.gz`, containing `bundle/`.

This opt-in profile enables CONFIG_FPU and generates a separate private DT
with the F/D ISA extensions. Default kernel configuration and little.dts
remain unchanged. Declaring F/D in the DT is not hardware discovery: actual
instruction execution is the acceptance test. The vendor startup enables
FP state, while a CSI header says no FPU and even uses ARM-style feature
macros; that conflicting header cannot decide SG2000 hardware capability.

The userspace probe keeps the LP64 soft-float calling convention and existing
soft-float libraries, but compiles its explicit assembly with RV64IMAFDC.
Disassembly verifies `fadd.d`, `fmul.s`, and FCSR access instructions. It checks:

- Kernel AT_HWCAP advertises F and D.
- FP64 1.5 + 2.0 equals 3.5; FP32 1.5 * 2.0 equals 3.0.
- Distinct FP32/FP64 register bit patterns and round-down FCSR survive 100
  20 ms sleeps that allow scheduler switches to other kernel tasks.
- Subsequent heartbeat/PASS reporting continues.

Expected: `duos-fp: arithmetic PASS`, then `duos-fp: DONE PASS` and repeated
`alive result=PASS`. This does not yet test two competing FP userspace tasks,
all registers, all rounding/exception cases, or compiler-generated hard-float
OpenCV/libc. Those need further validation before treating the FPU runtime as
complete. The probe contains no FP performance claim.

On A53, extract to `/mnt/data/duos-nommu-fp-probe`, check the bundle SHA256SUMS,
install both `fip-nommu.bin` and `boot-nommu.sd` to `/boot`, sync and power-cycle.
Collect with the bundle reader into `/mnt/data/nommu-fp.txt` using `2>&1` to
preserve the header. The bundle also has the established shell rollback pair.
If startup fails, restore that matched pair rather than repeatedly running
unsupported instructions in a production profile.

A hardware FP improvement, if confirmed, is most relevant to quad fitting,
geometry and corner refinement. The portable threshold kernel is integer code;
its roughly fivefold C906L/A53 gap cannot primarily be blamed on software FP.
OpenCV's small-area box-filter path also mostly uses integer arithmetic.

## FP hardware result and hard-float builds

The user reported `duos-fp: arithmetic PASS` at 0.066609 s and `DONE PASS`
at 3.061303 s, followed by alive/PASS. AT_HWCAP was `0x112d`. Thus the actual
C906L executed FP32/FP64 instructions and retained the tested FP registers
and FCSR across all 100 scheduler sleeps. This resolves the contradictory
vendor header for these exercised hardware capabilities. It does not prove
all-register isolation between two simultaneously active FP userspace tasks.

The next profiles use **RV64IMAFDC / LP64D**, consistently throughout musl,
C++/libgcc/libatomic, OpenCV and the executable. musl and OpenCV are freshly
built in separate `musl-hardfloat`, `runtime-hardfloat`, `opencv-hardfloat`
directories. The existing soft-float libraries/bundles remain intact.
No vector support or fast-math is enabled. The kernel and private DT enable
F/D exactly as in the successful instruction probe.

Build inside duodocker, sequentially (boot packaging shares an intermediate
kernel directory):

```sh
NOMMU_FLOAT_ABI=lp64d bash apps/duos_nommu/runtime/build.sh
NOMMU_CXX_PROBE=runtime_hardfloat_probe bash apps/duos_nommu/package.sh
NOMMU_CXX_PROBE=opencv_hardfloat_probe bash apps/duos_nommu/package.sh
```

Outputs: `build/duos-nommu-runtime_hardfloat_probe.tar.gz` and
`build/duos-nommu-opencv_hardfloat_probe.tar.gz`. ELF verification now requires
the selected ABI (default LP64; LP64D must be requested explicitly), so a
soft/hard ABI mix cannot silently pass packaging. Each is a static PIE with
no interpreter/shared dependencies and validated startup relocations.

Test the **runtime_hardfloat_probe** first. Extract its archive under
`/mnt/data/duos-nommu-runtime-hardfloat`, check SHA256SUMS, install its matched
boot pair, sync and power-cycle. Expect `ABI=LP64D hardware FP`, then
`duos-cxx: PASS pid=1` and alive/PASS. This validates compiler-generated math,
formatting/ABI, allocation and exceptions with the new library combination.
Save the reader output to `/mnt/data/nommu-runtime-hardfloat.txt` with `2>&1`.

After that passes, extract **opencv_hardfloat_probe** under
`/mnt/data/duos-nommu-opencv-hardfloat`, install its pair and power-cycle.
Expect `ABI RV64IMAFDC LP64D hardware FP`, full `validation PASS cases=1890`,
short timing record pairs and `DONE PASS`. Save the log to
`/mnt/data/nommu-opencv-hardfloat.txt` with `2>&1`. Every output still must
match the SHIFT=23 reference before any valid timing conclusion.
Compare its means with the preserved soft-float TSV; do not assume hardware
FP substantially speeds the integer k=15 threshold loop. The full ArUco Nano
decoder and inter-core job transport are not part of these profiles yet.
Use the bundle's shell rollback pair if either profile fails.

Build/ABI checks passed. The board subsequently passed the LP64D C++ runtime
probe and all 1890 hard-float OpenCV validation cases. The captured timing
results below are partial; completion of the hard-float benchmark is not yet
recorded.

## Retained test suite

Keep these sources/build scripts as repeatable regression tests; generated
libraries, executables and boot bundles remain in ignored `build/`.

- `rounding_test.c`: a focused unit regression for the reported 2x3 rounding
  case, compiled once with SHIFT=16 and once with SHIFT=23. It also checks
  output padding. The existing `apps/tinytag_detect/tests/roi_threshold_test.c`
  protects the original default across 1176 portable cases.
- `opencv_probe.cc` validation: the 1890-case OpenCV/scalar regression covers
  strides, isolated borders, tiny ROIs, thresholds and library rounding.
  It is an on-target integration/regression test rather than a fast host-only
  unit test. Both soft/hard-float profiles must retain mandatory validation.
- `runtime_probe.cc` and `fp_probe.cc`: board smoke/integration tests of
  startup, C++/libc ABI and exercised FP context preservation.
- `opencv_probe.cc` timing section: a benchmark run only after validation.
  Preserve data, but do not turn specific millisecond figures into correctness
  pass/fail limits. CPU clocks, load and library versions affect timings.

Do not silently loosen pixel comparison when versions differ. Select the
explicit matching normalizer and retain the old version's regression coverage.

Run the fast unit/portable regressions on the host with:

```sh
bash apps/duos_nommu/runtime/run_host_tests.sh
```

This runner needs a native C compiler and cleans its temporary binaries. It
requires neither OpenCV nor the board and does not substitute for the target
OpenCV/runtime/FP tests above.

## Hard-float board results, 2026-10-02

The LP64D C++ runtime reported PASS at 0.069232 s and repeated alive/PASS
through 15.091299 s. The exercised allocation, math, exceptions and clock
operations passed with the freshly built hardware-FP libraries.

The user-provided hard-float OpenCV log reported **validation PASS cases=1890**
at **83.556200 s**, compared with 135.716572 s in the soft-float run. The
captured excerpt ends at the OpenCV 320x240 timing record; it does not include
all timing modes/sizes or DONE PASS. The following means are from that excerpt:

| ROI | Soft OpenCV ms | Hard OpenCV ms | Soft scalar ms | Hard scalar ms |
| --- | ---: | ---: | ---: | ---: |
| 32x32 | 0.234992 | 0.213833 | 0.083013 | 0.084369 |
| 64x64 | 0.601426 | 0.579723 | 0.298679 | 0.307502 |
| 128x128 | 1.500518 | 1.479264 | 1.166173 | 1.204567 |
| 200x200 | 3.308456 | 3.281586 | 2.795900 | 2.898294 |
| 320x240 | 5.915259 | 5.891052 | 5.398118 | pending |

Both scalar columns run on C906L, not A53. Validation finished about 38%
sooner, but the timed k=15 threshold loop showed little OpenCV improvement.
This is consistent with much of that loop using integer arithmetic. It does
not establish the speed of floating-point-heavy quad fitting or refinement;
benchmark the full ArUco Nano decoder before deciding on an offload split.

`read_nommu_log` produces a snapshot and exits. Re-run it to refresh a saved
file; `tail -f` alone cannot make that file reflect new small-core output.
