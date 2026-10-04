--TEST--
Native x64 typed bodies that fail repeat the call through the Zend entry
--DESCRIPTION--
A typed body has no frame. An operation that needs Zend semantics (a double returned as int, an integer overflow the plan excludes) returns a zero status through every typed caller, and the outermost Zend-entry call repeats the effect-free call on its canonical path, which throws, coerces or deprecates exactly once with the complete trace.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function Ak(int $m, int $n): int { if ($m == 0) return $n + 1; if ($n == 0) return Ak($m - 1, 1); return Ak($m - 1, Ak($m, $n - 1)); }
function wrapA(int $x): int { return Ak(0, $x) - 1; }
function Au($m, $n): int { if ($m == 0) return $n + 1; if ($n == 0) return Au($m - 1, 1); return Au($m - 1, Au($m, $n - 1)); }
function g($n): int { if ($n < 1) return 1; return g($n - 1) * 2; }
function coerce(int $n): int { if ($n > 100) return $n; return $n * 1.0 + 0; }
function frac(int $n): int { if ($n > 100) return $n; return $n * 1.5 + 0; }
function trace(Throwable $e) {
    echo $e->getMessage(), " |";
    foreach ($e->getTrace() as $frame) echo " ", $frame['function'], "(", implode(",", array_map('json_encode', $frame['args'] ?? [])), ")";
    echo "\n";
}
for ($i = 0; $i < 2; $i++) {
    var_dump(Ak(2, 3), Ak(3, 3), wrapA(5), Au(2, 3), Au(3, 3), g(10), g(62));
    foreach ([fn() => Ak(0, PHP_INT_MAX), fn() => wrapA(PHP_INT_MAX), fn() => Au(0, PHP_INT_MAX), fn() => g(63), fn() => g(64)] as $f) {
        try { var_dump($f()); } catch (TypeError $e) { trace($e); }
    }
    var_dump(coerce(5), frac(3));
}
?>
--EXPECTF--
int(9)
int(61)
int(5)
int(9)
int(61)
int(1024)
int(4611686018427387904)
Ak(): Return value must be of type int, float returned | Ak(0,9223372036854775807) {closure:%s:15}()
Ak(): Return value must be of type int, float returned | Ak(0,9223372036854775807) wrapA(9223372036854775807) {closure:%s:15}()
Au(): Return value must be of type int, float returned | Au(0,9223372036854775807) {closure:%s:15}()
g(): Return value must be of type int, float returned | g(63) {closure:%s:15}()
g(): Return value must be of type int, float returned | g(63) g(64) {closure:%s:15}()

Deprecated: Implicit conversion from float 4.5 to int loses precision in %s on line 7
int(5)
int(4)
int(9)
int(61)
int(5)
int(9)
int(61)
int(1024)
int(4611686018427387904)
Ak(): Return value must be of type int, float returned | Ak(0,9223372036854775807) {closure:%s:15}()
Ak(): Return value must be of type int, float returned | Ak(0,9223372036854775807) wrapA(9223372036854775807) {closure:%s:15}()
Au(): Return value must be of type int, float returned | Au(0,9223372036854775807) {closure:%s:15}()
g(): Return value must be of type int, float returned | g(63) {closure:%s:15}()
g(): Return value must be of type int, float returned | g(63) g(64) {closure:%s:15}()

Deprecated: Implicit conversion from float 4.5 to int loses precision in %s on line 7
int(5)
int(4)
