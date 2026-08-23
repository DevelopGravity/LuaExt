--TEST--
A sandbox can never be constructed twice, open or closed
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;

$openSandbox = new Sandbox();

try {
	$openSandbox->__construct();
	echo "open: not rejected\n";
} catch (ConfigurationError $configurationError) {
	echo "open: ConfigurationError\n";
}

// Reconstructing a CLOSED sandbox is the dangerous case: it would create a
// second interpreter while the closed flag still short-circuits close(), so
// the new state could never be torn down and the freed object would stay
// linked into the per-thread live list the request-shutdown sweep walks.
$closedSandbox = new Sandbox();
$closedSandbox->close();

try {
	$closedSandbox->__construct();
	echo "closed: not rejected\n";
} catch (ConfigurationError $configurationError) {
	echo "closed: ConfigurationError\n";
}

var_dump($closedSandbox->isClosed());

?>
--EXPECT--
open: ConfigurationError
closed: ConfigurationError
bool(true)
