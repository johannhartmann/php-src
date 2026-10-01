--TEST--
Native ?? and &&/|| decide inline in a single branch node
--DESCRIPTION--
COALESCE of a defined non-null temporary moves it into the result, of a
CV copies it with a reference added, and an undefined or null operand
falls through to the default; JMPZ_EX/JMPNZ_EX of a null, boolean or
integer publish their boolean result. References, persistent and other
values take the helper.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class O { public $p = null; public $q = 'q'; }
function run($a, $o, $n) {
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $s = str_repeat('s', $i + 1);
        $r = &$a['ref'];
        $arr = [$i];
        $out[] = ($u ?? 'undef') . '|' . ($s ?? 'x') . '|' . ($a['k'] ?? 'nok') . '|' . ($a['n'] ?? 'null')
            . '|' . ($a['f'] ?? 'f') . '|' . var_export($a['z'] ?? 9, true) . '|' . ($o->p ?? 'op') . '|' . ($o->q ?? 'oq')
            . '|' . ($r ?? 'r') . '|' . implode(',', $arr ?? []) . '|' . (str_repeat('t', $i) ?: 'e') . '|' . (strtoupper($s) ?? 'U');
        unset($r);
    }
    return implode("\n", $out);
}
echo run(['k' => 'v', 'n' => null, 'f' => false, 'z' => 0, 'ref' => 'rv'], new O, 3), "\n";
?>
--EXPECT--
undef|s|v|null||0|op|q|rv|0|e|S
undef|ss|v|null||0|op|q|rv|1|t|SS
undef|sss|v|null||0|op|q|rv|2|tt|SSS
