# Exceptions

Everything the extension throws implements `LuaThrowable`, and every class below lives in
`DevelopGravity\LuaExt\Exception\`. The hierarchy encodes one decision that matters more
than the names: **whether a Lua script can catch it**.

## The one distinction that matters

| | Root | A script's `pcall` | Meaning |
|---|---|---|---|
| **Catchable** | `RuntimeError` | catches it | the host said "the script is meant to handle this" |
| **Fatal** | `FatalError` | **cannot** catch it | a limit was reached, or the host failed |
| **Host misuse** | `LuaLogicException` | never reaches Lua | the *calling code* is wrong |

That split is enforced in C, not by convention. The extension replaces `pcall`, `xpcall`,
`coroutine.resume` and `coroutine.wrap` so a fatal is re-raised through every one of them —
including through a `__gc` finaliser and a `<close>` handler. See
[SECURITY.md](../SECURITY.md) for the nine escapes that are covered and gating.

## The hierarchy

```
Throwable
└── LuaThrowable                     (interface — everything below implements it)
    ├── LuaException                 (abstract, extends \RuntimeException)
    │   ├── RuntimeError             ← CATCHABLE inside Lua
    │   │   ├── VfsError                 the backend refused: not found, no space
    │   │   └── ModuleNotFoundError      require() found nothing
    │   └── FatalError               (abstract) ← UNCATCHABLE inside Lua
    │       ├── SyntaxError              the chunk did not parse
    │       ├── SourceLimitError         the chunk was too large to parse at all
    │       ├── BytecodeIntegrityError   a blob that could not be vouched for
    │       ├── MemoryLimitError         Limits::$memoryBytes
    │       ├── CpuLimitError            Limits::$cpuSeconds
    │       ├── WallClockLimitError      Limits::$wallClockSeconds
    │       ├── OutputLimitError         Limits::$outputBytes under Fail
    │       ├── CoroutineLimitError      maxLiveCoroutines / maxCoroutineDepth
    │       ├── HostAbortError           Sandbox::interrupt(), or a VfsQuota
    │       ├── ErrorHandlerError        the error handler itself failed
    │       ├── PanicError               the interpreter panicked
    │       └── ConversionError          a value that cannot cross the boundary
    └── LuaLogicException            (abstract, extends \LogicException)
        ├── ConfigurationError           a SandboxConfig that cannot be satisfied
        ├── CapabilityError              a call the granted capabilities forbid
        ├── ClosedSandboxError           the sandbox is closed
        └── ThreadAffinityError          called from a thread that does not own it
```

`SourceLimitError` is a **sibling** of `SyntaxError`, not a subclass, and the distinction
is deliberate: a chunk refused for exceeding `Limits::$maxSourceBytes` never reached the
parser, so there is nothing wrong with it and it carries no line. Catching `SyntaxError`
to mean "bad script" would silently swallow "script too big", which is a different problem
with a different fix.

`LuaLogicException` extends `\LogicException` because those four are bugs in the calling
code rather than conditions of the run — the same reason PHP separates the two roots.

## Reading a failure

`LuaThrowable` adds five accessors on top of `Throwable`:

| Method | Returns |
|---|---|
| `getLuaTrace()` | `list<array{source, what, currentLine, name, nameWhat, lineDefined}>\|null` — innermost frame first; null when the failure did not originate inside Lua |
| `getLuaTraceAsString()` | the stack formatted the way the interpreter prints it |
| `getSandbox()` | the `Sandbox`, or null once it is closed or if the failure preceded it |
| `getChunkName()` | the chunk the failure occurred in |
| `getLuaLine()` | the line **within the Lua chunk** |

**There is no `getLuaFile()`, and `getLine()` is not the Lua line.** PHP's
`Exception::getLine()` is `final`, so it cannot be overridden and always reports the PHP
call site that entered the sandbox. `getLuaLine()` exists because that is the only way to
report the other one.

**`getChunkName()` is attribution, not provenance.** A script granted `compileAtRuntime`
names its own chunks — `load(code, chunkname)` takes the name as an argument, in every Lua
embedding — so a host that logs `getChunkName()` alone is logging a value the script
chose. `getLuaTraceAsString()` is the attributable one: the full traceback still carries
the real calling frame beneath whatever the script named.

## Serialization

Everything under `LuaException` implements `__serialize`/`__unserialize`, so a failure can
survive a queue. Three things are deliberately dropped on the way out:

- **the sandbox**, because it wraps a live `lua_State`; `getSandbox()` returns null on the
  far side rather than a revived object that is not the same one;
- **`getTrace()`** and **`getPrevious()`**, because both can capture arbitrary live objects.

Coming back in, the Lua traceback is validated field by field and rejected whole if it does
not match the shape the capture path produces. `unserialize()` writes into an object
without running a constructor, and that is the one route by which a payload could otherwise
hand an exception a Lua context it never had.
