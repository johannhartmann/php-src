--TEST--
Native CV assignment of literals and over shared collectable values
--DESCRIPTION--
Assigning a literal to a CV copies it from the literal table; overwriting
a CV whose array or object has another owner only drops a reference when
the value is already a GC root candidate or not collectable, and leaves
the root buffering, destruction and cycle collection to the helper
otherwise.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Node { public $next; public $name; function __construct($n) { $this->name = $n; } function __destruct() { echo "~{$this->name} "; } }
function literals($r) {
    $a = [1, 2, 3]; $s = 'lit'; $n = null;
    $ref = &$s; $ref = 'via-ref';
    $a = ['x' => $r];
    return [$a, $s, $n];
}
function shared($r) {
    $keep = [];
    $arr = [$r, [$r]];
    for ($i = 0; $i < 5; $i++) {
        $keep[] = $arr;
        $arr = array_merge($arr, [$i]);     // old array still owned by $keep
    }
    $o = new Node("o$r"); $o->next = $o;    // a cycle
    $holder = $o;
    $o = new Node("p$r");                   // cycle object keeps another owner
    unset($holder);
    $collected = gc_collect_cycles();
    $last = new Node("q$r");
    $last = 1;                              // last owner: destructor now
    return [count($keep), count($arr), $collected, $last];
}
for ($i = 0; $i < 3; $i++) {
    echo json_encode(literals($i)), " ", json_encode(shared($i)), "|\n";
}
?>
--EXPECT--
[{"x":0},"via-ref",null] ~o0 ~q0 ~p0 [5,7,1,1]|
[{"x":1},"via-ref",null] ~o1 ~q1 ~p1 [5,7,1,1]|
[{"x":2},"via-ref",null] ~o2 ~q2 ~p2 [5,7,1,1]|
