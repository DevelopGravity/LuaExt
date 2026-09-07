--TEST--
Two sandboxes over one FileSystem and ModuleResolver keep independent module caches and counters
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A shared host resource is the one channel two sandboxes have in common,
// and the concrete bleed it could introduce is a shared require() cache: a
// module table loaded in one sandbox surfacing in the other, or one
// sandbox's loads spending the other's Limits::$maxModules. Nothing pinned
// that down until now.
final class TalliedResolver implements ModuleResolver
{
	/** @var array<string, int> */
	public array $asked = [];

	public function resolve(string $module, string $requestedBy): ?ModuleSource
	{
		$this->asked[$module] = ($this->asked[$module] ?? 0) + 1;

		return new ModuleSource('return { stamp = {} }', '@resolver/' . $module);
	}
}

$filesystem = new MemoryFileSystem(['/shared.lua' => 'return { stamp = {} }']);
$resolver = new TalliedResolver();

$make = static function () use ($filesystem, $resolver): Sandbox {
	return new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(require: true, vfs: true),
		limits: new Limits(maxModules: 2),
		filesystem: $filesystem,
		moduleResolver: $resolver,
	));
};

$first = $make();
$second = $make();

// Within one sandbox the module is one table; across sandboxes it must not
// be. A shared table would be shared mutable state between two scripts that
// are supposed to be isolated from each other.
$first->setGlobal('mine', []);
$second->setGlobal('mine', []);

(void) $first->eval('local m = require("shared") mine.same = (m == require("shared")) m.mark = "first"', '=first');
(void) $second->eval('local m = require("shared") mine.leak = tostring(m.mark)', '=second');

var_dump($first->getGlobal('mine')['same'], $second->getGlobal('mine')['leak']);

// Each sandbox resolved through the backend for itself: two loads, not one
// globally deduplicated load.
(void) $first->eval('require("resolved")', '=r1');
(void) $second->eval('require("resolved")', '=r2');

var_dump($resolver->asked['resolved']);

// And the counters stayed per sandbox too.
var_dump($first->stats()->modulesLoaded, $second->stats()->modulesLoaded);

// maxModules is a per-sandbox budget: the first sandbox is at its cap of
// two, while the second -- same backend, same limits object -- still has
// room only for what IT loaded.
try {
	(void) $first->eval('require("third")', '=cap');
	echo "cap: NOT ENFORCED\n";
} catch (FatalError $error) {
	printf("cap: %s\n", $error->getMessage());
}

$first->close();
$second->close();

?>
--EXPECT--
bool(true)
string(3) "nil"
int(2)
int(2)
int(2)
cap: The sandbox has already loaded 2 module(s), which is its Limits::$maxModules
