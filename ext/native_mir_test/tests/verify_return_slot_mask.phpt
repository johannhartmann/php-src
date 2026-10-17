--TEST--
VERIFY_RETURN_TYPE tests a returned frame slot against the return mask inline
--DESCRIPTION--
A CV or temporary whose type the declared return type names passes without the
helper; coercion, references, undefined CVs and wrong types take it.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function str(string $s): string { $t = $s . '!'; return $t; }
function arr(array $a): array { $b = $a; $b[] = 1; return $b; }
function flag(int $n): bool { $f = $n > 1; return $f; }
function coerce(string $s): int { $v = $s; return $v; }
function to_float(int $n): float { $v = $n; return $v; }
function nullable(?string $s): ?string { $v = $s; return $v; }
function by_ref(string $s): string { $v = $s; $r = &$v; return $v; }
function wrong(int $n): string { $v = [$n]; return $v; }
function undefined_cv(bool $c): string { if ($c) { $v = 'set'; } return $v; }
function union_ret(int $n): int|string { $v = $n > 1 ? 'big' : $n; return $v; }
function from_tmp(string $s): string { return strtoupper($s); }
for ($i = 0; $i < 3; $i++) {
    var_dump(str("a$i"), arr([$i]), flag($i), coerce("4$i"), to_float($i), nullable($i % 2 ? null : "x"), by_ref("r$i"), union_ret($i), from_tmp("t$i"));
    try { wrong($i); } catch (TypeError $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
    try { var_dump(undefined_cv($i % 2 === 0)); } catch (TypeError $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
}
--EXPECTF--
string(3) "a0!"
array(2) {
  [0]=>
  int(0)
  [1]=>
  int(1)
}
bool(false)
int(40)
float(0)
string(1) "x"
string(2) "r0"
int(0)
string(2) "T0"
TypeError: wrong(): Return value must be of type string, array returned
string(3) "set"
string(3) "a1!"
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(1)
}
bool(false)
int(41)
float(1)
NULL
string(2) "r1"
int(1)
string(2) "T1"
TypeError: wrong(): Return value must be of type string, array returned

Warning: Undefined variable $v in %s on line 10
TypeError: undefined_cv(): Return value must be of type string, null returned
string(3) "a2!"
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(1)
}
bool(true)
int(42)
float(2)
string(1) "x"
string(2) "r2"
string(3) "big"
string(2) "T2"
TypeError: wrong(): Return value must be of type string, array returned
string(3) "set"
