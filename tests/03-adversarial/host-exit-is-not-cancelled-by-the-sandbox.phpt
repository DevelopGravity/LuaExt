--TEST--
A host that ends the request from inside a callback actually ends it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// exit() FROM INSIDE A CALLBACK IS NOT AN EXCEPTION, AND IS NOT A BAILOUT EITHER.
//
// An internal function is on the stack when a Lua-invoked callback runs, so PHP
// cannot longjmp past it. Instead it sets EG(exception) to a sentinel built by
// zend_create_unwind_exit() -- an object that implements nothing, whose only job
// is to unwind the VM.
//
// This extension treated any pending exception as a host exception: it retained
// the object on the Lua error value, called zend_clear_exception(), and re-threw
// it on the way out. Two things went wrong, and the second is the serious one:
//
//   1. The re-throw reported "Cannot throw objects that do not implement
//      Throwable", which is nonsense and was ours.
//   2. zend_clear_exception() CANCELLED THE EXIT. A host that called exit()
//      did not exit, and the exit status was lost with it.
//
// The status is what this test really pins. A message can be argued about; an
// exit code of 3 either survives the sandbox or it does not.

$script = <<<'PHP'
    use DevelopGravity\LuaExt\Sandbox;

    $sandbox = new Sandbox();
    $sandbox->registerLibrary('host', [
        'stop' => static function (): void {
            exit(3);
        },
    ]);

    echo "before\n";
    (void) $sandbox->eval('host.stop() return "unreachable"', '=exiting');
    echo "AFTER -- the exit was cancelled\n";
    PHP;

$file = tempnam(sys_get_temp_dir(), 'luaext-exit-') . '.php';
file_put_contents($file, "<?php\n" . $script);

$command = sprintf(
    '%s -n -d extension=%s %s 2>&1',
    escapeshellarg(PHP_BINARY),
    escapeshellarg(ini_get('extension_dir') . '/luaext.so'),
    escapeshellarg($file),
);

exec($command, $output, $status);
unlink($file);

printf("output: %s\n", implode(' | ', $output));
printf("status: %d\n", $status);

?>
--EXPECT--
output: before
status: 3
