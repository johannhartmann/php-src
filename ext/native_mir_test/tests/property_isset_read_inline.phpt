--TEST--
Native inline FETCH_OBJ_IS of cached declared properties
--DESCRIPTION--
FETCH_OBJ_IS of a literal property into a temporary, as in $this->p ?? x
and isset($this->p[$k]), copies a cached declared property holding a value
inline. A missing, null, reference, uninitialized typed or magic property
and a different class keep the helper's semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Box { public $map = ['k' => 'v']; public $nil = null; public $ref; public ?array $typed; public $str = 'abc';
    function __construct() { $a = ['r' => 1]; $this->ref = &$a; }
    function probe($k) {
        return [$this->map[$k] ?? 'none', $this->nil ?? 'nil', $this->ref['r'] ?? 'noref', $this->typed['x'] ?? 'untyped',
            isset($this->map[$k]) ? 'set' : 'unset', $this->missing ?? 'missing', $this->str[1] ?? '-', isset($this->str) ? 's' : '!s'];
    }
}
class Other extends Box { public $map = ['k' => 'other']; }
class Magic { function __isset($n) { echo "__isset($n) "; return $n === 'yes'; } function __get($n) { echo "__get($n) "; return ['k' => strtoupper($n)]; }
    function probe() { return [$this->yes['k'] ?? 'Y', $this->no['k'] ?? 'N']; } }
for ($i = 0; $i < 3; $i++) {
    echo json_encode((new Box)->probe('k')), json_encode((new Other)->probe($i ? 'k' : 'z')), "\n";
    echo json_encode((new Magic)->probe()), "\n";
}
?>
--EXPECT--
["v","nil",1,"untyped","set","missing","b","s"]["none","nil",1,"untyped","unset","missing","b","s"]
__isset(yes) __get(yes) __isset(no) ["YES","N"]
["v","nil",1,"untyped","set","missing","b","s"]["other","nil",1,"untyped","set","missing","b","s"]
__isset(yes) __get(yes) __isset(no) ["YES","N"]
["v","nil",1,"untyped","set","missing","b","s"]["other","nil",1,"untyped","set","missing","b","s"]
__isset(yes) __get(yes) __isset(no) ["YES","N"]
