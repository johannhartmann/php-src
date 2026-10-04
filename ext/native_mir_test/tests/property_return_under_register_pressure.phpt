--TEST--
Native property read returned under register pressure
--DESCRIPTION--
A property read whose boxed result lives in registers must also leave the
owned value in its temporary slot, as the helper does: with many live locals
the RETURN reads the returned property from that slot.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Q { public $terms; public $vars; public $meta = ['k' => 'v'];
  function format($a, $f) { return $f === 'all' ? $a : array_keys($a); }
  function get() {
    $args = $this->vars;
    $v0 = $args['n'] + 0; $s0 = 'p' . $v0;
    $v1 = $args['n'] + 1; $s1 = 'p' . $v1;
    $v2 = $args['n'] + 2; $s2 = 'p' . $v2;
    $v3 = $args['n'] + 3; $s3 = 'p' . $v3;
    $v4 = $args['n'] + 4; $s4 = 'p' . $v4;
    $v5 = $args['n'] + 5; $s5 = 'p' . $v5;
    $v6 = $args['n'] + 6; $s6 = 'p' . $v6;
    $v7 = $args['n'] + 7; $s7 = 'p' . $v7;
    $v8 = $args['n'] + 8; $s8 = 'p' . $v8;
    $v9 = $args['n'] + 9; $s9 = 'p' . $v9;
    $v10 = $args['n'] + 10; $s10 = 'p' . $v10;
    $v11 = $args['n'] + 11; $s11 = 'p' . $v11;
    $v12 = $args['n'] + 12; $s12 = 'p' . $v12;
    $v13 = $args['n'] + 13; $s13 = 'p' . $v13;
    $v14 = $args['n'] + 14; $s14 = 'p' . $v14;
    $v15 = $args['n'] + 15; $s15 = 'p' . $v15;
    $v16 = $args['n'] + 16; $s16 = 'p' . $v16;
    $v17 = $args['n'] + 17; $s17 = 'p' . $v17;
    $v18 = $args['n'] + 18; $s18 = 'p' . $v18;
    $v19 = $args['n'] + 19; $s19 = 'p' . $v19;
    $v20 = $args['n'] + 20; $s20 = 'p' . $v20;
    $v21 = $args['n'] + 21; $s21 = 'p' . $v21;
    $v22 = $args['n'] + 22; $s22 = 'p' . $v22;
    $v23 = $args['n'] + 23; $s23 = 'p' . $v23;
    $v24 = $args['n'] + 24; $s24 = 'p' . $v24;
    $v25 = $args['n'] + 25; $s25 = 'p' . $v25;
    $v26 = $args['n'] + 26; $s26 = 'p' . $v26;
    $v27 = $args['n'] + 27; $s27 = 'p' . $v27;
    $v28 = $args['n'] + 28; $s28 = 'p' . $v28;
    $v29 = $args['n'] + 29; $s29 = 'p' . $v29;
    $objs = [];
    foreach ($args['list'] as $k => $t) { $o = new stdClass; $o->id = $t; $o->k = $this->meta['k']; $objs[$k] = $o; }
    $sum = $v0 + $v1 + $v2 + $v3 + $v4 + $v5 + $v6 + $v7 + $v8 + $v9 + $v10 + $v11 + $v12 + $v13 + $v14 + $v15 + $v16 + $v17 + $v18 + $v19 + $v20 + $v21 + $v22 + $v23 + $v24 + $v25 + $v26 + $v27 + $v28 + $v29;
    $str = $s0 . $s3 . $s6 . $s9 . $s12 . $s15 . $s18 . $s21 . $s24 . $s27;
    if ($args['cache']) { $cache = array_map(fn($o) => $o->id, $objs); }
    $this->terms = $this->format($objs, $args['fields']);
    return $this->terms;
  }
  function query($q) { $this->vars = $q; return $this->get(); } }
$q = new Q;
for ($i = 0; $i < 4; $i++) { $r = $q->query(['n' => $i, 'cache' => $i > 1, 'fields' => $i ? 'all' : 'ids', 'list' => [3, 4]]); echo gettype($r), count((array) $r), "\n"; }

?>
--EXPECT--
array2
array2
array2
array2
