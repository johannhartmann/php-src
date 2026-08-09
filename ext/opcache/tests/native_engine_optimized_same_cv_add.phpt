--TEST--
Native engine preserves optimized CV binary results across loop PHIs
--INI--
opcache.enable=1
opcache.enable_cli=1
--FILE--
<?php
function native_optimized_same_cv_add(int $count, bool $exit): int
{
    $total = 0;
    for ($i = 0; $i < $count; $i++) {
        $total += false;
        if ($exit) {
            return $total;
        }
    }
    return $total;
}

function native_optimized_nested_same_cv_add(array $values, $exit): int
{
    $total = 0;
    $count = count($values);
    if ($count == 0) {
        return 0;
    }
    $copy = [];
    foreach ($values as $value) {
        $copy[] = $value;
    }
    $count = $copy[5];
    for ($i = 0; $i < $count; $i++) {
        $left = $values[$i];
        for ($k = $i + 1; $k < $count; $k++) {
            $right = $values[$k];
            $total += $left > $right;
        }
        if ($exit) {
            return $total;
        }
    }
    return $total;
}

class NativeOptimizedSameCvPropertyBox
{
    public function __construct(public int $value) {}
}

class NativeOptimizedSameCvPropertyChild extends NativeOptimizedSameCvPropertyBox
{
}

function native_optimized_property_same_cv_add(
    NativeOptimizedSameCvPropertyBox $box,
    int $count,
    bool $exit,
): int {
    $total = 0;
    for ($i = 0; $i < $count; $i++) {
        $total += $box->value;
        if ($exit && $i === 2) {
            return $total;
        }
    }
    return $total;
}

function native_optimized_property_same_cv_call(
    NativeOptimizedSameCvPropertyBox $box,
    int $count,
): array {
    $total = 0;
    $seen = [];
    for ($i = 0; $i < $count; $i++) {
        $total += $box->value;
        array_push($seen, $total);
    }
    return [$total, $seen];
}

function native_optimized_property_same_cv_overflow(
    NativeOptimizedSameCvPropertyBox $box,
): bool {
    $total = PHP_INT_MAX;
    for ($i = 0; $i < 2; $i++) {
        $total += $box->value;
    }
    return is_float($total);
}

function native_optimized_property_same_cv_reference(
    NativeOptimizedSameCvPropertyBox $box,
    int $count,
): array {
    $total = 0;
    $alias =& $total;
    for ($i = 0; $i < $count; $i++) {
        $total += $box->value;
        if ($i === 1) {
            $alias += 5;
        }
    }
    return [$total, $alias];
}

class NativeOptimizedCvResultDestructor
{
    public static int $destroyed = 0;

    public function __destruct()
    {
        self::$destroyed++;
    }
}

function native_optimized_distinct_cv_bitwise(int $count): array
{
    $key = new NativeOptimizedCvResultDestructor();
    $total = 0;
    for ($i = 0; $i < $count; $i++) {
        $key = $i & 7;
        $total += [1, 2, 3, 4, 5, 6, 7, 8][$key];
    }
    return [$total, $key, NativeOptimizedCvResultDestructor::$destroyed];
}

function native_optimized_distinct_cv_reference(int $count): array
{
    $key = 'initial';
    $alias =& $key;
    $total = 0;
    for ($i = 0; $i < $count; $i++) {
        $key = $i & 7;
        $total += $key;
    }
    return [$total, $key, $alias];
}

var_dump(native_optimized_same_cv_add(6, true));
var_dump(native_optimized_same_cv_add(6, false));
var_dump(native_optimized_nested_same_cv_add([1, 2, 3, 4, 5, 6], true));
var_dump(native_optimized_nested_same_cv_add([1, 2, 3, 4, 5, 6], true));
var_dump(native_optimized_nested_same_cv_add([1, 2, 3, 4, 5, 6], false));
$box = new NativeOptimizedSameCvPropertyBox(3);
var_dump(native_optimized_property_same_cv_add($box, 6, true));
var_dump(native_optimized_property_same_cv_add($box, 6, false));
var_dump(native_optimized_property_same_cv_add(
    new NativeOptimizedSameCvPropertyChild(4),
    3,
    false,
));
var_dump(native_optimized_property_same_cv_call($box, 3));
var_dump(native_optimized_property_same_cv_overflow(
    new NativeOptimizedSameCvPropertyBox(1),
));
var_dump(native_optimized_property_same_cv_reference($box, 3));
var_dump(native_optimized_distinct_cv_bitwise(32));
var_dump(native_optimized_distinct_cv_reference(8));
unset($box->value);
try {
    native_optimized_property_same_cv_add($box, 1, false);
} catch (Error $error) {
    echo $error->getMessage(), "\n";
}
?>
--EXPECT--
int(0)
int(0)
int(0)
int(0)
int(0)
int(9)
int(18)
int(12)
array(2) {
  [0]=>
  int(9)
  [1]=>
  array(3) {
    [0]=>
    int(3)
    [1]=>
    int(6)
    [2]=>
    int(9)
  }
}
bool(true)
array(2) {
  [0]=>
  int(14)
  [1]=>
  int(14)
}
array(3) {
  [0]=>
  int(144)
  [1]=>
  int(7)
  [2]=>
  int(1)
}
array(3) {
  [0]=>
  int(28)
  [1]=>
  int(7)
  [2]=>
  int(7)
}
Typed property NativeOptimizedSameCvPropertyBox::$value must not be accessed before initialization
