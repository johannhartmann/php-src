--TEST--
Native persistent code for scripts whose classes link at runtime
--DESCRIPTION--
A cached script whose class extends a class of another file links it when the
include runs. Its persistent generation compiles on demand, binds only the
linked, immutable class entry once it exists, and serves the functions and
methods of the script, including a function next to an anonymous class.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$base = __DIR__ . '/opcache_runtime_linked_owner_generations_base.inc';
$child = __DIR__ . '/opcache_runtime_linked_owner_generations_child.inc';
file_put_contents($base, <<<'PHP'
<?php
class NativeLinkedBase
{
    protected array $items = [];
    public function add(string $item): static { $this->items[] = $item; return $this; }
    public function count(): int { return count($this->items); }
}
PHP);
file_put_contents($child, <<<'PHP'
<?php
class NativeLinkedChild extends NativeLinkedBase implements Countable
{
    public function addTwice(string $item): static { return $this->add($item)->add(strtoupper($item)); }
    public function joined(): string { return implode(',', $this->items); }
    public static function make(): static { return new static(); }
}
function native_linked_describe(NativeLinkedChild $child): string
{
    $wrapper = new class($child) {
        public function __construct(private NativeLinkedChild $child) {}
        public function text(): string { return $this->child->count() . ':' . $this->child->joined(); }
    };
    return $wrapper->text();
}
PHP);
try {
    require $base;
    require $child;
    $results = [];
    for ($i = 0; $i < 3; $i++) {
        $object = NativeLinkedChild::make()->addTwice('a')->addTwice('b' . $i);
        $results[] = native_linked_describe($object) . '|' . count($object);
    }
    echo implode("\n", $results), "\n";
} finally {
    unlink($base);
    unlink($child);
}
?>
--EXPECT--
4:a,A,b0,B0|4
4:a,A,b1,B1|4
4:a,A,b2,B2|4
