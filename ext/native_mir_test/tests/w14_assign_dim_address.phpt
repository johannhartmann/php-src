--TEST--
Native ASSIGN_DIM takes its container, key and value by address
--DESCRIPTION--
Literal, CV and temporary keys, appends, a property container reached
through FETCH_OBJ_W, counted and literal values, a separated shared array,
a reference element and a string container take the address form and, where
needed, its general path.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class H { public $a = []; }
function run($k, $v) {
    $a = [];
    $a['lit'] = 1;
    $a[$k] = $v;
    $a[$k . 'x'] = str_repeat('s', 2);
    $a[] = 'appended';
    $a[7] = [1, 2];
    $shared = $a;
    $a['lit'] = 'changed';
    $r = 1;
    $a['ref'] = &$r;
    $a['ref'] = 'through';
    $h = new H;
    $h->a['p'] = $v;
    $h->a[] = 'q';
    $s = 'abc';
    try { $s[] = 'd'; } catch (Error $e) { $s = get_class($e); }
    return json_encode([$a, $shared['lit'], $r, $h->a, $s]);
}
echo run('key', 42), "\n";
echo run('5', 'five'), "\n";
?>
--EXPECT--
[{"lit":"changed","key":42,"keyx":"ss","0":"appended","7":[1,2],"ref":"through"},1,"through",{"p":42,"0":"q"},"Error"]
[{"lit":"changed","5":"five","5x":"ss","6":"appended","7":[1,2],"ref":"through"},1,"through",{"p":"five","0":"q"},"Error"]
