--TEST--
Native: constant-expression defaults on the fast call path
--DESCRIPTION--
Defaults such as self::X or a global constant are evaluated as RECV_INIT
does, cached per request when they are not counted and had no side
effects; a value the parameter type would coerce, objects created by new
in an initializer, undefined constants and constants defined later take
the generic receive or report as the VM does.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeConstDefault;

class Counter { public static int $made = 0; public function __construct() { self::$made++; } }
class Router {
    const ABSOLUTE = 1;
    const RELATIVE = 'rel';
    const LIST = [1, 2, 3];
    const SCALE = 2;
    public function generate($name, array $parameters = [], int $type = self::ABSOLUTE, string $mode = self::RELATIVE) {
        return "$name/" . count($parameters) . "/$type/$mode";
    }
    public function sum(array $list = self::LIST, float $scale = self::SCALE) { return array_sum($list) * $scale . gettype($scale); }
    public function made(Counter $c = new Counter) { return Counter::$made; }
    public function late($v = LATE_CONST) { return $v; }
}

$r = new Router;
for ($i = 0; $i < 3; $i++) {
    echo $r->generate("home$i"), ' ', $r->generate('x', [1]), ' ', $r->generate('y', [], 2), ' ', $r->sum(), ' ', $r->made(), "\n";
    try {
        echo $r->late(), "\n";
    } catch (\Error $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
        define('NativeConstDefault\LATE_CONST', "late$i");
    }
}
?>
--EXPECT--
home0/0/1/rel x/1/1/rel y/0/2/rel 12double 1
Error: Undefined constant "NativeConstDefault\LATE_CONST"
home1/0/1/rel x/1/1/rel y/0/2/rel 12double 2
late0
home2/0/1/rel x/1/1/rel y/0/2/rel 12double 3
late0
