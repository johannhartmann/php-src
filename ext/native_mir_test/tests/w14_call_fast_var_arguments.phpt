--TEST--
Native fast call sites sending VAR arguments with SEND_VAR
--DESCRIPTION--
A call site whose arguments include VARs sent with SEND_VAR or
SEND_VAR_EX, such as dynamic property reads, assignment results or
by-reference function results, is a fast site: the VAR moves into the
argument, and a reference it holds is unwrapped as ZEND_SEND_VAR does.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function show($a, $b = 'd', $c = 'e') { if (is_array($a)) { $a[] = 'mod'; } return json_encode([$a, $b, $c]); }
function &ref_ret(array &$arr) { return $arr['k']; }
class Holder { public $name = 'n'; public $list = [1]; public static $st = ['s'];
    function viaThis() { return show(get_class($this), $this->name); }
    function selfArg() { return strlen(show($this)); }
}
#[AllowDynamicProperties]
class Bag {}
function run($i) {
    $o = [];
    $h = new Holder; $field = 'name'; $lf = 'list';
    $o[] = show($h->$field, $h->$lf);
    $o[] = show($x = "x$i", $y = [$i]);
    $data = ['k' => "ref$i"];
    $o[] = show(ref_ret($data), $data['k']);
    $o[] = show(Holder::$st, $h->list);
    $var = 'dyn'; $dyn = "D$i";
    $o[] = show($$var);
    $o[] = $h->viaThis() . $h->selfArg();
    $b = new Bag; $p = 'p' . $i; $b->$p = [$i, $i];
    $o[] = show($b->$p, $h->list);
    echo json_encode($h->list), json_encode($b->$p), ' ';
    return implode(' ', $o);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
?>
--EXPECT--
[1][0,0] ["n",[1],"e"] ["x0",[0],"e"] ["ref0","ref0","e"] [["s","mod"],[1],"e"] ["D0","d","e"] ["Holder","n","e"]33 [[0,0,"mod"],[1],"e"]
[1][1,1] ["n",[1],"e"] ["x1",[1],"e"] ["ref1","ref1","e"] [["s","mod"],[1],"e"] ["D1","d","e"] ["Holder","n","e"]33 [[1,1,"mod"],[1],"e"]
[1][2,2] ["n",[1],"e"] ["x2",[2],"e"] ["ref2","ref2","e"] [["s","mod"],[1],"e"] ["D2","d","e"] ["Holder","n","e"]33 [[2,2,"mod"],[1],"e"]
