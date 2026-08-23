<?php

/**
 * Checks that PHP samples in the documentation only use API that really exists.
 *
 * Syntax checking is not enough: `new FileStat(sizeInBytes: 10)` parses fine and
 * still fails at runtime, because the parameter is called $size. This walks every
 * ```php block in the docs, resolves the classes our stubs declare, and verifies
 * that referenced classes and named constructor arguments actually exist.
 *
 * Usage: php tools/check-docs-api.php [file.md ...]
 * Exits non-zero on the first problem found, listing every problem.
 */

declare(strict_types=1);

const STUB_FILES = [
    __DIR__ . '/../stubs/luaext_exceptions.stub.php',
    __DIR__ . '/../stubs/luaext.stub.php',
];

const DEFAULT_DOCS = [
    __DIR__ . '/../README.md',
    __DIR__ . '/../SECURITY.md',
    __DIR__ . '/../docs/cookbook.md',
    __DIR__ . '/../docs/lua-api.md',
];

/**
 * Load the stub declarations so the real API can be inspected by reflection.
 *
 * The stubs declare method bodies as empty, which is fine: nothing here calls
 * them, it only reads signatures.
 */
function loadStubbedApi(): void
{
    foreach (STUB_FILES as $stubFile) {
        require_once $stubFile;
    }
}

/** @return list<string> every class, interface and enum the stubs declare */
function declaredApiNames(): array
{
    $isOurs = static fn (string $name): bool => str_starts_with($name, 'DevelopGravity\\LuaExt\\');

    return array_values(array_filter(
        [...get_declared_classes(), ...get_declared_interfaces()],
        $isOurs,
    ));
}

/** @return list<array{file: string, line: int, code: string}> */
function extractPhpSamples(string $markdownFile): array
{
    $lines = file($markdownFile, FILE_IGNORE_NEW_LINES);
    $samples = [];
    $buffer = null;
    $startLine = 0;

    foreach ($lines as $index => $line) {
        if ($buffer === null && preg_match('/^\s*```php\s*$/', $line) === 1) {
            $buffer = [];
            $startLine = $index + 2;
            continue;
        }
        if ($buffer !== null && preg_match('/^\s*```\s*$/', $line) === 1) {
            $samples[] = ['file' => $markdownFile, 'line' => $startLine, 'code' => implode("\n", $buffer)];
            $buffer = null;
            continue;
        }
        if ($buffer !== null) {
            $buffer[] = $line;
        }
    }

    return $samples;
}

/**
 * Find `new ClassName(...)` calls and the named arguments each one passes.
 *
 * Uses the tokenizer rather than a regular expression so that nested calls,
 * arrays and strings containing parentheses do not confuse the argument scan.
 *
 * @return list<array{class: string, arguments: list<string>}>
 */
function findConstructorCalls(string $code): array
{
    $source = str_starts_with(ltrim($code), '<?php') ? $code : "<?php\n" . $code;
    $tokens = array_values(array_filter(
        token_get_all($source),
        static fn (array|string $token): bool => is_string($token) || !in_array($token[0], [T_WHITESPACE, T_COMMENT, T_DOC_COMMENT], true),
    ));

    $calls = [];
    $count = count($tokens);

    for ($position = 0; $position < $count; $position++) {
        $token = $tokens[$position];
        if (!is_array($token) || $token[0] !== T_NEW) {
            continue;
        }

        // Collect the class name, which may be qualified.
        $className = '';
        $cursor = $position + 1;
        while ($cursor < $count && is_array($tokens[$cursor])
            && in_array($tokens[$cursor][0], [T_STRING, T_NAME_QUALIFIED, T_NAME_FULLY_QUALIFIED], true)) {
            $className .= $tokens[$cursor][1];
            $cursor++;
        }
        if ($className === '' || $cursor >= $count || $tokens[$cursor] !== '(') {
            continue;
        }

        // Walk the argument list, recording `name:` pairs at the top level only.
        $depth = 0;
        $arguments = [];
        for ($scan = $cursor; $scan < $count; $scan++) {
            $current = $tokens[$scan];
            if ($current === '(' || $current === '[') {
                $depth++;
                continue;
            }
            if ($current === ')' || $current === ']') {
                $depth--;
                if ($depth === 0) {
                    break;
                }
                continue;
            }
            if ($depth === 1 && is_array($current) && $current[0] === T_STRING
                && ($tokens[$scan + 1] ?? null) === ':') {
                $arguments[] = $current[1];
            }
        }

        $calls[] = ['class' => $className, 'arguments' => $arguments];
    }

    return $calls;
}

/** Resolve a name as written in a sample to a declared API class, if it is one. */
function resolveApiClass(string $written, array $apiNames): ?string
{
    $normalised = ltrim($written, '\\');
    foreach ($apiNames as $apiName) {
        if ($apiName === $normalised) {
            return $apiName;
        }
        $shortName = substr((string) strrchr($apiName, '\\'), 1);
        if ($shortName === $normalised) {
            return $apiName;
        }
    }

    return null;
}

loadStubbedApi();
$apiNames = declaredApiNames();
$documents = $argc > 1 ? array_slice($argv, 1) : DEFAULT_DOCS;

$problems = [];
$checkedSamples = 0;
$checkedCalls = 0;

foreach ($documents as $document) {
    if (!is_file($document)) {
        $problems[] = sprintf('%s: not found', $document);
        continue;
    }

    foreach (extractPhpSamples($document) as $sample) {
        $checkedSamples++;

        foreach (findConstructorCalls($sample['code']) as $call) {
            $resolved = resolveApiClass($call['class'], $apiNames);
            if ($resolved === null) {
                continue; // A class from the host application, not ours to police.
            }
            $checkedCalls++;

            $reflection = new ReflectionClass($resolved);
            $constructor = $reflection->getConstructor();
            if ($constructor === null) {
                if ($call['arguments'] !== []) {
                    $problems[] = sprintf('%s:%d  %s takes no constructor arguments', $sample['file'], $sample['line'], $resolved);
                }
                continue;
            }

            $known = array_map(
                static fn (ReflectionParameter $parameter): string => $parameter->getName(),
                $constructor->getParameters(),
            );

            foreach ($call['arguments'] as $argumentName) {
                if (!in_array($argumentName, $known, true)) {
                    $problems[] = sprintf(
                        '%s:%d  %s has no parameter $%s (accepts: %s)',
                        $sample['file'], $sample['line'], $resolved, $argumentName, implode(', ', $known),
                    );
                }
            }
        }
    }
}

if ($problems !== []) {
    fwrite(STDERR, "docs-api: problems found\n");
    foreach ($problems as $problem) {
        fwrite(STDERR, '  ' . $problem . "\n");
    }
    exit(1);
}

printf("docs-api: ok -- %d samples, %d api constructor calls verified\n", $checkedSamples, $checkedCalls);
