--TEST--
C::m() of a class autoloaded again in each request stays correct across requests
--DESCRIPTION--
A C::m() site runs in a later request before C is loaded: the autoloader
runs once per class and request, with the same backtrace, and a class that
fails to load, whose autoloader throws, or whose name binds another
declaration in this request behaves as in the first request; internal
static targets, discarded results, by-reference parameters and instance
methods called statically from $this take the same calls.
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

$dir = sys_get_temp_dir() . '/fpm-static-autoload';
@mkdir($dir);
$state = "$dir/state.txt";
@unlink($state);
file_put_contents("$dir/FpmStaticA.php", '<?php class FpmStaticA { static $n = 0; static function make($x, $y) { self::$n++; return $x * 10 + $y + self::$n; } static function bump(&$v) { $v++; } }');
file_put_contents("$dir/FpmStaticDate.php", '<?php class FpmStaticDate extends DateTimeImmutable {}');
file_put_contents("$dir/gone.php", '<?php class FpmStaticGone { static function v($x) { return "gone$x"; } }');
file_put_contents("$dir/throw.php", '<?php class FpmStaticThrow { static function v($x) { return "throw$x"; } }');
file_put_contents("$dir/swap1.php", '<?php class FpmStaticSwap { static function v($x) { return "one$x"; } }');
file_put_contents("$dir/swap2.php", '<?php class FpmStaticSwap { static function v($x) { return "two$x"; } }');
file_put_contents("$dir/trace.php", '<?php class FpmStaticTrace { static function v($x) { return $x + 40; } }');
file_put_contents("$dir/FpmStaticBase.php", '<?php class FpmStaticBase { function who() { return static::class . "/" . get_class($this); } }');
file_put_contents("$dir/FpmStaticChild.php", '<?php class FpmStaticChild extends FpmStaticBase { function run() { return FpmStaticBase::who(); } }');

$code = <<<EOT
<?php
\$dir = '$dir';
\$state = '$state';
EOT . <<<'EOT'

$n = (int) @file_get_contents($state);
file_put_contents($state, $n + 1);
$loads = [];
spl_autoload_register(function ($class) use ($dir, $n, &$loads) {
    $loads[] = $class;
    switch ($class) {
        case 'FpmStaticGone':
            if ($n % 2 == 0) {
                require "$dir/gone.php";
            }
            return;
        case 'FpmStaticThrow':
            if ($n == 0) {
                require "$dir/throw.php";
                return;
            }
            throw new RuntimeException("no $class");
        case 'FpmStaticSwap':
            require $n % 2 ? "$dir/swap2.php" : "$dir/swap1.php";
            return;
        case 'FpmStaticTrace':
            foreach (debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS) as $frame) {
                $loads[] = preg_replace('/^\{closure:.*\}$/', '{closure}',
                    $frame['function'] ?? '') . '@' . ($frame['line'] ?? '-');
            }
            require "$dir/trace.php";
            return;
        default:
            require "$dir/$class.php";
    }
});
function run($k, $f) {
    $g = $f + 1.25;
    $a = FpmStaticA::make($k, 3);
    FpmStaticA::make($k, 4);
    $v = $k;
    FpmStaticA::bump($v);
    $d = FpmStaticDate::createFromFormat('Y-m-d', '2026-10-1' . $k)->format('d');
    try {
        $gone = FpmStaticGone::v($k);
    } catch (Error $e) {
        $gone = get_class($e) . ':' . $e->getMessage();
    }
    try {
        $thrown = FpmStaticThrow::v($k);
    } catch (Exception $e) {
        $thrown = get_class($e) . ':' . $e->getMessage();
    }
    $s = FpmStaticSwap::v($k);
    $tr = FpmStaticTrace::v($k);
    $who = (new FpmStaticChild)->run();
    return sprintf('%d %d %s %s %s %s %d %s %.2f %.2f', $a, $v, $d, $gone,
        $thrown, $s, $tr, $who, $f + $k, $g * $k);
}
$out = run(1, 0.5 * $n);
echo $n, ' ', $out, ' ', implode(',', $loads), "\n";
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
0 14 2 11 gone1 throw1 one1 41 FpmStaticChild/FpmStaticChild 1.00 1.25 FpmStaticA,FpmStaticDate,FpmStaticGone,FpmStaticThrow,FpmStaticSwap,FpmStaticTrace,{closure}@53,run@58,FpmStaticChild,FpmStaticBase
1 14 2 11 Error:Class "FpmStaticGone" not found RuntimeException:no FpmStaticThrow two1 41 FpmStaticChild/FpmStaticChild 1.50 1.75 FpmStaticA,FpmStaticDate,FpmStaticGone,FpmStaticThrow,FpmStaticSwap,FpmStaticTrace,{closure}@53,run@58,FpmStaticChild,FpmStaticBase
2 14 2 11 gone1 RuntimeException:no FpmStaticThrow one1 41 FpmStaticChild/FpmStaticChild 2.00 2.25 FpmStaticA,FpmStaticDate,FpmStaticGone,FpmStaticThrow,FpmStaticSwap,FpmStaticTrace,{closure}@53,run@58,FpmStaticChild,FpmStaticBase
3 14 2 11 Error:Class "FpmStaticGone" not found RuntimeException:no FpmStaticThrow two1 41 FpmStaticChild/FpmStaticChild 2.50 2.75 FpmStaticA,FpmStaticDate,FpmStaticGone,FpmStaticThrow,FpmStaticSwap,FpmStaticTrace,{closure}@53,run@58,FpmStaticChild,FpmStaticBase
4 14 2 11 gone1 RuntimeException:no FpmStaticThrow one1 41 FpmStaticChild/FpmStaticChild 3.00 3.25 FpmStaticA,FpmStaticDate,FpmStaticGone,FpmStaticThrow,FpmStaticSwap,FpmStaticTrace,{closure}@53,run@58,FpmStaticChild,FpmStaticBase
5 14 2 11 Error:Class "FpmStaticGone" not found RuntimeException:no FpmStaticThrow two1 41 FpmStaticChild/FpmStaticChild 3.50 3.75 FpmStaticA,FpmStaticDate,FpmStaticGone,FpmStaticThrow,FpmStaticSwap,FpmStaticTrace,{closure}@53,run@58,FpmStaticChild,FpmStaticBase
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$dir = sys_get_temp_dir() . '/fpm-static-autoload';
foreach (glob("$dir/*") as $file) {
    @unlink($file);
}
@rmdir($dir);
?>
