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
        // Strip a blockquote marker first: a sample inside a `>` quote is still
        // a sample, and the original fence pattern anchored on whitespace alone,
        // so every quoted block in the docs was being skipped silently.
        $line = preg_replace('/^\s*>\s?/', '', $line) ?? $line;

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

/**
 * Which documentation tables carry API names, and which column holds them.
 *
 * Tables are matched on their header row rather than a line number, so editing
 * prose around one does not silently switch the check off -- and every rule
 * below must match at least one real table or this tool fails, which is the
 * only thing standing between "the check passed" and "the check evaporated".
 *
 * Keys are the header cells joined with '|'. Values map a zero-based column to
 * how its cells should be read.
 */
const TABLE_RULES = [
    // README: the capability matrix. Column 0 names a Capabilities property.
    'Capability|Untrusted default|`trusted()`|Notes' => [
        0 => ['kind' => 'property', 'class' => 'Capabilities'],
    ],
    // README: the limits table. Column 0 names a Limits property.
    'Limit|Default' => [
        0 => ['kind' => 'property', 'class' => 'Limits'],
    ],
    // README: platform support. The last column is a LimitSupport case.
    "Platform|Arch|Install|CPU clock source|Typical resolution|`features()['cpuLimit']`" => [
        5 => ['kind' => 'enumCase', 'class' => 'LimitSupport'],
    ],
    // README: the migration table, whose preamble promises it "match[es] the
    // stubs exactly". Column 0 is the OLD extension's API and is deliberately
    // not checked; column 2 is prose dense enough that policing it would cost
    // more in false positives than it catches.
    'LuaSandbox|LuaExt|Notes' => [
        1 => ['kind' => 'apiSymbols'],
    ],
];

/**
 * Cell fragments that look like symbols but are not ours to resolve: PHP types
 * and keywords, generics, attributes, and the prose markers the tables use for
 * "this had no predecessor".
 */
const NON_SYMBOLS = [
    'array', 'string', 'int', 'float', 'bool', 'void', 'mixed', 'null', 'true',
    'false', 'callable', 'iterable', 'object', 'self', 'static', 'list', 'new',
    'none', 'NoDiscard', 'LuaSandbox', 'LuaSandboxFunction',
];

/**
 * Split a markdown table row into trimmed cells.
 *
 * @return list<string>
 */
function splitTableRow(string $line): array
{
    $trimmed = trim($line);
    $trimmed = preg_replace('/^\|/', '', $trimmed) ?? $trimmed;
    $trimmed = preg_replace('/\|$/', '', $trimmed) ?? $trimmed;

    // Escaped pipes inside a cell (\|, used in code spans) are not separators.
    $cells = preg_split('/(?<!\\\\)\|/', $trimmed) ?: [];

    return array_map(static fn (string $cell): string => trim(str_replace('\\|', '|', $cell)), $cells);
}

/**
 * Pull every markdown table out of a document, ignoring fenced code blocks.
 *
 * @return list<array{file: string, line: int, signature: string, rows: list<array{line: int, cells: list<string>}>}>
 */
function extractTables(string $markdownFile): array
{
    $lines = file($markdownFile, FILE_IGNORE_NEW_LINES);
    $tables = [];
    $inFence = false;
    $current = null;

    foreach ($lines as $index => $rawLine) {
        $line = preg_replace('/^\s*>\s?/', '', $rawLine) ?? $rawLine;

        if (preg_match('/^\s*```/', $line) === 1) {
            $inFence = !$inFence;
            continue;
        }
        if ($inFence) {
            continue;
        }

        $isRow = str_contains($line, '|') && preg_match('/^\s*\|?[^|]*\|/', $line) === 1;

        if (!$isRow) {
            if ($current !== null) {
                $tables[] = $current;
                $current = null;
            }
            continue;
        }

        $cells = splitTableRow($line);

        if ($current === null) {
            $current = [
                'file' => $markdownFile,
                'line' => $index + 1,
                'signature' => implode('|', $cells),
                'rows' => [],
            ];
            continue;
        }

        // The |---|---| separator under the header is not data.
        if (preg_match('/^[\s:|-]+$/', $line) === 1) {
            continue;
        }

        $current['rows'][] = ['line' => $index + 1, 'cells' => $cells];
    }

    if ($current !== null) {
        $tables[] = $current;
    }

    return $tables;
}

/**
 * The backtick-delimited spans in a cell -- the only places these tables put
 * API names. Unquoted prose is deliberately ignored.
 *
 * @return list<string>
 */
function codeSpans(string $cell): array
{
    preg_match_all('/`([^`]+)`/', $cell, $matches);

    return $matches[1];
}

/**
 * Check one cell against a column rule, returning human-readable problems.
 *
 * @param  list<string> $apiNames
 * @return list<string>
 */
function checkCell(string $cell, array $rule, array $apiNames): array
{
    $problems = [];

    foreach (codeSpans($cell) as $span) {
        switch ($rule['kind']) {
            case 'property':
                // Bare identifiers only: `load()` in this column is a Lua
                // global being named, not a property of ours.
                if (preg_match('/^[a-z][A-Za-z0-9]*$/', $span) !== 1) {
                    break;
                }
                $class = resolveApiClass($rule['class'], $apiNames);
                if ($class === null) {
                    $problems[] = sprintf('rule names unknown class %s', $rule['class']);
                    break;
                }
                if (!(new ReflectionClass($class))->hasProperty($span)) {
                    $problems[] = sprintf('%s has no property $%s', $class, $span);
                }
                break;

            case 'enumCase':
                if (preg_match('/^[A-Z][A-Za-z0-9]*$/', $span) !== 1) {
                    break;
                }
                $class = resolveApiClass($rule['class'], $apiNames);
                if ($class === null || !enum_exists($class)) {
                    $problems[] = sprintf('rule names unknown enum %s', $rule['class']);
                    break;
                }
                if (!(new ReflectionEnum($class))->hasCase($span)) {
                    $problems[] = sprintf('%s has no case %s', $class, $span);
                }
                break;

            case 'apiSymbols':
                $problems = [...$problems, ...checkApiSymbols($span, $apiNames)];
                break;
        }
    }

    return $problems;
}

/**
 * Verify every API name inside one code span of the migration table.
 *
 * Handles the four shapes those cells use: `Class::method()`, `Class::CASE`,
 * `->method()`, `->property`, and a bare `ClassName`. A bare `->method()` is
 * accepted if any class in the API declares it -- the table mixes Sandbox and
 * LuaFunction rows, and guessing a receiver would invent failures rather than
 * find them. That still catches the thing this is for: a rename or a typo.
 *
 * @param  list<string> $apiNames
 * @return list<string>
 */
function checkApiSymbols(string $span, array $apiNames): array
{
    $problems = [];

    // Class::member
    if (preg_match_all('/\b([A-Z][A-Za-z0-9]*)::(\w+)/', $span, $matches, PREG_SET_ORDER) > 0) {
        foreach ($matches as [, $className, $member]) {
            if (in_array($className, NON_SYMBOLS, true)) {
                continue;
            }
            $resolved = resolveApiClass($className, $apiNames);
            if ($resolved === null) {
                $problems[] = sprintf('unknown class %s', $className);
                continue;
            }
            $reflection = new ReflectionClass($resolved);
            $known = $reflection->hasMethod($member)
                || $reflection->hasConstant($member)
                || (enum_exists($resolved) && (new ReflectionEnum($resolved))->hasCase($member));
            if (!$known) {
                $problems[] = sprintf('%s has no member %s', $resolved, $member);
            }
        }
    }

    // ->member, with or without a call
    if (preg_match_all('/->(\w+)(\s*\()?/', $span, $matches, PREG_SET_ORDER) > 0) {
        foreach ($matches as $match) {
            $member = $match[1];
            $isCall = isset($match[2]) && $match[2] !== '';
            $found = false;

            foreach ($apiNames as $apiName) {
                $reflection = new ReflectionClass($apiName);
                if ($isCall ? $reflection->hasMethod($member) : $reflection->hasProperty($member)) {
                    $found = true;
                    break;
                }
            }

            if (!$found) {
                $problems[] = sprintf('no documented class has %s%s', $member, $isCall ? '()' : ' as a property');
            }
        }
    }

    // A bare class name, with every already-handled shape stripped out first.
    $bare = preg_replace(['/\b[A-Z][A-Za-z0-9]*::\w+/', '/->\w+/', '/\$\w+/'], ' ', $span) ?? $span;
    if (preg_match_all('/\b([A-Z][A-Za-z0-9]{2,})\b/', $bare, $matches) > 0) {
        foreach ($matches[1] as $className) {
            if (in_array($className, NON_SYMBOLS, true)) {
                continue;
            }
            if (resolveApiClass($className, $apiNames) === null) {
                $problems[] = sprintf('unknown class %s', $className);
            }
        }
    }

    return $problems;
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

// ---------------------------------------------------------------------------
// Tables. README's migration section promises its names "match the stubs
// exactly", which nothing enforced until now.
// ---------------------------------------------------------------------------

$checkedCells = 0;
$rulesMatched = [];

foreach ($documents as $document) {
    if (!is_file($document)) {
        continue; // Already reported above.
    }

    foreach (extractTables($document) as $table) {
        $rules = TABLE_RULES[$table['signature']] ?? null;
        if ($rules === null) {
            continue;
        }

        $rulesMatched[$table['signature']] = true;

        foreach ($table['rows'] as $row) {
            foreach ($rules as $columnIndex => $rule) {
                $cell = $row['cells'][$columnIndex] ?? null;
                if ($cell === null || $cell === '') {
                    continue;
                }

                $checkedCells++;

                foreach (checkCell($cell, $rule, $apiNames) as $problem) {
                    $problems[] = sprintf('%s:%d  %s', $table['file'], $row['line'], $problem);
                }
            }
        }
    }
}

// A rule that matches nothing is a check that has quietly stopped running --
// the exact failure this tool exists to prevent, one level up.
foreach (array_keys(TABLE_RULES) as $signature) {
    if (!isset($rulesMatched[$signature])) {
        $problems[] = sprintf(
            'no table matched the rule for header "%s" -- the table was edited or removed, so its column is no longer checked',
            $signature,
        );
    }
}

if ($problems !== []) {
    fwrite(STDERR, "docs-api: problems found\n");
    foreach ($problems as $problem) {
        fwrite(STDERR, '  ' . $problem . "\n");
    }
    exit(1);
}

printf(
    "docs-api: ok -- %d samples, %d api constructor calls, %d table cells across %d tables verified\n",
    $checkedSamples,
    $checkedCalls,
    $checkedCells,
    count($rulesMatched),
);
