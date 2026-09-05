--TEST--
preloadModule() refuses a LuaFunction that belongs to a different sandbox
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A LuaFunction is an index into ONE sandbox's registry table. Handed to a
// different sandbox it does not fail -- it reads that sandbox's slot of the
// same number, which is some unrelated function the host never offered, or
// nil. The conversion path has always refused a foreign handle; preloadModule()
// reached the same registry through a second door and did not.
//
// Both sandboxes below compile a function first, so each has a live slot 1.
// That is what makes the confusion observable rather than merely a nil: without
// the guard, preloading B's handle into A silently installs A's OWN function,
// and require('shared') answers "A" -- a function the caller never passed.

$config = static fn (): SandboxConfig => new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true),
);

$first = new Sandbox($config());
$second = new Sandbox($config());

// Slot 1 in each. Distinguishable so a mix-up is visible in the output.
$fromFirst = $first->compile('return function () return "first" end')->call()[0];
$fromSecond = $second->compile('return function () return "second" end')->call()[0];

try {
	$first->preloadModule('shared', $fromSecond);
	$outcome = 'ACCEPTED A FOREIGN HANDLE';
} catch (Throwable $error) {
	$outcome = $error::class . ': ' . $error->getMessage();
}

echo $outcome, "\n";

// The sandbox's own handle still works, so the guard refuses the foreign case
// rather than the feature.
$first->preloadModule('mine', $fromFirst);
echo "own handle preloads: ", $first->eval('return require("mine")', '=own')[0] ?? 'nothing', "\n";

// And nothing was installed under the refused name.
try {
	(void) $first->eval('return require("shared")', '=shared');
	echo "refused name resolved anyway\n";
} catch (Throwable $error) {
	echo "refused name stays unresolvable: yes\n";
}

$first->close();
$second->close();

?>
--EXPECT--
DevelopGravity\LuaExt\Exception\ConfigurationError: That LuaFunction belongs to a different sandbox
own handle preloads: first
refused name stays unresolvable: yes
