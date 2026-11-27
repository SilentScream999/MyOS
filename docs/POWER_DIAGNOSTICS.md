# USB boot performance on battery

The `power <output-path>` console command saves a text report from the running
machine. It reads a fixed list of CPU power registers and supported Gen9 GPU
status registers, with feature/model checks and recovery for unsupported MSRs.
It does not change CPU/GPU clocks, performance requests, thermal protection,
power limits, or firmware settings. Unsupported fields say `unavailable`.

To compare boot conditions (the reported slowdown occurs when starting or
restarting on battery):

1. Boot with AC connected, wait for startup to settle, then run
   `power /home/boot-ac.txt` in Console.
2. Shut down, unplug AC, boot on battery, wait for startup to settle, then run
   `power /home/boot-battery.txt`. Keep the same apps/windows open as before.
3. Reconnect AC without restarting, wait about 20 seconds, then run
   `power /home/battery-boot-reconnected.txt`.
4. Supply these three files. `cat /home/boot-battery.txt` displays a report.

## Evidence and startup repair

The Kaby Lake target (CPUID `0x806e9`) produced:

| Boot condition | Legacy request (`PERF_CTL`) | CPU work active MHz | Work elapsed ms |
| --- | --- | --- | --- |
| AC boot | `0x1b00` | 2813 | 64 |
| Battery boot | `0x0400` | 797 | 193 |
| Battery boot, then AC reconnected | `0x0400` | 797 | 193 |

HWP was disabled in all three reports. Package power limits, clock modulation,
performance-limit status and sampled GPU frequency were the same; thermal
throttling was inactive. This identifies a persistent low legacy CPU request as
a strong lead. The sampled GPU state does not rule out GPU behaviour under a
different workload. The user confirmed the startup repair worked immediately
on this laptop. Other CPU models and firmware configurations remain unvalidated.

At boot, MyOS now checks bare-metal Intel family 6 models `4e/5e/8e/9e`, with
SpeedStep supported and already enabled and HWP disabled. When `PERF_CTL`'s
requested ratio is at or below the processor's advertised minimum-efficiency
ratio, it requests the advertised maximum non-turbo/base ratio instead. On the
reported machine that changes the request from `0x0400` to `0x1d00` (ratio 29).
The AC request `0x1b00` is above the minimum and stays unchanged.

Only bits 15:8 of `PERF_CTL` change, with readback verification. Thermal
protection, package power limits, clock modulation, turbo controls and existing
HWP policies are preserved. Unsupported CPUs, virtual machines, disabled
SpeedStep, missing registers and invalid ratio ranges are skipped. The kernel
boot log records an applied repair or a write/verification failure. It runs
once on the BSP before timer and device initialization, with no periodic or
per-frame work. MyOS currently schedules only the BSP; future AP scheduling
will require per-CPU policy initialization.

This is a startup repair, not a load-aware governor. Requesting normal CPU
performance can consume more battery power while executing work than leaving
the CPU at its lowest performance level. Idle halt/sleep scheduling is unchanged.
The `power` command itself remains read-only.

The path is the operator's power-source label; the tool does not automatically
detect AC/battery. Each report contains a two-second observation followed by
32 million dependent integer operations, yielding between blocks. The second
stage intentionally adds CPU load for a short time. Keep animations and app
activity the same between runs; avoid launching apps during collection. For
animation-specific evidence, repeat the comparison with that animation running.

`active_cpu_mhz`, when available, uses APERF/MPERF and nominal CPU frequency.
It is the BSP's average active frequency across **all tasks** during the
interval, not the diagnostic process's exclusive frequency. Invariant TSC speed
is reported separately and is not a measurement of current CPU clock speed.
Work elapsed time includes scheduling, other tasks and the short yields.

HWP values show inherited performance bounds, desired performance and energy
preference when HWP is enabled. The package request is included when supported;
package control can make that request relevant to effective policy. Raw thermal
and performance-limit registers include sticky history: log bits alone do not
prove the machine is currently throttling. GPU status is only sampled when the
existing driver is ready and already holds render forcewake; otherwise it is
unavailable. Compositor/GPU counter differences describe work during each stage,
not a per-app or per-frame latency distribution.

If battery slows the fixed work and active CPU frequency falls, investigate CPU
policy/firmware limits. If CPU work stays fast but rendering slows, compare GPU
frequency and compositor activity. These are leads; a report does not by itself
establish causality. A second comparison booted on battery can distinguish
inherited boot policy from changes after unplugging.

Register references: [Intel software developer manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[Linux CPU MSR definitions](https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/msr-index.h),
[Intel GPU RPS implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gt/intel_rps.c).
The legacy ratio fields also follow
[Intel's Linux CPU performance driver](https://github.com/torvalds/linux/blob/v6.12/drivers/cpufreq/intel_pstate.c)
(`core_get_min_pstate`, `core_get_max_pstate_physical`, `core_get_val`).

Validation (WSL/Linux, from the repository root):

```
clang++ -std=c++20 -I. tests/power_diag_test.cpp -o .storage-test/power_diag_test
.storage-test/power_diag_test
clang++ -std=c++20 -O1 -g -no-pie -fsanitize=address,undefined -I. tests/cpu_power_policy_test.cpp -o .storage-test/cpu-power-policy-test
.storage-test/cpu-power-policy-test
make iso
python3 tests/qemu_power_diag_test.py
python3 tests/power_diag_image_test.py
```

QEMU verifies the diagnostic interface and unsupported-hardware behaviour;
physical AC/battery measurements are still necessary.
