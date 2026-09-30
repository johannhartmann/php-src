--TEST--
Native call-site fast path for resolved user functions and methods
--DESCRIPTION--
Calls to functions and methods of another file take the native fast path of
ADR 0025 once their site publishes the target: the frame is pushed and
linked as the VM does, arguments are stored directly and the native entry
is called. Defaults, typed parameters and return types, extra arguments,
argument exceptions with pending frames, undefined arguments, discarded
results with destructors, receiver rebinding during argument evaluation,
alternating receiver classes, variadics, directly sent scalar results and
deep recursion and calls pending across a yield keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/w14_call_fast_path.inc';
function run($round, $p, $q) {
    $out = [];
    $out[] = lib_add($round, 2);
    $out[] = lib_defaults($round);
    $out[] = lib_defaults($round, 'b');
    $out[] = lib_typed("s$round");
    $out[] = lib_typed("s", [1, 2]);
    $out[] = lib_extra($round, 'x', 'y');
    $out[] = lib_rec(3 + $round);
    $out[] = $p->get($round);
    $out[] = $q->get($round);                 // other class at a monomorphic site
    $out[] = $p->self_call($round);
    $out[] = $p->defaults($round);
    $out[] = $p->typed($round);
    foreach ([$p, $q, $p] as $obj) { $out[] = $obj->get(1); }   // alternating classes
    try { $out[] = $p->typed("x$round"); } catch (TypeError $e) { $out[] = 'TypeError'; }
    try { $out[] = lib_typed(); } catch (ArgumentCountError $e) { $out[] = 'ArgumentCountError'; }
    try { $out[] = $p->bad_return("str"); } catch (TypeError $e) { $out[] = 'ReturnTypeError'; }
    try { $out[] = lib_add(lib_throw($round), 1); } catch (LogicException $e) { $out[] = $e->getMessage(); }
    try { $out[] = $p->get($p->thrower($round)); } catch (RuntimeException $e) { $out[] = $e->getMessage(); }
    $out[] = lib_add($undefined_var, 1);       // warning, null
    echo "[discard ";
    lib_make("a$round");                        // result discarded: destructor now
    $p->make("b$round");
    echo "] ";
    $d = lib_make("c$round");
    $o = $p;
    $out[] = $o->get(($o = $q) ? 5 : 0);        // receiver bound before argument evaluation
    $s = 'str';
    $out[] = lib_ref_arg($s) . $s;
    $big = str_repeat('x', 10);
    $out[] = strlen(lib_ref_arg($big));
    $out[] = lib_variadic($round);
    $out[] = lib_variadic($round, 1, 2);
    $v1 = $round; $v2 = 5;
    lib_variadic_ref($v1, $v2);                 // by-reference variadic stays universal
    $out[] = "$v1/$v2";
    $out[] = lib_sq(lib_sq($round + 1) - 1);    // scalar results sent directly
    $out[] = $p->typed($p->typed($round) + 1);
    $out[] = lib_typed_defaults($round);
    $out[] = lib_typed_defaults($round, 2.5, "x");
    $out[] = lib_coerce($round);                // int coerced to float
    $out[] = lib_coerce("1.5", 7);
    $out[] = lib_const_default();
    $out[] = lib_obj($p);
    try { $out[] = lib_typed_defaults("nope"); } catch (TypeError $e) { $out[] = 'DefaultsTypeError'; }
    $h = new Holder;
    $out[] = $h->p->get($round);                 // temporary receiver
    $out[] = (new Chain($round))->next()->val(10);  // receivers released after the call
    try { $out[] = $h->p->get(lib_throw($round)); } catch (LogicException $e) { $out[] = $e->getMessage(); }
    unset($d);
    echo "|\n";
    return $out;
}
$p = new Point; $q = new Other;
for ($i = 0; $i < 3; $i++) { echo json_encode(run($i, $p, $q)), "\n"; }
echo lib_rec(20000), "\n";                      // deep recursion grows the VM stack
$g1 = lib_gen($p); $g1->current();             // calls pending across a yield
$g2 = lib_gen($q); $g2->current();
$g1->send(1); $g2->send(2);
echo $g1->getReturn(), ' ', $g2->getReturn(), "\n";
?>
--EXPECTF--

Warning: Undefined variable $undefined_var in %s on line %d
[discard ~a0 ~b0 ] ~chain0 ~chain1 ~c0 |
[2,"[0,[1,2],null]","[0,\"b\",null]","s00","s2","3:0",3,1,0,2,"0\/10\/c",0,2,-1,2,"TypeError","ArgumentCountError","ReturnTypeError","l0","t0",1,6,"strstr",10,"0:","0:1,2","1\/6",0,3,"[0,1.5,null,[1,2],true]","[0,2.5,\"x\",[1,2],true]","0.0\/3","1.5\/7","8D",2,"DefaultsTypeError",1,11,"l0"]

Warning: Undefined variable $undefined_var in %s on line %d
[discard ~a1 ~b1 ] ~chain1 ~chain2 ~c1 |
[3,"[1,[1,2],null]","[1,\"b\",null]","s10","s2","3:1",4,2,-1,4,"1\/10\/c",3,2,-1,2,"TypeError","ArgumentCountError","ReturnTypeError","l1","t1",1,6,"strstr",10,"1:","1:1,2","2\/6",9,12,"[1,1.5,null,[1,2],true]","[1,2.5,\"x\",[1,2],true]","1.0\/3","1.5\/7","8D",2,"DefaultsTypeError",2,12,"l1"]

Warning: Undefined variable $undefined_var in %s on line %d
[discard ~a2 ~b2 ] ~chain2 ~chain3 ~c2 |
[4,"[2,[1,2],null]","[2,\"b\",null]","s20","s2","3:2",5,3,-2,6,"2\/10\/c",6,2,-1,2,"TypeError","ArgumentCountError","ReturnTypeError","l2","t2",1,6,"strstr",10,"2:","2:1,2","3\/6",64,21,"[2,1.5,null,[1,2],true]","[2,2.5,\"x\",[1,2],true]","2.0\/3","1.5\/7","8D",2,"DefaultsTypeError",3,13,"l2"]
20000
3 -3
