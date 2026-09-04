<?php

declare(strict_types=1);

/**
 * bench-matrix.php -- what the sandbox costs across the configurations a host
 * can actually choose.
 *
 * WHY THIS EXISTS ALONGSIDE bench-vm.sh. That script answers one question very
 * well: what the patched interpreter costs against stock Lua, by building the
 * same tree three ways. It says nothing about the rest of the configuration
 * space, and that space has grown -- a compile cache, two shapes of filesystem
 * backend, three output modes, an opt-in profiler. docs/performance.md reported
 * a single headline per feature, which describes the default and nothing else.
 *
 * So this measures ONE workload per axis, varying only that axis, and prints a
 * table a host can find its own configuration in.
 *
 * WHAT IT DELIBERATELY DOES NOT DO. It does not compare against stock Lua --
 * bench-vm.sh owns that question and does it better. It does not run under CI:
 * a shared runner measures its neighbours as much as it measures this
 * extension, which is the same reason the memory figures are not a .phpt.
 *
 * Usage:
 *     php -d extension=modules/luaext.so tools/bench-matrix.php [repeats]
 */

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\FileSystem;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\RangedFileSystem;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

if (!extension_loaded('luaext')) {
	fwrite(STDERR, "luaext is not loaded; run with -d extension=modules/luaext.so\n");
	exit(1);
}

/** Best-of-N, because the interesting number is the machine at its least contended. */
const DEFAULT_REPEATS = 7;

$repeats = (int) ($argv[1] ?? DEFAULT_REPEATS);

/**
 * Time one closure, best of $repeats, returning seconds per iteration.
 *
 * Best-of rather than mean: a slow round is contention, and averaging it in
 * measures the neighbours. bench-vm.sh takes the same view.
 */
function bestOf(int $repeats, int $iterations, callable $body): float
{
	$best = INF;

	for ($round = 0; $round < $repeats; $round++) {
		$start = hrtime(true);
		$body($iterations);
		$elapsed = (hrtime(true) - $start) / 1e9;

		if ($elapsed < $best) {
			$best = $elapsed;
		}
	}

	return $best / $iterations;
}

function micros(float $seconds): string
{
	return sprintf('%.1f µs', $seconds * 1e6);
}

function ratio(float $baseline, float $other): string
{
	if ($baseline <= 0.0) {
		return '--';
	}

	return sprintf('%.2f×', $other / $baseline);
}

/** An in-memory backend, whole-file. The same shape tests/06-vfs uses. */
final class BenchFileSystem implements FileSystem
{
	/** @var array<string, string> */
	public array $files = [];

	public function exists(string $path): bool { return isset($this->files[$path]); }
	public function stat(string $path): ?FileStat
	{
		return isset($this->files[$path]) ? new FileStat(strlen($this->files[$path]), 0) : null;
	}
	public function read(string $path): string { return $this->files[$path] ?? ''; }
	public function write(string $path, string $contents): void { $this->files[$path] = $contents; }
	public function delete(string $path): void { unset($this->files[$path]); }
	public function rename(string $from, string $to): void
	{
		$this->files[$to] = $this->files[$from];
		unset($this->files[$from]);
	}
	public function list(string $path): array { return array_keys($this->files); }
}

/** The same store, reached through the streaming interface instead. */
final class BenchRangedFileSystem implements RangedFileSystem
{
	/** @var array<string, string> */
	public array $files = [];

	public function exists(string $path): bool { return isset($this->files[$path]); }
	public function stat(string $path): ?FileStat
	{
		return isset($this->files[$path]) ? new FileStat(strlen($this->files[$path]), 0) : null;
	}
	public function read(string $path): string { return $this->files[$path] ?? ''; }
	public function write(string $path, string $contents): void { $this->files[$path] = $contents; }
	public function delete(string $path): void { unset($this->files[$path]); }
	public function rename(string $from, string $to): void
	{
		$this->files[$to] = $this->files[$from];
		unset($this->files[$from]);
	}
	public function list(string $path): array { return array_keys($this->files); }

	public function readRange(string $path, int $offset, int $length): string
	{
		return substr($this->files[$path] ?? '', $offset, $length);
	}
	public function writeRange(string $path, int $offset, string $data): void
	{
		$existing = $this->files[$path] ?? '';

		if (strlen($existing) < $offset) {
			$existing = str_pad($existing, $offset, "\0");
		}

		$this->files[$path] = substr_replace($existing, $data, $offset, strlen($data));
	}
	public function truncate(string $path, int $size): void
	{
		$this->files[$path] = substr($this->files[$path] ?? '', 0, $size);
	}
}

/* -------------------------------------------------------------------------
 * The workloads
 *
 * Each is a fixed Lua source, sized so one call is long enough to measure
 * against a ~30 ns clock and short enough that a table of them finishes.
 * ---------------------------------------------------------------------- */

/** ~3.5 KB, the size docs/performance.md already uses for the cache figure. */
$SIXTY_FUNCTIONS = (static function (): string {
	$parts = [];

	for ($index = 1; $index <= 60; $index++) {
		$parts[] = "local function helper{$index}(value) return value + {$index} end";
	}

	$parts[] = 'return helper1(1)';

	return implode("\n", $parts);
})();

$TINY = 'return 1';
$SMALL_LOOP = 'local total = 0 for index = 1, 20 do total = total + index end return total';

$COMPUTE = <<<'LUA'
	local total = 0
	for index = 1, 2000 do
		total = total + index * 0.5
	end
	local words = {}
	for index = 1, 100 do
		words[index] = ('item' .. index):upper()
	end
	return total + #table.concat(words, ',')
	LUA;

$OUTPUT_HEAVY = <<<'LUA'
	for index = 1, 500 do
		io.write('line ', index, '\n')
	end
	return true
	LUA;

$VFS_HEAVY = <<<'LUA'
	local handle = assert(io.open('/bench.txt', 'w'))
	for index = 1, 40 do
		handle:write('record ', index, '\n')
	end
	handle:close()

	local total = 0
	for line in io.lines('/bench.txt') do
		total = total + #line
	end
	return total
	LUA;

printf("LuaExt configuration matrix\n");
printf("%s · PHP %s %s · best of %d\n\n", php_uname('m'), PHP_VERSION, PHP_ZTS ? 'ZTS' : 'NTS', $repeats);

/* -------------------------------------------------------------------------
 * 1. The compile cache, across chunk sizes
 * ---------------------------------------------------------------------- */

printf("## eval() compile cache\n\n");
printf("| Chunk | Cache off | Cache on | |\n|---|---:|---:|---:|\n");

foreach ([
	'`return 1` (8 B)' => $TINY,
	'small loop (42 B)' => $SMALL_LOOP,
	'compute (~250 B)' => $COMPUTE,
	'60 functions (3.5 KB)' => $SIXTY_FUNCTIONS,
] as $label => $source) {
	$measure = static function (bool $cached) use ($repeats, $source): float {
		$sandbox = new Sandbox(new SandboxConfig(cacheCompiledChunks: $cached));
		(void) $sandbox->eval($source, '=bench');   // warm the cache

		$seconds = bestOf($repeats, 200, static function (int $n) use ($sandbox, $source): void {
			for ($i = 0; $i < $n; $i++) {
				(void) $sandbox->eval($source, '=bench');
			}
		});

		$sandbox->close();

		return $seconds;
	};

	$off = $measure(false);
	$on = $measure(true);

	printf("| %s | %s | %s | **%s** |\n", $label, micros($off), micros($on), ratio($on, $off));
}

/* -------------------------------------------------------------------------
 * 2. The profiler
 * ---------------------------------------------------------------------- */

printf("\n## Profiler\n\n");
printf("| Workload | Off | On | |\n|---|---:|---:|---:|\n");

// The profiler's cost is per INSTRUCTION DISPATCHED, so the workloads here have
// to dispatch. $SIXTY_FUNCTIONS is the wrong shape for this table -- compiled
// once and then calling one helper, it executes almost nothing and reports a
// ratio of ~1.0 that says nothing about sampling.
$CALL_HEAVY = <<<'LUA'
	local function inner(value) return value + 1 end
	local total = 0
	for index = 1, 2000 do
		total = inner(total)
	end
	return total
	LUA;

$LOOP_HEAVY = <<<'LUA'
	local total = 0
	local index = 0
	while index < 5000 do
		index = index + 1
		total = total + index
	end
	return total
	LUA;

foreach ([
	'tight `while` loop' => $LOOP_HEAVY,
	'function calls' => $CALL_HEAVY,
	'mixed compute' => $COMPUTE,
] as $label => $source) {
	$measure = static function (bool $profiling) use ($repeats, $source): float {
		$sandbox = new Sandbox(new SandboxConfig(
			limits: new Limits(cpuSeconds: 30.0, wallClockSeconds: 60.0),
		));

		if ($profiling) {
			$sandbox->enableProfiler(0.002);
		}

		$chunk = $sandbox->compile($source, '=bench');

		$seconds = bestOf($repeats, 200, static function (int $n) use ($chunk): void {
			for ($i = 0; $i < $n; $i++) {
				(void) $chunk->call();
			}
		});

		$sandbox->close();

		return $seconds;
	};

	$off = $measure(false);
	$on = $measure(true);

	printf("| %s | %s | %s | **%s** |\n", $label, micros($off), micros($on), ratio($off, $on));
}

/* -------------------------------------------------------------------------
 * 3. Output mode
 * ---------------------------------------------------------------------- */

printf("\n## Output mode (500 io.write calls per iteration)\n\n");
printf("| Mode | Per call | |\n|---|---:|---:|\n");

$outputBaseline = null;

foreach ([
	'Buffer' => [OutputMode::Buffer, null],
	'Discard' => [OutputMode::Discard, null],
	'Callback' => [OutputMode::Callback, static function (string $chunk): void { /* counted, not kept */ }],
] as $label => [$mode, $callback]) {
	$sandbox = new Sandbox(new SandboxConfig(
		limits: new Limits(outputBytes: 0),
		outputMode: $mode,
		outputCallback: $callback,
	));

	$chunk = $sandbox->compile($OUTPUT_HEAVY, '=bench');

	$seconds = bestOf($repeats, 50, static function (int $n) use ($chunk, $sandbox, $mode): void {
		for ($i = 0; $i < $n; $i++) {
			(void) $chunk->call();

			if ($mode === OutputMode::Buffer) {
				(void) $sandbox->takeOutput();
			}
		}
	});

	$sandbox->close();

	$outputBaseline ??= $seconds;

	printf("| %s | %s | %s |\n", $label, micros($seconds), ratio($outputBaseline, $seconds));
}

/* -------------------------------------------------------------------------
 * 4. Filesystem backend shape
 * ---------------------------------------------------------------------- */

printf("\n## Filesystem backend (40 writes + a full read-back per iteration)\n\n");
printf("| Backend | Per call | |\n|---|---:|---:|\n");

$vfsBaseline = null;

foreach ([
	'FileSystem (buffered)' => static fn (): object => new BenchFileSystem(),
	'RangedFileSystem (streamed)' => static fn (): object => new BenchRangedFileSystem(),
] as $label => $make) {
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
		filesystem: $make(),
	));

	$chunk = $sandbox->compile($VFS_HEAVY, '=bench');

	$seconds = bestOf($repeats, 50, static function (int $n) use ($chunk): void {
		for ($i = 0; $i < $n; $i++) {
			(void) $chunk->call();
		}
	});

	$sandbox->close();

	$vfsBaseline ??= $seconds;

	printf("| %s | %s | %s |\n", $label, micros($seconds), ratio($vfsBaseline, $seconds));
}

/* -------------------------------------------------------------------------
 * 5. What a sandbox costs before it runs anything
 * ---------------------------------------------------------------------- */

printf("\n## Sandbox lifecycle\n\n");
printf("| Step | Cost |\n|---|---:|\n");

$construct = bestOf($repeats, 300, static function (int $n): void {
	for ($i = 0; $i < $n; $i++) {
		(new Sandbox())->close();
	}
});

printf("| construct + close (no script) | %s |\n", micros($construct));

$withVfs = bestOf($repeats, 300, static function (int $n): void {
	for ($i = 0; $i < $n; $i++) {
		(new Sandbox(new SandboxConfig(
			capabilities: (new Capabilities())->with(vfs: true),
			filesystem: new BenchFileSystem(),
		)))->close();
	}
});

printf("| construct + close (vfs granted) | %s |\n", micros($withVfs));

$perRequest = bestOf($repeats, 200, static function (int $n) use ($COMPUTE): void {
	for ($i = 0; $i < $n; $i++) {
		$sandbox = new Sandbox();
		(void) $sandbox->eval($COMPUTE, '=bench');
		$sandbox->close();
	}
});

printf("| construct + eval + close | %s |\n", micros($perRequest));
printf("\n");
