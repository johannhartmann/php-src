--TEST--
QM_ASSIGN that cannot fail runs without a cold block, without OPcache
--DESCRIPTION--
Without OPcache a ternary's QM_ASSIGN moves the temporary of a fetch it
follows; the copy runs without a cold block and takes no helper input.
--INI--
opcache.enable=0
opcache.enable_cli=0
--FILE--
<?php
final class Noisy
{
    public function __construct(public string $name) {}
    public function __destruct() { echo "destruct {$this->name}\n"; }
}

function make(string $s) { return str_repeat($s, 2); }
function arr(int $n) { return [$n, $n + 1]; }

function literals($c) {
    $a = $c ? 1 : 2;
    $b = $c ? 'x' : null;
    $d = $c ? [1, 2] : [];
    $e = $c ? 1.5 : false;
    return [$a, $b, $d, $e, $c ? 'lit' : 'other'];
}

function moves($c, $n) {
    $s = $c ? make("a$n") : make("b$n");
    $t = $s;
    $s .= '!';
    $l = $c ? arr($n) : arr(-$n);
    $m = $l;
    $l[] = 'appended';
    $o = $c ? new Noisy("o$n") : null;
    return [$s, $t, $l, $m, $o?->name];
}

function scalars($c, int $i, float $f) {
    $x = $c ? $i : $i + 1;
    $y = $c ? $f : $f * 2;
    $z = $x;
    $x++;
    return [$x, $y, $z];
}

function counted_cv($c, $v) {
    $w = $c ? $v : 'none';
    $v[] = 'changed';
    return [$w, $v];
}

function undefined_source($c) {
    $r = $c ? $missing : 'set';
    return $r;
}

function reference_source($c) {
    $base = 'base';
    $ref = &$base;
    $r = $c ? $ref : 'other';
    $base = 'changed';
    return [$r, $ref];
}

function loop_results(array $items) {
    $out = [];
    foreach ($items as $k => $v) {
        $label = is_int($k) ? 'int' : 'str';
        $value = $v ?: 'empty';
        $out[] = "$label:$value";
    }
    return $out;
}

final class Holder
{
    private $map = ['k' => 'value', 'n' => null];
    public function get($k) { return isset($this->map[$k]) ? $this->map[$k] : 'default'; }
    public function via($k) { return $this->get($k); }
}

for ($i = 0; $i < 3; $i++) {
    $c = $i % 2 === 0;
    echo json_encode(literals($c)), "\n";
    echo json_encode(moves($c, $i)), "\n";
    echo json_encode(scalars($c, $i, $i / 2)), "\n";
    echo json_encode(counted_cv($c, [$i])), "\n";
    var_dump(undefined_source($c));
    echo json_encode(reference_source($c)), "\n";
    echo json_encode(loop_results(['a' => 0, 'b' => 'x', 3 => [], 4 => 'y'])), "\n";
    $h = new Holder();
    echo $h->via('k'), ' ', $h->via('n'), ' ', $h->via('z'), "\n";
}
echo "done\n";
--EXPECTF--
[1,"x",[1,2],1.5,"lit"]
destruct o0
["a0a0!","a0a0",[0,1,"appended"],[0,1],"o0"]
[1,0,0]
[[0],[0,"changed"]]

Warning: Undefined variable $missing in %s on line 45
NULL
["base","changed"]
["str:empty","str:x","int:empty","int:y"]
value default default
[2,null,[],false,"other"]
["b1b1!","b1b1",[-1,0,"appended"],[-1,0],null]
[3,1,2]
["none",[1,"changed"]]
string(3) "set"
["other","changed"]
["str:empty","str:x","int:empty","int:y"]
value default default
[1,"x",[1,2],1.5,"lit"]
destruct o2
["a2a2!","a2a2",[2,3,"appended"],[2,3],"o2"]
[3,1,2]
[[2],[2,"changed"]]

Warning: Undefined variable $missing in %s on line 45
NULL
["base","changed"]
["str:empty","str:x","int:empty","int:y"]
value default default
done
