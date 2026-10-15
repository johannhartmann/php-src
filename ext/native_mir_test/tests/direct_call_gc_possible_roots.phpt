--TEST--
Native direct calls buffer the possible GC roots their completion releases
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
zend.enable_gc=1
--FILE--
<?php
function inner_arr($a) { $b = $a; return count($b); }
final class Holder { public $items; function size() { return count($this->items); } }
function outer_arr(int $n) {
    $s = 0;
    $keep = [];
    for ($i = 0; $i < $n; $i++) {
        $arr = [$i, $i + 1];
        $keep[] = $arr;
        $s += inner_arr($arr);
        $h = new Holder;
        $h->items = $arr;
        $keep[] = $h;
        $s += $h->size();
    }
    return [$s, gc_status()['roots']];
}
gc_collect_cycles();
echo json_encode(outer_arr(20)), "\n";
?>
--EXPECT--
[80,40]
