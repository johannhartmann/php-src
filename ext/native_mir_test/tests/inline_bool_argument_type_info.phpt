--TEST--
Native: an inlined bool parameter takes its value from the argument's type
--DESCRIPTION--
A boxed bool argument held in registers is unboxed for an inlined bool
parameter. A bool zval carries its value in type_info; the payload is
undefined, both after !$bool and after Zend wrote the bool to a property.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeInlineBoolArgument;

class Box { public bool $flag = false; }

function pick(int $a, bool $flag, int $c): int { return $flag ? 1 : 0; }

function alternate(int $n) {
    $acc = 0;
    $t = true;
    for ($i = 0; $i < $n; $i++) {
        $acc += pick($i, $t, 5);
        $t = !$t;
    }
    return $acc;
}

function countTrue(int $n, array $arr) {
    $acc = 0;
    $t = true;
    $box = new Box;
    $box->flag = in_array(9, $arr);
    for ($i = 0; $i < $n; $i++) {
        $acc += pick($i, $t, 5);
        $t = $box->flag;
    }
    return $acc;
}

for ($r = 0; $r < 3; $r++) {
    var_dump(alternate(5), countTrue(5, [1, 2, 3]), countTrue(5, [9]));
}
?>
--EXPECT--
int(3)
int(1)
int(5)
int(3)
int(1)
int(5)
int(3)
int(1)
int(5)
