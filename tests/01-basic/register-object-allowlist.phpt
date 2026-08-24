--TEST--
registerObject() exposes exactly the methods an allowlist names and refuses the rest
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;

// Selection is explicit only. There is no "expose everything public" mode,
// because that is how a host accidentally hands a script its own internals the
// day someone adds a method to a class that happens to be registered.

final class TextService
{
	public function upper(string $text): string
	{
		return strtoupper($text);
	}

	public function lower(string $text): string
	{
		return strtolower($text);
	}

	public static function version(): string
	{
		return '1.0';
	}

	public function __toString(): string
	{
		return 'TextService';
	}

	private function secret(): string
	{
		return 'secret';
	}

	protected function hidden(): string
	{
		return 'hidden';
	}
}

$service = new TextService();

$sandbox = new Sandbox();
$sandbox->registerObject('text', $service, ['upper']);

// Only what the allowlist named. A method the host did not choose is not
// merely unreachable, it is absent.
var_dump($sandbox->eval(
	'return text.upper("hi there"), type(text.lower), type(text.secret), type(text.version)'
));

// The table holds bound closures and nothing else -- no properties, and nothing
// that leads back to the instance.
var_dump($sandbox->eval(<<<'LUA'
	local names = 0

	for _, value in pairs(text) do
		names = names + 1
		assert(type(value) == "function")
	end

	return names
LUA));

// Everything a host might reach for by mistake is refused at registration time,
// where it is still cheap to notice, rather than silently skipped.
$refusals = [
	'a private method' => ['secret'],
	'a protected method' => ['hidden'],
	'a static method' => ['version'],
	'a magic method' => ['__toString'],
	'a method that does not exist' => ['nope'],
	'a name that is not a string' => [42],
	'an empty allowlist' => [],
];

foreach ($refusals as $label => $methods) {
	$fresh = new Sandbox();

	try {
		$fresh->registerObject('text', $service, $methods);
		printf("%-28s EXPOSED\n", $label);
	} catch (ConfigurationError $error) {
		printf("%-28s refused\n", $label);
	}

	$fresh->close();
}

$sandbox->close();

?>
--EXPECT--
array(4) {
  [0]=>
  string(8) "HI THERE"
  [1]=>
  string(3) "nil"
  [2]=>
  string(3) "nil"
  [3]=>
  string(3) "nil"
}
array(1) {
  [0]=>
  int(1)
}
a private method             refused
a protected method           refused
a static method              refused
a magic method               refused
a method that does not exist refused
a name that is not a string  refused
an empty allowlist           refused
