--TEST--
Assignments under missing integer keys insert into the array
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function descending(int $n) {
    $a = [];
    for ($i = $n - 1; $i >= 0; $i--) { $a[$i] = $i * 2; }
    $a[] = 'next';
    return $a;
}
function gaps(int $n) {
    $a = [0, 1];
    for ($i = 5; $i < $n; $i += 3) { $a[$i] = $i; }
    $a[] = 'next';
    return $a;
}
function negatives(int $n) {
    $a = [];
    for ($i = -1; $i > -$n; $i--) { $a[$i] = $i; }
    $a[] = 'next';
    return $a;
}
function by_reference(array &$a, int $n) {
    for ($i = $n; $i > 0; $i--) { $a[$i * 7] = $i; }
}
function shared(int $n) {
    $a = [10 => 'x'];
    $copies = [];
    for ($i = 0; $i < $n; $i++) {
        $b = $a;
        $a[$i + 20] = $i;
        $copies[] = count($b);
    }
    return [$a, $copies];
}
function literal_keys() {
    $a = [1 => 'a'];
    $a[100] = 'b';
    $a[-5] = 'c';
    $a[PHP_INT_MAX] = 'd';
    return $a;
}
function refcounted_values(int $n) {
    $a = [];
    $s = str_repeat('v', 3);
    for ($i = $n; $i > 0; $i--) { $a[$i] = $s; }
    $s .= 'w';
    return [$a, $s];
}
for ($r = 0; $r < 3; $r++) {
    echo json_encode(descending(6)), "\n";
    echo json_encode(gaps(15)), "\n";
    echo json_encode(negatives(5)), "\n";
    $x = [3 => 'three'];
    $y = &$x;
    by_reference($y, 4);
    echo json_encode($x), "\n";
    echo json_encode(shared(4)), "\n";
    echo json_encode(literal_keys()), "\n";
    echo json_encode(refcounted_values(3)), "\n";
}
?>
--EXPECT--
{"5":10,"4":8,"3":6,"2":4,"1":2,"0":0,"6":"next"}
{"0":0,"1":1,"5":5,"8":8,"11":11,"14":14,"15":"next"}
{"-1":-1,"-2":-2,"-3":-3,"-4":-4,"0":"next"}
{"3":"three","28":4,"21":3,"14":2,"7":1}
[{"10":"x","20":0,"21":1,"22":2,"23":3},[1,2,3,4]]
{"1":"a","100":"b","-5":"c","9223372036854775807":"d"}
[{"3":"vvv","2":"vvv","1":"vvv"},"vvvw"]
{"5":10,"4":8,"3":6,"2":4,"1":2,"0":0,"6":"next"}
{"0":0,"1":1,"5":5,"8":8,"11":11,"14":14,"15":"next"}
{"-1":-1,"-2":-2,"-3":-3,"-4":-4,"0":"next"}
{"3":"three","28":4,"21":3,"14":2,"7":1}
[{"10":"x","20":0,"21":1,"22":2,"23":3},[1,2,3,4]]
{"1":"a","100":"b","-5":"c","9223372036854775807":"d"}
[{"3":"vvv","2":"vvv","1":"vvv"},"vvvw"]
{"5":10,"4":8,"3":6,"2":4,"1":2,"0":0,"6":"next"}
{"0":0,"1":1,"5":5,"8":8,"11":11,"14":14,"15":"next"}
{"-1":-1,"-2":-2,"-3":-3,"-4":-4,"0":"next"}
{"3":"three","28":4,"21":3,"14":2,"7":1}
[{"10":"x","20":0,"21":1,"22":2,"23":3},[1,2,3,4]]
{"1":"a","100":"b","-5":"c","9223372036854775807":"d"}
[{"3":"vvv","2":"vvv","1":"vvv"},"vvvw"]
