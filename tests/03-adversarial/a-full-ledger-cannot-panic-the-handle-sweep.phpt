--TEST--
A memory ceiling met at call return cannot panic the handle sweep
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

// The backend squeezes the memory ceiling BELOW what is currently live before
// answering the big read — a state setLimits() documents as legal, where every
// later allocation is refused and no collection can help. The call aborts with
// a clean MemoryLimitError; what must NOT happen is the call-scope handle
// sweep, which runs after the protected call returned, making a raising
// allocation of its own: that raise has no handler and killed the whole
// request as an unprotected panic.
final class ThrottlingFileSystem implements RangedFileSystem
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
				memoryBytes: max(1, $this->sandbox->stats()->memoryBytes - 4096),
			));
		}

		return substr($this->payload, $offset, $length);
	}

	public function writeRange(string $path, int $offset, string $data): void {}

	public function truncate(string $path, int $size): void {}
}

$backend = new ThrottlingFileSystem(str_repeat('x', 1048576));
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: $backend,
));
$backend->sandbox = $sandbox;
$original = $sandbox->limits();

for ($iteration = 0; $iteration < 3; $iteration++) {
	try {
		(void) $sandbox->eval(<<<'LUA'
			local f = io.open('/big', 'r')
			f:read(4)
			f:seek('set', 0)
			return f:read(1048576)
		LUA);
		echo "iteration {$iteration}: NOT REFUSED\n";
	} catch (MemoryLimitError) {
		echo "iteration {$iteration}: refused\n";
	}

	$sandbox->setLimits($original);
}

// The sandbox survived three swept calls at a ceiling below its live set.
var_dump($sandbox->eval('return 21 * 2')[0]);

$sandbox->close();

?>
--EXPECT--
iteration 0: refused
iteration 1: refused
iteration 2: refused
int(42)
