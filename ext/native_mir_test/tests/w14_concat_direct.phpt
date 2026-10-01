--TEST--
Native direct concatenation of literal, CV and temporary operands
--DESCRIPTION--
ZEND_CONCAT and ZEND_FAST_CONCAT into a temporary pass precomputed operand
kinds and offsets to a helper that concatenates two strings without
decoding the operation: an empty side passes the other string on, an owned
temporary is extended, temporaries are released. Other operand types take
the general path.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class S { function __toString(): string { return 'obj'; } }
function run($a, $b, $n) {
    $out = [];
    $e = '';
    for ($i = 0; $i < $n; $i++) {
        $t = str_repeat('t', $i);
        $out[] = $a . 'lit' . '|' . 'lit' . $b . '|' . $a . $b . '|' . $t . $a . '|' . $e . $b . '|' . $a . $e
            . '|' . strtoupper($a) . strtolower($t) . '|' . $i . $a . '|' . $a . 1.5 . '|' . (new S) . $a . '|' . $a . null;
    }
    return implode("\n", $out);
}
echo run('A', 'b', 3), "\n";
?>
--EXPECT--
Alit|litb|Ab|A|b|A|A|0A|A1.5|objA|A
Alit|litb|Ab|tA|b|A|At|1A|A1.5|objA|A
Alit|litb|Ab|ttA|b|A|Att|2A|A1.5|objA|A
