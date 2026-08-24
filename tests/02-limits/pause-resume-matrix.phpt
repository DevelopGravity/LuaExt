--TEST--
Pausing bills the chain the way the reference implementation defines it
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// The only --SKIPIF-- shape this suite allows, and it earns its place: every
// assertion below runs an unbounded loop to prove the CPU limit stops it. On a
// build where features() says the limit cannot be enforced at all, running an
// infinite loop to demonstrate that it is not enforced is pure waste -- and the
// harness would have to time each one out. The build that reports Unsupported
// is covered by tests/02-limits/hook-count-zero-voids-limits.phpt instead.
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

if (Sandbox::features()['cpuLimit'] === LimitSupport::Unsupported) {
	echo "skip this build reports LimitSupport::Unsupported for the CPU limit";
}
?>
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * Ported from the reference implementation's tests/timer.phpt, which is the
 * best specification anyone has written of what pausing is supposed to mean.
 *
 * The rule the fifteen rows below pin down, and which no single row states on
 * its own: in a nested PHP -> Lua -> PHP chain, ANY frame that did not
 * explicitly pause makes the whole chain count. A deeper frame may not un-bill
 * a caller that chose to be billed, which is why "paused-PHP to PHP to
 * paused-PHP" counts while "paused-PHP to paused-PHP" does not.
 *
 * The second column is what pauseTimers() returned in the innermost frame that
 * asked, which is that rule stated directly rather than inferred: false means
 * the pause was refused because an enclosing frame is being billed.
 *
 * Deliberately absent: any assertion about elapsed or consumed time. The
 * reference printed both, which made it a test of the machine it ran on. Here
 * the burn is twice the limit and the assertion is only whether the budget ran
 * out, so a 15.6 ms Windows tick and a 1 ns Linux clock give the same answer.
 *
 * The wall-clock limit is an order of magnitude above the CPU limit and exists
 * only as a backstop: a row that wedges fails in a second with the wrong class
 * rather than hanging CI.
 */

const CPU_SECONDS = 0.10;
const WALL_BACKSTOP_SECONDS = 1.0;
const BURN_SECONDS = 0.20;

// Long enough that, under the reference's old "reconstruct what expired while
// we were not looking" scheme, the pause outlasted the whole remaining budget.
// Nothing expires while paused here, because while paused nothing is measured.
const OVERRUN_SECONDS = 0.45;

$sandbox = null;
$pauseGranted = null;

function burn(float $seconds): void
{
	// Wall time, not sleep: the CPU limit measures CPU, so the loop has to
	// actually spend some. This is the same reason the reference used a busy
	// loop rather than usleep().
	$deadline = microtime(true) + $seconds;

	while (microtime(true) < $deadline) {
	}
}

function pauseTimers(): bool
{
	global $sandbox, $pauseGranted;

	$pauseGranted = $sandbox->pauseTimers();

	return $pauseGranted;
}

/* -------------------------------------------------------------------------
 * The host side of the matrix
 * ---------------------------------------------------------------------- */

$library = [
	// Billed, because it never asks not to be.
	'expensive' => static function (): void {
		burn(BURN_SECONDS);
	},

	// Asks not to be billed, then burns.
	'paused' => static function (): void {
		pauseTimers();
		burn(BURN_SECONDS);
	},

	// Asks, changes its mind, then burns. The burn is billed.
	'unpaused' => static function (): void {
		global $sandbox;

		pauseTimers();
		$sandbox->resumeTimers();
		burn(BURN_SECONDS);
	},

	// Stays paused for longer than the entire remaining budget.
	'overrun' => static function (): void {
		pauseTimers();
		burn(OVERRUN_SECONDS);
	},

	// Sets the limit again from inside the call. This must NOT refund what has
	// already been spent, and must not resume: a callback able to do either
	// could loop on it and run forever.
	'resetLimit' => static function (): void {
		global $sandbox;

		pauseTimers();
		$sandbox->setCpuLimit(CPU_SECONDS);
		burn(BURN_SECONDS);
	},

	// Re-enters Lua without pausing, so everything below it is billed.
	'call' => static function (string $path, string ...$rest): void {
		global $sandbox;

		(void) $sandbox->call($path, ...$rest);
	},

	// Re-enters Lua from a paused frame. Lua time always counts, so the pause
	// is lifted for the duration of the inner call and restored afterwards.
	'pauseCall' => static function (string $path, string ...$rest): void {
		global $sandbox;

		pauseTimers();
		(void) $sandbox->call($path, ...$rest);
	},

	// The fallback clock for the Lua side; see below.
	'now' => static fn (): float => microtime(true),
];

/* -------------------------------------------------------------------------
 * The Lua side
 * ---------------------------------------------------------------------- */

$script = <<<'LUA'
	lua = {}

	-- os.clock reports this sandbox's own billed CPU, which is exactly the
	-- quantity the limit enforces -- but it belongs to the standard-library
	-- wave and may not be installed yet, so the wall clock stands in. Either
	-- one works here: these loops only ever run unpaused.
	local function clock()
		if os ~= nil and os.clock ~= nil then
			return os.clock()
		end

		return php.now()
	end

	local function burn(seconds)
		local deadline = clock() + seconds

		while clock() < deadline do end
	end

	function lua.expensive()
		burn(BURN)
	end

	-- A paused callback returns without resuming. The boundary resumes for it,
	-- so the Lua work that follows is billed.
	function test_auto_resume()
		php.paused()
		lua.expensive()
	end

	-- The pause outlasts the remaining budget, then Lua burns. The script must
	-- stop on the burn, not be granted a second budget for having waited.
	function test_pause_overrun()
		php.overrun()
		burn(BURN)
	end
LUA;

$script = str_replace('BURN', (string) BURN_SECONDS, $script);

/* -------------------------------------------------------------------------
 * The matrix
 * ---------------------------------------------------------------------- */

function row(string $label, string $path, string ...$args): void
{
	global $sandbox, $pauseGranted, $library, $script;

	$pauseGranted = null;

	// A fresh sandbox per row. A budget spent by the previous row would stop
	// this one before it started, which would make every row after the first
	// pass for the wrong reason.
	$sandbox = new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			cpuSeconds: CPU_SECONDS,
			wallClockSeconds: WALL_BACKSTOP_SECONDS,
		),
	));

	$sandbox->registerLibrary('php', $library);
	(void) $sandbox->eval($script, '=matrix');

	$outcome = 'no';

	try {
		(void) $sandbox->call($path, ...$args);
	} catch (CpuLimitError) {
		$outcome = 'yes';
	} catch (Throwable $error) {
		$outcome = 'WRONG CLASS ' . $error::class;
	}

	printf(
		"%-46s %-3s %s\n",
		$label,
		$outcome,
		$pauseGranted === null ? '-' : var_export($pauseGranted, true),
	);

	$sandbox->close();
	$sandbox = null;
}

row('Lua usage counted', 'lua.expensive');
row('PHP usage counted', 'php.expensive');
row('Paused PHP usage not counted', 'php.paused');
row('Resume works', 'php.unpaused');
row('Auto-resume works', 'test_auto_resume');
row('Setting the limit again does not refund', 'php.resetLimit');
row('Pause overrun prevented', 'test_pause_overrun');

row('PHP to Lua counted', 'php.call', 'lua.expensive');
row('PHP to paused-PHP counted', 'php.call', 'php.paused');
row('PHP to paused-PHP to paused-PHP counted', 'php.call', 'php.pauseCall', 'php.paused');
row('paused-PHP to Lua counted', 'php.pauseCall', 'lua.expensive');
row('paused-PHP to PHP counted', 'php.pauseCall', 'php.expensive');
row('paused-PHP to paused-PHP not counted', 'php.pauseCall', 'php.paused');
row('paused-PHP to paused-PHP to paused-PHP not counted', 'php.pauseCall', 'php.pauseCall', 'php.paused');
row('paused-PHP to PHP to paused-PHP counted', 'php.pauseCall', 'php.call', 'php.paused');

?>
--EXPECT--
Lua usage counted                              yes -
PHP usage counted                              yes -
Paused PHP usage not counted                   no  true
Resume works                                   yes true
Auto-resume works                              yes true
Setting the limit again does not refund        no  true
Pause overrun prevented                        yes true
PHP to Lua counted                             yes -
PHP to paused-PHP counted                      yes false
PHP to paused-PHP to paused-PHP counted        yes false
paused-PHP to Lua counted                      yes true
paused-PHP to PHP counted                      yes true
paused-PHP to paused-PHP not counted           no  true
paused-PHP to paused-PHP to paused-PHP not counted no  true
paused-PHP to PHP to paused-PHP counted        yes false
