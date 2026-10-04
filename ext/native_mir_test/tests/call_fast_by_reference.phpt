--TEST--
Native fast call sites passing CVs to by-reference parameters
--DESCRIPTION--
A fast call site may target a function or method taking some parameters
by reference when the site sends those CVs with SEND_VAR_EX: the published
site records the positions, the send copies the CV without warning when it
is undefined, and the fast Do passes a reference to the CV instead, an
undefined CV becoming null, as ZEND_SEND_VAR_EX does.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Cache { private $data = ['a' => 1];
    function get($key, $group = '', $force = false, &$found = null) { $found = isset($this->data[$key]); return $found ? $this->data[$key] : false; }
    function set($key, $v) { $this->data[$key] = $v; }
}
class Other { function get($key, $group = '', $force = false, $found = null) { return "other:$key"; } }
function bump(&$counter, $by) { $counter += $by; return $counter; }
function swap(&$a, &$b) { [$a, $b] = [$b, $a]; }
function wp_cache_get($key, $group = '', $force = false, &$found = null) { global $cache; return $cache->get($key, $group, $force, $found); }
$cache = new Cache;
function run($i) {
    global $cache;
    $o = [];
    $o[] = var_export(wp_cache_get('a', '', false, $found), true) . var_export($found, true);
    $o[] = var_export(wp_cache_get('b', '', false, $found2), true) . var_export($found2, true);
    $cache->set("k$i", $i);
    $o[] = var_export($cache->get("k$i", 'g', false, $hit), true) . var_export($hit, true);
    $n = 10; $alias = &$n;
    $o[] = bump($alias, $i) . '/' . $n;
    $arr = ['c' => 1];
    $o[] = bump($arr['c'], 2) . '/' . $arr['c'];
    $x = 5; $o[] = bump($x, 1) . bump($x, 1) . '/' . $x;
    $y = 'y'; $z = [$i]; swap($y, $z); $o[] = json_encode([$y, $z]);
    $p = 1; $o[] = bump($p, $p = 7) . '/' . $p;
    $w = new Other; $o[] = $w->get('z', '', false, $nothing) . var_export($nothing ?? 'unset', true);
    return implode(' ', $o);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
?>
--EXPECTF--

Warning: Undefined variable $nothing in %s on line 25
1true falsefalse 0true 10/10 3/3 67/7 [[0],"y"] 14/14 other:z'unset'

Warning: Undefined variable $nothing in %s on line 25
1true falsefalse 1true 11/11 3/3 67/7 [[1],"y"] 14/14 other:z'unset'

Warning: Undefined variable $nothing in %s on line 25
1true falsefalse 2true 12/12 3/3 67/7 [[2],"y"] 14/14 other:z'unset'
