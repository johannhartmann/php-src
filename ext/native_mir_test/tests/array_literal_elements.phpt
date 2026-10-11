--TEST--
Native array literals with literal, variable, reference and unusual keys
--DESCRIPTION--
INIT_ARRAY and ADD_ARRAY_ELEMENT of integer, string, numeric-string, computed, float, bool and null keys, references, temporaries, undefined variables, spreads and a full next index keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n}|"; } }
function k($v) { return $v; }
function arrays($i) {
    $x = 'xv';
    $ref = 'rv';
    $alias = &$ref;
    $list = [
        [1, 2, 3],
        ['a' => 1, 'b' => $x, '5' => 'five', 7 => 'seven', '07' => 'zero-seven'],
        [k('a') => 1, k(2) => 2, k('3') => 3, $x => 4, $i => 5],
        [$alias, &$ref, $x . 'c', new D("d$i")],
        [...[1, 2], ...['k' => 'v']],
        [1.5 => 'f', true => 't', null => 'n'],
        [$undefined, 'after'],
    ];
    $ref = 'changed';
    try { $bad = [k([]) => 1]; } catch (Error $e) { $list[] = $e->getMessage(); }
    $big = [PHP_INT_MAX => 1];
    try { $big2 = [PHP_INT_MAX => 1, 2]; } catch (Error $e) { $list[] = $e->getMessage(); }
    $list[] = $big;
    return $list;
}
echo json_encode(arrays(1)), "\n";
echo json_encode(arrays(2)), "\n";
--EXPECTF--

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 14

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 14

Warning: Undefined variable $undefined in %s on line 15
~d1|[[1,2,3],{"a":1,"b":"xv","5":"five","7":"seven","07":"zero-seven"},{"a":1,"2":2,"3":3,"xv":4,"1":5},["rv","changed","xvc",{"n":"d1"}],{"0":1,"1":2,"k":"v"},{"1":"t","":"n"},[null,"after"],"Cannot access offset of type array on array","Cannot add element to the array as the next element is already occupied",{"9223372036854775807":1}]

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 14

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 14

Warning: Undefined variable $undefined in %s on line 15
~d2|[[1,2,3],{"a":1,"b":"xv","5":"five","7":"seven","07":"zero-seven"},{"a":1,"2":5,"3":3,"xv":4},["rv","changed","xvc",{"n":"d2"}],{"0":1,"1":2,"k":"v"},{"1":"t","":"n"},[null,"after"],"Cannot access offset of type array on array","Cannot add element to the array as the next element is already occupied",{"9223372036854775807":1}]
