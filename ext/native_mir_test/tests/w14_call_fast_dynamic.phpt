--TEST--
Native dynamic call fast path for call_user_func*() and $f()
--DESCRIPTION--
call_user_func_array(), call_user_func() and $f(...) sites whose callable
names a recorded native target push its frame without the general
resolution (ADR 0025 section 3). Function names, closures with and without
bound objects, [$object, 'method'] with visibility, ['Class', 'method'] with
late static binding, by-reference and typed parameters, defaults, variadics,
named and non-packed arrays, exceptions, generators, recursion through the
same site and temporary callables keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/w14_call_fast_dynamic.inc';
function run($round) {
    $out = [];
    $b = new DynBase; $c = new DynChild;
    $k = 3;
    $closure = function ($x) use ($k) { return $x * $k; };
    $bound = Closure::bind(function ($x) { return $this->v + $x; }, $c, DynChild::class);
    $static = static fn($x) => $x - 1;
    $callables = ['dyn_add', $closure, $bound, $static, [$b, 'get'], [$c, 'get'], ['DynBase', 'who'], 'DynChild::who'];
    foreach ($callables as $f) {
        $out[] = call_user_func_array($f, [$round]);
        $out[] = call_user_func($f, $round + 1);
        $out[] = $f($round + 2);
    }
    $v = $round;
    $args = [&$v];
    $out[] = call_user_func_array('dyn_ref', $args) . "/$v";
    $out[] = @call_user_func('dyn_ref', $v) . "/$v";          // warning: by value
    $out[] = call_user_func_array('dyn_typed', ["4"]);         // coercion
    try { $out[] = call_user_func_array('dyn_typed', ["x"]); } catch (TypeError $e) { $out[] = 'TypeError'; }
    $out[] = call_user_func_array('dyn_variadic', [$round, 1, 2, 3]);
    $out[] = call_user_func_array('dyn_named', ['c' => 9, 'a' => 8]);
    $out[] = call_user_func_array('dyn_named', [1 => 5, 0 => 4]);   // non-packed order
    $holes = [0 => 'x', 2 => 'z']; unset($holes[1]);
    $out[] = call_user_func_array('dyn_named', $holes);
    try { $out[] = call_user_func_array('dyn_throw', [$round]); } catch (LogicException $e) { $out[] = $e->getMessage(); }
    try { $out[] = call_user_func_array('dyn_add', "notarray"); } catch (TypeError $e) { $out[] = 'ArgsTypeError'; }
    $out[] = implode(',', iterator_to_array(call_user_func('dyn_gen', $round)));
    $out[] = dyn_rec('dyn_rec', 20);
    $out[] = $b->callSecret($round);
    try { $out[] = call_user_func([$b, 'secret'], 1); } catch (Error $e) { $out[] = 'NotCallable'; }
    $out[] = DynChild::callWho();
    $out[] = $c->callWhoFromInstance();
    $out[] = call_user_func_array([new DynDtor, 'm'], [$round]);   // temporary receiver
    $out[] = call_user_func(function ($x) { return "tmp$x"; }, $round);  // temporary closure
    $out[] = call_user_func_array('dyn_add', [$round]);           // default
    return $out;
}
for ($i = 0; $i < 3; $i++) { echo json_encode(run($i)), "\n"; }
?>
--EXPECTF--
~dtor [10,11,12,0,3,6,7,8,9,-1,0,1,5,6,7,7,8,9,"DynBase","DynBase","DynBase","DynChild","DynChild","DynChild","1\/1","2\/1",8,"TypeError","0:1,2,3","829","543","xz3","thrown 0","ArgsTypeError","0,1",20,"secret0","NotCallable","DynBase","DynChild","m0","tmp0",10]
~dtor [11,12,13,3,6,9,8,9,10,0,1,2,6,7,8,8,9,10,"DynBase","DynBase","DynBase","DynChild","DynChild","DynChild","2\/2","3\/2",8,"TypeError","1:1,2,3","829","543","xz3","thrown 1","ArgsTypeError","1,2",20,"secret1","NotCallable","DynBase","DynChild","m1","tmp1",11]
~dtor [12,13,14,6,9,12,9,10,11,1,2,3,7,8,9,9,10,11,"DynBase","DynBase","DynBase","DynChild","DynChild","DynChild","3\/3","4\/3",8,"TypeError","2:1,2,3","829","543","xz3","thrown 2","ArgsTypeError","2,3",20,"secret2","NotCallable","DynBase","DynChild","m2","tmp2",12]
