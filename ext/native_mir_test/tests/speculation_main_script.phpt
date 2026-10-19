--TEST--
Native: a specialized main script assumes its globals until a boundary
--DESCRIPTION--
With speculation on, a main script with a loop compiles as a specialized
version whose entry checks the type each global held at compile time and
that it is no reference. Only globals that no call, include, diagnostic,
destructor or output handler can reach between a definition and a use
keep the narrowed types; quiet operations and internal math functions are
no boundary. Overflow and a pending interrupt leave for the generic copy.
Callees, error handlers, destructors and output handlers see and change
the globals as in the VM.
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_SPECULATE=1
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
// Longs that stay longs in the specialized main script.
$acc = $argc;
for ($ia = 0; $ia < 1000; $ia++) {
    $acc = ($acc * 3 + ($ia & 7)) & 0x3FFFFFFF;
}
echo $acc, "\n";

// Overflow leaves for the generic copy, which continues in double.
$o = PHP_INT_MAX - 3; $p = PHP_INT_MIN + 2; $m = 3037000499;
for ($io = 0; $io < 6; $io++) { $o = $o + 1; $p--; }
for ($jo = 0; $jo < 2; $jo++) { $m = $m * 3037000499; }
echo $o, " ", $p, " ", $m, "\n";

set_error_handler(function ($no, $msg) {
    echo "handler: $msg, e=", $GLOBALS['e'] ?? '-', "\n";
    $GLOBALS['n'] = 10;
    return true;
});
class D { function __destruct() { echo "dtor: k=", $GLOBALS['k'], "\n"; $GLOBALS['k'] = 100; } }
function bump() { global $x; $x = "changed"; }
function peek() { global $acc, $ia; echo "peek: acc=$acc ia=$ia\n"; }

// Callees see the loop's globals.
peek();

// A callee changes a global between its uses.
$x = 1; $s = 0;
for ($ix = 0; $ix < 5; $ix++) { $s += $x; if ($ix == 2) bump(); $x = 1; }
var_dump($s, $x);

// An error handler changes a global the loop reads.
$e = 0; $n = 3; $t = 0; $a = [1, 2];
for ($e = 0; $e < 3; $e++) { $t += $n; $v = $a[$e + 5]; $t += $n; $n = 3; }
var_dump($t);

// A destructor reads and changes a global.
$d = new D; $k = 1; $u = 0;
for ($ik = 0; $ik < 3; $ik++) { $u += $k; if ($ik == 1) { $d = null; } $u += $k; }
var_dump($u, $k);

// References.
$r = 1; $q = &$r; $w = 0;
for ($ir = 0; $ir < 3; $ir++) { $q = $q + 1; $w += $r; }
var_dump($w, $r);

// Doubles and quiet math functions.
$f = 0.0;
for ($if = 1; $if <= 20; $if++) { $f += sqrt($if) + 1.5 * abs(-$if) + intdiv($if, 3); }
printf("%.6f\n", $f);
?>
--EXPECT--
569837813
9.2233720368548E+18 -9.2233720368548E+18 2.8011385460386E+28
peek: acc=569837813 ia=1000
int(5)
int(1)
handler: Undefined array key 5, e=0
handler: Undefined array key 6, e=1
handler: Undefined array key 7, e=2
int(39)
dtor: k=1
int(303)
int(100)
int(9)
int(4)
439.665978
