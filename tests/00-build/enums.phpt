--TEST--
Every enum of the public API exists with exactly its documented cases
--EXTENSIONS--
luaext
--FILE--
<?php

use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\ProfilerUnit;

$enums = [
	OutputMode::class => ['Buffer', 'Callback', 'Discard'],
	OverflowBehavior::class => ['Truncate', 'Fail'],
	ProfilerUnit::class => ['Samples', 'Seconds', 'Percent'],
	LimitSupport::class => ['Enforced', 'Degraded', 'Unsupported'],
];

foreach ($enums as $enum => $expected) {
	$reflection = new ReflectionEnum($enum);
	$cases = array_column($enum::cases(), 'name');

	printf("%-16s pure=%s cases=%s matches=%s\n",
		$reflection->getShortName(),
		var_export(!$reflection->isBacked(), true),
		implode(',', $cases),
		var_export($cases === $expected, true));
}

?>
--EXPECT--
OutputMode       pure=true cases=Buffer,Callback,Discard matches=true
OverflowBehavior pure=true cases=Truncate,Fail matches=true
ProfilerUnit     pure=true cases=Samples,Seconds,Percent matches=true
LimitSupport     pure=true cases=Enforced,Degraded,Unsupported matches=true
