--TEST--
Every script-reachable VFS entry point refuses a path that climbs, hides a NUL, or names nothing
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\VfsError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The canonicaliser's central rule -- a '..' that would pop past the root is
// an ERROR, never a silent clamp -- had unit-level coverage but no test that
// drove it end-to-end through anything a script can actually call. This does,
// through every entry point, and uses the backend's own call recorder to
// prove a refused path never reaches host code at all.

$filesystem = new MemoryFileSystem(['/f.txt' => 'payload']);

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: $filesystem,
));

$attacks = [
	'io.open climbing'    => 'io.open("../x", "r")',
	'io.lines climbing'   => 'io.lines("/a/../../x")',
	'os.remove climbing'  => 'os.remove("/..")',
	'os.rename climbing'  => 'os.rename("a/./../../x", "/y")',
	'embedded NUL'        => 'io.open("/a\0b", "r")',
	'dots and separators' => 'io.open("/./.", "r")',
];

foreach ($attacks as $label => $attack) {
	try {
		(void) $sandbox->eval($attack, '=attack');
		printf("%-19s REACHED THE BACKEND\n", $label);
	} catch (VfsError $error) {
		printf("%-19s %s\n", $label, $error->getMessage());
	}
}

printf("backend calls after every refusal: %d\n", count($filesystem->calls));

// The refusal is catchable from inside the script too -- an escape attempt is
// a script error, not a sandbox-fatal -- and pcall gets the same wording.
[$caught] = $sandbox->eval('local ok, err = pcall(io.open, "../x", "r") return tostring(err)', '=pcall');
printf("pcall sees: %s\n", preg_replace('/^.*: This path/', 'This path', $caught));

// What DOES reach the backend arrives canonical: rooted, no '.', no '..'.
[$payload] = $sandbox->eval('local f = io.open("valid/../f.txt", "r") return f:read("a")', '=ok');
printf("accepted read returned %s via: %s\n", $payload, implode(', ', $filesystem->calls));

$sandbox->close();

?>
--EXPECT--
io.open climbing    This path cannot be used: climbs above the root with '..'
io.lines climbing   This path cannot be used: climbs above the root with '..'
os.remove climbing  This path cannot be used: climbs above the root with '..'
os.rename climbing  This path cannot be used: climbs above the root with '..'
embedded NUL        This path cannot be used: contains a NUL byte
dots and separators This path cannot be used: names nothing once '.' and separators are resolved
backend calls after every refusal: 0
pcall sees: This path cannot be used: climbs above the root with '..'
accepted read returned payload via: exists(/f.txt), read(/f.txt)
