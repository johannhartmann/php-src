--TEST--
Inline array probes compare string keys of equal hash by content
--DESCRIPTION--
Keys built at runtime are other strings than the interned or runtime keys
the array was built with. The inline probe decides them by content up to
16 bytes (below 8 bytes past the key's end), leaves longer ones to the
helper and keeps numeric strings, absent keys and keys that differ only
after the eighth byte exact.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function probe(array $a, string $k) {
    return [isset($a[$k]), $a[$k] ?? '-', array_key_exists($k, $a), empty($a[$k])];
}
function build(int $n, string $c) { $s = ''; for ($i = 0; $i < $n; $i++) { $s .= $c; } return $s; }
$a = [];
foreach ([1, 3, 7, 8, 9, 15, 16, 17, 20] as $n) { $a[build($n, 'k')] = $n; }
$a['abcdefgh1'] = 'x1'; $a['abcdefgh2'] = 'x2'; $a['12'] = 'int'; $a['zero'] = 0;
$out = [];
for ($round = 0; $round < 3; $round++) {
    foreach ([1, 3, 7, 8, 9, 15, 16, 17, 20, 2, 18] as $n) { $out[] = probe($a, build($n, 'k')); }
    foreach (['abcdefgh' . '1', 'abcdefgh' . '2', 'abcdefgh' . '3', '1' . '2', 'ze' . 'ro', 'zer' . 'o!'] as $k) { $out[] = probe($a, $k); }
}
echo md5(json_encode($out)), "\n", json_encode(array_slice($out, 0, 17)), "\n";
?>
--EXPECT--
6e9b9b856776a30856cbd5e50b3d7847
[[true,1,true,false],[true,3,true,false],[true,7,true,false],[true,8,true,false],[true,9,true,false],[true,15,true,false],[true,16,true,false],[true,17,true,false],[true,20,true,false],[false,"-",false,true],[false,"-",false,true],[true,"x1",true,false],[true,"x2",true,false],[false,"-",false,true],[true,"int",true,false],[true,0,true,true],[false,"-",false,true]]
