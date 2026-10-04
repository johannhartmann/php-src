--TEST--
Native x64 return checks see folded constants and overflowing sums
--DESCRIPTION--
OPcache folds a constant return into VERIFY_RETURN_TYPE with a result; the proven check publishes the literal. The sum of two longs is an exact long only when inference excludes overflow, so a returned double still fails an int return type.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function i7(): int { return 777; }
function sr(): int { return i7() + i7(); }
function Ak(int $m, int $n): int { if ($m == 0) return $n + 1; return 0; }
function w($x) { return Ak(0, $x); }
var_dump(sr(), w(3));
try { var_dump(w(PHP_INT_MAX)); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
$f = fn() => Ak(0, PHP_INT_MAX);
try { var_dump($f()); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECTF--
int(1554)
int(4)
Ak(): Return value must be of type int, float returned
Ak(): Return value must be of type int, float returned
