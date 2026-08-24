--TEST--
The standard library a sandbox sees is an allow list, not upstream minus four names
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Every table a script can reach is built here out of an allow list, so this is
// the whole surface -- adding a member to it is a deliberate act, and a member
// a future Lua release adds fails the drift check at construction instead of
// appearing in every untrusted sandbox unannounced.

$members = static function (Sandbox $sandbox, string $table): string {
	return $sandbox->eval(sprintf(
		'local names = {} for name in pairs(%s) do names[#names + 1] = name end
		table.sort(names) return table.concat(names, " ")',
		$table,
	))[0];
};

$sandbox = new Sandbox();

foreach (['_G', 'string', 'table', 'math', 'utf8'] as $table) {
	printf("%s: %s\n", $table, $members($sandbox, $table));
}

$sandbox->close();

// The two file-opening members of the base library are withheld at every
// capability level: the sandbox's only filesystem is the VFS, and these two open
// real paths by name.
$presets = [
	'untrusted' => new Capabilities(),
	'compileAtRuntime' => (new Capabilities())->with(compileAtRuntime: true),
	'warn' => (new Capabilities())->with(warn: true),
	'both' => (new Capabilities())->with(compileAtRuntime: true, warn: true),
];

foreach ($presets as $label => $capabilities) {
	$sandbox = new Sandbox(new SandboxConfig(capabilities: $capabilities));

	printf(
		"%-16s load=%s warn=%s dofile=%s loadfile=%s\n",
		$label,
		...$sandbox->eval('return type(load), type(warn), type(dofile), type(loadfile)'),
	);

	$sandbox->close();
}

// print composes one write out of all its arguments and answers with nothing,
// and warn is a string-only front end onto the same sink. Where those bytes end
// up is the output subsystem's contract, not this one's; what is pinned here is
// that both are the sandbox's own functions and neither returns a value.
$sandbox = new Sandbox(new SandboxConfig(capabilities: (new Capabilities())->with(warn: true)));

var_dump($sandbox->eval('return select("#", print("a", 1, nil, true, {}))'));
var_dump($sandbox->eval('return select("#", warn("something", " happened"))'));

// A control message in Lua's sense -- upstream's warning function switches
// itself on and off with these. There is nothing here to switch, so it is
// dropped rather than emitted as though a script had asked to print it.
var_dump($sandbox->eval('return select("#", warn("@off"))'));

// Strings only, exactly as upstream: warn is not a second print.
var_dump($sandbox->eval('return pcall(warn, {})')[0]);

$sandbox->close();

?>
--EXPECT--
_G: _G _VERSION assert collectgarbage debug error getmetatable ipairs math next os pairs pcall print rawequal rawget rawlen rawset select setmetatable string table tonumber tostring type utf8 xpcall
string: byte char find format gmatch gsub len lower match pack packsize rep reverse sub unpack upper
table: concat create insert move pack remove sort unpack
math: abs acos asin atan ceil cos deg exp floor fmod frexp huge ldexp log max maxinteger min mininteger modf pi rad random randomseed sin sqrt tan tointeger type ult
utf8: char charpattern codepoint codes len offset
untrusted        load=nil warn=nil dofile=nil loadfile=nil
compileAtRuntime load=function warn=nil dofile=nil loadfile=nil
warn             load=nil warn=function dofile=nil loadfile=nil
both             load=function warn=function dofile=nil loadfile=nil
array(1) {
  [0]=>
  int(0)
}
array(1) {
  [0]=>
  int(0)
}
array(1) {
  [0]=>
  int(0)
}
bool(false)
