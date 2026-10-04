--TEST--
Native direct internal calls of plain sites check only run-time facts
--DESCRIPTION--
A direct internal call whose descriptor the compiler found plain takes a
lean helper that checks only what can change at run time (observers, a
pending exception, an undefined CV argument, a VAR that is no IS_INDIRECT
property) and otherwise falls back to the general direct call. By-value
literals, CVs and temporaries, by-reference CVs and properties behave as
in the VM.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class Holder { public $list = [3, 1, 2]; }
function run($flag) {
    $h = new Holder;
    $out = [];
    for ($i = 0; $i < 2; $i++) {
        $out[] = str_repeat("ab", 2);
        if ($flag) { $u = "x"; }
        $out[] = @str_repeat((string) $u, 2) . '|' . strlen("$i");
        $arr = [$i];
        array_push($arr, 5, 6);
        sort($h->list);
        $out[] = implode(',', $arr) . ' ' . implode(',', $h->list) . ' ' . count($arr);
    }
    return implode("\n", $out);
}
echo run(false), "\n", run(true), "\n";
?>
--EXPECT--
abab
|1
0,5,6 1,2,3 3
abab
|1
1,5,6 1,2,3 3
abab
xx|1
0,5,6 1,2,3 3
abab
xx|1
1,5,6 1,2,3 3
