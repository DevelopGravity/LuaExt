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

// debug, os and io are enumerated too, or the claim above is false: all three
// are in the untrusted baseline (debugTraceback and osTime default on, os.clock
// and io's output half are unconditional), so leaving them out would let a
// member appear in every untrusted sandbox without this test noticing.
//
// io is the one to watch. Its output half needs no capability, and its
// filesystem half appears as GATE STUBS without vfs -- so the conflation
// signal is no longer io.open's name on this line (it is always there) but an
// io.open that RUNS: the classification rows below are what catch that.
foreach (['_G', 'string', 'table', 'math', 'utf8', 'debug', 'os', 'io'] as $table) {
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

// type() stopped distinguishing a grant from a gate stub, so each member is
// classified by CALLING it: "works", "gate", or "nil" (dofile/loadfile stay
// wholly absent -- no capability could ever grant them).
$classify = static function (Sandbox $sandbox, string $call): string {
	try {
		$kind = $sandbox->eval(
			sprintf('if %s == nil then return "nil" end %s return "works"', explode('(', $call)[0], $call),
			'=classify',
		)[0];

		return $kind;
	} catch (DevelopGravity\LuaExt\Exception\FeatureNotGrantedError) {
		return 'gate';
	} catch (Throwable) {
		return 'works';
	}
};

foreach ($presets as $label => $capabilities) {
	$sandbox = new Sandbox(new SandboxConfig(capabilities: $capabilities));

	printf(
		"%-16s load=%s warn=%s dofile=%s loadfile=%s\n",
		$label,
		$classify($sandbox, 'load("return 1")'),
		$classify($sandbox, 'warn("x")'),
		$classify($sandbox, 'dofile("/x")'),
		$classify($sandbox, 'loadfile("/x")'),
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
_G: _G _VERSION assert collectgarbage coroutine debug error getmetatable io ipairs load math next os pairs pcall print rawequal rawget rawlen rawset select setmetatable string table tonumber tostring type utf8 warn xpcall
string: byte char dump find format gmatch gsub len lower match pack packsize rep reverse sub unpack upper
table: concat create insert move pack remove sort unpack
math: abs acos asin atan ceil cos deg exp floor fmod frexp huge ldexp log max maxinteger min mininteger modf pi rad random randomseed sin sqrt tan tointeger type ult
utf8: char charpattern codepoint codes len offset
debug: gethook getinfo getlocal getmetatable getregistry getupvalue getuservalue sethook setlocal setmetatable setupvalue setuservalue traceback upvalueid upvaluejoin
os: clock date difftime getenv remove rename time
io: close lines open stderr stdout write
untrusted        load=gate warn=gate dofile=nil loadfile=nil
compileAtRuntime load=works warn=gate dofile=nil loadfile=nil
warn             load=gate warn=works dofile=nil loadfile=nil
both             load=works warn=works dofile=nil loadfile=nil
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
