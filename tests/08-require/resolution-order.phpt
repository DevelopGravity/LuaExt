--TEST--
require() consults preload, then the VFS, then the resolver, and caches the result
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The order is the specification, and the only way to test an order is to make
// more than one step able to answer and see which one does. Every module below
// is available from the resolver as well, so a resolver that gets asked at all
// for `preloaded` or `vendored` means an earlier step was skipped.

final class RecordingResolver implements ModuleResolver
{
	/** @var list<string> */
	public array $asked = [];

	public function resolve(string $module, string $requestedBy): ?ModuleSource
	{
		$this->asked[] = $module;

		return new ModuleSource(
			sprintf('return { who = "resolver:%s" }', $module),
			'@resolver/' . $module,
		);
	}
}

$filesystem = new MemoryFileSystem([
	'/preloaded.lua' => 'return { who = "vfs" }',
	'/vendored.lua' => 'return { who = "vfs" }',
	'/pkg/init.lua' => 'return { who = "vfs-init" }',
]);

$resolver = new RecordingResolver();

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true, vfs: true),
	filesystem: $filesystem,
	moduleResolver: $resolver,
));

// Also present in the VFS and at the resolver; preload must win.
$sandbox->preloadModule('preloaded', static fn (): array => ['who' => 'preload']);

foreach (['preloaded', 'vendored', 'pkg', 'only-at-resolver'] as $module) {
	printf("%-16s %s\n", $module, $sandbox->eval(
		sprintf('local m = require("%s") return m.who', $module),
		'=require',
	)[0]);
}

// Asked for exactly the one nothing earlier could answer.
printf("resolver asked: %s\n", implode(', ', $resolver->asked));

// Cached: a second require returns the same table rather than reloading.
var_dump($sandbox->eval('return require("vendored") == require("vendored")', '=require')[0]);

// package carries three names and no loader toolkit. cpath, searchers and
// loadlib all exist upstream to reach a shared object, and a sandbox that can
// dlopen has no boundary left.
printf("package: %s\n", $sandbox->eval(
	'local names = {} for name in pairs(package) do names[#names + 1] = name end
	table.sort(names) return table.concat(names, ",")',
	'=require',
)[0]);

printf("path: %s\n", $sandbox->eval('return package.path', '=require')[0]);

$sandbox->close();

// Without the capability, require and package are absent rather than
// present-and-failing -- a script can test for them.
$plain = new Sandbox();
printf("ungranted: require=%s package=%s\n", ...$plain->eval('return type(require), type(package)'));
$plain->close();

?>
--EXPECT--
preloaded        preload
vendored         vfs
pkg              vfs-init
only-at-resolver resolver:only-at-resolver
resolver asked: only-at-resolver
bool(true)
package: loaded,path,preload
path: /?.lua;/?/init.lua
ungranted: require=nil package=nil
