<?php

declare(strict_types=1);

/**
 * The harness the tests/10-lua/ conformance files share.
 *
 * WHY THIS DIRECTORY EXISTS. tests/golden/stdlib/ records the 2301 names a
 * script can reach and CI checks that list on every push, so `string.gsub` is
 * proven to EXIST. Nothing proved it returns the right answer. The interpreter
 * here is patched -- interrupt checks at the four back edges of lvm.c,
 * LUAEXT_CHECK inside the pattern matcher and table.move/sort/concat, a replaced
 * tostring, a replaced math.randomseed, a rewritten io and os -- and those
 * patches live in exactly the functions a conformance suite covers. Before this
 * directory, a patch that changed an answer shipped green.
 *
 * WHY IT REPORTS VALUES, NOT PASS/FAIL. A row here prints what Lua actually
 * produced. A regression then shows up as the wrong answer rather than as the
 * word FAIL, which is the difference between a diff that tells you what broke
 * and one that tells you only that something did.
 *
 * PORTABILITY IS THE DESIGN CONSTRAINT. These files run on Linux, macOS and
 * Windows and must produce byte-identical output on all three. So: no CPU or
 * wall-clock assertions (Windows' thread clock is a ~15.6 ms scheduler tick), no
 * host paths (the VFS root is always '/'), no locale-dependent case or number
 * formatting, and no assertion on table iteration order. Floats go through Lua's
 * own tostring, which is a round-trip-exact %.17g in 5.5 and identical on every
 * C library. Anything that cannot be made platform-neutral belongs in
 * tests/02-limits/ instead.
 */

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/**
 * Lua-side helpers, prepended to every conformance chunk.
 *
 * `row(label, ...)` records one line per check, showing every value it was
 * given. `try(f, ...)` turns an error into a row so a test can assert on a
 * refusal without a pcall dance at every site.
 */
const LUA_CONFORMANCE_PRELUDE = <<<'LUA'
	local out = {}

	-- Deliberately not string.format('%q', s): %q escapes a newline as a
	-- backslash followed by a REAL newline, which would split one check across
	-- two lines and make the expectation block unreadable. This keeps every row
	-- on one line, and is byte-identical on every platform.
	local function quote(s)
		return '"' .. (s:gsub('[%c"\\]', function (c)
			if c == '"' then return '\\"' end
			if c == '\\' then return '\\\\' end
			return string.format('\\%d', c:byte())
		end)) .. '"'
	end

	local function show(value)
		local kind = type(value)

		if kind == 'string' then
			return quote(value)
		elseif kind == 'number' then
			-- The integer/float distinction is a real part of the language and
			-- several checks below exist to pin it, so it must survive display:
			-- 3 and 3.0 have to render differently. A plain %g would give "1"
			-- for 1.0 and lose exactly that -- so floats go through Lua's own
			-- tostring, which is LUA_NUMBER_FMT_N (%.17g in 5.5) plus the ".0"
			-- that keeps an integral float apart from an integer.
			if math.type(value) == 'integer' then
				return string.format('%d', value)
			end

			-- The three non-finite values are spelled by the platform's printf,
			-- and "-nan" vs "nan" is a real difference between C libraries. Named
			-- here instead, so a row carrying an infinity means the same thing on
			-- every platform this suite runs on.
			if value ~= value then
				return 'nan'
			elseif value == math.huge then
				return 'inf'
			elseif value == -math.huge then
				return '-inf'
			end

			return tostring(value)
		elseif kind == 'table' then
			local parts = {}

			-- ipairs, never pairs: hash-part order is unspecified and seeded per
			-- sandbox, so a pairs() walk would print in a different order on
			-- different runs. A check that needs the hash part sorts it itself.
			for index, item in ipairs(value) do
				parts[index] = show(item)
			end

			return '{' .. table.concat(parts, ', ') .. '}'
		elseif kind == 'nil' or kind == 'boolean' then
			return tostring(value)
		end

		-- Functions, threads and userdata: the type only. Their tostring()
		-- carries an address in stock Lua, and this build removes that on
		-- purpose (see SECURITY.md on ASLR disclosure).
		return '<' .. kind .. '>'
	end

	local function row(label, ...)
		local count = select('#', ...)
		local parts = {}

		for index = 1, count do
			parts[index] = show((select(index, ...)))
		end

		out[#out + 1] = label .. ' = ' .. table.concat(parts, ', ')
	end

	-- Records the outcome either way. A refusal shows as `!` plus the message
	-- with its chunk prefix stripped, because that prefix carries a line number
	-- that moves whenever a check is inserted above it.
	local function try(label, f, ...)
		local ok, err = pcall(f, ...)

		if ok then
			row(label, 'no error')
		elseif type(err) == 'string' then
			row(label, '! ' .. (err:gsub('^.-:%d+: ', '')))
		else
			row(label, '! ' .. type(err))
		end
	end

	local function report()
		return table.concat(out, '\n')
	end
	LUA;

/**
 * Run a conformance chunk and print its rows.
 *
 * Untrusted capabilities by default -- the baseline every host gets -- so a
 * check that needs more says so, and the reader can see which surface is being
 * exercised.
 */
function conformance(string $lua, ?Capabilities $capabilities = null, ?object $filesystem = null): void
{
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: $capabilities ?? new Capabilities(),
		filesystem: $filesystem,
	));

	[$report] = $sandbox->eval(LUA_CONFORMANCE_PRELUDE . "\n" . $lua . "\nreturn report()", '=conformance');

	echo $report, "\n";

	$sandbox->close();
}
