--TEST--
A backend reply refused at the memory ceiling is released, never stranded
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\RangedFileSystem;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A ranged backend that, on the big read only, shrinks the sandbox's memory
// ceiling to just above what is currently live before handing back its reply.
// Back in the extension, the refusal lands on the first allocation after the
// squeeze — the collector-owned box meant to anchor the reply, or the push of
// the reply's bytes right behind it — and either way the megabyte the frame
// owns must be released, never stranded behind the longjmp. The script warms
// the call shape and collects first so the emergency collection the allocator
// retries with finds as little as possible.
final class SqueezingFileSystem implements RangedFileSystem
{
	public ?Sandbox $sandbox = null;

	public function __construct(private readonly string $payload) {}

	public function exists(string $path): bool { return true; }

	public function stat(string $path): ?FileStat { return new FileStat(strlen($this->payload), 0); }

	public function read(string $path): string { return $this->payload; }

	public function write(string $path, string $contents): void {}

	public function delete(string $path): void {}

	public function rename(string $from, string $to): void {}

	/** @return list<string> */
	public function list(string $path): array { return []; }

	public function readRange(string $path, int $offset, int $length): string
	{
		if ($length > 4 && $this->sandbox !== null) {
			$this->sandbox->setLimits($this->sandbox->limits()->with(
				memoryBytes: $this->sandbox->stats()->memoryBytes + 16,
			));
		}

		return substr($this->payload, $offset, $length);
	}

	public function writeRange(string $path, int $offset, string $data): void {}

	public function truncate(string $path, int $size): void {}
}

$backend = new SqueezingFileSystem(str_repeat('x', 1048576));
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: $backend,
));
$backend->sandbox = $sandbox;
$original = $sandbox->limits();

$before = memory_get_usage();

for ($iteration = 0; $iteration < 8; $iteration++) {
	try {
		(void) $sandbox->eval(<<<'LUA'
			local f = io.open('/big', 'r')
			f:read(4)
			f:seek('set', 0)
			collectgarbage('collect')
			return f:read(1048576)
		LUA);
		echo "iteration {$iteration}: NOT REFUSED\n";
	} catch (MemoryLimitError) {
		echo "iteration {$iteration}: refused\n";
	}

	$sandbox->setLimits($original);
}

// Eight stranded replies would be eight megabytes; a fixed build holds the
// request arena flat to within noise.
var_dump(memory_get_usage() - $before < 2 * 1048576);

$sandbox->close();

?>
--EXPECT--
iteration 0: refused
iteration 1: refused
iteration 2: refused
iteration 3: refused
iteration 4: refused
iteration 5: refused
iteration 6: refused
iteration 7: refused
bool(true)
