# Duo S Ethernet PHY contention diagnosis and fix

The user observed detector throughput alternating between approximately
105 fps and 62-67 fps despite similar ROI pixels/candidates and stable detector
thread CPU time. The supplied 20-sample `top` output identified PID 91,
`kworker/0:2`, consuming 32-35% CPU during busy periods. TinyTag's CPU share fell
from about 68-72% to 50-54%; system CPU rose from about 20-22% to 48-51% and idle
fell to zero. Samples 8-12 and 15-20 had this worker load. The callback is now
identified as `phy_state_machine`; see the results below. The raw top file was copied from
`/mnt/data/tinytag-top.txt` on the board; a host copy is in `/tmp/tinytag-top.txt`.

## Diagnostic method (temporary tool retired)

The SDK kernel has modules and symbols but no ftrace, kprobes or optional
process stack traces. A temporary module sampled the interrupted task every
10 ms and read its current workqueue callback, using the matching kernel's
private worker layout. It held a task reference, bounded its histogram and
stopped after 30 seconds; the launcher unloaded it after saving results.
It did not change scheduling or driver logic.

These are CPU occupancy samples, not invocation counts or exact execution
durations. Sampling can include nested interrupt activity and can miss short
callbacks; monitoring one worker does not cover every worker. PIDs can change
across reboots. The tool has served its diagnostic purpose and its source,
module build script and launcher were removed before sealing this progress.
The supplied measurements below are retained as historical evidence.

## Board result: Ethernet PHY busy waits

The 30-second probe completed and unloaded successfully. All 679 target CPU
samples were `phy_state_machine`, out of 2999 total samples (approximately
22.6% sampled CPU occupancy across the interval). There were no unknown
callbacks or histogram overflow. The result is preserved in
[the probe output](benchmarks/duo-s-kworker-phy-probe.txt).

The board's `/var/log/messages` confirms the vendor CVITEK PHY status workaround
was running during that same interval. At uptime 3050.883 s it logged
`lp1=4d61`, randomized advertised capabilities, and ultimately logged a false
link at 3058.932 s. It repeated at 3066.085-3074.130 s, then again from
3076.165 s. These multi-second episodes match the observed intermittent load.
Selected messages are preserved in
[the PHY log excerpt](benchmarks/duo-s-kworker-phy-log-excerpt.txt).

The source path is:

```text
phy_state_machine (drivers/net/phy/phy.c)
  -> phy_check_link_status
  -> phy_read_status
  -> cv182xa_read_status (drivers/net/phy/cvitek.c)
       link speed 100, autoneg enabled, partner register == 0x4d61
       randomize advertisement, restart negotiation, poll status
       three active mdelay(10) loops, bounded at 150/1000/1000 iterations
```

`mdelay` burns CPU while waiting. This callback runs in a workqueue and already
calls sleep-capable MDIO methods; its long status-poll waits should allow the
CPU to sleep. The sampler and matching driver messages identify a concrete
CPU-consuming path, rather than merely guessing from worker names.

The correction replaces those three active 10 ms
busy waits with `usleep_range(10000, 11000)`, retaining
the vendor negotiation workaround until its hardware purpose is understood.
This SDK has high-resolution timers enabled. Sleepable waits can slightly
change polling cadence and need an Ethernet link/recovery test in addition to
the detector performance test. The driver is built into this kernel, so a fix
requires updating the kernel boot image and rebooting; replacing the live app
or the FIP alone cannot apply it. The ARM64 kernel and boot FIT have been
rebuilt and installed. Board CPU validation is recorded below; Ethernet traffic
and disconnect/reconnect validation remain pending.

### Post-reboot CPU validation

After installing the fix and rebooting, the user captured 20 one-second `top`
samples. SCP retrieval of `/boot/boot.sd` matched the staged patched FIT
byte-for-byte. TinyTag stayed at 67-73% CPU; PID 91 stayed at 0% in all samples.
Across all kernel workers, the maximum was 1% (PID 42 in four samples).
System CPU was 17-22%, with 14-18% idle. The earlier 32-35% worker spikes and
50-54% TinyTag CPU troughs did not recur in this capture. The live command used
the v7 area-cost model, no RTSP, adaptive decoding with minimum area 3600,
maximum six ROIs, no tag output and direct compact input.

[The per-sample summary](benchmarks/duo-s-phy-sleep-top.tsv)
preserves the measurements. The raw capture is retained on the host at
`/tmp/tinytag-top-after-phy-sleep.txt`.
[The post-reboot PHY log excerpt](benchmarks/duo-s-phy-sleep-log-excerpt.txt)
shows continued `lp1=4d61`, `i=150` and repeated false-link results. Thus the
negotiation workaround still executes while its CPU contention is greatly
reduced, consistent with sleepable polling. Its worker need not retain PID 91;
the comparison checks all workers. Ethernet cable state was not explicitly
reported for this capture. These results validate removal of the observed CPU
spikes during active workaround checks; they do not measure detector FPS or
prove normal Ethernet traffic/disconnect recovery. Those checks remain pending.

## Active Ethernet cable experiment

The board currently uses USB networking. Before installing the patched kernel,
keep USB connected and connect Ethernet to a powered switch/router. A real
negotiating partner may stop the repeated false-link cycle. This is a hypothesis,
not a guarantee: the workaround depends on the partner capabilities, and a real
link can still enter it. A cable alone need not have an IP address to test PHY
negotiation. An Ethernet traffic/RTSP test additionally needs an IP configuration.

Keep the same TinyTag model, adaptive threshold, scene and preview settings.
After allowing the link to settle, collect at least 30 seconds of detector/top
logs. Inspect all workers in `top`, since PIDs can change. Check the physical
Ethernet interface's carrier
and `/var/log/messages` for new `false link`/`true link` messages. Compare the
unplugged and connected runs before rebooting, then repeat both after the patch.
If worker occupancy falls with the cable but not without it, that supports a
link-state explanation. If it remains high, the sleepable fix is still needed.
The patch should remove CPU spinning even when false-link polling continues;
it does not shorten the vendor's negotiation timeout or release the PHY mutex.

## Cable-test result before the kernel fix

With an active Ethernet cable connected, the user reported that periodic
TinyTag slowdowns disappeared. The repeated 30-second probe of PID 91 recorded
**0 target CPU samples out of 2999**, compared with **679/2999** in the earlier
unplugged run. The supplied output is preserved in
[the active-cable probe record](benchmarks/duo-s-kworker-phy-active-cable-probe.txt).
No intervening kernel installation/reboot was reported, so this is recorded as
the cable-only test. Zero samples means no observed occupancy of this worker,
not that the PHY callback never ran or that every worker was idle. Together
with the earlier callback identification and false-link logs, this strongly
supports repeated PHY negotiation as the cause of the periodic contention.

The sleepable-wait fix remains appropriate: it prevents the same CPU spinning
when Ethernet is unplugged or negotiation otherwise enters the workaround.
This cable result alone does not validate the patched kernel; the separate
post-reboot CPU result is recorded above. Link traffic and connected/disconnected
timing tests remain pending.

## Build and install the sleepable-wait kernel

```sh
docker exec duodocker bash -lc 'cd /home/work && source build/envsetup_milkv.sh milkv-duos-glibc-arm64-sd && build_kernel'
```

For the SD card's FAT `/boot/boot.sd`, copy the **raw FIT** from
`install/soc_sg2000_milkv_duos_glibc_arm64_sd/rawimages/boot.sd`.
The top-level install `boot.sd` has a vendor packaging header and is not the
file to copy directly onto the mounted FAT partition.

The tested build is staged at `/mnt/data/boot-phy-sleep.sd` on the board. Stop
TinyTag cleanly, then run:

```sh
cp -p /boot/boot.sd /mnt/data/boot-before-phy-sleep.sd
cp /mnt/data/boot-phy-sleep.sd /boot/boot.sd
sync
reboot
```

To revert, copy `/mnt/data/boot-before-phy-sleep.sd` back to `/boot/boot.sd`,
sync and reboot. If Linux cannot boot, restore that file to the SD card's FAT
partition from another computer. A host backup of the board's original boot
image is retained under the ignored
`apps/tinytag_detect/build_phy_sleep_fix/boot-board-backup.sd`.

Build verification: the driver object has three calls to `usleep_range` and no
delay calls; the extracted FIT kernel matches the freshly generated compressed
Image. Kernel configuration is unchanged and the FIT device tree is byte-for-byte
identical to the board's original. SCP readback matched the staged image. SHA256:
`09bac496c6dd1850a56a473e32c3dab691075d2033d31d39ec6d13e7d6f956e0`.
The fix is in kernel source and therefore applies to future kernel/SD builds.
Ethernet negotiation, disconnect/reconnect, normal traffic and TinyTag timing
still need separate board validation; the post-reboot CPU check is recorded above.
