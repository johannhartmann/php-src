--TEST--
Cached method and static call sites stay correct across requests while a class file changes
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

$lib = sys_get_temp_dir() . '/fpm-call-sites-lib.php';
$state = sys_get_temp_dir() . '/fpm-call-sites-state.txt';
file_put_contents($lib, "<?php class CallSitesLib { function version() { return 'v0'; } static function make() { return new CallSitesLib; } }");
@unlink($state);

$code = <<<EOT
<?php
\$lib = '$lib';
\$state = '$state';
EOT . <<<'EOT'

$n = (int) @file_get_contents($state);
file_put_contents($state, $n + 1);
require_once $lib;
class Base {
    public $p = 1;
    function v() { return 'base' . $this->p; }
    static function s() { return static::class; }
    function viaSelf() { return self::s() . '/' . static::s(); }
}
class Child extends Base {
    function v() { return 'child:' . parent::v() . self::w() . $this->viaSelf(); }
    static function w() { return '!'; }
}
$objects = [new Base, new Child, new ArrayObject([1, 2, 3])];
$out = [];
for ($i = 0; $i < 4; $i++) {
    $o = $objects[$i % 2];
    $out[] = $o->v();
    $out[] = $o::s();
}
$out[] = $objects[2]->count();
$made = new CallSitesLib;
$out[] = $made->version();
$out[] = CallSitesLib::make()->version();
if ($n % 2 == 1) {
    file_put_contents($lib, "<?php class CallSitesLib { function version() { return 'v" . ($n + 1)
        . "'; } static function make() { return new CallSitesLib; } }");
    opcache_invalidate($lib, true);
}
echo $n, ' ', implode(',', $out), "\n";
EOT;

$tester = new FPM\Tester($cfg, $code);
$tester->start(iniEntries: [
    'opcache.enable' => '1',
    'opcache.file_update_protection' => '0',
    'opcache.validate_timestamps' => '0',
]);
$tester->expectLogStartNotices();
for ($i = 0; $i < 6; $i++) {
    echo $tester->request()->getBody(), "\n";
}
$tester->terminate();
$tester->close();

?>
--EXPECT--
0 base1,Base,child:base1!Child/Child,Child,base1,Base,child:base1!Child/Child,Child,3,v0,v0
1 base1,Base,child:base1!Child/Child,Child,base1,Base,child:base1!Child/Child,Child,3,v0,v0
2 base1,Base,child:base1!Child/Child,Child,base1,Base,child:base1!Child/Child,Child,3,v2,v2
3 base1,Base,child:base1!Child/Child,Child,base1,Base,child:base1!Child/Child,Child,3,v2,v2
4 base1,Base,child:base1!Child/Child,Child,base1,Base,child:base1!Child/Child,Child,3,v4,v4
5 base1,Base,child:base1!Child/Child,Child,base1,Base,child:base1!Child/Child,Child,3,v4,v4
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
@unlink(sys_get_temp_dir() . '/fpm-call-sites-lib.php');
@unlink(sys_get_temp_dir() . '/fpm-call-sites-state.txt');
?>
