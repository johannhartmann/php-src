--TEST--
Native foreach fetches and &&/|| results decide through one register
--DESCRIPTION--
foreach over empty, packed and hash arrays (inline fetch), objects,
iterators and generators (helper fetch), with the value used and unused,
and the boolean results of && and || over every scalar kind. The inline
paths and the helper path meet with their decision in RAX; a fetched
value nothing reads is not loaded.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class NativeFetchBag
{
    public $a = 1;
    public $b = 'two';
    public $c = [3];
}

function native_fetch_sum(iterable|object $items): string
{
    $count = 0;
    $sum = 0;
    $last = null;
    foreach ($items as $value) {
        $count++;
        if (is_int($value)) {
            $sum += $value;
        }
        $last = $value;
    }
    return $count . ':' . $sum . ':' . var_export($last, true);
}

function native_fetch_count(iterable|object $items): int
{
    $n = 0;
    foreach ($items as $unused) {
        $n++;
    }
    return $n;
}

function native_fetch_keys(array $items): string
{
    $out = '';
    foreach ($items as $key => $value) {
        $out .= $key . '=' . (is_scalar($value) ? $value : gettype($value)) . ';';
    }
    return $out;
}

function native_fetch_gen(): Generator
{
    yield 5;
    yield 6;
}

function native_fetch_and(mixed $a, mixed $b): string
{
    $x = $a && $b;
    $y = $a || $b;
    return var_export($x, true) . var_export($y, true);
}

$inputs = [
    [],
    [1, 2, 3],
    ['x' => 4, 'y' => 'five', 7 => 6],
    new NativeFetchBag(),
    new ArrayIterator([8, 9]),
];
for ($round = 0; $round < 3; $round++) {
    foreach ($inputs as $input) {
        echo native_fetch_sum($input), ' ', native_fetch_count($input), "\n";
    }
    echo native_fetch_sum(native_fetch_gen()), "\n";
    echo native_fetch_keys(['a' => 1, 2 => [2], 'c' => null, 3 => 'd']), "\n";
    $truth = [];
    foreach ([null, false, true, 0, 7, '', '0', 'a', 1.5, []] as $a) {
        $truth[] = native_fetch_and($a, $round) . native_fetch_and($round, $a);
    }
    echo implode(' ', $truth), "\n";
}
--EXPECT--
0:0:NULL 0
3:6:3 3
3:10:6 3
3:1:array (
  0 => 3,
) 3
2:17:9 2
2:11:6
a=1;2=array;c=NULL;3=d;
falsefalsefalsefalse falsefalsefalsefalse falsetruefalsetrue falsefalsefalsefalse falsetruefalsetrue falsefalsefalsefalse falsefalsefalsefalse falsetruefalsetrue falsetruefalsetrue falsefalsefalsefalse
0:0:NULL 0
3:6:3 3
3:10:6 3
3:1:array (
  0 => 3,
) 3
2:17:9 2
2:11:6
a=1;2=array;c=NULL;3=d;
falsetruefalsetrue falsetruefalsetrue truetruetruetrue falsetruefalsetrue truetruetruetrue falsetruefalsetrue falsetruefalsetrue truetruetruetrue truetruetruetrue falsetruefalsetrue
0:0:NULL 0
3:6:3 3
3:10:6 3
3:1:array (
  0 => 3,
) 3
2:17:9 2
2:11:6
a=1;2=array;c=NULL;3=d;
falsetruefalsetrue falsetruefalsetrue truetruetruetrue falsetruefalsetrue truetruetruetrue falsetruefalsetrue falsetruefalsetrue truetruetruetrue truetruetruetrue falsetruefalsetrue
