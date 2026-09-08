--TEST--
A backend that closes the handle mid-operation is refused at the next step
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\RangedFileSystem;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A backend may re-enter the sandbox — eval() is not refused mid-call — and
// nothing stops what it runs from closing the very handle whose operation is
// in flight. Multi-step operations cross into the host once per argument or
// format, so the steps after the betrayal must see the closed handle and
// refuse it, never dereference the NULLs its release left behind.
final class SelfClosingFileSystem implements RangedFileSystem
{
	public ?Sandbox $sandbox = null;

	public string $data = '';

	private bool $armed = true;

	public function exists(string $path): bool { return true; }

	public function stat(string $path): ?FileStat { return new FileStat(strlen($this->data), 0); }

	public function read(string $path): string { return $this->data; }

	public function write(string $path, string $contents): void {}

	public function delete(string $path): void {}

	public function rename(string $from, string $to): void {}

	/** @return list<string> */
	public function list(string $path): array { return []; }

	public function readRange(string $path, int $offset, int $length): string
	{
		$this->betray();

		return substr($this->data, $offset, $length);
	}

	public function writeRange(string $path, int $offset, string $data): void
	{
		$this->betray();
	}

	public function truncate(string $path, int $size): void {}

	public function arm(): void
	{
		$this->armed = true;
	}

	private function betray(): void
	{
		if ($this->armed && $this->sandbox !== null) {
			$this->armed = false;
			(void) $this->sandbox->eval('victim:close()');
		}
	}
}

$backend = new SelfClosingFileSystem();
$backend->data = str_repeat('x', 64);
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: $backend,
));
$backend->sandbox = $sandbox;

// Two arguments, one crossing each: the handle dies inside the first, and the
// second must name the closed file.
var_dump($sandbox->eval(<<<'LUA'
	victim = io.open('/t', 'w')
	local ok, err = pcall(function() return victim:write('aaaa', 'bbbb') end)
	return ok, tostring(err):match('attempt to use a closed file') ~= nil
LUA));

// The same betrayal between two formats of one read.
$backend->arm();
var_dump($sandbox->eval(<<<'LUA'
	victim = io.open('/t', 'r')
	local ok, err = pcall(function() return victim:read(4, 4) end)
	return ok, tostring(err):match('attempt to use a closed file') ~= nil
LUA));

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(false)
  [1]=>
  bool(true)
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  bool(true)
}
