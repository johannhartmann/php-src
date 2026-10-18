--TEST--
Native: result-carrying jumps of unoptimized code compile and keep VM semantics
--DESCRIPTION--
Without the OPcache optimizer, ?:, ??, ?-> and the && / || jumps keep both
definitions of their temporary and type inference may leave a jump's result
without a possible value (JMP_SET of a value that is always false). Such a
jump takes the source branch, which writes the result zval, instead of being
rejected; the others keep the machine branch.
--INI--
opcache.enable_cli=0
--FILE--
<?php
function jmp_set_false($n) { $f = false; $x = 0; for ($i = 0; $i < $n; ++$i) { $x = $f ?: $i; } return $x; }
function jmp_set_mixed($n) { $f = 0; $x = 0; for ($i = 0; $i < $n; ++$i) { $x = $f ?: "s$i"; $f = $i % 2 ? 0 : $i; } return $x; }
function coalesce_null($n) { $f = null; $x = 0; for ($i = 0; $i < $n; ++$i) { $x = $f ?? $i * 2; } return $x; }
function coalesce_undef($n) { $x = 0; for ($i = 0; $i < $n; ++$i) { $x = $u ?? $i + 3; } return $x; }
function jmp_null($n) { $o = null; $x = 0; for ($i = 0; $i < $n; ++$i) { $x = $o?->p ?? $i; } return $x; }
function and_false($n) { $f = false; $x = true; for ($i = 0; $i < $n; ++$i) { $x = $f && $i > 1; } return $x; }
function or_true($n) { $t = true; $x = false; for ($i = 0; $i < $n; ++$i) { $x = $t || $i > 1; } return $x; }
function or_false($n) { $f = false; $x = 0; for ($i = 0; $i < $n; ++$i) { $x = $f || $i > 1; } return $x; }
function ternary_int($n) { $z = 0; $x = 0; for ($i = 0; $i < $n; ++$i) { $x = $z ?: $i * 1.5; } return $x; }
foreach (['jmp_set_false', 'jmp_set_mixed', 'coalesce_null', 'coalesce_undef', 'jmp_null', 'and_false', 'or_true', 'or_false', 'ternary_int'] as $f) {
    var_dump($f(0), $f(1), $f(5));
}
?>
--EXPECT--
int(0)
int(0)
int(4)
int(0)
string(2) "s0"
string(2) "s4"
int(0)
int(0)
int(8)
int(0)
int(3)
int(7)
int(0)
int(0)
int(4)
bool(true)
bool(false)
bool(false)
bool(false)
bool(true)
bool(true)
int(0)
bool(false)
bool(true)
int(0)
float(0)
float(6)
