--TEST--
list() element reads into CVs and from call results without decoding
--DESCRIPTION--
[$a, $b] = $array writes existing elements straight into the target CVs,
releasing their old values afterwards (destructors run), and reads from
the VAR of a call result; referenced targets, missing elements and a
target that is the container keep the general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __construct(public $n) {} function __destruct() { echo "~{$this->n};"; } }
function pair($k) { return ["x$k", "y$k"]; }
function run($k) {
    $a = new D("a$k"); $b = 'old';
    [$a, $b] = pair($k);
    ['p' => $p, 'q' => $q] = ['p' => $k, 'q' => [$k]];
    $r = 1; $ref = &$r; [$ref, $s] = [10 + $k, 's'];
    $self = [5, 6]; [$self, $t] = $self;
    [, $second] = explode(',', "u,v$k");
    [$m1, $m2] = [1];
    return json_encode([$a, $b, $p, $q, $r, $s, $self, $t, $second, $m1, $m2]);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
?>
--EXPECTF--
~a0;
Warning: Undefined array key 1 in %s on line 11
["x0","y0",0,[0],10,"s",5,6,"v0",1,null]
~a1;
Warning: Undefined array key 1 in %s on line 11
["x1","y1",1,[1],11,"s",5,6,"v1",1,null]
~a2;
Warning: Undefined array key 1 in %s on line 11
["x2","y2",2,[2],12,"s",5,6,"v2",1,null]
