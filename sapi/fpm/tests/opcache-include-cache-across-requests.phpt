--TEST--
Cached includes stay correct across requests while data files are rewritten and discarded
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

$dir = sys_get_temp_dir() . '/fpm-include-cache';
@mkdir($dir);
$state = "$dir/state.txt";
@unlink($state);
file_put_contents("$dir/data.php", "<?php return ['n' => -1];");
file_put_contents("$dir/fixed.php", "<?php return 'fixed';");
file_put_contents("$dir/lib.php", "<?php class IncludeCacheLib { static function v() { return 'lib'; } }");
for ($i = 0; $i < 70; $i++) {
    file_put_contents("$dir/burst$i.php", "<?php return $i;");
}

$code = <<<EOT
<?php
\$dir = '$dir';
\$state = '$state';
EOT . <<<'EOT'

$n = (int) @file_get_contents($state);
file_put_contents($state, $n + 1);
require_once "$dir/lib.php";
$out = [];
$out[] = (include "$dir/data.php")['n'];
$out[] = include "$dir/fixed.php";
$out[] = include "$dir/fixed.php";
$out[] = IncludeCacheLib::v();
$sum = 0;
for ($i = 0; $i < 70; $i += 10) {
    $sum += include "$dir/burst$i.php";
}
$out[] = $sum;
/* A data file nothing binds to, rewritten every request. */
file_put_contents("$dir/data.php", "<?php return ['n' => $n];");
opcache_invalidate("$dir/data.php", true);
if ($n == 2) {
    /* More discards than a process keeps track of between requests. */
    for ($i = 0; $i < 70; $i++) {
        file_put_contents("$dir/burst$i.php", "<?php return " . ($i * 2) . ";");
        opcache_invalidate("$dir/burst$i.php", true);
    }
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
0 -1,fixed,fixed,lib,210
1 0,fixed,fixed,lib,210
2 1,fixed,fixed,lib,210
3 2,fixed,fixed,lib,420
4 3,fixed,fixed,lib,420
5 4,fixed,fixed,lib,420
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$dir = sys_get_temp_dir() . '/fpm-include-cache';
foreach (glob("$dir/*") as $file) {
    @unlink($file);
}
@rmdir($dir);
?>
