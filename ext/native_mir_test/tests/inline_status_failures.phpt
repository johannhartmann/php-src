--TEST--
Native inline primitives report helper failures from the cold area only
--DESCRIPTION--
CHECK_UNDEF_ARGS of a named call, an array literal append past
PHP_INT_MAX, a concatenation with a throwing __toString() and a plain
internal call that throws, inside try/catch and propagating to the
caller, with values live across each operation. The fast paths set no
status; only the helper's status is tested, and its failure leaves
without changing the register state of the continuation.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class NativeStatusThrows
{
    public function __toString(): string
    {
        throw new RuntimeException('toString');
    }
}

function native_status_named(int $a, int $b, int $c = 3): int
{
    return $a * 100 + $b * 10 + $c;
}

/* CHECK_UNDEF_ARGS: a skipped required parameter throws in the helper. */
function native_status_undef_args(int $i): string
{
    $k = $i * 3;
    $l = $i + 7.5;
    try {
        $r = native_status_named(b: $i, c: $k);
    } catch (ArgumentCountError $e) {
        $r = 'caught ' . get_class($e);
    }
    return $r . ' ' . $k . ' ' . $l . ' ' . native_status_named(b: 2, a: $i);
}

function native_status_undef_args_out(int $i): int
{
    return native_status_named(c: $i, b: 1);
}

/* ADD_ARRAY_ELEMENT: the append past PHP_INT_MAX throws in the helper. */
function native_status_array(int $i): string
{
    $k = $i * 5;
    $v = $i + 1;
    try {
        $a = [PHP_INT_MAX => $v, $v];
        $r = count($a);
    } catch (Error $e) {
        $r = 'caught ' . $e->getMessage();
    }
    $b = [1 => $v, $k];
    return $r . ' ' . $k . ' ' . implode(',', $b);
}

function native_status_array_out(int $i): array
{
    return [PHP_INT_MAX => $i, $i];
}

/* CONCAT: a throwing __toString takes the helper's exception status. */
function native_status_concat($o, int $i): string
{
    $k = $i * 7;
    try {
        $r = $o . 'x';
    } catch (RuntimeException $e) {
        $r = 'caught ' . $e->getMessage();
    }
    return $r . ' ' . $k . ' ' . ('a' . $k);
}

function native_status_concat_out($o): string
{
    return $o . 'y';
}

/* A plain internal call that throws: the inline Do's finish. */
function native_status_internal(int $i): string
{
    $k = $i * 11;
    $l = $k + 0.5;
    try {
        $r = str_repeat('x', $i - 10);
    } catch (ValueError $e) {
        $r = 'caught ' . get_class($e);
    }
    return $r . ' ' . $k . ' ' . $l . ' ' . str_repeat('y', $i % 3) . '|';
}

function native_status_internal_out(int $i): string
{
    return str_repeat('z', $i);
}

$o = new NativeStatusThrows();
for ($i = 0; $i < 3; $i++) {
    echo native_status_undef_args($i), "\n";
    try {
        echo native_status_undef_args_out($i), "\n";
    } catch (ArgumentCountError $e) {
        echo 'outer ', get_class($e), "\n";
    }
    echo native_status_array($i), "\n";
    try {
        var_dump(native_status_array_out($i));
    } catch (Error $e) {
        echo 'outer ', $e->getMessage(), "\n";
    }
    echo native_status_concat($i % 2 ? $o : 'str', $i), "\n";
    try {
        echo native_status_concat_out($i % 2 ? $o : 'str'), "\n";
    } catch (RuntimeException $e) {
        echo 'outer ', $e->getMessage(), "\n";
    }
    echo native_status_internal($i * 6), "\n";
    try {
        echo native_status_internal_out($i - 1), "\n";
    } catch (ValueError $e) {
        echo 'outer ', get_class($e), "\n";
    }
}

/* At top level, where the context is held: the inline internal Do. */
$acc = 0;
for ($i = 0; $i < 4; $i++) {
    $k = $i * 13;
    $f = $k + 0.25;
    try {
        $s = str_repeat('q', $i - 2);
        $acc += strlen($s) + $k;
    } catch (ValueError $e) {
        $acc += 1000 + $k;
    }
    $acc += (int) $f;
}
echo $acc, "\n";
$k = 77;
echo str_repeat('e', $k - 80), "\n";
echo "not reached\n";
--EXPECTF--
caught ArgumentCountError 0 7.5 23
outer ArgumentCountError
caught Cannot add element to the array as the next element is already occupied 0 1,0
outer Cannot add element to the array as the next element is already occupied
strx 0 a0
stry
caught ValueError 0 0.5 |
outer ValueError
caught ArgumentCountError 3 8.5 123
outer ArgumentCountError
caught Cannot add element to the array as the next element is already occupied 5 2,5
outer Cannot add element to the array as the next element is already occupied
caught toString 7 a7
outer toString
caught ValueError 66 66.5 |

caught ArgumentCountError 6 9.5 223
outer ArgumentCountError
caught Cannot add element to the array as the next element is already occupied 10 3,10
outer Cannot add element to the array as the next element is already occupied
strx 14 a14
stry
xx 132 132.5 |
z
2157

Fatal error: Uncaught ValueError: str_repeat(): Argument #2 ($times) must be greater than or equal to 0 in %s:%d
Stack trace:
#0 %s(%d): str_repeat('e', -3)
#1 {main}
  thrown in %s on line %d
