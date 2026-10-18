--TEST--
Native: an exact scalar result of an internal call into a CV is defined
--DESCRIPTION--
The optimizer writes internal call results straight into CVs
(`CV = DO_ICALL`). The consumers of the CV's SSA variable read the
register scalar that the call result was given, so the call must define
it although no MIR value use exists.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function ints($s) {
    $l = crc32($s);
    var_dump($l);
    $n = strlen($s);
    printf("%d %d\n", $n, $l % 7);
    return $n;
}
function bools($s) {
    $b = is_numeric($s);
    var_dump($b);
    $c = str_contains($s, "1");
    echo json_encode([$b, $c]), "\n";
    return $c;
}
function floats($x) {
    $f = sqrt($x);
    var_dump($f);
    $g = floor($f);
    echo number_format($g, 1), "\n";
    return $f;
}
function loop($a) {
    $sum = 0;
    foreach ($a as $s) {
        $l = strlen($s);
        var_dump($l);
        $sum += $l;
    }
    return $sum;
}
function branch($s) {
    $l = strlen($s);
    if ($l > 2) {
        $l = strtoupper($s);
    }
    var_dump($l);
    $m = max(1, 2);
    var_dump($m);
}
$fp = fopen('php://memory', 'a+');
function write($fp, $format, $args) {
    ftruncate($fp, 0);
    $length = vfprintf($fp, $format, $args);
    rewind($fp);
    $content = stream_get_contents($fp);
    var_dump($content, $length);
}
foreach (['abc', '12', ''] as $s) {
    var_dump(ints($s), bools($s));
}
var_dump(floats(2), floats(16.0));
var_dump(loop(['a', 'bb', 'ccc']));
branch('ab');
branch('abcd');
write($fp, "Foo is %d and %s", [30, 'bar']);
write($fp, "%e %E", [1000, 2e4]);
?>
--EXPECT--
int(891568578)
3 5
bool(false)
[false,false]
int(3)
bool(false)
int(1330857165)
2 1
bool(true)
[true,true]
int(2)
bool(true)
int(0)
0 0
bool(false)
[false,false]
int(0)
bool(false)
float(1.4142135623730951)
1.0
float(4)
4.0
float(1.4142135623730951)
float(4)
int(1)
int(2)
int(3)
int(6)
int(2)
int(2)
string(4) "ABCD"
int(2)
string(17) "Foo is 30 and bar"
int(17)
string(23) "1.000000e+3 2.000000E+4"
int(23)
