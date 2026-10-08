--TEST--
Tier 2 copies: element reads of changing types, isset reuse, references, ?? and warnings
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_TIER2_THRESHOLD=20
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = sys_get_temp_dir() . '/tier2_speculation_' . getmypid();
@mkdir($dir);
$lib = $dir . '/lib.php';
file_put_contents($lib, <<<'LIB'
<?php
function pick($a) { $x = $a['v']; return $x . '|'; }
function opt($a, $k) { $v = $a[$k] ?? 'none'; return is_array($v) ? 'array' . count($v) : var_export($v, true); }
function sum_first($rows) { $t = 0; foreach ($rows as $row) { $t += $row[0]; } return $t; }
function cached($path) {
    $path = (string) $path;
    static $cache = array();
    if (isset($cache[$path])) {
        return $cache[$path];
    }
    $original = $path;
    $path = str_replace('\\', '/', $path);
    $cache[$original] = $path;
    return $cache[$original];
}
function between($a, $b, $k) {
    $has = isset($a[$k]);
    $mid = $b['m'];
    return ($has ? $a[$k] : '-') . '/' . $mid;
}
function warn($a) { return $a['missing'] . '.'; }
LIB);
require $lib;
$ref = 'r';
$out = [];
for ($i = 0; $i < 40; $i++) {
    $out[] = pick(['v' => "s$i"]);
    $out[] = opt(['a' => "x$i"], $i % 2 ? 'a' : 'b');
    $out[] = sum_first([[1], [$i]]);
    $out[] = cached("a\\b$i");
    $out[] = between(['k' => 'K'], ['m' => 'M'], 'k');
}
echo md5(implode(',', $out)), "\n";
/* Other types: each read deoptimizes until its copy retires. */
$arr = [1, 2];
$r = [];
for ($i = 0; $i < 40; $i++) {
    $r[] = pick(['v' => $i % 3 ? 1.5 : 7]);
    $r[] = opt(['a' => $i % 2 ? [1, 2] : null, 'b' => $i], $i % 3 ? 'a' : 'b');
    $r[] = sum_first([[1.5], ["7"]]);
    $r[] = cached(1000 + $i % 5);
    $r[] = between(['k' => $i], ['m' => $i % 2 ? null : 3], $i % 2 ? 'k' : 'z');
}
$r[] = pick(['v' => $arr]);
$r[] = pick(['v' => &$ref]);
$ref = 'changed';
$r[] = pick(['v' => &$ref]);
echo implode(',', array_slice($r, 0, 12)), "\n";
echo md5(implode(',', $r)), "\n";
for ($i = 0; $i < 5; $i++) { echo warn(['missing' => 'x']), "\n"; }
echo warn([]), "\n";
try {
    sum_first([[new stdClass]]);
} catch (TypeError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
unlink($lib); rmdir($dir);
--EXPECTF--
5d59329588f82aaae64690a69bf07a50

Warning: Array to string conversion in %slib.php on line 2
7|,0,8.5,1000,-/3,1.5|,array2,8.5,1001,1/,1.5|,'none'
8502d9a930de553513c30bcd55b1da5c
x.
x.
x.
x.
x.

Warning: Undefined array key "missing" in %slib.php on line 21
.
TypeError: Unsupported operand types: int + stdClass
