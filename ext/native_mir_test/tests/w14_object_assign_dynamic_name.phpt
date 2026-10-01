--TEST--
Native ASSIGN_OBJ of string names without decoding the operation
--DESCRIPTION--
$this->$name = $value and $object->name = $value with an unused result
call the object's write_property handler directly with the operands, as
the general path does: references are dereferenced, temporaries consumed,
typed properties coerce or throw, __set and readonly behave as before, and
non-string names, undefined values and non-object receivers take the
general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
#[AllowDynamicProperties]
class Post { public $ID = 0; public string $title = ''; public int $count = 0;
    function __construct($data) { foreach ($data as $key => $value) { $this->$key = $value; } }
}
class Magic { public $log = []; function __set($n, $v) { $this->log[] = "$n=" . json_encode($v); } }
class RO { public function __construct(public readonly int $id) {} function poke($k) { try { $this->$k = 5; } catch (Error $e) { return get_class($e); } return 'ok'; } }
function build($i) {
    $ref = "r$i"; $alias = &$ref;
    $p = new Post(['ID' => $i, 'title' => "t$i", 'count' => "4$i", 'extra' => [$i], 'alias' => $alias, 'tmp' => str_repeat('x', $i + 1)]);
    $name = 'dyn' . $i; $p->$name = strtoupper($name);
    $num = 7; $p->$num = 'seven';
    $m = new Magic; $k = 'magic'; $m->$k = [$i]; $m->{'x' . $i} = $i;
    try { $p->count = 'nope'; } catch (TypeError $e) { $p->extra[] = 'TE'; }
    $n = null; try { $n->$name = 1; } catch (Error $e) { $err = get_class($e); }
    return json_encode([get_object_vars($p), $m->log, (new RO(1))->poke('id'), $err ?? '-']);
}
for ($i = 0; $i < 3; $i++) { echo build($i), "\n"; }
?>
--EXPECT--
[{"ID":0,"title":"t0","count":40,"extra":[0,"TE"],"alias":"r0","tmp":"x","dyn0":"DYN0","7":"seven"},["magic=[0]","x0=0"],"Error","Error"]
[{"ID":1,"title":"t1","count":41,"extra":[1,"TE"],"alias":"r1","tmp":"xx","dyn1":"DYN1","7":"seven"},["magic=[1]","x1=1"],"Error","Error"]
[{"ID":2,"title":"t2","count":42,"extra":[2,"TE"],"alias":"r2","tmp":"xxx","dyn2":"DYN2","7":"seven"},["magic=[2]","x2=2"],"Error","Error"]
