--TEST--
Native: a script rewritten with the same content reuses its compiled bundle, closures included
--DESCRIPTION--
A cache file rewritten in every request (as Symfony's PhpFilesAdapter does)
is compiled once; a later version with the same op_arrays attaches the kept
bundle, which holds its closures, and a version with other content compiles
again.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$file = __DIR__ . '/content_keyed_bundle.inc';
$versions = [
    '<?php $k = 3; return [PHP_INT_MAX, static function () use ($k) { $f = fn($x) => $x * $k; return array_map($f, [1, 2, $k]); }];',
    '<?php $k = 3; return [PHP_INT_MAX, static function () use ($k) { $f = fn($x) => $x * $k; return array_map($f, [1, 2, $k]); }];',
    '<?php $k = 4; return [PHP_INT_MAX, static function () use ($k) { $f = fn($x) => $x * $k; return array_map($f, [1, 2, $k]); }];',
    '<?php $k = 3; return [PHP_INT_MAX, static function () use ($k) { $f = fn($x) => $x * $k; return array_map($f, [1, 2, $k]); }];',
];
foreach ($versions as $source) {
    file_put_contents($file, $source);
    opcache_invalidate($file, true);
    $entry = require $file;
    echo implode(',', $entry[1]()), "\n";
}
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/content_keyed_bundle.inc');
?>
--EXPECT--
3,6,9
3,6,9
4,8,16
3,6,9
