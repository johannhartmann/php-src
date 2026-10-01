--TEST--
Native direct frameless calls into a CV result
--DESCRIPTION--
A frameless internal call whose result the optimizer placed in a CV takes
the direct helper with precomputed offsets; the CV is overwritten without
a release, as ZEND_FRAMELESS_ICALL_* overwrites it.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function run($subject, $n) {
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $pos = strpos($subject, 'b');
        $starts = str_starts_with($subject, 'ab');
        $lower = strtolower($subject);
        $part = substr($subject, $i, 2);
        $replaced = str_replace('a', 'x', $subject);
        $trimmed = rtrim($subject . '  ');
        $in = in_array('c', [$i, 'c'], true);
        $out[] = var_export($pos, true) . '|' . var_export($starts, true) . "|$lower|$part|$replaced|$trimmed|" . var_export($in, true);
    }
    $none = strpos($subject, 'zz');
    $out[] = var_export($none, true);
    return implode("\n", $out);
}
echo run('aBcabc', 3), "\n";
?>
--EXPECT--
4|false|abcabc|aB|xBcxbc|aBcabc|true
4|false|abcabc|Bc|xBcxbc|aBcabc|true
4|false|abcabc|ca|xBcxbc|aBcabc|true
false
