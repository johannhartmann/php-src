--TEST--
Native concatenation of temporaries and CV assignment releasing the old value
--DESCRIPTION--
String concatenation follows ZEND_CONCAT: an empty side passes the other
string on, an owned temporary left string is extended in place and a shared
one is copied. Assigning a temporary or a CV to a CV releases the previous
value, running destructors, as zend_assign_to_variable() does.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function id($v) { return $v; }
function run($s, $n) {
    $out = [];
    $a = str_repeat('a', $n + 1) . $s;      // owned temporary left: extended in place
    $b = id($s) . '!';                      // shared temporary left: new string, $s unchanged
    $c = str_repeat('c', $n) . '';          // empty right: the temporary is passed on
    $d = '' . str_repeat('d', $n + 1);      // empty left
    $e = $s . str_repeat('', $n);           // empty temporary right
    $f = id($a) . id($b);                   // two temporaries
    $out[] = "$a|$b|$c|$d|$e|$f|$s";
    $g = $s; $g = $g . 'x'; $h = $g;        // CV over a shared string
    $t = $h; $t = 'y' . $n;                 // CV overwrites its last reference
    $out[] = "$g|$h|$t";
    $u = [$n]; $u = [$n + 1];               // array released by the assignment
    $o = new ArrayObject([1]); $o = null;
    $r = &$s; $r = $r . '?';                // assignment through a reference
    $out[] = json_encode($u) . "|$s";
    return implode(' ', $out);
}
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n} "; } }
function dtor($n) { $v = new D($n); $v = 'replaced' . $n; return $v; }
for ($i = 0; $i < 3; $i++) {
    echo run("s$i", $i), "\n";
    echo dtor($i), "\n";
}
?>
--EXPECT--
as0|s0!||d|s0|as0s0!|s0 s0x|s0x|y0 [1]|s0?
~0 replaced0
aas1|s1!|c|dd|s1|aas1s1!|s1 s1x|s1x|y1 [2]|s1?
~1 replaced1
aaas2|s2!|cc|ddd|s2|aaas2s2!|s2 s2x|s2x|y2 [3]|s2?
~2 replaced2
