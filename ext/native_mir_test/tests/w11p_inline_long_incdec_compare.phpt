--TEST--
Native baseline executes guarded long incdec and comparisons inline
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function inline_long_paths(int $left, int $right): array
{
    $preInc = ++$left;
    $postInc = $left++;
    $preDec = --$right;
    $postDec = $right--;
    return [
        $preInc,
        $postInc,
        $left,
        $preDec,
        $postDec,
        $right,
        $left < $right,
        $left <= $right,
        $left == $right,
        $left != $right,
        $left === $right,
        $left !== $right,
    ];
}

function inline_long_overflow(int $value): mixed
{
    return ++$value;
}

function inline_long_binary(int $value): array
{
    return [
        $value + 1,
        1 + $value,
        $value - 1,
        20 - $value,
        $value < 10,
        10 < $value,
        $value <= 10,
        10 <= $value,
        $value == 10,
        10 != $value,
        $value === 10,
        10 !== $value,
    ];
}

function inline_long_add_overflow(int $value): mixed
{
    return $value + 1;
}

function inline_long_sub_overflow(int $value): mixed
{
    return $value - 1;
}

function inline_long_typed_add_leaf(int $value): int
{
    return $value + 1;
}

function inline_long_typed_add_root(int $value): int
{
    return inline_long_typed_add_leaf($value);
}
PHP;

$paths = native_mir_test_compile_execute(
    $source,
    'w11p-inline-long-paths.php',
    [2, 9],
    [
        'wave' => 11,
        'function' => 'inline_long_paths',
    ],
);
var_dump($paths['execution']['return_value']);

$overflow = native_mir_test_compile_execute(
    $source,
    'w11p-inline-long-overflow.php',
    [PHP_INT_MAX],
    [
        'wave' => 11,
        'function' => 'inline_long_overflow',
    ],
);
var_dump(
    $overflow['status'],
    is_float($overflow['execution']['return_value']),
);

$binary = native_mir_test_compile_execute(
    $source,
    'w11p-inline-long-binary.php',
    [10],
    [
        'wave' => 11,
        'function' => 'inline_long_binary',
    ],
);
var_dump(
    $binary['execution']['return_value'],
    $binary['execution']['vm_handler_calls'],
    $binary['execution']['execute_ex_calls'],
    $binary['execution']['opline_handler_calls'],
);

$addOverflow = native_mir_test_compile_execute(
    $source,
    'w11p-inline-long-add-overflow.php',
    [PHP_INT_MAX],
    [
        'wave' => 11,
        'function' => 'inline_long_add_overflow',
    ],
);
$subOverflow = native_mir_test_compile_execute(
    $source,
    'w11p-inline-long-sub-overflow.php',
    [PHP_INT_MIN],
    [
        'wave' => 11,
        'function' => 'inline_long_sub_overflow',
    ],
);
var_dump(
    $addOverflow['status'],
    is_float($addOverflow['execution']['return_value']),
    $subOverflow['status'],
    is_float($subOverflow['execution']['return_value']),
);

$typed = native_mir_test_compile_execute(
    $source,
    'w11p-inline-long-typed.php',
    [41],
    [
        'wave' => 11,
        'function' => 'inline_long_typed_add_root',
        'repeat' => 10,
    ],
);
var_dump(
    $typed['execution']['return_value'],
    $typed['execution']['performance']['direct_leaf_scalar_sites'],
    $typed['execution']['vm_handler_calls'],
    $typed['execution']['execute_ex_calls'],
    $typed['execution']['opline_handler_calls'],
);

try {
    native_mir_test_compile_execute(
        $source,
        'w11p-inline-long-typed-overflow.php',
        [PHP_INT_MAX],
        [
            'wave' => 11,
            'function' => 'inline_long_typed_add_root',
        ],
    );
    echo "typed-overflow=missing-error\n";
} catch (TypeError $error) {
    echo "typed-overflow=TypeError\n";
}
?>
--EXPECT--
array(12) {
  [0]=>
  int(3)
  [1]=>
  int(3)
  [2]=>
  int(4)
  [3]=>
  int(8)
  [4]=>
  int(8)
  [5]=>
  int(7)
  [6]=>
  bool(true)
  [7]=>
  bool(true)
  [8]=>
  bool(false)
  [9]=>
  bool(true)
  [10]=>
  bool(false)
  [11]=>
  bool(true)
}
string(8) "accepted"
bool(true)
array(12) {
  [0]=>
  int(11)
  [1]=>
  int(11)
  [2]=>
  int(9)
  [3]=>
  int(10)
  [4]=>
  bool(false)
  [5]=>
  bool(false)
  [6]=>
  bool(true)
  [7]=>
  bool(true)
  [8]=>
  bool(true)
  [9]=>
  bool(false)
  [10]=>
  bool(true)
  [11]=>
  bool(false)
}
int(0)
int(0)
int(0)
string(8) "accepted"
bool(true)
string(8) "accepted"
bool(true)
int(42)
int(0)
int(0)
int(0)
int(0)
typed-overflow=TypeError
