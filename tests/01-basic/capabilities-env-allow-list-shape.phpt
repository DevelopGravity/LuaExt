--TEST--
Capabilities::$osEnvAllowList entries are refused at the source, construct and with() alike
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ConfigurationError;

$malformed = [
	'int-entry' => [123],
	'empty' => [''],
	'equals' => ['PATH=/tmp'],
	'nul' => ["HO\0ME"],
];

foreach ($malformed as $label => $list) {
	try {
		new Capabilities(osEnv: true, osEnvAllowList: $list);
		echo "construct {$label}: NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo "construct {$label}: ", $error->getMessage(), "\n";
	}
}

// with() must not be a path around the constructor's rule.
$capabilities = new Capabilities(osEnv: true, osEnvAllowList: ['PATH']);

foreach ($malformed as $label => $list) {
	try {
		$capabilities->with(osEnvAllowList: $list);
		echo "with {$label}: NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo "with {$label}: ", $error->getMessage(), "\n";
	}
}

// A well-formed list passes both routes.
var_dump($capabilities->with(osEnvAllowList: ['HOME', 'PATH'])->osEnvAllowList);

?>
--EXPECT--
construct int-entry: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
construct empty: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
construct equals: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
construct nul: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
with int-entry: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
with empty: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
with equals: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
with nul: Capabilities::$osEnvAllowList must hold non-empty environment variable names without NUL or '=' bytes
array(2) {
  [0]=>
  string(4) "HOME"
  [1]=>
  string(4) "PATH"
}
