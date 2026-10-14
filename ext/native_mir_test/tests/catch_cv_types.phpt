--TEST--
Native CV types after a catch block
--DESCRIPTION--
Zend's SSA has no exception edges: after a catch block a CV may hold any
value the try block left in it, also when an instruction that defines it
threw. The native code must not trust a narrower inferred type there.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function h($q) {
    $v = 7;
    try { $v = 2.5; if ($q) { throw new Exception; } $v = 3; } catch (Exception $e) {}
    return $v + 1;
}
function h2($q) {
    $v = "a";
    try { $v = ""; if ($q) { throw new Exception; } $v = "x"; } catch (Exception $e) {}
    if ($v) { return "t"; }
    return "f";
}
function h3($q) {
    $s = "a";
    try { $s = null; if ($q) { throw new Exception; } $s = "x"; } catch (Exception $e) {}
    return strlen($s ?? 'zz');
}
function h4($q) {
    $v = 1;
    try { $v = 5; if ($q) { throw new Exception; } $v = 3; } catch (Exception $e) {}
    $a = [];
    $a[] = $v * 2;
    return $a[0];
}
for ($i = 0; $i < 30; $i++) { $a = h($i % 2); $b = h2($i % 2); $c = h3($i % 2); $d = h4($i % 2);}
function undefined_after_throw() {
    try {
        $null->a = 42;
    } catch (Throwable $e) {
        echo $e::class, "\n";
    }
    var_dump(@$null);
}
function param_after_catch(int $p) {
    try { $p = "0"; throw new Exception; } catch (Exception $e) {}
    if ($p) { return "t"; }
    return "f";
}
undefined_after_throw();
undefined_after_throw();
var_dump($a, $b, $c, $d, param_after_catch(1));
function two_regions($q) {
    $v = 1;
    try { $v = 2; } catch (Exception $e) { $v = "c"; }
    try { $v = 2.5; if ($q) { throw new Exception; } $v = 4; } catch (Exception $e) {}
    return $v + 1;
}
function reference_in_try($q) {
    $a = 1;
    $b = 10;
    try { $a = &$b; if ($q) { throw new Exception; } } catch (Exception $e) {}
    $b = 20;
    return $a + 1;
}
function loop_catch(array $items) {
    $sum = 0;
    $last = 0;
    foreach ($items as $item) {
        try {
            $last = $item * 1.5;
            if ($item > 2) { throw new Exception; }
            $last = $item;
        } catch (Exception $e) {
            $sum += 100;
        }
        $sum += $last;
    }
    return $sum;
}
for ($i = 0; $i < 30; $i++) {
    $r = [two_regions($i % 2), reference_in_try($i % 2), loop_catch([1, 2, 3, 4])];
}
var_dump($r);
?>
--EXPECT--
Error
NULL
Error
NULL
float(3.5)
string(1) "f"
int(2)
int(10)
string(1) "f"
array(3) {
  [0]=>
  float(3.5)
  [1]=>
  int(21)
  [2]=>
  float(213.5)
}
