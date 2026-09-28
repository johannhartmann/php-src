--TEST--
Native frameless calls and string appends with precomputed operand offsets
--DESCRIPTION--
Frameless internal calls with literal, CV and temporary arguments and $cv .=
value call their runtime helpers with frame offsets instead of encoded
operands. Undefined CVs, references, typed references and non-string targets
keep VM semantics.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function h($n) { $x = []; for ($i = 1; $i <= $n; $i++) { $x[dechex($i)] = $i; } $c = 0; for ($i = $n; $i > 0; $i--) { if ($x[dechex($i)]) $c++; } return $c; }
function t() { $s = " ab "; $o = [trim($s), strlen("x" . $s), in_array(2, [1, 2]), str_contains("abc", "b"), min(3, 1), max(1.5, 2, -1), implode(",", [1, 2])];
  $u = @dechex($undef); $r = 5; $ref = &$r; $o[] = dechex($ref); return json_encode($o) . $u; }
echo h(300), " ", t(), "\n", t(), "\n";
function cf($n) { $s = ""; $t = "x"; while ($n-- > 0) { $s .= "hello\n"; $s .= $t; $s .= $n; $s .= $n . "!"; } return strlen($s) . md5($s); }
function g() { $u .= "a"; $r = "p"; $ref = &$r; $ref .= "q"; $i = 5; $i .= "z"; $a = [1]; try { $a .= "x"; } catch (Throwable $e) { echo get_class($e); } return "$u $r $i " . gettype($a); }
class T { public string $p = "s"; }
function hh() { $o = new T; $x = &$o->p; $x .= "!"; return $o->p; }
echo cf(50), " ", @g(), " ", hh(), "\n", cf(3), "\n";
?>
--EXPECTF--
300 ["ab",5,true,true,1,2,"1,2","5"]0
["ab",5,true,true,1,2,"1,2","5"]0
5800580edfdb33883fda4fac3814e3de462 a pq 5z string s!
30814ee8da80d8a60cd77cc195cd7b018f
