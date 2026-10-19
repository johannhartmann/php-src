--TEST--
Native: a ternary that returns its received argument stays in the typed body
--DESCRIPTION--
`return $n < 2 ? $n : f($n - 1) + f($n - 2)` with a declared return type
forwards the received argument through the ternary's QM_ASSIGN and verifies
a phi of it and the sum. The typed body accepts both, and a double that the
int return type cannot take fails the body, so the call runs again through
its Zend entry and coerces or throws exactly.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function fib($n): int { return $n < 2 ? $n : fib($n - 1) + fib($n - 2); }
function fib_typed(int $n): int { return $n < 2 ? $n : fib_typed($n - 1) + fib_typed($n - 2); }
function grow(int $n): int { return $n < 2 ? $n : grow($n - 1) * 1000000000; }
function half($n): int { return $n < 2 ? $n : half($n - 1) + 0.5; }
function pass($n): float { return $n < 2 ? $n : pass($n - 1) * 2; }

var_dump(fib(20), fib_typed(25), fib("7"), fib(3.0));
var_dump(grow(2), grow(3));
try {
    var_dump(grow(4));
} catch (TypeError $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
try {
    var_dump(half(3));
} catch (TypeError $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
var_dump(pass(1), pass(10));
try {
    var_dump(fib("x"));
} catch (TypeError $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
?>
--EXPECTF--
int(6765)
int(75025)
int(13)
int(2)
int(1000000000)
int(1000000000000000000)
TypeError: grow(): Return value must be of type int, float returned

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 5

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 5
int(1)
float(1)
float(512)
TypeError: Unsupported operand types: string - int
