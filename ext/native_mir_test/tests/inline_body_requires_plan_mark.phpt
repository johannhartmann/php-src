--TEST--
Native: only calls the plan marked effect-closed clone the callee body
--DESCRIPTION--
The weakly coerced string argument makes the plan reject the call as an
effect-closed clone, so the call keeps no guarded cold block. The adaptor
must not clone the single-block body anyway: the canonical call would never
consume the clone's value.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function leaf(string $s, int $a): int { return $a + 1; }
function root(int $count): int {
    $v = 0;
    for ($i = 0; $i < $count; $i++) {
        $v = leaf(123, $v);
    }
    return $v;
}
var_dump(root(3));
?>
--EXPECT--
int(3)
