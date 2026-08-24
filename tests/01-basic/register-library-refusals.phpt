--TEST--
registerLibrary() refuses a malformed table before it builds anything in the interpreter
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;

// A registration either happens completely or not at all. The global is
// assigned last, so a refusal partway through leaves the interpreter without a
// half-built library in it for a script to find.

$refusals = [
	'no name' => ['', ['f' => 'strlen']],
	'no functions' => ['host', []],
	'an integer key' => ['host', [0 => 'strlen']],
	'an empty key' => ['host', ['' => 'strlen']],
	'a value that is not callable' => ['host', ['f' => 'no_such_function_at_all']],
	'one bad entry among good ones' => ['host', ['ok' => 'strlen', 'bad' => 42]],
];

foreach ($refusals as $label => [$name, $functions]) {
	$sandbox = new Sandbox();

	try {
		$sandbox->registerLibrary($name, $functions);
		printf("%-30s REGISTERED\n", $label);
	} catch (ConfigurationError $error) {
		printf("%-30s refused, host global is %s\n",
			$label, var_export($sandbox->eval('return type(host)')[0], true));
	}

	$sandbox->close();
}

// Two calls build one namespace rather than the second replacing the first.
$sandbox = new Sandbox();
$sandbox->registerLibrary('host', ['first' => static fn (): string => 'one']);
$sandbox->registerLibrary('host', ['second' => static fn (): string => 'two']);

var_dump($sandbox->eval('return host.first(), host.second()'));

$sandbox->close();

?>
--EXPECT--
no name                        refused, host global is 'nil'
no functions                   refused, host global is 'nil'
an integer key                 refused, host global is 'nil'
an empty key                   refused, host global is 'nil'
a value that is not callable   refused, host global is 'nil'
one bad entry among good ones  refused, host global is 'nil'
array(2) {
  [0]=>
  string(3) "one"
  [1]=>
  string(3) "two"
}
