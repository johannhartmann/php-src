--TEST--
Native: namespaced calls to frameless functions decide from the cache slot
--DESCRIPTION--
ZEND_JMP_FRAMELESS checks once whether the namespaced function exists and
keeps the answer in its run-time cache slot: the global function when it
does not, the namespaced one when it does, also when it is declared after
the first call of another site.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeFrameless {
    function lengths(array $values) {
        $out = [];
        foreach ($values as $value) {
            $out[] = strlen($value) . ':' . in_array($value, ['a', 'bb'], true);
        }
        return implode(',', $out);
    }
    function own(string $value) { return str_contains($value, 'x') ? 'x' : '-'; }
    function str_contains($haystack, $needle) { return 'own'; }
    for ($i = 0; $i < 3; $i++) {
        echo lengths(['a', 'bb', 'ccc']), ' ', own('abc'), "\n";
    }
}
namespace NativeFramelessLate {
    function late($value) { return strtolower($value); }
    echo late('ABC'), "\n";
    if (!function_exists('NativeFramelessLate\strtoupper')) {
        function strtoupper($value) { return 'late:' . $value; }
    }
    function upper($value) { return strtoupper($value); }
    echo upper('abc'), ' ', upper('def'), "\n";
}
?>
--EXPECT--
1:1,2:1,3: x
1:1,2:1,3: x
1:1,2:1,3: x
abc
late:abc late:def
