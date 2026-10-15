--TEST--
Native fast sends of CVs and literals follow the inferred argument types
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function take($a, $b) { return gettype($a) . ":" . (is_array($b) ? count($b) : $b); }
function run(int $n) {
    $int = $n;
    $text = str_repeat("t", $n);
    $list = [$n, $n];
    $ref = $n;
    $alias = &$ref;
    $alias++;
    if ($n > 5) { $maybe = 1; }
    echo take($int, 1), "\n";
    echo take($text, [1, 2, 3]), "\n";
    echo take($list, "lit"), "\n";
    echo take($ref, $alias), "\n";
    echo take(@$maybe, $text), "\n";
    echo take($maybe, null), "\n";
    unset($text);
    return $list;
}
var_dump(run(2));
?>
--EXPECTF--
integer:1
string:3
array:lit
integer:3
NULL:tt

Warning: Undefined variable $maybe in %s on line %d
NULL:
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(2)
}
