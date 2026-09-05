# Configuration

Everything a host chooses when it builds a `Sandbox`, in one place. Names, defaults and
types below match `stubs/luaext.stub.php` exactly, and `tools/check-docs-api.php` fails
the build if they drift.

`SandboxConfig` is the single argument to `new Sandbox()`. Every field has a default, so
`new Sandbox()` is a complete, untrusted, fully-bounded interpreter.

Trust is a single object, `Capabilities`, passed inside `SandboxConfig`. The default constructor (`new Capabilities()`) *is* the untrusted baseline — there is no separate "safe mode" flag to forget. `Capabilities::trusted()` is a preset that flips some flags; individual flags can always be overridden with `with()` regardless of preset.

| Capability | Untrusted default | `trusted()` | Notes |
|---|---|---|---|
| `loadBytecode` | `false` | `false` | Stays off even when trusted — see [SECURITY.md](../SECURITY.md). Must be opted into explicitly with `with()`. |
| `compileAtRuntime` (Lua-visible `load()`) | `false` | `true` | |
| `dumpBytecode` | `false` | `true` | |
| `require` | `false` | `true` | Resolution order is `package.loaded` → cycle guard → limits → `package.preload` → the VFS along `modulePaths` → `ModuleResolver`. |
| `vfs` / `vfsWrite` | `false` / `false` | `true` / `false` | Write access is a separate flag even when trusted. Both need a `FileSystem`, and `vfsWrite` cannot be granted without `vfs` — it widens read access rather than replacing it. |
| `coroutines` | `true` | `true` | On by default in both presets, capped by `maxLiveCoroutines`/`maxCoroutineDepth`, and strictly call-scoped. |
| `osTime` | `true` | `true` | |
| `osEnv` (+ allowlist) | `false` | `false` | |
| `debugTraceback` | `true` | `true` | |
| `debugIntrospect` | `false` | `true` | |
| `debugMutate` | `false` | `false` | Never flipped by a preset. |
| `debugHooks` | `false` | `false` | Defeats the watchdog by design; enabling it while a CPU **or** wall-clock limit is set throws `ConfigurationError` — both are delivered through the interpreter hook `debug.sethook` would displace. Never flipped by a preset. |
| `utf8` | `true` | `true` | |
| `gcControl` | `false` | `true` | |
| `warn` | `false` | `true` | |

> **Every flag in this table gates real behaviour and is covered by tests.** That was not always true, so the map that says so is still worth asking:
>
> ```php
> Sandbox::features()['capabilities']['vfs']; // true in this build
> ```
>
> It covers every boolean capability and reports whether *this build implements it*, which is a different question from whether a `Capabilities` object will accept it. `vfs` and `vfsWrite` are still refused at construction without a `FileSystem` — that is a permanent rule about configuration, not a statement about what is built.

`Limits` (defaults shown) caps resource use independent of trust level:

| Limit | Default |
|---|---|
| `memoryBytes` | 32 MiB |
| `cpuSeconds` | 1.0 |
| `wallClockSeconds` | 5.0 |
| `outputBytes` (+ `outputOverflow`) | 1 MiB, overflow `Fail` |
| `maxLiveCoroutines` | 64 |
| `maxCoroutineDepth` | 16 |
| `maxCallDepth` | 200 |
| `maxModules` | 64 |
| `maxRequireDepth` | 16 |
| `maxStringLength` | 64 MiB |
| `maxSourceBytes` | 1 MiB |
| `maxConversionDepth` | 64 |
| `maxCachedChunks` | 64 |

### Where the timing limits are actually checked

`cpuSeconds` and `wallClockSeconds` are enforced by a watchdog thread that does one thing
when a deadline passes: it sets a flag. Something has to *read* that flag, and the reads
all live in code this extension controls — the interpreter loop's backward jumps and tail
calls, the patched loops inside the string and `utf8` libraries, and the boundary a host
callback returns through.

**Nothing reads it inside PHP.** A registered callable that blocks — `sleep()`, a slow
query, an HTTP request without a timeout — runs to completion no matter what the deadline
says, because there is no safe way to preempt arbitrary PHP from another thread.

The time is still *billed*. With `wallClockSeconds: 0.5` and a callback that sleeps two
seconds:

```
outcome:        WallClockLimitError
actual elapsed: 2.00s        <- ran 4x its limit
stats wall:     2.004s       <- the callback's time was counted
```

So the limit is honoured but late: the breach is delivered the instant the callback
returns, the script cannot run another instruction, and `stats()` reports the real elapsed
time rather than the limit. The overshoot is exactly how long the callback blocked.

**Bounding that is the host's job** — put timeouts on your own I/O. `pauseTimers()` does
the opposite and is for the case where callback time genuinely should not be billed to the
script.

CPU limits are less exposed by this: a sleeping callback burns no CPU, so `cpuSeconds` is
unaffected by a blocked host. A callback that *spins* is billed and, like the wall clock,
is stopped only on return.

## VfsQuota

Filesystem access has its own budget, independent of `Limits` and applied before a backend
is ever called. Every field is enforced — see
[tests/06-vfs/quotas-bound-what-a-script-can-demand.phpt](../tests/06-vfs/quotas-bound-what-a-script-can-demand.phpt),
which drives each one at the bound it names.

| Field | Default | Bounds |
|---|---:|---|
| `maxOpenHandles` | 16 | file handles open at once |
| `maxFileBytes` | 1 MiB | the size of any one file |
| `maxTotalBytes` | 8 MiB | bytes buffered across all handles |
| `maxFiles` | 128 | files that may exist in the namespace |
| `maxOperations` | 10000 | calls into the backend **per sandbox call** |
| `maxPathLength` | 255 | the canonical path, in bytes |
| `maxPathDepth` | 16 | path components |
| `billWallTime` | `false` | whether time inside a backend call counts against the wall clock |

Two of these behave differently from the rest and are worth stating plainly.

**`maxOperations` refills.** It is spent per sandbox *call* and reset for the next one, so
a host that runs many calls does not find the hundredth refused because the first
ninety-nine spent the budget. A nested call reached through a host callback is part of the
call already running and does not get a fresh budget — otherwise a script could reset its
own quota by bouncing through the host.

**`billWallTime` defaults to off**, which means a backend that hangs will not trip
`WallClockLimitError` on its own. That is a deliberate trade — the host, not the script, is
doing that work — and it is the one place a slow backend can outlast a sandbox's limits.
See [SECURITY.md](../SECURITY.md) on what that costs.

The split between fatal and catchable follows what a refusal costs the host: reaching a
*resource* bound (handles, bytes, files, operations) is fatal, because a script able to
catch it would retry and the host would pay again. Refusing a *request* before any work
happens (an overlong path, too deep a path, a range past `maxFileBytes`) is catchable,
because the script can legitimately adapt and the refusal cost nothing.
