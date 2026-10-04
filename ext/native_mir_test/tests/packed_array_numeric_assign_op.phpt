--TEST--
Native x64 compound numeric assignment to packed array elements
--DESCRIPTION--
$a[$k] += / -= / *= $v on an existing number of an owned packed array is
computed inline with the VM's overflow and long/double promotion. Shared
arrays, references, holes, non-numeric operands and errors keep the
helper.
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
function ops(array $y, $v) {
    $r = [];
    for ($i = 0; $i < count($y); $i++) {
        $row = [];
        try { $a = $y; $a[$i] += $v; $row[] = $a[$i]; } catch (Throwable $e) { $row[] = get_class($e); }
        try { $b = $y; $b[$i] -= $v; $row[] = $b[$i]; } catch (Throwable $e) { $row[] = get_class($e); }
        try { $c = $y; $c[$i] *= $v; $row[] = $c[$i]; } catch (Throwable $e) { $row[] = get_class($e); }
        $r[] = $row;
    }
    return $r;
}
function owned(int $n) { $y = array_fill(0, $n, 0); $x = range(1, $n); for ($k = 0; $k < 3; $k++) { for ($i = $n - 1; $i >= 0; $i--) { $y[$i] += $x[$i]; } } return $y; }
function shared() { $y = [1, 2]; $z = $y; for ($i = 0; $i < 2; $i++) { $y[$i] += 10; } return [$y, $z]; }
function by_ref(array &$y) { for ($i = 0; $i < count($y); $i++) { $y[$i] *= 2; } }
function refs() { $x = 1; $y = [&$x, 2]; $y[0] += 5; $y[1] += 5; return [$y, $x]; }
function holes() { $y = [1, 2, 3]; unset($y[1]); @$y[1] += 7; return $y; }
function overflow() { $y = [PHP_INT_MAX, PHP_INT_MIN, 3]; $y[0] += 1; $y[1] -= 1; $y[2] *= PHP_INT_MAX; return $y; }
foreach ([1, 2.5, -3, '4', 'x', null, true, [1]] as $v) { try { var_dump(@ops([1, 1.5, '7', null, PHP_INT_MAX, 'z'], $v)); } catch (Throwable $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; } }
var_dump(owned(5), shared(), refs(), holes(), overflow());
$arr = [1, 2, 3]; by_ref($arr); var_dump($arr);
?>
--EXPECTF--
array(6) {
  [0]=>
  array(3) {
    [0]=>
    int(2)
    [1]=>
    int(0)
    [2]=>
    int(1)
  }
  [1]=>
  array(3) {
    [0]=>
    float(2.5)
    [1]=>
    float(0.5)
    [2]=>
    float(1.5)
  }
  [2]=>
  array(3) {
    [0]=>
    int(8)
    [1]=>
    int(6)
    [2]=>
    int(7)
  }
  [3]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(-1)
    [2]=>
    int(0)
  }
  [4]=>
  array(3) {
    [0]=>
    float(9.223372036854776E+18)
    [1]=>
    int(9223372036854775806)
    [2]=>
    int(9223372036854775807)
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(6) {
  [0]=>
  array(3) {
    [0]=>
    float(3.5)
    [1]=>
    float(-1.5)
    [2]=>
    float(2.5)
  }
  [1]=>
  array(3) {
    [0]=>
    float(4)
    [1]=>
    float(-1)
    [2]=>
    float(3.75)
  }
  [2]=>
  array(3) {
    [0]=>
    float(9.5)
    [1]=>
    float(4.5)
    [2]=>
    float(17.5)
  }
  [3]=>
  array(3) {
    [0]=>
    float(2.5)
    [1]=>
    float(-2.5)
    [2]=>
    float(0)
  }
  [4]=>
  array(3) {
    [0]=>
    float(9.223372036854776E+18)
    [1]=>
    float(9.223372036854776E+18)
    [2]=>
    float(2.305843009213694E+19)
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(6) {
  [0]=>
  array(3) {
    [0]=>
    int(-2)
    [1]=>
    int(4)
    [2]=>
    int(-3)
  }
  [1]=>
  array(3) {
    [0]=>
    float(-1.5)
    [1]=>
    float(4.5)
    [2]=>
    float(-4.5)
  }
  [2]=>
  array(3) {
    [0]=>
    int(4)
    [1]=>
    int(10)
    [2]=>
    int(-21)
  }
  [3]=>
  array(3) {
    [0]=>
    int(-3)
    [1]=>
    int(3)
    [2]=>
    int(0)
  }
  [4]=>
  array(3) {
    [0]=>
    int(9223372036854775804)
    [1]=>
    float(9.223372036854776E+18)
    [2]=>
    float(-2.7670116110564327E+19)
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(6) {
  [0]=>
  array(3) {
    [0]=>
    int(5)
    [1]=>
    int(-3)
    [2]=>
    int(4)
  }
  [1]=>
  array(3) {
    [0]=>
    float(5.5)
    [1]=>
    float(-2.5)
    [2]=>
    float(6)
  }
  [2]=>
  array(3) {
    [0]=>
    int(11)
    [1]=>
    int(3)
    [2]=>
    int(28)
  }
  [3]=>
  array(3) {
    [0]=>
    int(4)
    [1]=>
    int(-4)
    [2]=>
    int(0)
  }
  [4]=>
  array(3) {
    [0]=>
    float(9.223372036854776E+18)
    [1]=>
    int(9223372036854775803)
    [2]=>
    float(3.6893488147419103E+19)
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(6) {
  [0]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [1]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [2]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [3]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [4]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(6) {
  [0]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(1)
    [2]=>
    int(0)
  }
  [1]=>
  array(3) {
    [0]=>
    float(1.5)
    [1]=>
    float(1.5)
    [2]=>
    float(0)
  }
  [2]=>
  array(3) {
    [0]=>
    int(7)
    [1]=>
    int(7)
    [2]=>
    int(0)
  }
  [3]=>
  array(3) {
    [0]=>
    int(0)
    [1]=>
    int(0)
    [2]=>
    int(0)
  }
  [4]=>
  array(3) {
    [0]=>
    int(9223372036854775807)
    [1]=>
    int(9223372036854775807)
    [2]=>
    int(0)
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(6) {
  [0]=>
  array(3) {
    [0]=>
    int(2)
    [1]=>
    int(0)
    [2]=>
    int(1)
  }
  [1]=>
  array(3) {
    [0]=>
    float(2.5)
    [1]=>
    float(0.5)
    [2]=>
    float(1.5)
  }
  [2]=>
  array(3) {
    [0]=>
    int(8)
    [1]=>
    int(6)
    [2]=>
    int(7)
  }
  [3]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(-1)
    [2]=>
    int(0)
  }
  [4]=>
  array(3) {
    [0]=>
    float(9.223372036854776E+18)
    [1]=>
    int(9223372036854775806)
    [2]=>
    int(9223372036854775807)
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(6) {
  [0]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [1]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [2]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [3]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [4]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
  [5]=>
  array(3) {
    [0]=>
    string(9) "TypeError"
    [1]=>
    string(9) "TypeError"
    [2]=>
    string(9) "TypeError"
  }
}
array(5) {
  [0]=>
  int(3)
  [1]=>
  int(6)
  [2]=>
  int(9)
  [3]=>
  int(12)
  [4]=>
  int(15)
}
array(2) {
  [0]=>
  array(2) {
    [0]=>
    int(11)
    [1]=>
    int(12)
  }
  [1]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
}
array(2) {
  [0]=>
  array(2) {
    [0]=>
    int(6)
    [1]=>
    int(7)
  }
  [1]=>
  int(6)
}
array(3) {
  [0]=>
  int(1)
  [2]=>
  int(3)
  [1]=>
  int(7)
}
array(3) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  float(-9.223372036854776E+18)
  [2]=>
  float(2.7670116110564327E+19)
}
array(3) {
  [0]=>
  int(2)
  [1]=>
  int(4)
  [2]=>
  int(6)
}
