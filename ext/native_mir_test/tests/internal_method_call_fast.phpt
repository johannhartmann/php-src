--TEST--
Native: internal methods, static methods, constructors and variadic or by-reference internal functions on the fast call path
--DESCRIPTION--
Each site runs several times, so its later calls take the published fast
path: internal methods on CV, temporary and $this receivers, parent:: calls
of internal methods, internal static methods, new of internal classes
(also when the constructor throws), variadic internal functions and
by-reference CV arguments.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeInternalFast;

class Stack extends \ArrayObject {
    public function top() { return $this->count(); }
}

class Prop extends \ReflectionProperty {
    public function __construct(string $class, string $name) {
        parent::__construct($class, $name);
    }
    public function read(object $o) { return parent::getValue($o); }
}

class Holder { public $value = 3; private array $stack = [1, 2, 3]; public function last() { return end($this->stack); } }

function run(int $i) {
    $s = new Stack([1, 2, $i]);
    $out = [$s->count(), $s->top(), (new \ArrayObject([$i]))->count()];
    $it = new \ArrayIterator(['a' => $i, 'b' => 2]);
    $out[] = $it->key() . '=' . $it->current();
    $p = new Prop(Holder::class, 'value');
    $out[] = $p->read(new Holder);
    $out[] = \DateTimeImmutable::createFromFormat('Y-m-d', '2020-01-0' . ($i + 1))->format('d');
    $out[] = sprintf('%d-%s-%d', $i, 'x', $i * 2);
    $out[] = count(array_merge([1], [2, 3], [$i]));
    $out[] = implode(',', array_diff_key(['a' => 1, 'b' => 2, 'c' => $i], ['b' => 0]));
    $arr = [3, 1, $i];
    sort($arr);
    $out[] = implode(',', $arr);
    $m = null;
    $out[] = preg_match('/(\d)/', "x$i", $m) . ':' . $m[1];
    $out[] = (new Holder)->last();
    $e = new \RuntimeException('line');
    $out[] = $e->getLine();
    try {
        new \DateTimeZone($i == 1 ? 'Not/AZone' : 'UTC');
        $out[] = 'tz';
    } catch (\Exception $e) {
        $out[] = get_class($e);
    }
    return implode(' ', $out);
}

for ($i = 0; $i < 3; $i++) {
    echo run($i), "\n";
}
?>
--EXPECT--
3 3 1 a=0 3 01 0-x-0 4 1,0 0,1,3 1:0 3 34 tz
3 3 1 a=1 3 02 1-x-2 4 1,1 1,1,3 1:1 3 34 DateInvalidTimeZoneException
3 3 1 a=2 3 03 2-x-4 4 1,2 1,2,3 1:2 3 34 tz
