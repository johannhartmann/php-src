--TEST--
Native cached property reads of temporaries, references and dynamic properties
--DESCRIPTION--
The cached FETCH_OBJ_R/IS path also reads through a CV holding a
reference, from a temporary receiver that another reference keeps alive
(released after the read, also into its own slot), and dynamic properties
at their cached bucket or by name. A last reference to a temporary keeps
the general path, which runs the destructor.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
#[AllowDynamicProperties]
class Node { public $name = 'n'; public ?Node $next = null; public $list = [1, 2]; }
class D { public function __destruct() { echo "destruct\n"; } public $v = 'dv'; }
function make() { return new D; }
function run($n) {
    $a = new Node; $a->next = new Node; $a->next->name = 'second'; $a->extra = 'dyn';
    $o = new stdClass; $o->k = 'sk'; $o->m = [3];
    $r = &$a;
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        if ($i == 1) { $o->added = $i; unset($o->k); $o->k = "re$i"; }
        $out[] = $a->next->name . '|' . $r->name . '|' . $a->extra . '|' . $o->k . '|' . count($o->m)
            . '|' . ($o->missing ?? 'none') . '|' . $a->next->list[1] . '|' . isset($a->next->next->name);
    }
    $out[] = make()->v;
    return implode("\n", $out);
}
echo run(3), "\n";
?>
--EXPECT--
destruct
second|n|dyn|sk|1|none|2|
second|n|dyn|re1|1|none|2|
second|n|dyn|re1|1|none|2|
dv
