--TEST--
new self, new parent and new static stay correct across requests
--DESCRIPTION--
Fast new sites of self, parent and static outlive the request that
published them: in later requests they create objects of the class the
caller names, also when another subclass calls them than in the request
before.
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
catch_workers_output = yes
php_admin_flag[display_errors] = On
EOT;

$dir = sys_get_temp_dir() . '/fpm-new-static';
@mkdir($dir);
$state = "$dir/state.txt";
@unlink($state);
file_put_contents("$dir/lib.php", <<<'PHP'
<?php
class FpmColl {
    public $items;
    function __construct($items) { $this->items = $items; }
    function map($f) { return new static(array_map($f, $this->items)); }
    static function make($items) { return new self($items); }
    static function late($items) { return new static($items); }
}
class FpmSub extends FpmColl {
    function up($x) { return new parent([$x, $x]); }
}
class FpmOther extends FpmColl {}
class FpmBare { static function mk() { return new static; } }
class FpmBare2 extends FpmBare {}
PHP);

$code = <<<EOT
<?php
\$dir = '$dir';
\$state = '$state';
EOT . <<<'EOT'

require_once "$dir/lib.php";
$n = (int) @file_get_contents($state);
file_put_contents($state, $n + 1);
function show($o) { return get_class($o) . '(' . implode(',', $o->items) . ')'; }
$classes = ['FpmColl', 'FpmSub', 'FpmOther'];
$c = new $classes[$n % 3]([$n, 1]);
$out = [show($c->map(fn($v) => $v * 3)), show(FpmColl::make([$n])),
    show(FpmSub::make([$n])), show($classes[($n + 1) % 3]::late([$n])),
    show(FpmSub::late([$n])), show((new FpmSub([]))->up($n)),
    get_class(FpmBare::mk()), get_class(($n % 2 ? 'FpmBare2' : 'FpmBare')::mk())];
echo $n, ' ', implode(' ', $out), "\n";
EOT;

$tester = new FPM\Tester($cfg, $code);
$tester->start(iniEntries: [
    'opcache.enable' => '1',
    'opcache.file_update_protection' => '0',
]);
$tester->expectLogStartNotices();
for ($i = 0; $i < 6; $i++) {
    echo $tester->request()->getBody(), "\n";
}
$tester->terminate();
$tester->close();

?>
--EXPECT--
0 FpmColl(0,3) FpmColl(0) FpmColl(0) FpmSub(0) FpmSub(0) FpmColl(0,0) FpmBare FpmBare
1 FpmSub(3,3) FpmColl(1) FpmColl(1) FpmOther(1) FpmSub(1) FpmColl(1,1) FpmBare FpmBare2
2 FpmOther(6,3) FpmColl(2) FpmColl(2) FpmColl(2) FpmSub(2) FpmColl(2,2) FpmBare FpmBare
3 FpmColl(9,3) FpmColl(3) FpmColl(3) FpmSub(3) FpmSub(3) FpmColl(3,3) FpmBare FpmBare2
4 FpmSub(12,3) FpmColl(4) FpmColl(4) FpmOther(4) FpmSub(4) FpmColl(4,4) FpmBare FpmBare
5 FpmOther(15,3) FpmColl(5) FpmColl(5) FpmColl(5) FpmSub(5) FpmColl(5,5) FpmBare FpmBare2
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$dir = sys_get_temp_dir() . '/fpm-new-static';
foreach (glob("$dir/*") as $file) {
    @unlink($file);
}
@rmdir($dir);
?>
