--TEST--
Native x64 foreach over hashes and string-key compound updates
--DESCRIPTION--
foreach with keys walks hash buckets inline (skipping deleted ones, counting string keys), and $a[$string] op= $v probes the hash with the key's cached hash and bytes; other cases keep the helpers.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function t($n) {
    for ($i = 0; $i < $n; $i++) { $a["foo_$i"] = $i; $b["foo_$i"] = 0; }
    $b["x"] = 1.5; $b["big"] = PHP_INT_MAX; $b["s"] = "str"; $b[7] = 1;
    for ($r = 0; $r < 3; $r++) foreach ($a as $k => $v) { $b[$k] += $v; $b[$k] -= 1; $b[$k] *= 2; }
    $k = "x"; $b[$k] += 1; $k = "big"; $b[$k] += 1; $k = "s"; try { $b[$k] += 1; } catch (Error $e) { echo get_class($e), "\n"; }
    $k = "missing"; $b[$k] += 5; $k = "7"; $b[$k] += 1; $k = ""; $b[$k] += 2;
    $c = $b; $k = "foo_1"; $c[$k] += 100;
    var_dump($b["foo_0"], $b["foo_1"], $b["x"], $b["big"], $b["missing"], $b[7], $b[""], $c["foo_1"], $b["foo_1"]);
}
t(5);
?>
--EXPECTF--
TypeError

Warning: Undefined array key "missing" in %s on line 7

Warning: Undefined array key "" in %s on line 7
int(-14)
int(0)
float(2.5)
float(9.223372036854776E+18)
int(5)
int(2)
int(2)
int(100)
int(0)
