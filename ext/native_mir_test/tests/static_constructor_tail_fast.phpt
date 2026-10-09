--TEST--
Native: parent::__construct() and by-value arguments after a by-reference one on the fast call path
--DESCRIPTION--
A static call naming no method calls its class's constructor; arguments
after a by-reference parameter are decided at run time (runtime tail) and
still take the fast path where the parameter is by value. User and
internal targets, with exceptions from the constructor.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeCtorTail;

class Base {
    public array $log = [];
    public function __construct(public int $v, string $tag = 'b') {
        if ($v < 0) { throw new \InvalidArgumentException("neg $v"); }
        $this->log[] = $tag;
    }
}
class Child extends Base {
    public function __construct(int $v) { parent::__construct($v * 2, 'c'); $this->log[] = 'child'; }
}
class Grand extends Child {
    public function __construct(int $v) { parent::__construct($v + 1); $this->log[] = 'grand'; }
}
class Stamp extends \DateTimeImmutable {
    public function __construct(string $when) { parent::__construct($when, new \DateTimeZone('UTC')); }
}
function tail(&$out, $a, $b = 3) { $out .= "$a$b"; return strlen($out); }

for ($i = 0; $i < 3; $i++) {
    $g = new Grand($i);
    $s = new Stamp("2020-01-0" . ($i + 1));
    $out = '';
    $n = tail($out, $i, $i * 2) + tail($out, 'x');
    echo $g->v, ' ', implode(',', $g->log), ' ', $s->format('d'), ' ', $out, ' ', $n, ' ', preg_match_all('/\d/', "a1b2c$i", $m, PREG_PATTERN_ORDER), "\n";
    try {
        new Grand(-5 - $i);
    } catch (\InvalidArgumentException $e) {
        echo $e->getMessage(), ' ', $e->getLine(), "\n";
    }
}
?>
--EXPECT--
2 c,child,grand 01 00x3 6 3
neg -8 7
4 c,child,grand 02 12x3 6 3
neg -10 7
6 c,child,grand 03 24x3 6 3
neg -12 7
