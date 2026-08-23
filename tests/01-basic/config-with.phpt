--TEST--
with() returns a new object, changes only what was named, and leaves the source alone
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

/**
 * @return list<string> the properties that differ between two value objects
 */
function changedProperties(object $before, object $after): array
{
	$changed = [];

	foreach ((new ReflectionObject($before))->getProperties() as $property) {
		$name = $property->getName();

		if ($before->$name !== $after->$name) {
			$changed[] = $name;
		}
	}

	return $changed;
}

$capabilities = new Capabilities();
$derived = $capabilities->with(vfs: true);

var_dump($derived !== $capabilities, $derived instanceof Capabilities);
var_dump(changedProperties($capabilities, $derived));

// The source is untouched: with() is a copy, not an in-place edit that happens
// to hand back $this.
var_dump($capabilities->vfs, $derived->vfs);

$limits = new Limits();
$slower = $limits->with(cpuSeconds: 30.0, outputOverflow: OverflowBehavior::Truncate);

var_dump(changedProperties($limits, $slower));
var_dump($limits->cpuSeconds, $slower->cpuSeconds);
var_dump($limits->outputOverflow === OverflowBehavior::Fail);

// Nullable fields can be set back to null, which is how a limit is lifted.
var_dump($limits->with(cpuSeconds: null)->cpuSeconds);

$quota = new VfsQuota();
var_dump(changedProperties($quota, $quota->with(maxFiles: 4, billWallTime: true)));

$config = new SandboxConfig();
$configured = $config->with(deterministic: true, seed: 1234);

var_dump(changedProperties($config, $configured));
var_dump($config->seed, $configured->seed, $configured->deterministic);

// with() naming nothing is still a distinct object, not the receiver.
$same = $capabilities->with();
var_dump($same !== $capabilities, changedProperties($capabilities, $same));

// Chaining leaves every intermediate exactly as it was.
$chained = $capabilities->with(vfs: true)->with(vfsWrite: true)->with(require: true);
var_dump(changedProperties($capabilities, $chained));
var_dump($capabilities->vfs, $capabilities->vfsWrite, $capabilities->require);

// A wrong type for a real property is a TypeError from the property's declared
// type, not a silently coerced value.
try {
	$limits->with(maxCallDepth: []);
} catch (TypeError $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

// And with() honours the caller's strict_types, exactly as assigning to the
// property would: this file declares it, so a numeric string is not an int.
try {
	$limits->with(maxCallDepth: '5');
} catch (TypeError $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

?>
--EXPECT--
bool(true)
bool(true)
array(1) {
  [0]=>
  string(3) "vfs"
}
bool(false)
bool(true)
array(2) {
  [0]=>
  string(10) "cpuSeconds"
  [1]=>
  string(14) "outputOverflow"
}
float(1)
float(30)
bool(true)
NULL
array(2) {
  [0]=>
  string(8) "maxFiles"
  [1]=>
  string(12) "billWallTime"
}
array(2) {
  [0]=>
  string(4) "seed"
  [1]=>
  string(13) "deterministic"
}
NULL
int(1234)
bool(true)
bool(true)
array(0) {
}
array(3) {
  [0]=>
  string(7) "require"
  [1]=>
  string(3) "vfs"
  [2]=>
  string(8) "vfsWrite"
}
bool(false)
bool(false)
bool(false)
TypeError: Cannot assign array to property DevelopGravity\LuaExt\Limits::$maxCallDepth of type int
TypeError: Cannot assign string to property DevelopGravity\LuaExt\Limits::$maxCallDepth of type int
