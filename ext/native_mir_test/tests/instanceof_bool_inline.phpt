--TEST--
Inline INSTANCEOF through parents, interfaces, references and into CVs; truthiness of strings and arrays, counted temporaries included
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
interface I {} interface J extends I {}
class P implements J {} class C extends P {} class D {}
class Dtor { public function __destruct() { echo "dtor "; } }
function io($v) { $r = $v instanceof I; $s = $v instanceof P; $t = $v instanceof C; return ($r ? 'I' : '-') . ($s ? 'P' : '-') . ($t ? 'C' : '-'); }
function io_ref() { global $g; return $g instanceof J ? 'gJ' : 'g-'; }
function io_cv($v) { $was = new D; $was = $v instanceof P; return var_export($was, true); }
function io_undef() { return $undef instanceof P ? 'u' : 'n'; }
function b($v) { return ((bool) $v ? 'T' : 'F') . (!$v ? 'n' : 'y'); }
function b_tmp($a, $b) { return ((bool) ($a . $b) ? 'T' : 'F') . (!($a . $b) ? 'n' : 'y'); }
function b_arr($a) { return ((bool) array_filter($a) ? 'T' : 'F') . (!array_values($a) ? 'n' : 'y'); }
$vals = [new C, new P, new D, null, 1, 'x', [new C]];
for ($round = 0; $round < 3; $round++) {
    $out = [];
    foreach ($vals as $v) { $out[] = io($v); }
    $g = new C; $out[] = io_ref(); $g = new D; $out[] = io_ref(); $g = null; $out[] = io_ref();
    $out[] = io_cv(new C); $out[] = io_cv(new D);
    foreach (['', '0', '00', '0.0', 'a', ' ', 0, 1, -1, 0.0, -0.0, 0.1, [], [0], [null], null, false, true, new D] as $v) { $out[] = b($v); }
    foreach ([['', ''], ['0', ''], ['', '0'], ['a', 'b'], ['0', '0'], [0, '']] as [$x, $y]) { $out[] = b_tmp($x, $y); }
    $out[] = b_arr([0, 0]); $out[] = b_arr([0, 1]); $out[] = b_arr([]);
    echo implode(' ', $out), "\n";
}
echo io_undef(), "\n";
$k = new Dtor; echo b([$k]) . "\n"; $k = null; echo "end\n";
--EXPECTF--
IPC IP- --- --- --- --- --- gJ g- g- true false Fn Fn Ty Ty Ty Ty Fn Ty Ty Fn Fn Ty Fn Ty Ty Fn Fn Ty Ty Fn Fn Fn Ty Ty Fn Fy Ty Fn
IPC IP- --- --- --- --- --- gJ g- g- true false Fn Fn Ty Ty Ty Ty Fn Ty Ty Fn Fn Ty Fn Ty Ty Fn Fn Ty Ty Fn Fn Fn Ty Ty Fn Fy Ty Fn
IPC IP- --- --- --- --- --- gJ g- g- true false Fn Fn Ty Ty Ty Ty Fn Ty Ty Fn Fn Ty Fn Ty Ty Fn Fn Ty Ty Fn Fn Fn Ty Ty Fn Fy Ty Fn

Warning: Undefined variable $undef in %s on line 8
n
Ty
dtor end
