--TEST--
Native ASSIGN_DIM of array and object values
--DESCRIPTION--
The direct dimension store copies array and object values like
zend_assign_to_variable(): shared arrays, an overwritten object whose
destructor runs, appends, references in the container and an array
assigned into itself (which keeps the helper) keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class O { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n} "; } }
function t($r) {
  $a = ['x' => 1]; $b = [1, 2];
  $a['arr'] = $b; $b[] = 3; $a['obj'] = new O("o$r"); $a['obj'] = new O("p$r");
  $a[] = $a; $c = $a; $c['self'] = $c; $a['arr'][] = 9;
  $d = []; $d['k'] = &$b; $d['k'] = [7];
  $e = [1]; $e[] = $e;
  return [count($a), count($c), $a['arr'], $b, $e, $c['self']['x']];
}
for ($i = 0; $i < 3; $i++) { echo json_encode(t($i)), "|\n"; }
?>
--EXPECT--
~o0 ~p0 [4,5,[1,2,9],[7],[1,[1]],1]|
~o1 ~p1 [4,5,[1,2,9],[7],[1,[1]],1]|
~o2 ~p2 [4,5,[1,2,9],[7],[1,[1]],1]|
