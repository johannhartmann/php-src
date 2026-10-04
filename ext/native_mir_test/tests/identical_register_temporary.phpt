--TEST--
Native === and !== of register-held temporaries and literals inline
--DESCRIPTION--
A temporary held boxed in registers, such as an array or property fetch
result, is stored into its slot before the inline identity comparison
with a literal, which releases it or leaves a last reference to the
helper. Strings, integers, null, doubles and arrays compare as in the VM.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class O { public $kind = 'post'; public $n = 3; public $s; }
function run($a, $o, $n) {
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $k = str_repeat('k', $i % 3);
        $a[$k] = str_repeat('v', $i);
        $o->s = $i % 2 ? str_repeat('ab', 2) : 'abab';
        $out[] = ($a['type'] === 'page' ? 'P' : '-') . ($a['type'] !== 'post' ? 'n' : 'p')
            . ($o->kind === 'post' ? 'K' : '-') . ($o->n === 3 ? '3' : '-') . ($o->s === 'abab' ? 'S' : '-')
            . ($a[$k] === 'vv' ? 'V' : '-') . (($a['missing'] ?? null) === null ? 'N' : '-')
            . ($a['f'] === 1.0 ? 'F' : '-') . ($a['arr'] === [1] ? 'A' : '-');
    }
    return implode(' ', $out);
}
echo run(['type' => 'page', 'f' => 1.0, 'arr' => [1]], new O, 6), "\n";
echo run(['type' => 'post', 'f' => 1, 'arr' => [2]], new O, 3), "\n";
?>
--EXPECT--
PnK3S-NFA PnK3S-NFA PnK3SVNFA PnK3S-NFA PnK3S-NFA PnK3S-NFA
-pK3S-N-- -pK3S-N-- -pK3SVN--
