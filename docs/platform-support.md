# Platform support

CPU-limit enforcement is portable in the sense that it never silently does nothing — but
its *precision* is platform-dependent, and `Sandbox::features()` reports the honest number
rather than a boolean. This is the direct answer to `luasandbox`'s silent no-op problem.

| Platform | Arch | Install | CPU clock source | Typical resolution | `features()['cpuLimit']` |
|---|---|---|---|---|---|
| Linux | x64, arm64 | build from source | `pthread_getcpuclockid` + `clock_gettime` | ~nanoseconds | `Enforced` |
| macOS | x64, arm64 | build from source | `thread_info(THREAD_BASIC_INFO)` | ~microseconds | `Enforced` |
| Windows | x64 | prebuilt DLL | `GetThreadTimes` | **~15.6 ms** (scheduler tick) | `Degraded` |
| Windows | arm64 (WoA) | x64 DLL under emulation | `GetThreadTimes` | ~15.6 ms | `Degraded` |

The Linux and macOS rows are measured — CI asserts them on every push. **The two Windows rows are design intent, not observation**: that build does not compile yet, so its clock backend has never executed and `Degraded` is a prediction from `GetThreadTimes`' documented granularity.

On Windows, that ~15.6ms scheduler-tick resolution means short CPU limits can't be measured precisely. When `cpuSeconds` is set below roughly `4 × cpuResolutionSeconds`, the sandbox automatically arms a companion wall-clock deadline (`max(wallClockSeconds, cpuSeconds × 4 + 50ms)`) and reports `LimitSupport::Degraded`. A spinning script always dies on every platform; only timing *precision* degrades on Windows. Call `Sandbox::features()` at runtime rather than assuming a platform's behavior — it returns `cpuLimit`, `wallClockLimit`, `cpuResolutionSeconds`, `threadSafe`, `platform`, and `capabilities`.

Native Windows arm64 (rather than x64-under-emulation) and further calibration work (e.g. `QueryThreadCycleTime`) are open items, not committed features — see the project plan's risk list.
