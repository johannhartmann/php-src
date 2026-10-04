--TEST--
Native call_user_func*() and $f() through the cached callable resolution
--DESCRIPTION--
Dynamic call sites cache their target under a stable callable identity: an
interned function name, a closure's code, or a class and interned method
name, taking the receiver, bound object and scope from each call. Runtime
built names, static methods, "Class::method" strings, invokable objects,
temporary callables, by-reference, missing, extra and named arguments
unpacked from arrays keep their semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function native_sum($a, $b = 10, ...$rest) { return $a + $b + array_sum($rest); }
function native_bump(&$value) { $value++; return $value; }
class NativeBase {
    public function __construct(public $n) {}
    public function scale($x) { return $x * $this->n; }
    public static function twice($x) { return $x * 2; }
}
class NativeChild extends NativeBase {
    public function scale($x) { return -parent::scale($x); }
}
class NativeInvokable { public function __invoke($x) { return "invoked $x"; } }

function native_dispatch(array $hooks, array $args)
{
    $out = [];
    foreach ($hooks as $hook) {
        $out[] = call_user_func_array($hook, $args);
        $out[] = call_user_func($hook, $args[0]);
    }
    return $out;
}

function native_direct($callable, $value) { return $callable($value); }

function native_run($round)
{
    $one = new NativeBase(3);
    $two = new NativeBase(5);
    $child = new NativeChild(7);
    $name = 'native_' . 'sum';
    $hooks = [
        'native_sum', $name, [$one, 'scale'], [$two, 'scale'], [$child, 'scale'],
        [$one, 'twice'], 'NativeBase::twice', new NativeInvokable,
        fn ($x, ...$more) => $x + count($more), 'sprintf',
    ];
    $out = native_dispatch($hooks, ['4', 1]);
    $out[] = call_user_func_array('native_sum', [1, 2, 3, 4]);
    $out[] = call_user_func_array('native_sum', [5]);
    $out[] = call_user_func_array('native_sum', ['b' => 1, 'a' => 2]);
    $value = 1;
    $out[] = call_user_func_array('native_bump', [&$value]) . "/$value";
    foreach ([[$one, 'scale'], [$two, 'scale']] as $pair) {
        $out[] = native_direct($pair, $round);
    }
    $out[] = native_direct('native_sum', $round);
    try {
        call_user_func_array('native_sum', []);
    } catch (ArgumentCountError $e) {
        $out[] = get_class($e);
    }
    return $out;
}
for ($r = 0; $r < 3; $r++) {
    echo json_encode(native_run($r)), "\n";
}
?>
--EXPECT--
[5,14,5,14,12,12,20,20,-28,-28,8,8,8,8,"invoked 4","invoked 4",5,4,"4","4",10,15,3,"2\/2",0,0,10,"ArgumentCountError"]
[5,14,5,14,12,12,20,20,-28,-28,8,8,8,8,"invoked 4","invoked 4",5,4,"4","4",10,15,3,"2\/2",3,5,11,"ArgumentCountError"]
[5,14,5,14,12,12,20,20,-28,-28,8,8,8,8,"invoked 4","invoked 4",5,4,"4","4",10,15,3,"2\/2",6,10,12,"ArgumentCountError"]
