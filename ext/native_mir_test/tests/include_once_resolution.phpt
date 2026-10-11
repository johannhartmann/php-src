--TEST--
include_once and require_once resolve their path like the VM at every execution
--FILE--
<?php
$base = __DIR__ . '/include_once_resolution';
@mkdir($base);
@mkdir($base . '/sub');
file_put_contents($base . '/rel.inc', '<?php echo "top rel\n";');
file_put_contents($base . '/sub/rel.inc', '<?php echo "sub rel\n";');
file_put_contents($base . '/abs.inc', '<?php $GLOBALS["abs"]++; return 7;');
file_put_contents($base . '/gen.inc', '<?php echo "gen inc\n";');
file_put_contents($base . '/fiber.inc', '<?php echo "fiber inc\n";');
@unlink($base . '/link.inc');
$linked = @symlink($base . '/abs.inc', $base . '/link.inc');

function relative_once(): void { include_once 'rel.inc'; }
function absolute_once(): void { include_once __DIR__ . '/include_once_resolution/abs.inc'; }
function absolute_once_result(): mixed { return include_once __DIR__ . '/include_once_resolution/abs.inc'; }
function linked_once(): mixed { return require_once __DIR__ . '/include_once_resolution/link.inc'; }
function missing_once(): mixed { return @include_once __DIR__ . '/include_once_resolution/missing.inc'; }

/* A relative site completed once resolves again after chdir(). */
set_include_path('.');
chdir($base);
relative_once();
relative_once();
chdir($base . '/sub');
relative_once();
relative_once();

$GLOBALS['abs'] = 0;
var_dump(absolute_once_result());
for ($i = 0; $i < 3; $i++) {
    absolute_once();
    var_dump(absolute_once_result());
}
echo "abs included ", $GLOBALS['abs'], "\n";
var_dump($linked ? linked_once() : true);
echo "abs included ", $GLOBALS['abs'], "\n";
for ($i = 0; $i < 2; $i++) {
    var_dump(missing_once());
}

function gen(): Generator {
    yield 1;
    include_once __DIR__ . '/include_once_resolution/gen.inc';
    yield 2;
    include_once __DIR__ . '/include_once_resolution/gen.inc';
    yield 3;
}
foreach (gen() as $value) {
    echo "yield $value\n";
}

$fiber = new Fiber(function (): void {
    for ($i = 0; $i < 2; $i++) {
        include_once __DIR__ . '/include_once_resolution/fiber.inc';
        Fiber::suspend($i);
    }
});
var_dump($fiber->start());
include_once $base . '/fiber.inc';
var_dump($fiber->resume());
$fiber->resume();
var_dump($fiber->isTerminated());

echo count(array_filter(get_included_files(),
    fn($f) => str_contains($f, '/include_once_resolution/'))), " files\n";
require_once __DIR__ . '/include_once_resolution/missing-required.inc';
echo "not reached\n";
?>
--CLEAN--
<?php
$base = __DIR__ . '/include_once_resolution';
foreach (['rel.inc', 'sub/rel.inc', 'abs.inc', 'link.inc', 'gen.inc', 'fiber.inc'] as $file) {
    @unlink($base . '/' . $file);
}
@rmdir($base . '/sub');
@rmdir($base);
?>
--EXPECTF--
top rel
sub rel
int(7)
bool(true)
bool(true)
bool(true)
abs included 1
bool(true)
abs included 1
bool(false)
bool(false)
yield 1
gen inc
yield 2
yield 3
fiber inc
int(0)
int(1)
bool(true)
5 files

Warning: require_once(%A): Failed to open stream: No such file or directory in %s on line %d

Fatal error: Uncaught Error: Failed opening required '%sinclude_once_resolution/missing-required.inc' (include_path='.') in %s:%d
Stack trace:
#0 {main}
  thrown in %s on line %d
