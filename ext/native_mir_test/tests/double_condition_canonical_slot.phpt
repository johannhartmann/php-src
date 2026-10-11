--TEST--
Native conditions on double CVs read a current slot
--DESCRIPTION--
A JMPZ, JMPNZ, JMPZ_EX or JMPNZ_EX on an exact double is evaluated from its canonical zval, like zend_mir_machine_condition() decides; the lowering therefore keeps that CV's slot current instead of leaving the value only in a register.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function g($a, $b) { $f = $b ? 1.5 : 0.0; return ($f && $a) ? 'fa' : 'nf'; }
function h($a, $b) { $f = $b ? 1.5 : 0.0; if ($f) { return 'y'; } return 'n'; }
function or_($a, $b) { $f = $b ? -0.0 : 2.5; return ($f || $a) ? 'o' : 'x'; }
function tern($b) { $f = $b ? 1.5 : 0.0; return $f ? 1 : 2; }
function nan_($b) { $f = $b ? NAN : 0.0; return $f ? 'nan-true' : 'zero-false'; }
function mul(float $x) { $f = $x * 0.5; if ($f) { return "m$f"; } return 'mz'; }
function div(int $x) { $f = $x / 4; while ($f) { return "d$f"; } return 'dz'; }
function loop($n) { $f = 1.0; $c = 0; while ($f) { $f = $c++ < $n ? $f / 2 : 0.0; } return $c; }
function nested($a, $b) { $f = $a ? 0.25 : 0.0; $g = $b ? 0.0 : 3.0; if ($f && $g) return 'both'; if ($f || $g) return 'one'; return 'none'; }
function neg($b) { $f = $b ? 1.5 : 0.0; if (!$f) { return 'neg'; } return 'pos'; }
function jmpnz($b) { $f = $b ? 0.0 : 7.0; do { if ($f) break; return 'zero'; } while (false); return 'nonzero'; }
function intf($b) { $i = $b ? 3 : 0; $f = $b ? 1.5 : 0.0; return ($i && $f) ? 'if' : (($i || $f) ? 'i|f' : 'none'); }
foreach ([[1, 1], [1, 0], [0, 1], [0, 0]] as [$a, $b]) {
    var_dump(g($a, $b), h($a, $b), or_($a, $b), tern($b), nan_($b), nested($a, $b), neg($b), jmpnz($b), intf($b));
}
var_dump(mul(3.0), mul(0.0), mul(-0.0), div(2), div(0), loop(3), loop(0));
?>
--EXPECTF--

Warning: unexpected NAN value was coerced to bool in %s on line 6
string(2) "fa"
string(1) "y"
string(1) "o"
int(1)
string(8) "nan-true"
string(3) "one"
string(3) "pos"
string(4) "zero"
string(2) "if"
string(2) "nf"
string(1) "n"
string(1) "o"
int(2)
string(10) "zero-false"
string(4) "both"
string(3) "neg"
string(7) "nonzero"
string(4) "none"

Warning: unexpected NAN value was coerced to bool in %s on line 6
string(2) "nf"
string(1) "y"
string(1) "x"
int(1)
string(8) "nan-true"
string(4) "none"
string(3) "pos"
string(4) "zero"
string(2) "if"
string(2) "nf"
string(1) "n"
string(1) "o"
int(2)
string(10) "zero-false"
string(3) "one"
string(3) "neg"
string(7) "nonzero"
string(4) "none"
string(4) "m1.5"
string(2) "mz"
string(2) "mz"
string(4) "d0.5"
string(2) "dz"
int(4)
int(1)
