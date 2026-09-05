# The playground

A browser UI for the whole public API, in one self-contained PHP file: [`examples/playground/index.php`](../examples/playground/index.php). Write Lua in a textarea, flip capability and limit switches, hit **Run**, and read back everything the sandbox can tell you — return values, printed output, the full [`SandboxStats`](configuration.md) readout, profiler samples, and the exception that stopped a script that earned one.

It exists for three jobs: kicking the tires of a fresh build, reproducing a behaviour question without writing a harness, and demonstrating what "capability-based" actually means — every checkbox on the page is a real `Capabilities` field, and unchecking it makes the corresponding library genuinely disappear from the script's world.

## Running it

From the repository root, with the extension loaded (a normal `make install` plus conf.d entry — do **not** add `-d extension=` on top, PHP will warn about the double load):

```bash
php -S localhost:8080 examples/playground/index.php
```

Open <http://localhost:8080/>. There is no build step, no dependency, and no state outside your PHP session and your browser's localStorage.

## What is on the page

- **Script** — the Lua editor, a preset dropdown, and an input box whose JSON is decoded and handed to the script both as the global `input` and as the chunk's vararg (`local input = ...`). The presets are small worked examples: hello world, the `json` and `html` host libraries, a `#[LuaMethod]` object, user-defined host classes, VFS read/write, a binary file for the hex viewer, `require()` from the VFS, scripts that run into the CPU and memory limits on purpose, coroutines, and the profiler. Selecting a preset checks the capabilities it needs and seeds the VFS files it reads.
- **Results and Stats** — every run reports its return values (tables are shown key-by-key, exactly as PHP received them — a JSON array's key `0` sitting outside Lua's `#` sequence is visible here, not papered over), buffered or per-chunk output, and all sixteen `SandboxStats` fields. Failures show the exception class, the error category, and the Lua traceback with line numbers.
- **Capabilities, Limits, VFS quota, execution options** — the full `Capabilities`, `Limits` and `VfsQuota` surface as form fields, plus output mode (Buffer, Callback with visible chunk boundaries, Discard), the sampling profiler, and determinism (seed + `deterministic`).
- **VFS** — an in-memory `RangedFileSystem` implementation that persists in your PHP session across runs, so a file one script writes is there for the next. Files can be added, edited and deleted from the page, and every file has a viewer: UTF-8 content renders as text, anything else as an `xxd`-style hex dump.
- **Host classes** — a panel for extending the bridge from the browser. Each entry is a PHP snippet that must `return` either an object (registered with `registerObject()`, so its `#[LuaMethod]`-marked methods become visible) or an `array<string, callable>` (registered with `registerLibrary()`):

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;

return new class {
    #[LuaMethod('hello')]
    public function sayHello(string $name): string
    {
        return "Hello, {$name}!";
    }
};
```

Entries live in your browser's localStorage and are re-evaluated on every run, because every run is a fresh `Sandbox`.

## The part that is deliberately not sandboxed

The host classes panel is `eval()` of PHP you typed, running with the full privileges of the PHP process. That is the point — it demonstrates writing the *host* side of the bridge, and the host side is by definition trusted. The sandbox's `Capabilities` and `Limits` govern what the **Lua script** can reach; they say nothing about what a registered PHP callback does once called. The page states this in a banner, and the file refuses to serve anything but `127.0.0.1`/`::1` (checked against `REMOTE_ADDR` only — forwarded headers are client-supplied and are ignored). It is a development tool. Do not put it behind a proxy, a tunnel, or anything else that makes it reachable.

## Guardrails and honest quirks

The playground is itself a host, and it makes host decisions you should know about before mistaking them for extension behaviour:

- **CPU and wall-clock limits are clamped to 10 seconds**, whatever the form says. `php -S` serves one request at a time, so an unbounded script would hang the server for every other tab. The memory limit is clamped to 256 MiB the same way, and asking for "unlimited" (0) gets you the clamp, not zero.
- **`debugHooks` always fails here** — with the extension's own `ConfigurationError`, which explains why: that capability requires *both* time limits to be unlimited, and the playground never grants that. The refusal message is the demo.
- **`cacheCompiledChunks` is a visible no-op**, because every run constructs a fresh sandbox. It is on the page anyway, since discovering that in a form is cheaper than discovering it in production.
- **Xdebug distorts every number.** If it is loaded, the page says so in a banner — CPU and wall-clock stats will read high, and a slow-enough debugger can make an innocent script trip a limit.
- The `toolkit` demo object keeps its counter for exactly one run, while the VFS persists across runs — that contrast is intentional, and is the difference between per-call host state and host-owned storage.
