--TEST--
Native short-circuit returns and methods of classes linked at runtime
--DESCRIPTION--
`return $a && $b` returns the PHI of the JMPZ_EX result and the right
operand, not the right operand's definition, on the edge that skips it.
A method call on an object of a class that is linked only when its
declaration runs (here an anonymous class extending a class from another
file) must not bind the cached, unlinked class entry directly.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class W14Tag
{
    public $closing;
    public function __construct($closing) { $this->closing = $closing; }
    public function tag() { return 'DIV'; }
    public function closer() { return $this->closing && 'BR' !== $this->tag(); }
}
foreach ([true, false, null, 1, ''] as $closing) {
    $tag = new W14Tag($closing);
    var_dump($tag->closer(), $tag->closer());
}

$base = __DIR__ . '/w14_short_circuit_return_and_unlinked_receivers_base.inc';
$factory = __DIR__ . '/w14_short_circuit_return_and_unlinked_receivers_factory.inc';
file_put_contents($base, <<<'PHP'
<?php
class W14AnonymousBase
{
    protected $text;
    public function __construct($text) { $this->text = $text; }
    public function text() { return $this->text; }
}
PHP);
file_put_contents($factory, <<<'PHP'
<?php
function w14_anonymous_append($text)
{
    $object = new class($text) extends W14AnonymousBase {
        public function append(string $suffix) { $this->text .= $suffix; }
    };
    $object->append('!');
    return $object->text();
}
PHP);
try {
    require $base;
    require $factory;
    echo w14_anonymous_append('a'), w14_anonymous_append('b'), "\n";
} finally {
    unlink($base);
    unlink($factory);
}
?>
--EXPECT--
bool(true)
bool(true)
bool(false)
bool(false)
bool(false)
bool(false)
bool(true)
bool(true)
bool(false)
bool(false)
a!b!
