--TEST--
Native dimension fetches of ArrayAccess objects
--DESCRIPTION--
Write fetches through offsetGet() notice an element that cannot be modified unless it is returned by reference or is an object, read fetches copy it, as the VM's dimension fetches publish read_dimension()'s result.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class AA implements ArrayAccess {
    public $d = ['k' => [1], 'o' => null, 's' => 'str'];
    public function offsetExists($o): bool { return isset($this->d[$o]); }
    public function offsetGet($o): mixed { echo "get($o)|"; return $this->d[$o] ?? null; }
    public function offsetSet($o, $v): void { echo "set($o)|"; $this->d[$o] = $v; }
    public function offsetUnset($o): void { unset($this->d[$o]); }
}
class RefAA extends AA {
    public function &offsetGet($o): mixed { echo "rget($o)|"; if (!isset($this->d[$o])) $this->d[$o] = null; return $this->d[$o]; }
}
function run($i) {
    $a = new AA;
    $a['k'][] = 2;
    $a['n']['x'] = 1;
    $a['o'] = new ArrayObject([]);
    $a['o']['z'] = 3;
    $r = new RefAA;
    $r['k'][] = 2;
    $r['n']['x'] = $i;
    echo "\n", json_encode([$a->d['k'], $r->d]), "\n";
    $list = [new AA];
    $list[0]['k'][] = 5;
    [$x, $y] = $a['k'];
    echo json_encode([$x, $y, $a['s'][1], isset($a['k'][0]), empty($a['n'])]), "\n";
    $ref = &$r['k'];
    $ref[] = 9;
    echo json_encode($r->d['k']), "\n";
}
run(1);
run(2);
--EXPECTF--
get(k)|
Notice: Indirect modification of overloaded element of AA has no effect in %s on line 14
get(n)|
Notice: Indirect modification of overloaded element of AA has no effect in %s on line 15
set(o)|get(o)|rget(k)|rget(n)|
[[1],{"k":[1,2],"o":null,"s":"str","n":{"x":1}}]
get(k)|
Notice: Indirect modification of overloaded element of AA has no effect in %s on line 23
get(k)|
Warning: Undefined array key 1 in %s on line 24
get(s)|get(k)|[1,null,"t",true,true]
rget(k)|[1,2,9]
get(k)|
Notice: Indirect modification of overloaded element of AA has no effect in %s on line 14
get(n)|
Notice: Indirect modification of overloaded element of AA has no effect in %s on line 15
set(o)|get(o)|rget(k)|rget(n)|
[[1],{"k":[1,2],"o":null,"s":"str","n":{"x":2}}]
get(k)|
Notice: Indirect modification of overloaded element of AA has no effect in %s on line 23
get(k)|
Warning: Undefined array key 1 in %s on line 24
get(s)|get(k)|[1,null,"t",true,true]
rget(k)|[1,2,9]
