# OpenCV runtime and shared buffers on C906L Linux NOMMU

Status: following the verified diagnostic shell and two successful warm reboots,
soft-float static PIE C++ and minimal OpenCV threshold probes now build. See
[runtime probe instructions](runtime/README.md). C++ startup/allocation/math/exceptions/clocks passed on hardware. After matching
the 4.12 normalization precision, all 1890 OpenCV/scalar validation cases and
all timing runs completed successfully (135.7 s validation, 185.5 s completion).
Results are preserved in the runtime README and linked benchmark files.
Rust workers and image-job transport are not implemented.

## Runtime choice

Reuse the existing C++ ArUco Nano source and dictionary code, initially with
single-threaded OpenCV `core` and `imgproc`. Keep the portable scalar threshold
as a reference. Start with identical synthetic inputs and threshold-output
validation before testing the complete crop decoder. Record allocation peaks,
compute time, transport time, IDs and corners separately.

The read-only reference `/mnt/sda_500gb/git_repo/opencv-mobile-metalv`
(checkout c959763) includes `BAREMETAL_README.md`, a Newlib build recipe, and
`bare_metal_syscalls.c`. Its `_write`/`_read` hooks are placeholders; its linker
heap and hardware timer assumptions require adaptation. They cannot be used
unchanged as Linux userspace services. Upstream
[opencv-mobile](https://github.com/nihui/opencv-mobile) provides useful minimal
build options, but its normal Linux/Milk-V packages do not establish support
for our C906L NOMMU runtime or its ABI.

The first implementation selects PIC musl 1.2.5 plus the SDK's soft-float
musl C++ runtime, with explicit static PIE linking and narrowly validated local
TLS relocation handling. The reference Newlib route remains an alternative;
its placeholder hooks are not part of this implementation.

First prove startup, allocation, math, timing, error handling, and C++ runtime
support in a static executable accepted by our ELF loader. A small Newlib
adapter calling Linux syscalls is one candidate; do not equate a Newlib
bare-metal build with a conventional Linux libc. Audit static PIE relocation
and linker assumptions before selecting it. Exclude GUI, codecs, video I/O,
OpenMP and pthread parallelism from the initial build. Match the A53 OpenCV
version where practical and check algorithm parity if versions differ.

The current kernel disables FPU and vector support and /init uses RV64IMAC,
soft-float LP64. Do not link LP64D libraries or enable vector code by merely
changing compiler flags. Either build consistent soft-float dependencies or
first verify floating-point hardware and Linux context preservation.

### Rust

For this experiment Rust is not a simpler replacement for C/C++: OpenCV and
our detector still require their C++ runtime. Do not rewrite the algorithm to
introduce a second correctness variable. Rust can later implement a worker
supervisor with typed buffer ownership through a narrow C ABI:

- A `no_std` executable with syscall adapters and an allocator is a candidate;
  it still needs suitable PIE/startup handling and target/ABI verification.
- The documented `riscv64imac-unknown-none-elf` target is a bare target, not a
  ready Linux NOMMU runtime. Standard Linux Rust targets and their prebuilt
  `std` libraries must not be assumed compatible.
- Expose only fixed-width C job/result structs, opaque worker handles and
  borrowed byte buffers. Do not exchange C++ Mat/vector objects or Rust Vec,
  references, trait objects or allocator-owned objects across cores.
- Unsafe mapping/cache operations belong behind one audited interface. Rust
  atomics and ownership checks do not provide hardware cache coherence.

Choose Rust for the supervisor only if its allocation, timing, syscall and C
ABI probes meet the same requirements without increasing bring-up complexity.
Rust is an evaluated option, not an installed runtime component.
See [Rust target support](https://doc.rust-lang.org/rustc/platform-support.html).

## Addresses and buffer ownership

Treat inter-core shared DDR as non-coherent. Mailboxes carry notifications and
small descriptors; image bytes remain in explicitly shared memory. Each Linux
has its own allocator: neither may allocate the other's reservation.

Use registered buffer IDs plus bounded offsets/lengths, sequence numbers and a
boot generation. A53 virtual pointers are not transferable. The receiver
resolves the buffer ID to its own kernel mapping; validate ROI coordinates,
stride and lengths with overflow-safe arithmetic. NOMMU's lack of translation
is not a reason to accept arbitrary physical addresses from userspace.

Initially copy a grayscale ROI into a dedicated shared pool so camera frames
can be released independently. A later zero-copy path may register the ION/VPSS
physical plane and stride, but must retain the original frame until the remote
worker explicitly finishes reading it. Never put that ION region in the little
kernel's general allocator. Wrap a mapped plane in a local non-owning Mat;
OpenCV outputs/scratch belong to the worker, not to the camera buffer.

Use a bounded slot lifecycle:

```
A53 owns FREE -> fills input -> publishes READY
C906L accepts READY -> computes -> publishes DONE
A53 consumes DONE -> returns slot to FREE
```

Input is immutable from READY through DONE. Only the current owner writes a
slot. Put request, completion and producer/consumer indices on separate aligned
cache lines (64 bytes in the current experiments); round maintained ranges and
pad allocations so maintenance cannot affect unrelated writable data. Result
arrays need a capacity and an explicit count/overflow status.

## Cache maintenance and ordering

Memory barriers, volatile and atomics alone do not flush caches. Perform cache
maintenance through kernel/device interfaces, keeping architecture-specific
instructions out of the OpenCV algorithm and ordinary application code.

1. A53 prepares input and request, cleans dirty cache lines to shared memory,
   completes maintenance and orders writes, then publishes readiness and
   notifies C906L.
2. C906L invalidates the published request and input before reading, observes
   readiness with acquire ordering, validates the descriptor, then computes.
3. C906L cleans output/result lines, completes maintenance, then publishes and
   cleans the completion sequence with release ordering. Notify if supported.
4. A53 invalidates completion before polling, observes the matching generation
   and sequence, invalidates output/results, then reads and recycles the slot.

For DMA-produced camera frames, CPU cleaning is not the acquisition operation:
use the media driver's DMA ownership/cache contract, then make the plane
visible to the remote CPU. A53 CPU overlays/writes require their own clean
before transfer. Avoid conflicting cached/uncached aliases of the same data.

The existing threshold experiment demonstrates ION physical address + offset
handover, A53 CVI cache operations, and a separately published completion.
The NOMMU shell demonstrates separate shared ring indices and explicit C906
clean/invalidate. Neither is yet a production NOMMU image-job device.
The console's uncached A53 mapping is for control traffic; do not assume it
is an efficient way to read whole images for decoding.

## Completion, backpressure and recovery

Polling is sufficient for the first integrity benchmark. Later notification
should wake a sleeping Linux task; do not add another IRQ owner beside the
existing mailbox driver without auditing ownership. A doorbell does not
replace cache synchronization or prove completion.

Use a small bounded queue and latest-frame policy. Do not overwrite in-flight
input. A timeout is not permission to release the camera frame, free memory,
or reuse the slot: the remote worker might still access it. Quarantine the
slot until an acknowledged cancellation/drain or a proven remote stop/reset.
Reject results from earlier boot generations. A whole-board reset can reclaim
all slots; independent-core restart needs an explicit quiescence protocol.

## Acceptance sequence

1. Runtime startup, allocator, math and ABI probes on C906L.
2. OpenCV threshold validation against the A53 reference, then full decoder
   correctness and timing, initially with local deterministic buffers.
3. Shared-buffer integrity: CRC/pattern checks, strided/tiny ROIs, cache-line
   boundaries, sequence wrap, bounded queue pressure and stale generations.
4. Copied ROI jobs; measure copy/cache/queue/compute/result costs separately.
5. Registered camera buffers only after lifecycle and timeout tests pass.

These measurements determine useful offload partitions. Linux or Rust alone
should not be expected to remove the roughly 4–5x compute gap seen in the
700 MHz C906L threshold experiments; bare-metal and FreeRTOS kernel compute
were already nearly equal.

## Hardware FP milestone

Explicit FP32/FP64 arithmetic and the exercised register/FCSR preservation
passed all 100 scheduler sleeps on C906L. Separate LP64D musl/OpenCV libraries
and runtime/OpenCV boot probes now compile, pass ABI/relocation checks and have
been uploaded with verified readback. Complete hard-float runtime and threshold
validation/timing remain pending board tests. Keep full decoder porting behind
that gate. The shared image-job protocol remains a later milestone.
