--TEST--
SandboxConfig::$classes shape is refused identically by __construct and with()
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\SandboxConfig;

// The constructor's refusal, both malformed shapes.
foreach ([[123], ['']] as $index => $malformed) {
	try {
		new SandboxConfig(classes: $malformed);
		echo "construct {$index}: NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo "construct {$index}: ", $error->getMessage(), "\n";
	}
}

// with() must not be a path around it: deriving a malformed list is refused
// at derivation time, before any Sandbox constructor could meet it.
$config = new SandboxConfig(classes: ['App\\NotLoadedYet']);

foreach ([[123], ['']] as $index => $malformed) {
	try {
		$config->with(classes: $malformed);
		echo "with {$index}: NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo "with {$index}: ", $error->getMessage(), "\n";
	}
}

?>
--EXPECT--
construct 0: SandboxConfig::$classes must hold non-empty class-name strings
construct 1: SandboxConfig::$classes must hold non-empty class-name strings
with 0: SandboxConfig::$classes must hold non-empty class-name strings
with 1: SandboxConfig::$classes must hold non-empty class-name strings
