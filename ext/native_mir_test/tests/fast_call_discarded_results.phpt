--TEST--
Native fast calls release discarded counted results and keep scalar ones
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __destruct() { echo "destroyed\n"; } }
function scalar(int $n) { return $n + 1; }
function text(int $n) { return str_repeat("x", $n); }
function object(int $n) { return new D; }
function maybe(int $n) { if ($n > 1) { return [$n]; } return $n; }
function run() {
    for ($i = 0; $i < 3; $i++) {
        scalar($i);
        text($i + 3);
        maybe($i);
    }
    object(1);
    echo "after\n";
    var_dump(scalar(1), text(2), maybe(3));
}
run();
?>
--EXPECT--
destroyed
after
int(2)
string(2) "xx"
array(1) {
  [0]=>
  int(3)
}
