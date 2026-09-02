<?php

/**
 * Exception hierarchy of the luaext extension.
 *
 * Two families matter to callers:
 *
 *  - RuntimeError and its subclasses are ordinary script-level failures. A Lua
 *    script may catch them with pcall, and a host callback throws one to raise
 *    an error the script is meant to handle.
 *  - FatalError and its subclasses are not catchable inside Lua. The sandbox
 *    re-raises them through its own pcall, xpcall and coroutine.resume, so a
 *    script cannot swallow a limit breach or a host failure.
 *
 * The remaining classes report host misuse and never cross into Lua.
 *
 * Note: gen_stub.php rejects `declare` and `use` statements, so this file has
 * neither; cross-namespace names are written in full. Typing is governed by the
 * generated arginfo, not by a strict_types declaration here.
 *
 * @generate-class-entries
 */

namespace DevelopGravity\LuaExt\Exception;

/**
 * Implemented by everything this extension throws, so a host can catch the lot
 * with a single clause.
 */
interface LuaThrowable extends \Throwable
{
    /**
     * The Lua call stack at the point of failure, innermost frame first.
     *
     * @return list<array{
     *     source: string,
     *     what: string,
     *     currentLine: int,
     *     name: ?string,
     *     nameWhat: string,
     *     lineDefined: int
     * }>|null Null when the failure did not originate inside Lua.
     */
    public function getLuaTrace(): ?array;

    /** The Lua call stack formatted the way the interpreter prints it. */
    public function getLuaTraceAsString(): string;

    /** Null once the sandbox has been closed or the failure preceded it. */
    public function getSandbox(): ?\DevelopGravity\LuaExt\Sandbox;

    /** Chunk in which the failure occurred. */
    public function getChunkName(): ?string;

    /**
     * Line within the Lua chunk.
     *
     * Distinct from getLine(), which reports the PHP file that entered the
     * sandbox.
     */
    public function getLuaLine(): ?int;
}

/**
 * Base class for failures originating inside a sandbox.
 */
abstract class LuaException extends \RuntimeException implements LuaThrowable
{
    /**
     * Serialize without the sandbox.
     *
     * A sandbox wraps a live `lua_State`, so it cannot cross a process boundary
     * and is dropped here — `getSandbox()` returns null on the far side, which is
     * the honest answer rather than a revived object that is not the same one.
     * The Lua traceback survives as plain data, so a queued job that failed
     * inside Lua can still be inspected where it lands.
     *
     * PHP's own `getTrace()` and `getPrevious()` are NOT preserved: both can
     * capture arbitrary live objects — including the sandbox — and keeping them
     * would make serialization fail exactly where it is most needed.
     *
     * @return array<string, mixed>
     */
    public function __serialize(): array {}

    /**
     * Restore from {@see self::__serialize()}.
     *
     * Treats its input as hostile, because `unserialize()` writes into an object
     * without going through any constructor: a traceback whose shape does not
     * match what the sandbox produces is discarded whole rather than
     * partially honoured.
     *
     * @param array<string, mixed> $data
     */
    public function __unserialize(array $data): void {}

    /** @inheritDoc */
    public function getLuaTrace(): ?array {}

    /** @inheritDoc */
    public function getLuaTraceAsString(): string {}

    /** @inheritDoc */
    public function getSandbox(): ?\DevelopGravity\LuaExt\Sandbox {}

    /** @inheritDoc */
    public function getChunkName(): ?string {}

    /** @inheritDoc */
    public function getLuaLine(): ?int {}
}

/**
 * Base class for host misuse of the API.
 *
 * These report a mistake in the calling PHP code rather than a failure of the
 * script, are raised before or around execution, and never cross into Lua.
 * Catch this to separate "I configured it wrong" from "the script failed".
 */
abstract class LuaLogicException extends \LogicException implements LuaThrowable
{
    /** @inheritDoc */
    public function getLuaTrace(): ?array {}

    /** @inheritDoc */
    public function getLuaTraceAsString(): string {}

    /** @inheritDoc */
    public function getSandbox(): ?\DevelopGravity\LuaExt\Sandbox {}

    /** @inheritDoc */
    public function getChunkName(): ?string {}

    /** @inheritDoc */
    public function getLuaLine(): ?int {}
}

/**
 * A script-level error a Lua script may catch with pcall.
 *
 * Throw this from a host callback to raise an error the script is expected to
 * handle; anything else you throw aborts the script.
 */
class RuntimeError extends LuaException
{
}

/**
 * A filesystem condition a script should handle, such as a missing file or an
 * exhausted quota. Scripts see the usual `nil, message` result.
 */
class VfsError extends RuntimeError
{
}

/**
 * require() could not resolve a module through preloads, search paths or the
 * module resolver.
 */
class ModuleNotFoundError extends RuntimeError
{
}

/**
 * A failure a script is not allowed to intercept.
 *
 * Re-raised through the sandbox's pcall, xpcall and coroutine.resume so no
 * script can continue past its own limits or hide a host failure.
 */
abstract class FatalError extends LuaException
{
}

/**
 * A chunk did not compile.
 */
class SyntaxError extends FatalError
{
}

/**
 * A chunk was refused for exceeding Limits::$maxSourceBytes.
 *
 * Deliberately NOT a SyntaxError, which is what it used to be: nothing is wrong
 * with the chunk, it is simply larger than this sandbox accepts, and the parser
 * never saw it. That distinction is the reason this class exists — it is the one
 * refusal on the compile path with no line to report, and calling it a syntax
 * error sent whoever read the log looking for a mistake that was not there.
 *
 * Only the host-side path throws this. Lua's own load() keeps returning
 * `fail, message` for an oversized chunk, and require() keeps raising a module
 * error, because those are their own established contracts.
 */
class SourceLimitError extends FatalError
{
}

/**
 * The script reached its memory ceiling.
 */
class MemoryLimitError extends FatalError
{
}

/**
 * The script exhausted its CPU budget.
 */
class CpuLimitError extends FatalError
{
}

/**
 * The script exceeded its wall-clock deadline, including time spent waiting on
 * host callbacks.
 */
class WallClockLimitError extends FatalError
{
}

/**
 * The script produced more output than its budget allowed and the configured
 * overflow behaviour was OverflowBehavior::Fail.
 */
class OutputLimitError extends FatalError
{
}

/**
 * The script exceeded its live-coroutine or coroutine-nesting cap.
 */
class CoroutineLimitError extends FatalError
{
}

/**
 * The host called Sandbox::interrupt().
 */
class HostAbortError extends FatalError
{
}

/**
 * A Lua error handler failed while handling another error.
 */
class ErrorHandlerError extends FatalError
{
}

/**
 * The interpreter reported an unrecoverable internal fault. The sandbox is
 * closed and must not be reused.
 */
class PanicError extends FatalError
{
}

/**
 * A value could not be converted between PHP and Lua: an unsupported type, a
 * circular reference, colliding table keys, or excessive nesting.
 */
class ConversionError extends FatalError
{
}

/**
 * The configuration is contradictory or malformed.
 *
 * Raised at construction rather than on first use, so an unsatisfiable
 * combination such as debug hooks alongside a CPU limit fails immediately.
 */
class ConfigurationError extends LuaLogicException
{
}

/**
 * An operation requires a capability the sandbox was not granted.
 */
class CapabilityError extends LuaLogicException
{
}

/**
 * The sandbox has been closed.
 */
class ClosedSandboxError extends LuaLogicException
{
}

/**
 * A sandbox was used from a thread other than the one that created it.
 *
 * Only Sandbox::interrupt() is safe to call across threads.
 */
class ThreadAffinityError extends LuaLogicException
{
}
