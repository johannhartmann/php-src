--TEST--
new of a class autoloaded again in each request stays correct across requests
--DESCRIPTION--
A new C(...) site runs in a later request before C is loaded: the
autoloader runs once per class and request, with the same backtrace, and
a class that fails to load, whose autoloader throws, or whose name binds
another declaration in this request behaves as in the first request.
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

$dir = sys_get_temp_dir() . '/fpm-new-autoload';
@mkdir($dir);
$state = "$dir/state.txt";
@unlink($state);
file_put_contents("$dir/FpmNewA.php", '<?php class FpmNewA { public $v; function __construct($x, $y) { $this->v = $x * 10 + $y; } }');
file_put_contents("$dir/FpmNewB.php", '<?php class FpmNewB { public $p = 7; }');
file_put_contents("$dir/gone.php", '<?php class FpmNewGone { public $v; function __construct($x) { $this->v = "gone$x"; } }');
file_put_contents("$dir/throw.php", '<?php class FpmNewThrow { public $v; function __construct($x) { $this->v = "throw$x"; } }');
file_put_contents("$dir/swap1.php", '<?php class FpmNewSwap { public $v; function __construct($x) { $this->v = "one$x"; } }');
file_put_contents("$dir/swap2.php", '<?php class FpmNewSwap { public $v; function __construct($x) { $this->v = "two$x"; } }');
file_put_contents("$dir/trace.php", '<?php class FpmNewTrace { public $v; function __construct($x) { $this->v = $x + 40; } }');

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
        case 'FpmNewGone':
            if ($n % 2 == 0) {
                require "$dir/gone.php";
            }
            return;
        case 'FpmNewThrow':
            if ($n == 0) {
                require "$dir/throw.php";
                return;
            }
            throw new RuntimeException("no $class");
        case 'FpmNewSwap':
            require $n % 2 ? "$dir/swap2.php" : "$dir/swap1.php";
            return;
        case 'FpmNewTrace':
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
function make($k, $f) {
    $g = $f + 1.25;
    $a = new FpmNewA($k, 3);
    $b = new FpmNewB;
    try {
        $x = new FpmNewGone($k);
        $gone = $x->v;
    } catch (Error $e) {
        $gone = get_class($e) . ':' . $e->getMessage();
    }
    try {
        $t = new FpmNewThrow($k);
        $thrown = $t->v;
    } catch (Exception $e) {
        $thrown = get_class($e) . ':' . $e->getMessage();
    }
    $s = new FpmNewSwap($k);
    $tr = new FpmNewTrace($k);
    return sprintf('%d %d %s %s %s %d %.2f %.2f', $a->v, $b->p, $gone,
        $thrown, $s->v, $tr->v, $f + $k, $g * $k);
}
$out = make(1, 0.5 * $n);
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
0 13 7 gone1 throw1 one1 41 1.00 1.25 FpmNewA,FpmNewB,FpmNewGone,FpmNewThrow,FpmNewSwap,FpmNewTrace,{closure}@52,make@56
1 13 7 Error:Class "FpmNewGone" not found RuntimeException:no FpmNewThrow two1 41 1.50 1.75 FpmNewA,FpmNewB,FpmNewGone,FpmNewThrow,FpmNewSwap,FpmNewTrace,{closure}@52,make@56
2 13 7 gone1 RuntimeException:no FpmNewThrow one1 41 2.00 2.25 FpmNewA,FpmNewB,FpmNewGone,FpmNewThrow,FpmNewSwap,FpmNewTrace,{closure}@52,make@56
3 13 7 Error:Class "FpmNewGone" not found RuntimeException:no FpmNewThrow two1 41 2.50 2.75 FpmNewA,FpmNewB,FpmNewGone,FpmNewThrow,FpmNewSwap,FpmNewTrace,{closure}@52,make@56
4 13 7 gone1 RuntimeException:no FpmNewThrow one1 41 3.00 3.25 FpmNewA,FpmNewB,FpmNewGone,FpmNewThrow,FpmNewSwap,FpmNewTrace,{closure}@52,make@56
5 13 7 Error:Class "FpmNewGone" not found RuntimeException:no FpmNewThrow two1 41 3.50 3.75 FpmNewA,FpmNewB,FpmNewGone,FpmNewThrow,FpmNewSwap,FpmNewTrace,{closure}@52,make@56
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$dir = sys_get_temp_dir() . '/fpm-new-autoload';
foreach (glob("$dir/*") as $file) {
    @unlink($file);
}
@rmdir($dir);
?>
