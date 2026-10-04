--TEST--
Native dynamic calls transfer ordinary positional arguments directly
--DESCRIPTION--
Calls of functions and methods from another file resolve at runtime. A
positional by-value SEND of a CV, temporary, literal or register scalar is
stored straight into the callee frame; references, by-reference parameters,
named arguments, variadics, extra arguments and undefined variables keep the
general setter.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$library = __DIR__ . '/dynamic_call_direct_arguments_library.inc';
file_put_contents($library, <<<'PHP'
<?php
function native_join($a, $b = 'd', $c = null) { return json_encode([$a, $b, $c]); }
function native_bump(&$value) { $value++; return $value; }
function native_rest($first, ...$rest) { return $first . ':' . implode('|', $rest); }
function native_args() { return count(func_get_args()) . ':' . implode(',', func_get_args()); }
class NativeSink { public $log = []; public function put($key, $value) { $this->log[$key] = $value; return $this; } }
PHP);
require $library;
unlink($library);

function native_calls($n)
{
    $out = [];
    $text = str_repeat('s', 3);
    $list = [1, 2];
    $ref = 5;
    $alias = &$ref;
    for ($i = 0; $i < $n; $i++) {
        $out[] = native_join($i, $text, $list);
        $out[] = native_join($i * 2 + 1, 'lit', 2.5);
        $out[] = native_join($i > 0, null);
        $out[] = native_join($alias, $text . $i);
        $out[] = native_join(c: 'named', a: $i);
        $out[] = native_bump($ref) . '/' . $alias;
        $out[] = native_rest('x', $i, $text, $list[0]);
        $out[] = native_args($i, $text, 'z');
        $sink = new NativeSink;
        $out[] = json_encode($sink->put('k' . $i, $list)->put('t', $text)->log);
    }
    $out[] = @native_join($undefined);
    return $out;
}
echo implode("\n", native_calls(2)), "\n";
$list = [1];
native_join($list, $list);
var_dump($list);
?>
--EXPECT--
[0,"sss",[1,2]]
[1,"lit",2.5]
[false,null,null]
[5,"sss0",null]
[0,"d","named"]
6/6
x:0|sss|1
3:0,sss,z
{"k0":[1,2],"t":"sss"}
[1,"sss",[1,2]]
[3,"lit",2.5]
[true,null,null]
[6,"sss1",null]
[1,"d","named"]
7/7
x:1|sss|1
3:1,sss,z
{"k1":[1,2],"t":"sss"}
[null,"d",null]
array(1) {
  [0]=>
  int(1)
}
