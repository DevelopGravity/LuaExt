# luaext stubs

The public API of the [luaext](https://github.com/DevelopGravity/LuaExt) PHP extension,
declared in PHP so editors and static analysers can resolve it without loading the binary.

```bash
composer require --dev developgravity/lua-ext-stubs
```

Match the version to the extension you are running: the stub package is tagged from the
same commit as the release it describes, so `0.2.0` here is the API of `luaext` `0.2.0`.

## Never autoload these files

**This package deliberately declares no `autoload` section, and adding one would break
every consumer that has the extension installed.** The files declare real classes —
`require` one while `luaext` is loaded and PHP raises `Cannot redeclare enum
DevelopGravity\LuaExt\OutputMode`, fatally. They exist to be *read* by tooling, never
loaded by the runtime.

Nothing below loads them. If you write your own tooling that reads them, run it on a PHP
without the extension (`php -n`, which is what the extension's own documentation gate
does).

## Wiring it up

**PhpStorm** needs nothing. It indexes everything under `vendor/`, so completion and
type inference start working as soon as the package is installed.

**PHPStan** — use `stubFiles`, not `scanFiles`. `stubFiles` overrides whatever PHPStan
already knows about those symbols, which is the behaviour you want when the real
extension may also be present; `scanFiles` would add a second, conflicting declaration.

```neon
parameters:
    stubFiles:
        - vendor/developgravity/lua-ext-stubs/luaext.stub.php
        - vendor/developgravity/lua-ext-stubs/luaext_exceptions.stub.php
```

**Psalm**:

```xml
<stubs>
    <file name="vendor/developgravity/lua-ext-stubs/luaext.stub.php" />
    <file name="vendor/developgravity/lua-ext-stubs/luaext_exceptions.stub.php" />
</stubs>
```

## This repository is generated

Do not commit here. It is a `git subtree split` of `stubs/` in
[DevelopGravity/LuaExt](https://github.com/DevelopGravity/LuaExt), pushed from there at
each release, and the split only ever fast-forwards. A commit made directly on this side
breaks that fast-forward permanently.

The same two files generate the extension's C arginfo, so a signature here cannot drift
from the compiled binary — that is the whole reason this package is a split rather than a
hand-maintained copy. Report anything wrong with them on the
[extension's issue tracker](https://github.com/DevelopGravity/LuaExt/issues).

## Licence

MIT, the same as the extension. See [LICENSE](LICENSE).
