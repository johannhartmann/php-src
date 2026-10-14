--TEST--
Native: a generator keeps a CV it reads back from its own slot across yields
--DESCRIPTION--
The statepoint of a yield materializes the live values. A CV loaded from
its own slot is there already; storing the frame value into it instead
would replace the array with the frame pointer and leak the array.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function gen($array) {
    foreach ($array as $element) {
        yield $element;
    }
    var_dump($array);
}
foreach (gen([1, 2, 3]) as $v) {
    echo $v, "\n";
}
?>
--EXPECT--
1
2
3
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
}
