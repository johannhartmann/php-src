--TEST--
Array keys that may be integers, doubles, null or booleans
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function read_keys(array $a, int $mode): array {
    $out = [];
    for ($i = 0; $i < 4; $i++) {
        $k = $i;
        if ($mode === 1) { $k = $i + 0.0; }
        if ($mode === 2) { $k = $i === 2 ? null : $i; }
        if ($mode === 3) { $k = $i === 1; }
        $out[] = $a[$k] ?? 'none';
        $v = @$a[$k];
        $out[] = $v;
    }
    return $out;
}
function write_keys(int $mode): array {
    $a = [];
    for ($i = 0; $i < 3; $i++) {
        $k = $i;
        if ($mode === 1) { $k = $i + 0.5; }
        if ($mode === 2) { $k = $i === 1 ? null : $i; }
        $a[$k] = $i * 10;
    }
    return $a;
}
$a = [10, 11, 12, 13, '' => 'empty'];
for ($r = 0; $r < 3; $r++) {
    foreach ([0, 1, 2, 3] as $mode) {
        echo json_encode(read_keys($a, $mode)), "\n";
    }
    foreach ([0, 1, 2] as $mode) {
        echo json_encode(@write_keys($mode)), "\n";
    }
}
?>
--EXPECTF--
[10,10,11,11,12,12,13,13]
[10,10,11,11,12,12,13,13]

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 9
[10,10,11,11,"empty","empty",13,13]
[10,10,11,11,10,10,10,10]
[0,10,20]
[0,10,20]
{"0":0,"":10,"2":20}
[10,10,11,11,12,12,13,13]
[10,10,11,11,12,12,13,13]

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 9
[10,10,11,11,"empty","empty",13,13]
[10,10,11,11,10,10,10,10]
[0,10,20]
[0,10,20]
{"0":0,"":10,"2":20}
[10,10,11,11,12,12,13,13]
[10,10,11,11,12,12,13,13]

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 9
[10,10,11,11,"empty","empty",13,13]
[10,10,11,11,10,10,10,10]
[0,10,20]
[0,10,20]
{"0":0,"":10,"2":20}
