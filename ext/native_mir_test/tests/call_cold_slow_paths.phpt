--TEST--
Native fast call sites keep their rare send and Do paths out of line
--DESCRIPTION--
A fast user call site sends an undefined CV (warning, null), a function
result that is a reference and an unpacked argument list, and its callee
returns normally, throws and violates a scalar return contract: each rare
path leaves the cold code back into the site.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function id($v) { return $v; }
function &ref_of(array &$a) { return $a[0]; }
function sum3($a, $b, $c) { return $a + $b + $c; }
function thrower($v) { if ($v > 1) { throw new RuntimeException("t$v"); } return $v; }
function typed($v): int { return $v; }
function run() {
    $out = [];
    for ($i = 0; $i < 3; $i++) {
        if ($i == 1) {
            $out[] = var_export(id($undefined), true);
        }
        $arr = [$i];
        $out[] = id(ref_of($arr));
        $args = [$i, 1, 2];
        $out[] = sum3(...$args);
        try {
            $out[] = thrower($i);
        } catch (RuntimeException $e) {
            $out[] = $e->getMessage();
        }
        try {
            $out[] = typed($i == 2 ? "5" : $i);
            $out[] = typed($i == 2 ? [] : $i);
        } catch (TypeError $e) {
            $out[] = 'TypeError';
        }
    }
    return implode(',', $out);
}
echo run(), "\n";
?>
--EXPECTF--
Warning: Undefined variable $undefined in %s on line %d
0,3,0,0,0,NULL,1,4,1,1,1,2,5,t2,5,TypeError
