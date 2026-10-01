# Duo S AMP with RPMsg-Lite: investigation and port plan

Status: source and hardware-document investigation, 2026-09-30. No firmware,
kernel, image, or application changes have been implemented or deployed by this
investigation. The K230 reference repository was accessed read-only.

Follow-up finding: the upstream RT-Thread BSP explicitly documents the Duo S
small C906 core as having **no MMU** [8]. Conventional Linux on C906L, including
the reversed K230-style role assignment, is therefore ruled out by the
documented hardware configuration. See the small-core Linux assessment below.

## Recommendation and CPU roles

Start with **A53 Linux + C906L bare-metal firmware**, retaining the current
camera/VPSS/TPU stack on Linux. Port the tested K230 transport and its regression
tests, using the Duo SDK's C906L startup/cache/mailbox implementation as the
hardware backend. Add Embassy once this transport works; Embassy does not
replace that backend.

The SG2000 has two selectable main processors and a separate coprocessor:

| Arrangement | Assessment |
| --- | --- |
| A53 Linux + small C906L firmware | Recommended first target; SDK already boots this pair with FreeRTOS. |
| Large C906B Linux + small C906L firmware | Existing RISC-V Linux profile provides this role assignment. Requires RISC-V userspace and vendor libraries. |
| Small C906L conventional Linux + A53 firmware | Not viable: the documented C906L configuration has no MMU. |
| Small C906L conventional Linux + large C906B firmware | Not viable for the same reason; the K230 small-core Linux arrangement does not transfer. |
| A53 firmware + large C906B Linux, or A53 Linux + large C906B firmware | Unsupported hardware combination: these two main processors cannot run simultaneously. |

Milk-V explicitly documents the A53/C906B exclusion [1]. The SG2000 TRM
sections 2.3.1 and 3.1 describe A53+C906@700MHz or C906@1GHz+C906@700MHz
operation [2]. These are nominal capabilities, not measured clock rates for
the current image. The coprocessor is described with an FPU; the main C906 is
described with vector execution. Start C906L code with scalar `rv64imafdc`, as
the current SDK does. Do not transfer the K230 `rv64gcv` build or assume an
RVV speedup.

Selecting the existing RISC-V Linux profile **does not move Linux to the small
core**: it selects C906B as the main processor. CPU subsystem numbers, mailbox
recipient numbers, and architectural `mhartid` must also remain distinct.

## What the K230 reference actually provides

Reference root: `/mnt/sda_500gb/git_repo/k230_linux_sdk_metalv`.

The transport uses Linux remoteproc/virtio/RPMsg as host and NXP RPMsg-Lite
as remote, with an already running firmware loaded by U-Boot. It is not merely
a proprietary shared-memory message queue. The reference documentation records
hardware echo, queue pressure, cold-start, and restart testing; those results
are evidence for K230 only, not latency predictions for Duo S.

Relevant reference components:

| Reference path | Reuse or change |
| --- | --- |
| `third_party/rpmsg-lite` | Pin the same upstream revision initially; preserve its license. Reference notes identify v5.4.0. |
| `buildroot-overlay/package/metal_v_amp/src/rpmsg_config.h` | Reuse static allocation, 496-byte payload and 256 descriptors initially; replace platform constants/barriers as needed. |
| `.../src/rpmsg_service.c`, `rpmsg_protocol.h` | Reuse echo, HELLO/capabilities, generation checking and deferred endpoint recreation; separate K230 address and timing assumptions. |
| `.../src/virtqueue_k230.c` | Carry the descriptor-invalidation fix if using cached noncoherent rings. Rename and audit against the pinned upstream library. |
| `.../src/rpmsg_slot_state.h` | Reuse bounded slot ownership/backpressure semantics. |
| `.../src/rpmsg_buffer_platform.h` | Replace address-range validation, translation and cache backend. |
| `.../src/rpmsg_echo_test.c`, `rpmsg-regression.sh`, `rpmsg_slot_test.c` | Rebuild for AArch64; adapt device discovery, platform diagnostics and camera backend. |
| `buildroot-overlay/package/k230_amp_mailbox/src/k230_amp_mailbox.c` | Port the architecture, not the hardware constants or Linux 6.6 API calls. |
| `.../src/start.S`, `k230.ld`, `mailbox.c`, `cache.S`, `uart3.c`, `Makefile` | Replace with Duo startup, linker layout, mailbox, cache/timer/UART and build rules. |

Here `.../src` abbreviates `buildroot-overlay/package/metal_v_amp/src`.

Two correctness fixes are particularly useful:

- Invalidate a Linux-written descriptor **before** reading its address/length
  and consuming the ring entry. The reference records first-boot message loss
  from getting this ordering wrong.
- Wait for resource-table virtio `DRIVER_OK` before scanning/announcing the
  service. Uninitialized memory must not look like an established link.

The current K230 driver also explicitly provides `vdev0buffer` as a reserved
coherent DMA pool. An older paragraph in its RPMsg note says Linux allocates
buffers from coherent memory without explaining that confinement. Follow the
current source: do not let remote-visible buffers silently fall back to
arbitrary cached Linux RAM.

## Existing Duo S boot and memory support

Evidence in this repository:

- `build/.config`: `CONFIG_ARCH="arm64"`, FreeRTOS enabled and
  `CONFIG_FAST_IMAGE_TYPE=0` in the inspected build configuration.
- `freertos/cvitek/build_cv181x.sh`: always builds the coprocessor as
  `RUN_ARCH=riscv64`, producing `install/bin/cvirtos.bin`.
- `build/scripts/fip_v2.mk`: packages that file through `BLCP_2ND_PATH` into
  `fip.bin`.
- `fsbl/plat/cv181x/bl2/bl2_opt.c:load_blcp_2nd()`: loads, checks, flushes and
  starts the image at the configured run address.
- `fsbl/plat/cv181x/platform.c:reset_c906l()`: implements the coprocessor reset
  address and reset release sequence. This is a source reference, not proof
  that Linux can access all those security registers at runtime.
- `freertos/cvitek/arch/riscv64/src/start.S`, `vectors.S`, `trap_c.c`, `cache.c`
  and `scripts/cv181x_lscript.ld`: starting material for a bare-metal BSP.

The ARM and RISC-V Duo S `memmap.py` profiles currently reserve the same
2 MiB firmware area at **0x9fe00000–0x9fffffff**. Generated ARM memory maps
report 512 MiB DDR, 510 MiB Linux memory extent, and a 170 MiB ION region with
calculated base 0x95400000. The ION DT node uses a size-based reservation;
verify the effective runtime allocations as well as these generated values.

The stock 2 MiB reservation is not a ready-made transport/bulk-buffer pool.
Reserve explicit, aligned regions for firmware, resource table, both vrings,
RPMsg buffers, counters and eventually job buffers. Keep them out of Linux's
allocator and out of ION/camera allocations. Generate addresses once for DT,
firmware and host code; do not repeat K230's 0x1d... constants.

An initial budget could enlarge the top-of-DDR exclusion to 8 MiB, subdivided
into firmware/stacks, about 1 MiB control transport, and remaining scratch/job
slots. This is a proposal, not a finalized map or demonstrated firmware size.
Changing `FREERTOS_SIZE` also changes the generated ION and other addresses;
regenerate and audit every affected map, not just the firmware linker script.
Alternatively add a separate carveout with its own memory-map definitions.

For a compatible first firmware, package it in the existing BLCP second-image
slot. A new firmware selector should allow `freertos`, `baremetal`, and later
`embassy`, with FreeRTOS retained as a rollback option. Do not overload
`CONFIG_ENABLE_FREERTOS` permanently as the meaning of every payload type.

Initially firmware updates require a matched rebuilt FIP and a reboot. Copying
a standalone firmware binary into Linux does not change what FSBL starts.
Updating the boot partition's FIP/kernel image can avoid reburning the whole
card once the exact partition layout is verified. Keep a known-good boot set.
An independent loader/reset facility for updates from the rootfs is a later
feature, requiring quiescence, cache handling, register-access validation and
generation/recovery tests.

## Mailbox: keep a single owner of each interrupt and slot protocol

The Duo mailbox is materially different from K230's single-channel doorbell:

| Property | Duo SDK evidence |
| --- | --- |
| AP mailbox MMIO | 0x01900000, 4 KiB; context at +0x400 |
| Context queue | Eight 8-byte `cmdqu_t` slots, shared allocation/valid flags and hardware spinlock |
| Recipient indices | A53=0, C906B=1, C906L=2 |
| Linux A53 interrupt | DT `GIC_SPI 85` (not a fixed Linux virtual IRQ number) |
| Linux C906B interrupt | DT PLIC source 101 |
| C906L interrupt | Source 61, also confirmed in TRM table 3-3 |
| C906L interrupt/timer backend | SDK PLIC base 0x70000000, CLINT base 0x74000000; establish the correct local context on hardware |

Sources: `osdrv/interdrv/rtos_cmdqu/cvi_mailbox.h`, `rtos_cmdqu.h`,
`rtos_cmdqu.c`; `freertos/cvitek/hal/cv181x/config/top_reg.h`, `intr_conf.h`;
`freertos/cvitek/arch/riscv64/include/irq.h`; ARM64/RISC-V base DT includes.

`cv181x_rtos_cmdqu` already owns the Linux mailbox interrupt and interprets
every received slot as a command. Merely assigning a slot to a second RPMsg
driver is insufficient: the existing handler acknowledges, disables and clears
received slots. Two drivers must not independently claim this controller.

Preferred port: add an internal AMP kick/receive hook to the existing command
queue owner, and have a separate Duo remoteproc transport use that hook.
Reserve an audited command ID for RPMsg notifications, or add a matched new
IP ID with all sender/receiver bounds and dispatch tables updated. Do not use
an arbitrary unrecognized ID: the current handler indexes a callback array.
Preserve the command-slot allocation, validity flags, clearing and hardware
spinlock protocol. RPMsg provides the queued data; a mailbox message only
indicates that rings need attention.

Use nonblocking notification with a bounded retry/coalescing mechanism when
all eight command slots are occupied. Drain rings after each event; if the
notification does not encode queue ID, check both. Acknowledge in interrupt
context and dispatch RPMsg callbacks in a worker/main task. Keep one IRQ owner
on the firmware side too.

An optional intermediate bring-up can add an RPMsg task to the existing
FreeRTOS image, exercising the same hook before replacing the scheduler. The
bare-metal target can instead preserve the small required legacy command
subset directly. This is a risk-reduction choice, not an additional transport.

## Existing services that replacement firmware must account for

The small core is not established to be unused:

- MPI's inspected `Makefile.param` and `mpi_param.mk` set
  `ENABLE_ISP_C906L = 0`; this favors keeping ISP work on Linux. The SDK still
  contains optional ISP mailbox/offload code. Audit the actual rebuilt binary
  and camera behavior before declaring independence from RTOS services.
- `CONFIG_FAST_IMAGE_TYPE=0` disables the firmware's early camera-start path.
  However, `cv181x_fast_image` still loads in the normal vendor module script.
  Its probe sends `SYS_CMD_INFO_LINUX_INIT_DONE`, expects
  `SYS_CMD_INFO_RTOS_INIT_DONE` and consumes a transfer-config pointer.
  A silent replacement firmware is therefore not a compatible drop-in.
- The firmware includes command, region/display and audio task paths, plus
  debug/time/transfer-config commands. Determine which Linux callers use them.

Either preserve the needed initialization/transfer-config ABI, or explicitly
disable/rework the relevant Linux drivers and initialization hooks in the new
profile after verifying camera, display and audio requirements. Do not fabricate
a pointer or just let the handshake time out. The proposed firmware's memory
must also remain protected from `fast_image` mappings and allocations.

## Linux 5.10 transport port

The inspected built ARM kernel configuration has `CONFIG_REMOTEPROC` and
`CONFIG_RPMSG_VIRTIO` disabled. Enable remoteproc, virtio RPMsg and RPMsg
character support and build the matching kernel/modules/device tree.

The K230 module cannot be copied or simply recompiled unchanged:

- K230 uses Linux 6.6 `get_loaded_rsc_table`, `detach`, the `is_iomem` memory
  flag and a `da_to_va` callback with an `is_iomem` output argument. These APIs
  differ or do not exist in this checkout's 5.10 `remoteproc.h`.
- This 5.10 core supports attach to prebooted firmware, but its
  `rproc_actuate()` expects the platform driver to provide `rproc->table_ptr`
  before resource processing. Implement that lifecycle for 5.10, including
  table size, ownership, cleanup and supported removal behavior.
- Port the reserved vring mappings and dedicated `vdev0buffer` coherent DMA
  pool. 5.10 `remoteproc_virtio.c` already has the buffer-pool mechanism, but
  mapping types and RAM accessors need an explicit audit.
- Its `rpmsg_char.c` exposes a control device and creates endpoints through
  `RPMSG_CREATE_EPT_IOCTL`. It lacks the newer `rpmsg-raw` ID binding used by
  the K230 reference. Use the supported `driver_override`/bind path or a small
  explicit service driver; adapt the host test to create/discover endpoints
  rather than assuming `/dev/rpmsg0` immediately appears.

Keep Linux as RPMsg host regardless of which physical CPU runs it. The
transport roles are independent of core size. Start with the prebooted attach
model; implementing a full remoteproc firmware loader/reset lifecycle is extra
work and should not delay echo validation.

## Cache and buffer ownership

Treat the pair as noncoherent until verified. The SDK provides 64-byte cache
range clean/invalidate routines using T-Head opcodes. They are better initial
references than transplanting K230 C908 setup and sync sequences unchanged.
Memory fences alone do not flush dirty cache lines.

For transport and payloads:

1. Establish the mapping attributes and avoid cached/noncached aliases.
2. A producer writes, cleans if required, then publishes ownership and rings.
3. A consumer invalidates if required, performs an acquire barrier, then reads.
4. A completion releases ownership only after results are visible.
5. Separate producer/consumer writable metadata onto distinct cache lines;
   range invalidation must not discard unrelated dirty data.

Choose an explicit reserved DMA pool on Linux, with architecture-appropriate
attributes and synchronization. Validate actual physical addresses versus
DMA/device addresses on this SoC. A C906L identity mapping does not make Linux
virtual pointers suitable message payloads.

Use RPMsg for descriptors, commands and compact results. Its Linux-compatible
496-byte payload limit is ample for that; send images through separately owned
bulk slots. Validate offset+length overflow, capacity, stride, dimensions,
format, generation and buffer lifetime on both sides.

First use copied grayscale crops to make ownership simple. Later investigate
direct VPSS/ION buffers: hold the acquired frame until every remote reader has
completed, use the existing CVI cache operations correctly, and do not release
it back to VPSS while firmware is reading. K230's V4L2/MMZ camera client is not
a Duo CVI backend. Registration plus a frame sequence/generation can prevent
stale completion from returning a reused frame.

## Bare metal and Embassy

**Bare metal:** reuse the relevant Duo assembly/BSP with a small C event loop,
RPMsg-Lite static API and the bare-metal environment. Provide startup/linker
layout, traps, PLIC claim/complete, mailbox, timer, cache maintenance, minimal
libc and diagnostics. Establish the boot entry and privilege state; do not
reuse scheduler-specific exception handlers blindly. Retain BSD/MIT/GPL and
other source license notices appropriate to each copied component.

**Embassy:** feasible on C906L, with the C RPMsg-Lite transport exposed through
a narrow Rust FFI. Current upstream Embassy documents a RISC-V 64-bit thread
executor; pin a tested release because platform feature names vary by version
[3]. Start with `riscv64gc-unknown-none-elf`, matching the C `lp64d` ABI and
without vector instructions. Keep vendor CSR/cache operations in audited
assembly/C initially.

Required Duo support includes:

- Startup/traps, linker layout and panic diagnostics; no SG2000 Embassy HAL
  was established by this investigation.
- Interrupt-safe single-core critical sections with saved/restored interrupt
  state. These are not interprocessor locks for shared DDR.
- Mailbox wakeup with a race-free pending/recheck sequence around sleep.
- A monotonic time driver and programmed alarm/interrupt if using
  `embassy-time`; upstream requires `now()` and `schedule_wake()` [4]. Measure
  timer frequency; do not copy K230's 1.6GHz cycle-delay constant.
- One task/context owning RPMsg state; callbacks enqueue jobs rather than
  waiting for long computations or mutating endpoint lists during dispatch.

Embassy schedules cooperatively. A long synchronous decoder call inside an
async task still blocks that executor. Divide jobs into bounded stages or
service transport between ROIs, with interrupts only recording pending work.
It does not automatically parallelize computation on one core or make C++
OpenCV code usable without a runtime. A full ArUco Nano offload must separately
audit its OpenCV/STL/allocation dependencies and scratch-memory needs.

If the reversed experiment eventually uses A53 firmware in AArch64 mode,
Embassy's documented AArch32 platform is not an AArch64 backend. Use the raw
executor/custom platform (or spin executor for initial diagnostics), plus
A53 exception/GIC/timer/MMU/cache support. That is more work than C906L.

## Can C906L run conventional Linux or the K230 reversed arrangement?

**No, based on the documented C906L hardware configuration: it has no MMU.**
This finding supersedes the initial investigation's unresolved hardware gate
and the suggestion that two conventional Linux instances might be a practical
alternative to a FreeRTOS worker. It is a documentation/source finding; no
firmware probe or board boot was performed for this follow-up.

### Evidence specific to the small core

The upstream RT-Thread `bsp/cvitek/README.md` covers CV1800B, SG2002 and
**SG2000 / Milk-V Duo S**. Its section 3 BSP table explicitly contrasts [8]:

| BSP / core | Documented MMU capability |
| --- | --- |
| `cv18xx_risc-v` / main C906 | MMU supported |
| `c906-little` / small C906 | `无 MMU`: no MMU |
| `cv18xx_aarch64` / A53 | MMU supported |

The same BSP excludes RT-Smart on the little core. This is direct documentation
from the port maintainers, stronger than inferring capabilities from the C906
family name or from generic SDK CSR header definitions. Sophgo's own newsletter
also explicitly calls C906L a no-MMU core when describing the Milk-V Duo
ThreadX port [9]. That newsletter concerns the Duo family example; the
RT-Thread BSP is the evidence explicitly covering SG2000.

The older SG2000 TRM's descriptions of main-processor MMUs do not contradict
this little-core BSP table. Lack of MMU is enough to rule out the normal
paged-virtual-memory Linux environment; this investigation does not additionally
claim that C906L lacks S-mode, U-mode, atomics or caches. Those are separate
capabilities and must not be inferred from MMU absence.

### What the supplied NuttX references demonstrate

The Apache NuttX board documentation boots via the **main processor** RV
selection and U-Boot's normal `booti` path [10]. The original port author's
article explicitly identifies the 1 GHz main C906 as the NuttX/Linux processor
and separately identifies the 700 MHz coprocessor [11].

The current upstream `boards/risc-v/sg2000/milkv_duos/configs/nsh/defconfig`
also sets `CONFIG_ARCH_USE_MMU=y`, `CONFIG_BUILD_KERNEL=y`, and RAM start
`0x80200000` [12]. This is useful evidence of a main-core MMU-based OS port,
not a demonstrated C906L NuttX or Linux port. Porting NuttX to C906L with a
configuration suitable for no-MMU execution could be a separate alternative
for its POSIX-like APIs; the linked main-core port cannot simply be substituted
for the existing small-core firmware.

### Linux without an MMU is a different project

This SDK's Linux 5.10 RISC-V tree does contain a NOMMU path: `arch/riscv/Kconfig`
allows `CONFIG_MMU=n`, defaults to `RISCV_M_MODE` in that case, and includes
`nommu_virt_defconfig` and `nommu_k210_defconfig`. These establish generic
software infrastructure, not SG2000 C906L support.

A NOMMU experiment would need a C906L boot/interrupt/timer/memory port, a
suitable binary format and libc/userspace build, its own protected allocation
of DDR, and mailbox/buffer drivers that coexist with main-core Linux. An
initramfs-only worker could initially leave storage/network/camera devices
owned by the main OS. The current conventional Linux/OpenCV library set is
not established to be usable unchanged in that environment.

Linux documents materially different process/mapping semantics in NOMMU mode:
no ordinary `fork()`, restricted `clone()` and `mmap()`, and contiguous backing
allocations [13]. Linux threads and some POSIX interfaces may still be useful;
MMU absence does not prohibit C++ or every threaded program. The port and
dependency audit would decide whether it provides any advantage over an RTOS.
No working SG2000 C906L NOMMU Linux image was verified in this investigation.
There is, however, a directly relevant prior bring-up report: Jisheng Zhang's
April 2024 Linux patch series states that NOMMU Linux runs on the original
CV1800B Milk-V Duo's little core with the patches and a suitable DTS [17].
That is stronger evidence of feasibility than generic NOMMU examples; it is
not a board test or an SG2000 image verified here. The reported fixes cover
configurable RAM base and T-Head CLINT quirks: 32-bit `mtimecmp` accesses and
reading the time CSR instead of an absent CLINT `mtime` register. The local
5.10 timer driver uses ordinary MMIO `mtime` and RV64 `writeq` for the compare,
so it must not be assumed ready for C906L unchanged.

### Selective ROI worker: practical NOMMU limits and OS tradeoffs

For a dedicated grayscale ROI decoder, lack of an MMU is not inherently a
blocker. C++, STL containers, heap allocation, preemptive scheduling and
threads do not inherently require virtual memory. They do require a matching
toolchain/runtime and working allocation, synchronization, clock and FPU
context support. The current decoder uses OpenCV image operations, STL and
profiling clocks; its compute path does not require `fork()` or an independent
network stack. This is a source audit, not a completed portability test.

The practical limitations are:

- No ordinary process-private virtual address spaces or MMU-based isolation.
  A bad pointer can corrupt the worker OS or accessible shared memory; do not
  assume a worker crash is contained away from main-core Linux. PMP capability
  and protection would need a separate audit.
- No ordinary `fork()`/copy-on-write and restricted file/anonymous mappings
  [13]. These are mostly avoidable for a single worker and mailbox interface.
- No demand paging to make a large working set fit. Budget real RAM for code,
  stacks, ROI scratch and queues. Large contiguous allocations can fail after
  fragmentation; allocate/reuse bounded image buffers at startup.
- Userspace binary compatibility needs explicit work. In this SDK's Linux
  5.10 `fs/Kconfig.binfmt`, ordinary `BINFMT_ELF` requires MMU and
  `BINFMT_ELF_FDPIC` does not support RISC-V. Its RISC-V NOMMU example configs
  use `BINFMT_FLAT`: investigate a static bFLT/elf2flt userspace build, rather
  than copying conventional ELF executables/shared libraries. This statement
  concerns this SDK tree, not every future RISC-V Linux version.
- POSIX threads are not inherently excluded. This Buildroot's
  `package/uclibc/Config.in` offers LinuxThreads without MMU, while its NPTL
  choice has an MMU/FDPIC condition. Actual RISC-V runtime/toolchain behavior
  remains to be built and verified.

All the options below run on the same hardware. The main cost differences are
engineering effort, RAM taken from Linux/ION and maintenance. These are
qualitative judgments from the inspected ports, not measured speed rankings.

| Worker OS | Existing basis on C906L | Application compatibility / work | Assessment for this application |
| --- | --- | --- | --- |
| Vendor FreeRTOS | Already built and booted by this SDK | C++ runtime and selected OpenCV operations need porting; native tasks/queues or a small API adapter. FreeRTOS+POSIX covers only a subset [14]. | Lowest board bring-up effort; preserve existing vendor services initially. |
| NuttX flat build | Supplied SG2000 port targets main C906, not C906L | Flat build supports a common address space, with pthread APIs and optional C++ libraries [15,16]. Need little-core startup, interrupts/timer, mailbox and vendor-service compatibility; OpenCV still needs an audit. | Worth evaluating when POSIX compatibility materially reduces decoder port work. |
| Standard RT-Thread | Upstream CVITEK BSP explicitly supports the little core [8] | Audit its selected libc/C++/POSIX features and port decoder dependencies; integrate mailbox and preserve required SDK services. RT-Smart is not supported on this core. | Another concrete BSP candidate; do not overlook it in favor of an unverified Linux port. |
| Linux NOMMU | Generic RISC-V NOMMU examples plus an upstream developer's CV1800B little-core bring-up report [17]; no verified SG2000 image | Kernel platform adaptation plus matching bFLT/libc/C++ build; Linux APIs may reduce source changes, but existing Linux binaries cannot be assumed reusable. | A minimal boot feasibility experiment is justified; application compatibility and footprint still need separate evaluation. |

OS selection alone does not make thresholding or contour traversal faster.
Measure the same decoder/compiler settings on the same core, with the same
ROI corpus, clock, memory placement and transport. An RTOS can offer more
direct scheduling and a smaller configured system, but a compute-dominated
worker may show little OS-dependent difference. Linux NOMMU also does not
provide MMU isolation simply because it is Linux.

Recommended evaluation before choosing an OS:

1. Extract a synchronous grayscale-crop-to-tag-results worker interface and
   inventory the OpenCV/STL/runtime symbols it actually needs. Keep capture,
   TPU, tracking, final results, VENC and RTSP on main-core Linux initially.
2. Cross-build that representative worker for the existing FreeRTOS runtime
   and a candidate NuttX/RT-Thread runtime. A build/link probe can reveal API
   and footprint costs before a full board port; it is not proof of board
   execution. Avoid independently rewriting the decoder for each candidate.
3. Benchmark a useful subset on the already running C906L FreeRTOS: copied
   grayscale jobs first, bounded buffers and one synchronous worker task.
   Compare Linux-only, transport-only and real offload to separate IPC cost
   from compute benefit.
4. Pursue a richer RTOS or NOMMU Linux only if specific missing APIs or
   maintenance advantages warrant its additional platform work. For viable
   candidates, record code/RAM footprint, p50/p95/p99 decode and end-to-end
   latency, throughput, queue delay, recall and Linux camera stability.

No worker extraction, cross-build, OS replacement or benchmark was performed
for this comparison. The existing 2 MiB FreeRTOS reservation is a current
layout choice, not evidence that the decoder or another OS will fit.

### Proposed minimal C906L NOMMU boot experiment

The prior CV1800B bring-up justifies testing kernel feasibility before
committing to a decoder port. Use a separate opt-in boot set, keeping A53
Linux as the main OS. The experiment replaces the existing C906L FreeRTOS;
required vendor handshakes must therefore be disabled or supplied explicitly
in the experimental main-core configuration. Do not assume the production
camera/module startup continues to work during this first test.

1. Pin a kernel revision containing the relevant NOMMU RAM-base and T-Head
   CLINT fixes, or explicitly backport them into an isolated build of 5.10.
   Audit the chosen revision rather than assuming every newer kernel includes
   the required behavior. Build in Docker as required by this repository.
2. Reserve a measured, sufficiently sized DDR region consistently in FSBL,
   U-Boot and main Linux/ION configuration. Give the small kernel a separate
   DT exposing only its own RAM and required devices. Neither kernel may
   allocate the other's region. Select the reservation from image/BSS/DT and
   runtime RAM needs, not the current FreeRTOS image size.
3. Use one hart, `CONFIG_MMU=n`, M-mode, SMP disabled, and a minimal board DT
   with verified hart ID, timer frequency, CLINT/interrupt routing and debug
   output. The SDK's little-core definitions provide candidate CLINT
   `0x74000000` and PLIC `0x70000000` addresses; validate context and routing.
   Keep block, network, multimedia and RPMsg drivers out of the first image.
4. Package a small startup shim through the existing BLCP2ND/FIP path. It
   supplies the Linux entry contract (`a0` hart ID, `a1` DTB address) in M-mode
   with interrupts initially disabled, and any required C906L initialization.
   Use a dedicated UART only after checking pin availability and ownership;
   otherwise use a bounded shared-DDR log read by main Linux. Do not let two
   OSes reconfigure or independently drive the same UART.
5. First prove kernel entry, trap diagnostics and timer-driven scheduled
   heartbeats/worker activity. A banner or expected missing-init panic alone
   proves only partial boot. A temporary built-in kernel test thread can
   exercise scheduling and allocation before a userspace toolchain exists.
6. Then boot a tiny initramfs with a compatible static `/init` and verify
   syscalls, allocation, sleep and clean fault diagnostics. This establishes
   userspace feasibility separately from kernel boot; add pthread/FPU tests
   when those features are enabled and before claiming decoder readiness.

Acceptance: main Linux remains responsive; repeated cold boots reach the
chosen milestone; timer/scheduling works; no unexpected traps or memory
overlap occurs. Record actual image/BSS/reserved/used RAM and console logs.
Keep the original matched FIP/main-kernel/DT boot set available for recovery.
No experimental firmware has been built, installed or booted yet.

Therefore ordinary dual Linux and an ordinary MMU Linux SMP kernel spanning
C906B+C906L are not viable solutions on this documented hardware. A53+C906L
also cannot share one conventional SMP kernel across their different ISAs.
The useful current options remain main-core Linux plus a FreeRTOS, other RTOS,
or bare-metal worker; NOMMU Linux remains a separate research option.

If a hardware probe is wanted to independently verify the documentation,
implement it in a disposable C906L firmware image with a safe trap handler:
record CSR availability and test actual Sv39 translation under controlled
conditions. Merely finding a `satp` definition or reading/writing a CSR is not
proof of a functioning MMU. Such a probe replaces the running RTOS and requires
a matched firmware/rollback boot set; it was not performed here.

## Implementation sequence and acceptance gates

The immediate first experiment uses the existing FreeRTOS/command-queue path
to offload only decoder ROI thresholding. See
[the thresholding experiment](duo-s-freertos-threshold-experiment.md) for code,
build/boot instructions and the three-way timing comparison. It precedes
RPMsg-Lite and NOMMU Linux bring-up and does not require either one.

The standalone threshold experiment has now completed on the board. All
outputs matched OpenCV, but synchronous FreeRTOS offload took about 24.66 ms
at 640×360 versus 4.47 ms on the A53 OpenCV path. Keep thresholding on A53
for production. Both polling runs and the implementation audit are preserved
in the experiment document and its linked benchmark TSV. This result rejects
the tested threshold partition; it does not isolate FreeRTOS overhead or
decide performance of other kernels. Counter-derived C906L frequency is
approximately 425 MHz, versus a documented 700 MHz rating. A subsequent
Linux clock report confirms C906L at 425 MHz from TPLL divided by two and
A53 at 800 MHz. Their 1.88-fold frequency difference does not explain the
roughly sixfold portable kernel elapsed-time gap; code generation, cache
behavior and worker preemption remain follow-up questions.

| Stage | Work | Gate before proceeding |
| --- | --- | --- |
| 0 | Audit RTOS services; record current camera/module startup and clock/memory state. Add an opt-in AMP profile and a generated memory contract. | Existing profile remains reproducible; new regions do not overlap Linux/ION/boot buffers. |
| 1 | C906L bare-metal banner, heartbeat and trap counters through the FIP path; preserve required legacy initialization or explicitly configure it out. | Linux boots; camera/VPSS/TPU sanity test still works. |
| 2 | Shared-memory polling integrity test, then bidirectional command-queue notification hook. | Zero CRC/data/order errors across boundary sizes; no slot leaks; correct IRQ accounting. |
| 3 | 5.10 remoteproc attach, vrings/buffer pool, name service, endpoint creation and echo. | First traffic after cold/warm boot, 1–496-byte sweep, burst/full-queue/backpressure tests and wraparound accounting all pass. |
| 4 | Versioned HELLO/generation checks, bounded job queue and recovery behavior. | Stale jobs rejected; every buffer reclaimed safely after endpoint or whole-system restart; removal/reload behavior explicitly defined. |
| 5 | Copied grayscale jobs, then optional registered camera buffers and a small compute kernel. | Byte-identical results, no premature VPSS release; timing under live camera load. |
| 6 | Optional Embassy wrapper using the same ABI, then detector work selected by measured benefit. | Same transport regressions; no long-job starvation; end-to-end recall/timing compared to Linux-only baseline. |

Do not pursue a conventional reverse-role Linux build: the documented MMU
requirement already fails. Optional independent hardware verification or a
NOMMU experiment should remain separate from the normal A53 Linux AMP port.

Proposed code locations are `apps/duos_amp/` for portable service/tests and
firmware backends, an `osdrv` Duo remoteproc module plus the command-queue hook,
and board-local AMP configuration/DT/memory-map changes. Vendor RPMsg-Lite can
be pinned under an appropriate third-party directory. These paths are a plan;
they have not been created by this investigation.

Measure mailbox RTT and RPMsg RTT separately (p50/p95/p99/max), copy/cache cost,
remote compute, queue delay, missed frames, result age and total FPS. The K230
record of approximately 0.08–0.09 ms warm echo RTT is not a Duo estimate.
Leave camera capture, TPU scheduling and buffer lifetime on Linux initially;
use bounded work units and a Linux fallback while determining how much decoder
work the scalar C906L can usefully absorb.

## Sources

1. [Milk-V Duo S specifications and simultaneous-core constraint](https://milkv.io/duo-s).
2. [Official SG2000 TRM, sections 2.3.1, 3.1, 3.2 and 3.3](https://github.com/milkv-duo/duo-files/blob/main/duo-s/datasheet/SG2000_TRM_V1.0-alpha.pdf).
3. [Embassy executor platforms and custom-platform requirements](https://docs.embassy.dev/embassy-executor/git/riscv64/index.html).
4. [Embassy time-driver interface](https://docs.rs/embassy-time-driver/latest/embassy_time_driver/trait.Driver.html).
5. [NXP RPMsg-Lite configuration and porting overview](https://github.com/nxp-mcuxpresso/rpmsg-lite/blob/main/README.md).
6. [Linux RPMsg framework](https://docs.kernel.org/staging/rpmsg.html).
7. [Milk-V stock Linux/FreeRTOS mailbox example](https://milkv.io/docs/duo/getting-started/rtoscore).
8. [Upstream RT-Thread CVITEK BSP: supported SG2000 and no-MMU little-core table](https://github.com/RT-Thread/rt-thread/blob/master/bsp/cvitek/README.md#3-bsp-支持情况).
9. [Sophgo newsletter explicitly identifying C906L as no-MMU](https://forum.sophgo.com/t/sg2042-newsletter-2024-05-03-040/597).
10. [Apache NuttX Duo S board documentation](https://nuttx.apache.org/docs/latest/platforms/risc-v/sg2000/boards/milkv_duos/index.html).
11. [SG2000 NuttX port author's bring-up account](https://lupyuen.codeberg.page/articles/sg2000.html).
12. [Upstream NuttX Duo S NSH configuration](https://github.com/apache/nuttx/blob/master/boards/risc-v/sg2000/milkv_duos/configs/nsh/defconfig).
13. [Linux NOMMU mapping and process semantics](https://docs.kernel.org/admin-guide/mm/nommu-mmap.html).
14. [FreeRTOS+POSIX scope and limitations](https://github.com/FreeRTOS/Lab-Project-FreeRTOS-POSIX/blob/main/README.md).
15. [NuttX flat/protected/kernel builds and pthreads](https://nuttx.apache.org/docs/latest/guides/usingkernelthreads.html).
16. [NuttX C++ library test and configuration](https://github.com/apache/nuttx/blob/master/Documentation/applications/testing/cxxtest/index.rst).
17. [Jisheng Zhang's NOMMU Linux bring-up report and T-Head timer fixes for the original Duo little core](https://lists.openwall.net/linux-kernel/2024/04/10/1021).

K230 source evidence: `docs/notes/k230_amp_rpmsg_lite.md`,
`docs/notes/k230_amp_mailbox.md`, `docs/notes/k230_amp_launch.md`,
`docs/amp_bigcore_rvv_plan.md`, and the implementation paths above. Historical
planning notes contain superseded boot/transport assumptions; the inspected
implementation and later regression notes take precedence.
