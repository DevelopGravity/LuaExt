/*
 * luaext — where a script's output goes.
 *
 * A sandbox has no stdout. print, io.write and warn all end here, and the host
 * chooses between buffering, a streaming callback, or discarding.
 *
 * Two properties this subsystem owns:
 *
 *   The budget. Limits::$outputBytes caps what a script may emit, and
 *   OverflowBehavior decides what happens at the cap -- Truncate sets a flag
 *   and carries on, Fail raises an OutputLimitError a script cannot pcall past.
 *
 *   The billing. The buffer is host memory that lua_Alloc never sees, so it is
 *   charged against the sandbox's memoryBytes through luaext_alloc_charge().
 *   Without that, a script that cannot allocate a large Lua string can still
 *   exhaust the same budget by printing one.
 */

#ifndef LUAEXT_OUTPUT_H
#define LUAEXT_OUTPUT_H

#include "luaext_types.h"

/*
 * Wire the sink up from the resolved config. Called once at construction, after
 * the policy is resolved and before any library is installed.
 */
bool luaext_output_init(luaext_sandbox *sandbox, zval *config);

/* Flush anything buffered for a Callback-mode sink and release the buffer. */
void luaext_output_shutdown(luaext_sandbox *sandbox);

/*
 * Write one chunk.
 *
 * THE CONTRACT CALLERS CODE AGAINST -- print, io.write and warn all rely on it:
 *
 *   true   written, or truncated and recorded. Carry on; nothing to report.
 *   false  the caller must stop and raise.
 *
 * The Truncate-versus-Fail decision lives here rather than in the caller,
 * because this is what knows the OverflowBehavior. A caller that sees false
 * raises LUAEXT_ERR_OUTPUT as FATAL -- a script must not be able to pcall its
 * way past its own output budget.
 *
 * Callers that hold unreleased zvals must not raise directly; they ask the
 * timer layer to request an interrupt instead, and the next hook tick raises it
 * for them. print holds nothing and may raise on the spot.
 */
bool luaext_output_write(luaext_sandbox *sandbox, const char *data, size_t length);

/*
 * As above, naming the stream the bytes came from.
 *
 * The plain form is stdout. Switching channel flushes whatever is pending
 * first, because the host is told $isStderr once per chunk: a chunk carrying
 * both would have to lie about half of it.
 */
bool luaext_output_write_channel(luaext_sandbox *sandbox, const char *data, size_t length,
								 bool is_stderr);

/* What Sandbox::getOutput() / takeOutput() / getOutputLength() /
 * isOutputTruncated() report. `take` empties the buffer and resets the byte
 * count; the truncation flag survives, because a host that took the output
 * still needs to know it was incomplete. */
zend_string *luaext_output_get(luaext_sandbox *sandbox, bool take);
size_t luaext_output_length(const luaext_sandbox *sandbox);
bool luaext_output_truncated(const luaext_sandbox *sandbox);

#endif /* LUAEXT_OUTPUT_H */
