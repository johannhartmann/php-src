--TEST--
Native inline internal call frame push across VM stack pages
--DESCRIPTION--
A plain internal call pushes its frame inline; when the VM stack page is
full it calls zend_native_internal_call_push() out of line with every
caller-saved register in use preserved. Deep recursion with internal calls
grows the VM stack across many pages with live values in registers. (The
out-of-line push was validated by forcing it for every call over the
commit tier.)
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function deep($n, $f) {
    if ($n === 0) {
        return 0;
    }
    $s = sprintf('%d', $n);
    $p = str_pad($s, 3, '0', STR_PAD_LEFT);
    return strlen($p) + deep($n - 1, $f * 1.0) + (int) sprintf('%d', $f > 0 ? 0 : 0);
}
foreach ([10, 1000, 5000, 20000] as $depth) {
    echo $depth, ' ', deep($depth, 1.5), "\n";
}
?>
--EXPECT--
10 30
1000 3001
5000 19001
20000 89002
