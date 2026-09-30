--TEST--
Native ternaries of literals through an EncodeGen snippet
--DESCRIPTION--
QM_ASSIGN of literal arrays, strings, null, numbers and bools copies the
literal inline; later writes separate the literal arrays.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function pick($c, $v)
{
    $a = $c ? ['x' => 1, 'y' => [2]] : [];
    $s = $c ? 'yes' : 'no';
    $n = $c ? null : 42;
    $f = $c ? 1.5 : false;
    $d = $v ?: 'default';
    $a['z'] = $s;               // separates the literal array
    $list = [];
    for ($i = 0; $i < 3; $i++) { $list[] = $i % 2 ? 'odd' : ['even' => $i]; }
    return [$a, $s, $n, $f, $d, $list, $c ? PHP_INT_MAX : -1];
}
for ($i = 0; $i < 3; $i++) {
    echo json_encode(pick($i % 2, $i ? "v$i" : '')), "\n";
}
?>
--EXPECTF--
[{"z":"no"},"no",42,false,"default",[{"even":0},"odd",{"even":2}],-1]
[{"x":1,"y":[2],"z":"yes"},"yes",null,1.5,"v1",[{"even":0},"odd",{"even":2}],9223372036854775807]
[{"z":"no"},"no",42,false,"v2",[{"even":0},"odd",{"even":2}],-1]
