--TEST--
registerObject() exposes methods carrying #[LuaMethod], under the name it asks for
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Catalogue
{
	#[LuaMethod]
	public function lookup(string $sku): string
	{
		return "item:{$sku}";
	}

	// The Lua name and the PHP name are allowed to differ, so a host can keep
	// its own naming conventions without leaking them into scripts.
	#[LuaMethod('search')]
	public function findByKeyword(string $keyword): array
	{
		return [$keyword, 'match'];
	}

	// No attribute: invisible, so adding a public method here can never quietly
	// widen what untrusted code may call.
	public function purge(): void
	{
	}
}

$sandbox = new Sandbox();
$sandbox->registerObject('catalogue', new Catalogue());

var_dump($sandbox->eval('return catalogue.lookup("a1")'));

// Renamed means renamed: the PHP name is not also exposed as an alias.
var_dump($sandbox->eval(
	'return type(catalogue.search), type(catalogue.findByKeyword), type(catalogue.purge)'
));

// A returned PHP list keeps its own 0-based keys rather than being renumbered;
// see 09-conversion/array-key-mapping.phpt.
var_dump($sandbox->eval('local r = catalogue.search("axle") return r[0], r[1]'));

// An allowlist overrides the attributes entirely, renames included: what the
// host wrote in the allowlist is what a script sees.
$explicit = new Sandbox();
$explicit->registerObject('catalogue', new Catalogue(), ['findByKeyword', 'purge']);

var_dump($explicit->eval(
	'return type(catalogue.findByKeyword), type(catalogue.purge), '
	. 'type(catalogue.search), type(catalogue.lookup)'
));

// Neither an attribute nor an allowlist selects anything. An empty table would
// be a working registration that exposes nothing, which is far likelier to be a
// forgotten attribute than an intention.
final class Unmarked
{
	public function anything(): string
	{
		return 'anything';
	}
}

try {
	(new Sandbox())->registerObject('nothing', new Unmarked());
	echo "EXPOSED\n";
} catch (ConfigurationError $error) {
	echo "refused: selection is always explicit\n";
}

// Two methods cannot both claim one Lua name; one of them would otherwise win
// silently and the host would never learn which.
final class Ambiguous
{
	#[LuaMethod('run')]
	public function runFast(): string
	{
		return 'fast';
	}

	#[LuaMethod('run')]
	public function runSlow(): string
	{
		return 'slow';
	}
}

try {
	(new Sandbox())->registerObject('ambiguous', new Ambiguous());
	echo "EXPOSED\n";
} catch (ConfigurationError $error) {
	echo "refused: a Lua name belongs to one method\n";
}

$explicit->close();
$sandbox->close();

?>
--EXPECT--
array(1) {
  [0]=>
  string(7) "item:a1"
}
array(3) {
  [0]=>
  string(8) "function"
  [1]=>
  string(3) "nil"
  [2]=>
  string(3) "nil"
}
array(2) {
  [0]=>
  string(4) "axle"
  [1]=>
  string(5) "match"
}
array(4) {
  [0]=>
  string(8) "function"
  [1]=>
  string(8) "function"
  [2]=>
  string(3) "nil"
  [3]=>
  string(3) "nil"
}
refused: selection is always explicit
refused: a Lua name belongs to one method
