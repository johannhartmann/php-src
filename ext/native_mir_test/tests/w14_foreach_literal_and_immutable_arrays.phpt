--TEST--
Native x64 foreach over literal and immutable arrays
--DESCRIPTION--
OPcache propagates literal arrays into FE_RESET_R, and arrays assigned from
literals are immutable. The x64 reset copies them without a reference and
FE_FREE skips their release; counted arrays, empty arrays, early exits,
objects and by-reference iteration must keep VM results.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function literal_sum(int $n) {
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        foreach ([1, 2, 3, 4] as $v) {
            $r += $v;
        }
    }
    return $r;
}
function empty_literal(int $n) {
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        foreach ([] as $v) {
            $r += $v;
        }
        $r++;
    }
    return $r;
}
function immutable_cv(int $n) {
    $a = ['x' => 1, 'y' => 2];
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        foreach ($a as $v) {
            $r += $v;
        }
    }
    return $r;
}
function counted_cv(int $n) {
    $a = range(1, 3);
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        foreach ($a as $v) {
            $a[] = $v;
            $r += $v;
        }
    }
    return [$r, count($a)];
}
function literal_break() {
    $out = [];
    foreach (['a', 'b', 'c'] as $v) {
        if ($v === 'b') {
            break;
        }
        $out[] = $v;
    }
    return $out;
}
function literal_throw() {
    try {
        foreach ([1, 2, 3] as $v) {
            if ($v === 2) {
                throw new Exception("at $v");
            }
        }
    } catch (Exception $e) {
        return $e->getMessage();
    }
}
function nested_literals() {
    $out = '';
    foreach ([1, 2] as $a) {
        foreach (['x', 'y'] as $b) {
            $out .= $a . $b;
        }
    }
    return $out;
}
function object_iteration() {
    $r = '';
    foreach (new ArrayIterator(['p' => 1, 'q' => 2]) as $k => $v) {
        $r .= "$k$v";
    }
    foreach ((object) ['m' => 3] as $k => $v) {
        $r .= "$k$v";
    }
    return $r;
}
function by_reference() {
    $a = [1, 2, 3];
    foreach ($a as &$v) {
        $v *= 10;
    }
    unset($v);
    return $a;
}
for ($round = 0; $round < 3; $round++) {
    var_dump(literal_sum(5), empty_literal(4), immutable_cv(3), counted_cv(2));
    var_dump(literal_break(), literal_throw(), nested_literals());
    var_dump(object_iteration(), by_reference());
}
?>
--EXPECT--
int(50)
int(4)
int(9)
array(2) {
  [0]=>
  int(18)
  [1]=>
  int(12)
}
array(1) {
  [0]=>
  string(1) "a"
}
string(4) "at 2"
string(8) "1x1y2x2y"
string(6) "p1q2m3"
array(3) {
  [0]=>
  int(10)
  [1]=>
  int(20)
  [2]=>
  int(30)
}
int(50)
int(4)
int(9)
array(2) {
  [0]=>
  int(18)
  [1]=>
  int(12)
}
array(1) {
  [0]=>
  string(1) "a"
}
string(4) "at 2"
string(8) "1x1y2x2y"
string(6) "p1q2m3"
array(3) {
  [0]=>
  int(10)
  [1]=>
  int(20)
  [2]=>
  int(30)
}
int(50)
int(4)
int(9)
array(2) {
  [0]=>
  int(18)
  [1]=>
  int(12)
}
array(1) {
  [0]=>
  string(1) "a"
}
string(4) "at 2"
string(8) "1x1y2x2y"
string(6) "p1q2m3"
array(3) {
  [0]=>
  int(10)
  [1]=>
  int(20)
  [2]=>
  int(30)
}
