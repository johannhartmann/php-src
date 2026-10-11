--TEST--
Native internal calls whose argument warning throws run finally blocks
--DESCRIPTION--
An undefined variable passed to an internal function under a throwing error handler leaves the call through the enclosing finally and catch blocks as the VM does.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
set_error_handler(function ($no, $msg) { throw new ErrorException($msg); });
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n}|"; } }
function run($i) {
    $log = [];
    try {
        try {
            $log[] = str_pad($undef, 3);
        } finally {
            $log[] = 'fin1';
        }
    } catch (ErrorException $e) {
        $log[] = 'caught ' . $e->getMessage();
    }
    try {
        try {
            $log[] = str_repeat(new D("x$i") ? 'a' : 'b', $undef2);
        } finally {
            $log[] = 'fin2';
        }
    } catch (ErrorException $e) {
        $log[] = 'caught2 ' . $e->getMessage();
    }
    try {
        $x = strlen("a") + strlen($undef3) + strlen("b");
    } catch (ErrorException $e) {
        $log[] = 'caught3 ' . $e->getMessage();
    }
    try {
        foreach ([1] as $v) {
            try {
                echo sprintf('%s-%s', new D("y$i") ? 'q' : 'r', $undef4);
            } finally {
                $log[] = 'fin4';
            }
        }
    } catch (ErrorException $e) {
        $log[] = 'caught4 ' . $e->getMessage();
    }
    return $log;
}
echo json_encode(run(1)), "\n";
echo json_encode(run(2)), "\n";
--EXPECT--
~x1|~y1|["fin1","caught Undefined variable $undef","fin2","caught2 Undefined variable $undef2","caught3 Undefined variable $undef3","fin4","caught4 Undefined variable $undef4"]
~x2|~y2|["fin1","caught Undefined variable $undef","fin2","caught2 Undefined variable $undef2","caught3 Undefined variable $undef3","fin4","caught4 Undefined variable $undef4"]
