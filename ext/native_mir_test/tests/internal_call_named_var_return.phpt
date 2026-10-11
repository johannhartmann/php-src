--TEST--
Native internal calls with named arguments, call results as arguments, returns and finally
--DESCRIPTION--
Named arguments to internal functions use the SEND's run-time cache slot, a VAR call result moves into the internal frame, the RETURN helper handles discarded, by-reference and undefined returns, and finally blocks are entered without an exception.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n}|"; } }
function g($v) { return $v; }
function &gr(array &$a) { return $a[0]; }
function named($i) {
    $r = [];
    $r[] = str_pad(string: 'x', length: 4 + $i, pad_string: '-');
    $r[] = str_pad('y', pad_type: STR_PAD_LEFT, length: 3);
    $r[] = json_encode(value: [1, 2], flags: JSON_PRETTY_PRINT);
    $r[] = array_slice(array: [1, 2, 3], offset: 1, preserve_keys: true);
    try { $r[] = str_pad('z', nope: 1, length: 2); } catch (Error $e) { $r[] = $e->getMessage(); }
    try { $r[] = strlen(string: 'a', string2: 'b'); } catch (Error $e) { $r[] = $e->getMessage(); }
    $r[] = sprintf('%s-%s', ...['a', 'b']);
    $r[] = max(...[1, 5, 3]);
    try { $r[] = array_merge(a: [1], b: [2]); } catch (Error $e) { $r[] = $e->getMessage(); }
    try { $r[] = sprintf("%s", x: new D("named$i")); } catch (Error $e) { $r[] = $e->getMessage(); }
    try { $r[] = max(1, 2, y: 3); } catch (Error $e) { $r[] = $e->getMessage(); }
    return $r;
}
function vararg($i) {
    $r = [];
    $r[] = strlen(g('abc' . $i));
    $r[] = strtoupper(g('low'));
    $r[] = count(g([1, 2, 3]));
    $a = ['ref'];
    $r[] = strtoupper(gr($a));
    $r[] = implode(',', g([$i, $i + 1]));
    $r[] = get_class(g(new D("v$i")));
    echo "[after]|";
    $r[] = strrev(g(g('nested')));
    return $r;
}
function ret_discard($x) { return $x . '!'; }
function &ret_ref(array &$a) { return $a['k']; }
function &ret_ref_tmp() { $x = 1; return $x + 1; }
function ret_undef() { return $undefined; }
function ret_obj($i) { return new D("r$i"); }
function rets($i) {
    ret_discard('a');
    ret_obj($i);
    echo "[discarded]|";
    $a = ['k' => 1];
    $r = &ret_ref($a);
    $r = 2;
    ret_ref($a);
    $t = &ret_ref_tmp();
    $u = ret_undef();
    return [$a, $t, $u];
}
function fin($i) {
    $log = [];
    for ($j = 0; $j < 3; $j++) {
        try {
            $log[] = "try$j";
            if ($j == 1) {
                throw new RuntimeException("e$j");
            }
        } catch (RuntimeException $e) {
            $log[] = 'catch ' . $e->getMessage();
        } finally {
            $log[] = "finally$j";
        }
    }
    try {
        try {
            return $log;
        } finally {
            $log[] = 'inner';
        }
    } finally {
        $log[] = 'outer';
    }
}
function fin2() {
    try {
        try {
            throw new LogicException('x');
        } finally {
            echo "f1|";
        }
    } catch (LogicException $e) {
        echo "caught|";
    } finally {
        echo "f2|";
    }
    foreach ([1, 2] as $v) {
        try {
            if ($v == 2) break;
        } finally {
            echo "loop$v|";
        }
    }
    return 'done';
}
function plain_undef() {
    return [strlen($undef ?? 'x'), @strlen($undef2), str_repeat('a', 2)];
}
for ($i = 0; $i < 2; $i++) {
    echo json_encode(named($i)), "\n";
    echo json_encode(vararg($i)), "\n";
    echo json_encode(rets($i)), "\n";
    echo json_encode(fin($i)), "\n";
    echo fin2(), "\n";
    echo json_encode(plain_undef()), "\n";
}
--EXPECTF--
~named0|["x---","  y","[\n    1,\n    2\n]",{"1":2,"2":3},"Unknown named parameter $nope","Unknown named parameter $string2","a-b",5,"Internal function array_merge() does not accept named variadic arguments","Internal function sprintf() does not accept named variadic arguments","Internal function max() does not accept named variadic arguments"]
~v0|[after]|[4,"LOW",3,"REF","0,1","D","detsen"]
~r0|[discarded]|
Notice: Only variable references should be returned by reference in %s on line 35

Warning: Undefined variable $undefined in %s on line 36
[{"k":2},2,null]
["try0","finally0","try1","catch e1","finally1","try2","finally2"]
f1|caught|f2|loop1|loop2|done
[1,0,"aa"]
~named1|["x----","  y","[\n    1,\n    2\n]",{"1":2,"2":3},"Unknown named parameter $nope","Unknown named parameter $string2","a-b",5,"Internal function array_merge() does not accept named variadic arguments","Internal function sprintf() does not accept named variadic arguments","Internal function max() does not accept named variadic arguments"]
~v1|[after]|[4,"LOW",3,"REF","1,2","D","detsen"]
~r1|[discarded]|
Notice: Only variable references should be returned by reference in %s on line 35

Warning: Undefined variable $undefined in %s on line 36
[{"k":2},2,null]
["try0","finally0","try1","catch e1","finally1","try2","finally2"]
f1|caught|f2|loop1|loop2|done
[1,0,"aa"]
