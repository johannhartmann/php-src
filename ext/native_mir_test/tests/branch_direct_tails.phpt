--TEST--
Native namespaced frameless branches, foreach resets and boxed conditions branch directly
--DESCRIPTION--
A namespaced frameless call (shadowed or not) and FE_RESET without PHI
successors jump straight to their blocks and decide the helper's outcome
in the cold area; &&, || and if over nulls, booleans, integers, strings,
arrays, objects and doubles decide inline or in the helper as the VM does.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
namespace App;

function str_contains($h, $n) { return 'shadow'; }

function frameless_ns($s) {
    $out = [];
    $out[] = strlen($s);
    $out[] = str_contains($s, 'b');
    $out[] = in_array($s, ['ab', 'cd']) ? 'in' : 'out';
    $out[] = str_replace('a', 'z', $s);
    return implode(',', $out);
}

function loops($arrays) {
    $total = 0;
    $seen = [];
    foreach ($arrays as $key => $arr) {
        foreach ($arr as $k => $v) {
            $total += is_int($v) ? $v : 1;
            $seen[] = $k;
        }
    }
    foreach ([] as $never) { $total = -1; }
    foreach (new \ArrayIterator([5, 6]) as $v) { $total += $v; }
    return $total . ':' . implode(',', $seen);
}

function conds($a, $b, $c) {
    $r = [];
    $x = $a && $b;
    $y = $a || $c;
    $z = ($a && $b) || ($c && !$a);
    $r[] = var_export([$x, $y, $z], true);
    $s = is_array($a) ? 'arr' : $a . '';
    if ($s && $b) { $r[] = 'sb'; }
    $obj = $c ? new \stdClass : null;
    if ($obj && $a) { $r[] = 'oa'; }
    $f = $b ? 1.5 : null;
    $r[] = ($f && $a) ? 'fa' : 'nf';
    return str_replace(["\n", ' '], '', implode('|', $r));
}

foreach (['ab', 'xyz', '', 'b'] as $s) { echo frameless_ns($s), "\n"; }
echo loops([[1, 2], [], ['a' => 'x', 'b' => 3], [[1]]]), "\n";
$vals = [null, false, true, 0, 1, '', '0', 'a', [], [1], 0.0, 2.5];
foreach ($vals as $a) {
    foreach ([true, 0, 'x'] as $b) {
        echo conds($a, $b, $a), "\n";
    }
}
--EXPECT--
2,shadow,in,zb
3,shadow,out,xyz
0,shadow,out,
1,shadow,out,b
19:0,1,a,b,0
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>true,2=>false,)|oa|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>true,2=>false,)|oa|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>true,2=>false,)|oa|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>false,2=>false,)|sb|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|sb|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>true,2=>false,)|oa|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>false,1=>false,2=>false,)|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
array(0=>false,1=>true,2=>false,)|oa|nf
array(0=>true,1=>true,2=>true,)|sb|oa|fa
