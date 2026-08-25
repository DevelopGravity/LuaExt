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
 * The wall-clock limit is well over an order of magnitude above the CPU limit
 * and exists only as a backstop: a row that wedges fails in a few seconds with
 * the wrong class rather than hanging CI. It has to clear BURN_CEILING_SECONDS
 * comfortably, because a paused row legitimately spends that long waiting.
 */

const CPU_SECONDS = 0.10;
const WALL_BACKSTOP_SECONDS = 6.0;
const BURN_SECONDS = 0.20;

/*
 * How long a burn waits before concluding it is not being billed at all.
 *
 * A paused frame never advances the CPU counter -- while paused nothing is
 * measured, which is the whole point -- so those burns have to give up on
 * something. Giving up on a SHORT probe rather than on the full ceiling is what
 * keeps four deliberately-paused rows from dominating the runtime of the file.
 *
 * Long enough to survive a coarse clock: on Windows' ~15.6ms scheduler tick this
 * is still six ticks, so a genuinely-billed frame cannot look idle here.
 */
const BURN_PROBE_SECONDS = 0.10;

// Long enough that, under the reference's old "reconstruct what expired while
// we were not looking" scheme, the pause outlasted the whole remaining budget.
// Nothing expires while paused here, because while paused nothing is measured.
const OVERRUN_SECONDS = 0.45;

$sandbox = null;
$pauseGranted = null;

/*
 * How much CPU this machine actually hands a busy loop per second of wall time.
 *
 * Measured rather than assumed, because it is the single number that decides
 * whether a burn can finish. A hardcoded wall ceiling is a bet on runner speed:
 * set it where a fast machine likes it and a contended one starves; set it where
 * a slow machine likes it and every paused row pays for the slowest box in the
 * matrix. The previous version of this file made that bet and lost it on
 * macos-15-intel.
 *
 * getrusage() is used deliberately instead of the sandbox's own counter: no
 * sandbox is armed yet at this point, and process CPU is the right question
 * anyway. Where it is unavailable or nonsensical the fallback is pessimistic,
 * since guessing high is the failure that makes rows starve.
 */
function measureCpuEfficiency(): float
{
	$cpu = static function (): ?float {
		$usage = getrusage();

		if (!is_array($usage) || !isset($usage['ru_utime.tv_sec'], $usage['ru_stime.tv_sec'])) {
			return null;
		}

		return $usage['ru_utime.tv_sec'] + $usage['ru_utime.tv_usec'] / 1e6
			+ $usage['ru_stime.tv_sec'] + $usage['ru_stime.tv_usec'] / 1e6;
	};

	$before = $cpu();
	$start = microtime(true);

	while (microtime(true) - $start < 0.05) {
	}

	$wall = microtime(true) - $start;
	$after = $cpu();

	if ($before === null || $after === null || $wall <= 0.0) {
		return 0.25;
	}

	$efficiency = ($after - $before) / $wall;

	// A ratio outside (0, 1] means the clock is not telling us anything usable.
	return ($efficiency > 0.0 && $efficiency <= 1.0) ? $efficiency : 0.25;
}

/*
 * Derived, not chosen. Floored so a pathological reading cannot produce a
 * ceiling too short to ever finish, and capped so one wedged row fails inside
 * the harness timeout instead of hanging it. Stays under WALL_BACKSTOP_SECONDS
 * so a burn running long is an ordinary outcome, never a backstop trip reported
 * as the wrong exception class.
 */
define('BURN_CEILING_SECONDS', min(BURN_SECONDS / max(measureCpuEfficiency(), 0.10) * 1.5, 2.0));

function burn(float $seconds): void
{
	global $sandbox;

	/*
	 * Spends CPU, and measures the spending with getCpuUsage() -- which is the
	 * exact quantity the limit enforces -- rather than with a wall clock.
	 *
	 * A wall-clock loop looks equivalent and is not. On a contended runner a
	 * thread can pass a 0.20s wall deadline having been descheduled for most of
	 * it, spending far less than 0.20s of CPU, so a row that means "burn twice
	 * the limit" quietly burns half of it and the budget never runs out. That is
	 * not flakiness in the limit; it is the test failing to spend what it claims.
	 *
	 * Three ways out, and the order matters:
	 *   1. the CPU target is reached -- the only one that means the burn worked;
	 *   2. the counter has not moved at all by the end of the probe, so this
	 *      frame is paused and never will move;
	 *   3. the ceiling, which is starvation and is reported by the caller.
	 */
	$start = $sandbox->getCpuUsage();
	$target = $start + $seconds;
	$probeUntil = microtime(true) + BURN_PROBE_SECONDS;
	$ceiling = microtime(true) + BURN_CEILING_SECONDS;

	while (true) {
		$spent = $sandbox->getCpuUsage();

		if ($spent >= $target) {
			return;
		}

		$now = microtime(true);

		if ($now >= $ceiling) {
			return;
		}

		if ($now >= $probeUntil && $spent <= $start) {
			return;
		}
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

	// The two clocks the Lua side burns against; see below.
	'cpu' => static function (): float {
		global $sandbox;

		return $sandbox->getCpuUsage();
	},

	'now' => static fn (): float => microtime(true),
];

/* -------------------------------------------------------------------------
 * The Lua side
 * ---------------------------------------------------------------------- */

$script = <<<'LUA'
	lua = {}

	-- Billed CPU, which is exactly the quantity the limit enforces. os.clock
	-- reports it directly, but it needs the osTime capability and these
	-- sandboxes are the untrusted default, so php.cpu() -- getCpuUsage() across
	-- the boundary -- is what actually runs here. Both report the same counter.
	--
	-- Deliberately NOT a wall clock: see the note on the PHP burn() above. A
	-- descheduled thread passes a wall deadline without having spent the budget,
	-- which turns "burn twice the limit" into a row that never trips.
	local function clock()
		if os ~= nil and os.clock ~= nil then
			return os.clock()
		end

		return php.cpu()
	end

	-- The same three exits as the PHP burn(), for the same reasons: reaching the
	-- CPU target, discovering on a short probe that this frame is not billed at
	-- all, or giving up at the ceiling. See burn() in the PHP half.
	local function burn(seconds)
		local start = clock()
		local target = start + seconds
		local probe_until = php.now() + PROBE
		local ceiling = php.now() + CEILING

		while true do
			local spent = clock()

			if spent >= target then return end

			local now = php.now()

			if now >= ceiling then return end
			if now >= probe_until and spent <= start then return end
		end
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

// Longest token first. None of these is a substring of another today, but doing
// it in this order is the habit that stops it being a bug the day one is.
$script = str_replace('CEILING', (string) BURN_CEILING_SECONDS, $script);
$script = str_replace('PROBE', (string) BURN_PROBE_SECONDS, $script);
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
	$before = $sandbox->getCpuUsage();

	try {
		(void) $sandbox->call($path, ...$args);
	} catch (CpuLimitError) {
		$outcome = 'yes';
	} catch (Throwable $error) {
		$outcome = 'WRONG CLASS ' . $error::class;
	}

	/*
	 * Separates the two ways a row can fail to trip, which otherwise print
	 * identically and are the reason this file was previously undiagnosable from
	 * its own diff:
	 *
	 *   no        the pause held and nothing was billed -- what the paused rows
	 *             are asserting, and a pass for them
	 *   no/short  CPU WAS billed but never reached the limit, i.e. the burn was
	 *             starved by a slow or contended runner
	 *
	 * A row expecting `yes` that prints `no/short` is a test-environment problem.
	 * One that prints plain `no` is an accounting bug -- billing genuinely
	 * stopped. Worth the extra column: telling those apart previously cost an
	 * artifact download and a guess.
	 */
	if ($outcome === 'no' && $sandbox->getCpuUsage() - $before >= CPU_SECONDS * 0.25) {
		$outcome = 'no/short';
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
