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

The Linux and macOS rows are measured — CI asserts them on every push. **The Windows build compiles and is a required, gating CI check** (`windows-build.yml`, reused by both `ci.yml` and `release.yml`), but it runs only the language-conformance subset (`tests/10-lua/`) so far; the limit-enforcement suites have not executed there, and the ~15.6 ms figure is `GetThreadTimes`' documented granularity rather than a measurement from those tests.

That scheduler-tick resolution means short CPU limits can't be measured precisely on Windows. The rule is platform-neutral: whenever `cpuSeconds` amounts to fewer than 20 ticks of the platform's CPU clock (`cpuSeconds < 20 × cpuResolutionSeconds`), the sandbox arms a companion wall-clock deadline equal to the CPU limit itself and reports `LimitSupport::Degraded` — on Linux and macOS the threshold is microseconds or less, so in practice only Windows trips it. A spinning script always dies on every platform; only timing *precision* degrades. Call `Sandbox::features()` at runtime rather than assuming a platform's behavior — it returns `cpuLimit`, `wallClockLimit`, `cpuResolutionSeconds`, `threadSafe`, `platform`, and `capabilities`.

Native Windows arm64 (rather than x64-under-emulation) and further calibration work (e.g. `QueryThreadCycleTime`) are open items, not committed features — see the project plan's risk list.
