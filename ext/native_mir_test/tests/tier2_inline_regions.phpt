--TEST--
Tier 2 inlining: regions with defaults, bails, warnings, exceptions, instanceof, truthiness and isset, entered directly and through call_user_func(_array)
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_TIER2_THRESHOLD=3
ZEND_NATIVE_TIER2_INLINE=1
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = sys_get_temp_dir() . '/tier2_inline_' . getmypid();
@mkdir($dir);
$lib = $dir . '/lib.php';
file_put_contents($lib, <<<'LIB'
<?php
interface Shape {}
class Base implements Shape {}
class Square extends Base {}
class Other {}
function add_one($a) { return $a + 1; }
function with_default($a, $b = 10, $c = 'x') { return $a * $b . $c; }
function nested($a) { return strtoupper($a) . add_one(strlen($a)); }
function warn_key($a, $k) { return $a[$k] . '!'; }
function thrower($i) { if ($i % 5 === 4) { throw new LogicException("bad $i"); } return $i; }
function is_shape($v) { $is = ($v instanceof Shape); return $is; }
function is_base($v) { return $v instanceof Base; }
function truthy($v) { return (bool) $v; }
function falsy($v) { return !$v; }
function has($a, $k) { return isset($a[$k]); }
function make() { return ['a' => 1, '12' => 'twelve', 'k' => null]; }
class Reg {
    public $items = [];
    private $hits = 0;
    public function has($name) { return isset($this->items[$name]); }
    public function get($name) { if (!$this->has($name)) { return null; } $this->hits++; return $this->items[$name]; }
    public static function twice($v) { return $v * 2; }
    public function hits() { return $this->hits; }
}
function host($i, $v, $reg) {
    global $glob;
    $out = [add_one($i), with_default($i), with_default($i, 2), nested("ab$i")];
    $out[] = is_shape($v) ? 'S' : '-';
    $out[] = is_base($v) ? 'B' : '-';
    $out[] = is_shape($glob) ? 'gS' : 'g-';
    $out[] = truthy($v) ? 'T' : 'F';
    $out[] = falsy($v) ? 'f' : 't';
    $out[] = has(make(), $i % 3 ? 'a' : '12') ? 'h' : 'n';
    $out[] = has(make(), 'k') ? 'h' : 'n';
    $out[] = has(make(), (string) ($i + 7)) ? 'h' : 'n';
    $key = substr("core/a core/b x", ($i % 3) * 7, 6);
    $out[] = var_export($reg->get(rtrim($key)), true);
    $out[] = Reg::twice($i);
    if ($i === 3) { $out[] = warn_key(['a' => 1], 'b'); }
    $out[] = guarded($i);
    return implode(',', $out);
}
function throws_inline($i) { return thrower($i) + add_one($i); }
function guarded($i) {
    try { return throws_inline($i); } catch (LogicException $e) { return $e->getMessage() . '@' . basename($e->getFile()) . ':' . $e->getLine() . ':' . $e->getTrace()[0]['function'] . '<' . $e->getTrace()[1]['function']; }
}
LIB);
require $lib;
$reg = new Reg;
$reg->items = ['core/a' => 1, 'core/b' => [2]];
$glob = new Square;
$values = [0, 1, '0', '', 'a', [], [0], 0.0, 1.5, null, new Square, new Base, new Other, true];
set_error_handler(function ($no, $msg, $file, $line) { echo "warning: $msg @", basename($file), ":$line\n"; return true; });
$rounds = [];
for ($round = 0; $round < 3; $round++) {
    $lines = [];
    foreach ($values as $j => $v) {
        $lines[] = host($j, $v, $reg);
        $lines[] = call_user_func_array('host', [$j, $v, $reg]);
        $lines[] = call_user_func('host', $j, $v, $reg);
    }
    $rounds[] = $lines;
}
var_dump($rounds[0] === $rounds[2], $rounds[1] === $rounds[2]);
echo implode("\n", array_slice($rounds[2], 0, 42, true)), "\n";
echo $reg->hits(), "\n";
unlink($lib); rmdir($dir);
--EXPECT--
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
warning: Undefined array key "b" @lib.php:9
bool(true)
bool(true)
1,0x,0x,AB04,-,-,gS,F,f,h,n,n,1,0,1
1,0x,0x,AB04,-,-,gS,F,f,h,n,n,1,0,1
1,0x,0x,AB04,-,-,gS,F,f,h,n,n,1,0,1
2,10x,2x,AB14,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),2,3
2,10x,2x,AB14,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),2,3
2,10x,2x,AB14,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),2,3
3,20x,4x,AB24,-,-,gS,F,f,h,n,n,NULL,4,5
3,20x,4x,AB24,-,-,gS,F,f,h,n,n,NULL,4,5
3,20x,4x,AB24,-,-,gS,F,f,h,n,n,NULL,4,5
4,30x,6x,AB34,-,-,gS,F,f,h,n,n,1,6,!,7
4,30x,6x,AB34,-,-,gS,F,f,h,n,n,1,6,!,7
4,30x,6x,AB34,-,-,gS,F,f,h,n,n,1,6,!,7
5,40x,8x,AB44,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),8,bad 4@lib.php:10:thrower<throws_inline
5,40x,8x,AB44,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),8,bad 4@lib.php:10:thrower<throws_inline
5,40x,8x,AB44,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),8,bad 4@lib.php:10:thrower<throws_inline
6,50x,10x,AB54,-,-,gS,F,f,h,n,h,NULL,10,11
6,50x,10x,AB54,-,-,gS,F,f,h,n,h,NULL,10,11
6,50x,10x,AB54,-,-,gS,F,f,h,n,h,NULL,10,11
7,60x,12x,AB64,-,-,gS,T,t,h,n,n,1,12,13
7,60x,12x,AB64,-,-,gS,T,t,h,n,n,1,12,13
7,60x,12x,AB64,-,-,gS,T,t,h,n,n,1,12,13
8,70x,14x,AB74,-,-,gS,F,f,h,n,n,array (
  0 => 2,
),14,15
8,70x,14x,AB74,-,-,gS,F,f,h,n,n,array (
  0 => 2,
),14,15
8,70x,14x,AB74,-,-,gS,F,f,h,n,n,array (
  0 => 2,
),14,15
9,80x,16x,AB84,-,-,gS,T,t,h,n,n,NULL,16,17
9,80x,16x,AB84,-,-,gS,T,t,h,n,n,NULL,16,17
9,80x,16x,AB84,-,-,gS,T,t,h,n,n,NULL,16,17
10,90x,18x,AB94,-,-,gS,F,f,h,n,n,1,18,bad 9@lib.php:10:thrower<throws_inline
10,90x,18x,AB94,-,-,gS,F,f,h,n,n,1,18,bad 9@lib.php:10:thrower<throws_inline
10,90x,18x,AB94,-,-,gS,F,f,h,n,n,1,18,bad 9@lib.php:10:thrower<throws_inline
11,100x,20x,AB105,S,B,gS,T,t,h,n,n,array (
  0 => 2,
),20,21
11,100x,20x,AB105,S,B,gS,T,t,h,n,n,array (
  0 => 2,
),20,21
11,100x,20x,AB105,S,B,gS,T,t,h,n,n,array (
  0 => 2,
),20,21
12,110x,22x,AB115,S,B,gS,T,t,h,n,n,NULL,22,23
12,110x,22x,AB115,S,B,gS,T,t,h,n,n,NULL,22,23
12,110x,22x,AB115,S,B,gS,T,t,h,n,n,NULL,22,23
13,120x,24x,AB125,-,-,gS,T,t,h,n,n,1,24,25
13,120x,24x,AB125,-,-,gS,T,t,h,n,n,1,24,25
13,120x,24x,AB125,-,-,gS,T,t,h,n,n,1,24,25
14,130x,26x,AB135,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),26,27
14,130x,26x,AB135,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),26,27
14,130x,26x,AB135,-,-,gS,T,t,h,n,n,array (
  0 => 2,
),26,27
90
