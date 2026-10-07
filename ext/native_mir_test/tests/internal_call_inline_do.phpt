--TEST--
Plain internal calls finish inline: counted and shared arguments, unused and temporary results, values live across the call, notices, exceptions and throwing destructors
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($a, $s, $n) {
    $r = [];
    $r[] = count($a);
    $r[] = array_key_last($a);
    $r[] = sprintf('%05d-%s', $n, $s);
    $r[] = implode('|', array_map('strtoupper', $a));
    $r[] = json_encode(['n' => $n]);
    $r[] = max($n, 3) + min(1, $n);
    array_push($a, $n);
    $r[] = count($a);
    $r[] = str_pad($s, 6, '*');
    $r[] = ucfirst($s);
    $r[] = number_format($n * 1000.5, 2);
    $r[] = gettype($a);
    return implode(',', $r);
}
$t = '';
for ($i = 0; $i < 30; $i++) {
    $t .= f(['a', 'b', "c$i"], "s$i", $i) . "\n";
}
echo $t;
function g($x) { return intdiv(10, $x); }
try {
    g(0);
} catch (DivisionByZeroError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
function h() { trigger_error('note', E_USER_NOTICE); return 1; }
echo h(), "\n";
class D { function __destruct() { throw new Exception('dtor'); } }
function k() { return count([new D]); }
try {
    echo k(), "\n";
} catch (Exception $e) {
    echo 'caught ', $e->getMessage(), "\n";
}
function only($s) { strtoupper($s . '!'); return count_chars($s . 'x', 3); }
echo only('abc'), "\n";
function shared($a) { $b = $a; return count($a) + count($b); }
echo shared([1, 2, 3]), "\n";
function looped(int $n) {
    $a = range(1, 3);
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        foreach ($a as $v) {
            $a[] = $v;
            $r += $v;
        }
    }
    return $r . '/' . count($a);
}
echo looped(2), ' ', looped(3), "\n";
--EXPECTF--
3,2,00000-s0,A|B|C0,{"n":0},3,4,s0****,S0,0.00,array
3,2,00001-s1,A|B|C1,{"n":1},4,4,s1****,S1,1,000.50,array
3,2,00002-s2,A|B|C2,{"n":2},4,4,s2****,S2,2,001.00,array
3,2,00003-s3,A|B|C3,{"n":3},4,4,s3****,S3,3,001.50,array
3,2,00004-s4,A|B|C4,{"n":4},5,4,s4****,S4,4,002.00,array
3,2,00005-s5,A|B|C5,{"n":5},6,4,s5****,S5,5,002.50,array
3,2,00006-s6,A|B|C6,{"n":6},7,4,s6****,S6,6,003.00,array
3,2,00007-s7,A|B|C7,{"n":7},8,4,s7****,S7,7,003.50,array
3,2,00008-s8,A|B|C8,{"n":8},9,4,s8****,S8,8,004.00,array
3,2,00009-s9,A|B|C9,{"n":9},10,4,s9****,S9,9,004.50,array
3,2,00010-s10,A|B|C10,{"n":10},11,4,s10***,S10,10,005.00,array
3,2,00011-s11,A|B|C11,{"n":11},12,4,s11***,S11,11,005.50,array
3,2,00012-s12,A|B|C12,{"n":12},13,4,s12***,S12,12,006.00,array
3,2,00013-s13,A|B|C13,{"n":13},14,4,s13***,S13,13,006.50,array
3,2,00014-s14,A|B|C14,{"n":14},15,4,s14***,S14,14,007.00,array
3,2,00015-s15,A|B|C15,{"n":15},16,4,s15***,S15,15,007.50,array
3,2,00016-s16,A|B|C16,{"n":16},17,4,s16***,S16,16,008.00,array
3,2,00017-s17,A|B|C17,{"n":17},18,4,s17***,S17,17,008.50,array
3,2,00018-s18,A|B|C18,{"n":18},19,4,s18***,S18,18,009.00,array
3,2,00019-s19,A|B|C19,{"n":19},20,4,s19***,S19,19,009.50,array
3,2,00020-s20,A|B|C20,{"n":20},21,4,s20***,S20,20,010.00,array
3,2,00021-s21,A|B|C21,{"n":21},22,4,s21***,S21,21,010.50,array
3,2,00022-s22,A|B|C22,{"n":22},23,4,s22***,S22,22,011.00,array
3,2,00023-s23,A|B|C23,{"n":23},24,4,s23***,S23,23,011.50,array
3,2,00024-s24,A|B|C24,{"n":24},25,4,s24***,S24,24,012.00,array
3,2,00025-s25,A|B|C25,{"n":25},26,4,s25***,S25,25,012.50,array
3,2,00026-s26,A|B|C26,{"n":26},27,4,s26***,S26,26,013.00,array
3,2,00027-s27,A|B|C27,{"n":27},28,4,s27***,S27,27,013.50,array
3,2,00028-s28,A|B|C28,{"n":28},29,4,s28***,S28,28,014.00,array
3,2,00029-s29,A|B|C29,{"n":29},30,4,s29***,S29,29,014.50,array
DivisionByZeroError: Division by zero

Notice: note in %s on line 29
1
caught dtor
abcx
6
18/12 42/24
