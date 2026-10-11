--TEST--
Native generators keep typed loop values across send()
--DESCRIPTION--
A typed loop counter that an elided COPY carries into the loop body lives in the register of the copy's input. Every generator resume reloads that register from the counter's slot as well, since resume runs in a new native activation; yields after send() and back-to-back yields keep the counter and bound intact.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function y3(int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $i * 2; if ($r !== null) { yield $r; } } }
function y4($n) { for ($i = 0; $i < $n; $i++) { $r = yield $i; if ($r !== null) { yield $r; } } }
function y5(int $n) { $i = 0; while ($i < $n) { $r = yield $i; if ($r !== null) { yield $r; } $i++; } }
function both(int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $i * 2; yield $r; } }
function s1(int $n) { for ($i = 0; $i < $n; $i++) { yield $i; } }
function s2(int $n) { for ($i = 0; $i < $n; $i++) { yield $i; yield $i + 100; } }
function s3(int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $i; yield 7; } }
function s4(int $n) { for ($i = 0; $i < $n; $i++) { yield 5; yield $i; } }

$gen = y3(3);
$c = 0;
foreach ($gen as $x) {
    var_dump($x);
    if ($x === 2) {
        $gen->send("sent");
    }
    if (++$c > 20) { echo "runaway\n"; break; }
}
echo "--\n";
foreach (['y3', 'y4', 'y5'] as $f) {
    $gen = $f(3);
    $c = 0;
    foreach ($gen as $x) {
        var_dump($x);
        if ($x === 2 || $x === 1) $gen->send("sent");
        if (++$c > 20) { echo "runaway\n"; break; }
    }
    echo "--\n";
}
foreach (['both', 's1', 's2', 's3', 's4'] as $f) {
    $c = 0;
    foreach ($f(3) as $x) {
        echo var_export($x, true), " ";
        if (++$c > 20) { echo "runaway"; break; }
    }
    echo "\n";
}
?>
--EXPECT--
int(0)
int(2)
int(4)
--
int(0)
int(2)
int(4)
--
int(0)
int(1)
int(2)
--
int(0)
int(1)
int(2)
--
0 NULL 2 NULL 4 NULL 
0 1 2 
0 100 1 101 2 102 
0 7 1 7 2 7 
5 0 5 1 5 2 
