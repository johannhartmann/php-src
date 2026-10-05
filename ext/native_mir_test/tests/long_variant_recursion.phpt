--TEST--
Recursive calls with integer arguments enter the integer variant directly
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function fib($n) {
    return ($n < 2) ? 1 : fib($n - 2) + fib($n - 1);
}
function grow($n) {
    return ($n < 1) ? PHP_INT_MAX - 3 : grow($n - 1) + 1;
}
function big($n) {
    return ($n < 2) ? PHP_INT_MAX >> 1 : big($n - 1) + big($n - 2);
}
function sub_under($n) {
    return ($n < 1) ? PHP_INT_MIN + 2 : sub_under($n - 1) - 1;
}
function count_calls($n, &$calls) {
    $calls++;
    return ($n < 2) ? $n : count_calls($n - 1, $calls) + count_calls($n - 2, $calls);
}
function echoing($n) {
    if ($n < 1) { echo "base\n"; return 0; }
    return echoing($n - 1) + 1;
}
for ($r = 0; $r < 3; $r++) {
    var_dump(fib(15), fib(15.0), fib("15"), fib(true));
    var_dump(grow(2), grow(5), grow(7));
    var_dump(big(3), big(4));
    var_dump(sub_under(1), sub_under(4));
    $c = 0;
    var_dump(count_calls(10, $c), $c);
    var_dump(echoing(2));
}
try {
    fib([]);
} catch (TypeError $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
?>
--EXPECT--
int(987)
int(987)
int(987)
int(2)
int(9223372036854775806)
float(9.223372036854776E+18)
float(9.223372036854776E+18)
float(1.3835058055282164E+19)
float(2.305843009213694E+19)
int(-9223372036854775807)
float(-9.223372036854776E+18)
int(55)
int(177)
base
int(2)
int(987)
int(987)
int(987)
int(2)
int(9223372036854775806)
float(9.223372036854776E+18)
float(9.223372036854776E+18)
float(1.3835058055282164E+19)
float(2.305843009213694E+19)
int(-9223372036854775807)
float(-9.223372036854776E+18)
int(55)
int(177)
base
int(2)
int(987)
int(987)
int(987)
int(2)
int(9223372036854775806)
float(9.223372036854776E+18)
float(9.223372036854776E+18)
float(1.3835058055282164E+19)
float(2.305843009213694E+19)
int(-9223372036854775807)
float(-9.223372036854776E+18)
int(55)
int(177)
base
int(2)
TypeError: Unsupported operand types: array - int
