--TEST--
Native includes of cached scripts share persistent code and reentry
--DESCRIPTION--
An include of an OPcache script whose classes are linked when loaded runs the
code of the script's persistent generation. A script with a class that the
include links at runtime keeps its request-local compilation, so methods
called on $this see the runtime class. Repeated calls of an included function
resolve through one generation instead of compiling per call.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$library = __DIR__ . '/opcache_cached_include_generations_library.inc';
$linked = __DIR__ . '/opcache_cached_include_generations_linked.inc';
$body = __DIR__ . '/opcache_cached_include_generations_body.inc';
file_put_contents($library, <<<'PHP'
<?php
function native_cached_add(int $a, int $b): int { return $a + $b; }
function native_cached_scale(array $values, int $factor): array
{
    $result = [];
    foreach ($values as $key => $value) {
        $result[$key] = native_cached_add($value * $factor, 0);
    }
    return $result;
}
final class NativeCachedPlain
{
    private int $total = 0;
    public function add(int $value): static { $this->total = native_cached_add($this->total, $value); return $this; }
    public function total(): int { return $this->total; }
}
PHP);
file_put_contents($linked, <<<'PHP'
<?php
final class NativeCachedCounted implements Countable, IteratorAggregate
{
    private array $items = [];
    public function push(string $item): void { $this->items[] = $item; $this->touch(); }
    private function touch(): void { $this->items = array_values($this->items); }
    public function count(): int { return count($this->items); }
    public function getIterator(): Iterator { return new ArrayIterator($this->items); }
    public function joined(): string { return implode(',', iterator_to_array($this)); }
}
PHP);
file_put_contents($body, <<<'PHP'
<?php
$native_local = isset($native_local) ? $native_local + 1 : 1;
return native_cached_add($native_local, $native_base);
PHP);

try {
    require $library;
    require $linked;
    $native_base = 100;
    $sum = 0;
    for ($i = 0; $i < 40; $i++) {
        $sum += include $body;
    }
    var_dump($sum, $native_local);

    $plain = new NativeCachedPlain();
    for ($i = 1; $i <= 1000; $i++) {
        $plain->add($i);
    }
    var_dump($plain->total(), native_cached_scale([1, 2, 3], 7));

    $counted = new NativeCachedCounted();
    foreach (['a', 'b', 'c'] as $item) {
        $counted->push($item);
    }
    var_dump(count($counted), $counted->joined());

    function native_cached_scope(string $file): int
    {
        $native_base = 1000;
        return include $file;
    }
    var_dump(native_cached_scope($body));
} finally {
    unlink($library);
    unlink($linked);
    unlink($body);
}
?>
--EXPECT--
int(4820)
int(40)
int(500500)
array(3) {
  [0]=>
  int(7)
  [1]=>
  int(14)
  [2]=>
  int(21)
}
int(3)
string(5) "a,b,c"
int(1001)
