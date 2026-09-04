--TEST--
Lua conformance: the pattern matcher, which this build patched for interruptibility
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// THE HIGHEST-VALUE FILE IN THIS DIRECTORY. lstrlib.c's matcher is one of the
// places LUAEXT_CHECK was inserted, so that catastrophic backtracking can be
// interrupted instead of hanging the request -- the older luasandbox had no
// equivalent and simply hoped. Those insertions sit inside `match`, `max_expand`
// and `min_expand`, which is to say inside the loops that decide what a pattern
// matches. A misplaced check does not fail loudly; it changes an answer.

conformance(<<<'LUA'
	-- find: plain and pattern, with and without captures.
	row('find', ('hello world'):find('o'))
	row('find from', ('hello world'):find('o', 6))
	row('find plain', ('a.b'):find('.', 1, true))
	row('find pattern dot', ('a.b'):find('.'))
	row('find none', ('abc'):find('z'))
	row('find captures', ('key=value'):find('(%w+)=(%w+)'))

	-- match and its captures, including the empty capture and position capture.
	row('match', ('hello'):match('l+'))
	row('match captures', ('2026-09-04'):match('(%d+)-(%d+)-(%d+)'))
	row('position capture', ('abc'):match('b()'))
	row('optional capture', ('abc'):match('(x?)abc'))

	-- Character classes, and their complements.
	row('%d %a %s', ('a1 '):match('%a'), ('a1 '):match('%d'), (' '):match('%s'))
	row('%w %p %c', ('a_1!'):match('%w+'), ('!'):match('%p'), ('\1'):match('%c') ~= nil)
	row('%u %l %x', ('Ab9'):match('%u'), ('Ab9'):match('%l'), ('ff'):match('%x+'))
	row('complement', ('abc123'):match('%D+'), ('abc123'):match('%A+'))
	row('set', ('hello'):match('[aeiou]+'), ('hello'):match('[^aeiou]+'))
	row('range', ('a-z'):match('[a%-z]+'))

	-- Anchors, quantifiers and their greediness.
	row('anchor', ('aaa'):match('^a'), ('aaa'):match('^b'))
	row('greedy vs lazy', ('<a><b>'):match('<.*>'), ('<a><b>'):match('<.->'))
	row('star on empty', ('abc'):match('x*'))
	row('plus needs one', ('abc'):match('x+'))
	row('question', ('color'):match('colou?r'), ('colour'):match('colou?r'))

	-- %b balanced match and %f frontier, the two nobody remembers.
	row('%b', ('(a(b)c) tail'):match('%b()'))
	row('%b unbalanced', ('(a(b)c'):match('%b()'))
	row('%f frontier', ('THE (quick) fox'):gsub('%f[%a]%a+%f[%A]', function (w) return w:lower() end))

	-- gsub: string, table and function replacements, plus the count limit and
	-- the %1 back-reference in the replacement.
	row('gsub string', ('hello world'):gsub('o', '0'))
	row('gsub limited', ('hello world'):gsub('o', '0', 1))
	row('gsub capture ref', ('hello world'):gsub('(%w+)', '<%1>'))
	row('gsub whole match', ('abc'):gsub('%a', '%0%0'))
	row('gsub table', ('$name is $age'):gsub('%$(%w+)', {name = 'ada', age = 36}))
	row('gsub function', ('a1b2'):gsub('%d', function (d) return '[' .. d .. ']' end))
	row('gsub function nil keeps', ('a1b2'):gsub('%d', function () return nil end))
	row('gsub false keeps', ('a1b2'):gsub('%d', function () return false end))

	-- An empty match still advances, or gsub would never terminate.
	row('gsub empty pattern', ('abc'):gsub('', '-'))

	-- gmatch, including the two-capture form used for key/value parsing.
	local words = {}
	for word in ('the quick brown'):gmatch('%a+') do words[#words + 1] = word end
	row('gmatch', words)

	local pairs_found = {}
	for k, v in ('a=1,b=2'):gmatch('(%w+)=(%w+)') do pairs_found[#pairs_found + 1] = k .. v end
	row('gmatch two captures', pairs_found)

	-- Magic characters must be escaped, and %% is a literal percent.
	row('escaped magic', ('3+4'):match('%+'), ('100%'):match('%%'))
	row('gsub percent', ('x'):gsub('x', '100%%'))

	-- Malformed patterns are errors, not silent non-matches.
	try('unfinished capture', function () return ('abc'):match('(%a') end)
	try('malformed set', function () return ('abc'):match('[a') end)
	try('dangling escape', function () return ('abc'):match('%') end)
	try('bad capture index', function () return ('abc'):gsub('a', '%9') end)
	try('missing %b args', function () return ('abc'):match('%b') end)

	-- The capture ceiling is reported, not silently truncated. Deep BACKTRACKING
	-- is a different case and is not tested here: it is bounded by the CPU limit
	-- rather than by the matcher, and asserting on it would be a timing test.
	try('too many captures', function ()
		return (string.rep('a', 300)):match(string.rep('(a)', 300))
	end)
	LUA);

?>
--EXPECT--
find = 5, 5
find from = 8, 8
find plain = 2, 2
find pattern dot = 1, 1
find none = nil
find captures = 1, 9, "key", "value"
match = "ll"
match captures = "2026", "09", "04"
position capture = 3
optional capture = ""
%d %a %s = "a", "1", " "
%w %p %c = "a", "!", true
%u %l %x = "A", "b", "ff"
complement = "abc", "123"
set = "e", "h"
range = "a-z"
anchor = "a", nil
greedy vs lazy = "<a><b>", "<a>"
star on empty = ""
plus needs one = nil
question = "color", "colour"
%b = "(a(b)c)"
%b unbalanced = "(b)"
%f frontier = "the (quick) fox", 3
gsub string = "hell0 w0rld", 2
gsub limited = "hell0 world", 1
gsub capture ref = "<hello> <world>", 2
gsub whole match = "aabbcc", 3
gsub table = "ada is 36", 2
gsub function = "a[1]b[2]", 2
gsub function nil keeps = "a1b2", 2
gsub false keeps = "a1b2", 2
gsub empty pattern = "-a-b-c-", 4
gmatch = {"the", "quick", "brown"}
gmatch two captures = {"a1", "b2"}
escaped magic = "+", "%"
gsub percent = "100%", 1
unfinished capture = "! unfinished capture"
malformed set = "! malformed pattern (missing ']')"
dangling escape = "! malformed pattern (ends with '%')"
bad capture index = "! invalid capture index %9"
missing %b args = "! malformed pattern (missing arguments to '%b')"
too many captures = "! too many captures"
