--TEST--
String offset reads with integer offsets in the read helper's fast path
--DESCRIPTION--
$string[$index] with an integer offset inside the string, also negative and
on a temporary string, yields the one-character string without the general
dimension path; an offset outside warns and ?? stays silent as in stock PHP.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class P { public $html = '<p class="x">'; function at($i) { return $this->html[$i]; } }
function chars(string $s) { $o = ''; for ($i = 0; $i < strlen($s); $i++) { $o .= $s[$i] . $s[-1 - $i]; } return $o; }
function tmp(string $a, string $b, int $i) { return ($a . $b)[$i]; }
$p = new P;
for ($r = 0; $r < 3; $r++) {
    echo chars('abc'), ' ', $p->at(1), $p->at(-1), ' ', tmp('ab', 'cd', 2), tmp('ab', 'cd', -4), "\n";
    $s = 'xyz';
    echo $s[5] ?? 'none', ' ', var_export($s[3] ?? null, true), "\n";
    echo '[', $s[9], "]\n";
}
?>
--EXPECTF--
acbbca p> ca
none NULL
[
Warning: Uninitialized string offset 9 in %s on line 10
]
acbbca p> ca
none NULL
[
Warning: Uninitialized string offset 9 in %s on line 10
]
acbbca p> ca
none NULL
[
Warning: Uninitialized string offset 9 in %s on line 10
]
