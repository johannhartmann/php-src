--TEST--
Native Engine preserves optimized call arguments across TMP reuse
--EXTENSIONS--
opcache
--INI--
opcache.enable_cli=1
opcache.optimization_level=-1
opcache.file_update_protection=0
--FILE--
<?php
$values = [
    'first' => 10,
    'second' => [20],
];

var_dump($values['first'], $values['second']);

$items = [1, 2];
$alias = 'abc';
var_dump(count($items), isset($missing) ? 1 : 4096, strlen($alias));

$_GET = ['global' => 30];
$_POST = ['global' => [40]];
var_dump($_GET, $_POST);

$_SERVER = [
    'REQUEST_URI' => '/frontcontroller.php/index.php/extra',
    'PATH_INFO' => '/extra',
];
var_dump($_SERVER['REQUEST_URI'], $_SERVER['PATH_INFO']);

$indexed = [50, 60];
for ($index = 0; $index < count($indexed); $index++) {
    var_dump($indexed[$index]);
}
?>
--EXPECT--
int(10)
array(1) {
  [0]=>
  int(20)
}
int(2)
int(4096)
int(3)
array(1) {
  ["global"]=>
  int(30)
}
array(1) {
  ["global"]=>
  array(1) {
    [0]=>
    int(40)
  }
}
string(36) "/frontcontroller.php/index.php/extra"
string(6) "/extra"
int(50)
int(60)
