--TEST--
Native nested compound element assignment updates owned numbers in place
--DESCRIPTION--
$a[$i][$k] op= $v whose container is the array a write fetch's VAR
addresses updates an existing number of an owned packed array inline.
Shared inner arrays separate, references to the inner array or the
element, missing keys and rows, hash rows, overflow, non-numeric elements
and values, objects and properties keep the general path.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function step(array $b, int $n, float $dt): array {
    for ($i = 0; $i < $n; $i++) {
        for ($j = $i + 1; $j < $n; $j++) {
            $d = $b[$i][0] - $b[$j][0];
            $b[$i][1] -= $d * $dt;
            $b[$j][1] += $d * $dt;
            $b[$i][2] *= 2;
            $b[$j][3] += 1;
        }
    }
    return $b;
}
$rows = [[1.0, 0.0, 1, 0], [2.5, 0.0, 3, 5], [4.0, 1.0, 1.5, PHP_INT_MAX - 1]];
$keep = $rows;
var_dump(step($rows, 3, 0.5));
var_dump($keep === $rows);

function edges() {
    $a = [[1, 2], [3, 4]];
    $shared = $a[1];
    $a[1][0] += 10;
    var_dump($shared[0], $a[1][0]);

    $r = &$a[0];
    $a[0][1] -= 5;
    var_dump($r[1]);
    unset($r);

    $e = &$a[0][0];
    $a[0][0] *= 3;
    var_dump($e);
    unset($e);

    $a[0][5] += 1;
    $a[7][0] += 1;
    var_dump($a[0][5], $a[7]);

    $h = ['x' => ['k' => 1.5, 'j' => 2]];
    $h['x']['k'] += 1;
    $h['x']['j'] -= 1;
    var_dump($h);

    $o = [[PHP_INT_MAX], [PHP_INT_MIN], [PHP_INT_MAX]];
    $o[0][0] += 1;
    $o[1][0] -= 1;
    $o[2][0] *= 2;
    var_dump($o);

    $s = [["5", null, "a", [1]]];
    $s[0][0] += 1;
    $s[0][1] += 2;
    try { $s[0][2] += 1; } catch (TypeError $t) { echo $t->getMessage(), "\n"; }
    try { $s[0][3] += 1; } catch (TypeError $t) { echo $t->getMessage(), "\n"; }
    var_dump($s);

    $v = [[1, 2.0]];
    $v[0][0] += "3";
    $v[0][1] -= true;
    $v[0][0] += 0.5;
    var_dump($v);

    $obj = new stdClass;
    $obj->m = [[1, 2]];
    $obj->m[0][1] += 40;
    var_dump($obj->m[0][1]);

    $ao = [new ArrayObject([1, 2])];
    $ao[0][1] += 5;
    var_dump($ao[0][1]);

    $str = ["abc"];
    try { $str[0][0] += 1; } catch (Error $t) { echo get_class($t), ": ", $t->getMessage(), "\n"; }
}
edges();
?>
--EXPECTF--
array(3) {
  [0]=>
  array(4) {
    [0]=>
    float(1)
    [1]=>
    float(2.25)
    [2]=>
    int(4)
    [3]=>
    int(0)
  }
  [1]=>
  array(4) {
    [0]=>
    float(2.5)
    [1]=>
    float(0)
    [2]=>
    int(6)
    [3]=>
    int(6)
  }
  [2]=>
  array(4) {
    [0]=>
    float(4)
    [1]=>
    float(-1.25)
    [2]=>
    float(1.5)
    [3]=>
    float(9.223372036854776E+18)
  }
}
bool(true)
int(3)
int(13)
int(-3)
int(3)

Warning: Undefined array key 5 in %s on line %d

Warning: Undefined array key 7 in %s on line %d

Warning: Undefined array key 0 in %s on line %d
int(1)
array(1) {
  [0]=>
  int(1)
}
array(1) {
  ["x"]=>
  array(2) {
    ["k"]=>
    float(2.5)
    ["j"]=>
    int(1)
  }
}
array(3) {
  [0]=>
  array(1) {
    [0]=>
    float(9.223372036854776E+18)
  }
  [1]=>
  array(1) {
    [0]=>
    float(-9.223372036854776E+18)
  }
  [2]=>
  array(1) {
    [0]=>
    float(1.8446744073709552E+19)
  }
}
Unsupported operand types: string + int
Unsupported operand types: array + int
array(1) {
  [0]=>
  array(4) {
    [0]=>
    int(6)
    [1]=>
    int(2)
    [2]=>
    string(1) "a"
    [3]=>
    array(1) {
      [0]=>
      int(1)
    }
  }
}
array(1) {
  [0]=>
  array(2) {
    [0]=>
    float(4.5)
    [1]=>
    float(1)
  }
}
int(42)
int(7)
Error: Cannot use assign-op operators with string offsets
