--TEST--
The package table refuses every write to its own fields, while loaded entries stay writable
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The comment over this table promised a freeze; the metatable delivered only
// __metatable = false, so package.path = "..." and package.loaded = {}
// quietly succeeded. The visible `package` is now a proxy whose __newindex
// raises for every field -- __newindex on the real table would not have
// fired at all for the fields that already exist.
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true, vfs: true),
	filesystem: new MemoryFileSystem(['/mod.lua' => 'return { tag = "from vfs" }']),
));

[$report] = $sandbox->eval('
	local rows = {}

	local function attempt(label, write)
		local ok, err = pcall(write)
		rows[#rows + 1] = string.format("%s: %s", label, ok and "ALLOWED" or tostring(err))
	end

	attempt("path", function () package.path = "/evil/?.lua" end)
	attempt("loaded", function () package.loaded = {} end)
	attempt("preload", function () package.preload = {} end)
	attempt("searchers", function () package.searchers = {} end)

	-- Reads keep answering through the proxy...
	rows[#rows + 1] = "path reads: " .. tostring(package.path)

	-- ...and entries INSIDE loaded stay writable, which is the idiom modules
	-- themselves rely on.
	package.loaded["hand-made"] = { tag = "planted" }
	rows[#rows + 1] = "planted: " .. require("hand-made").tag

	-- The metatable stays out of reach, so the proxy cannot be re-pointed.
	rows[#rows + 1] = "metatable: " .. tostring(getmetatable(package))

	return table.concat(rows, "\n")
', '=frozen');

echo $report, "\n";

// The refusals changed nothing about resolution.
[$tag] = $sandbox->eval('return require("mod").tag', '=resolve');
var_dump($tag);

$sandbox->close();

?>
--EXPECT--
path: frozen:9: the package table is read-only in this sandbox
loaded: frozen:10: the package table is read-only in this sandbox
preload: frozen:11: the package table is read-only in this sandbox
searchers: frozen:12: the package table is read-only in this sandbox
path reads: /?.lua;/?/init.lua
planted: planted
metatable: false
string(8) "from vfs"
