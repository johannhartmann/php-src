--TEST--
Native: $object->$method(...$arguments) takes the recorded target of each class and method
--DESCRIPTION--
A method named by a CV, as Twig's attribute access calls getters, finds the
target the resolver recorded for the receiver's class and the name; the
arguments are sent and unpacked to the frame as the VM does: empty,
positional and named unpacking, defaults, by-reference parameters,
$this receivers, temporaries, __call and errors.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeVariableMethod;

class Post {
    public function __construct(private string $title) {}
    public function getTitle() { return $this->title; }
    public function format(string $prefix = '#', int $times = 1) { return str_repeat($prefix, $times) . $this->title; }
    public function bump(&$counter, $by = 1) { $counter += $by; return $counter; }
    public function self() { return $this; }
    public function call(string $m, array $a) { return $this->$m(...$a); }
}
class Magic { public function __call($name, $args) { return "magic $name(" . count($args) . ")"; } }
class Page extends Post { public function getTitle() { return 'page:' . parent::getTitle(); } }

function attribute($object, $method, array $arguments = []) {
    return $object->$method(...$arguments);
}

$objects = [new Post('a'), new Page('b'), new Post('c'), new Magic];
for ($round = 0; $round < 3; $round++) {
    $out = [];
    foreach ($objects as $o) {
        $out[] = attribute($o, 'getTitle');
        $out[] = attribute($o, 'format', ['*', $round + 1]);
        $out[] = attribute($o, 'format', ['times' => 2]);
    }
    $counter = 10;
    $args = [&$counter, 5];
    $out[] = attribute($objects[0], 'bump', $args) . '/' . $counter;
    $out[] = $objects[1]->call('getTitle', []);
    $m = 'getTitle';
    $out[] = (new Post("t$round"))->$m(...[]);
    $out[] = $objects[2]->self()->$m();
    echo implode(' ', $out), "\n";
}
try {
    attribute($objects[0], 'missing');
} catch (\Error $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
echo attribute($objects[0], 'format', ['-', 2, 3]), "\n";
try {
    attribute($objects[0], 'format', ['-', 'x' => 1]);
} catch (\Error $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
?>
--EXPECT--
a *a ##a page:b *b ##b c *c ##c magic getTitle(0) magic format(2) magic format(1) 15/15 page:b t0 c
a **a ##a page:b **b ##b c **c ##c magic getTitle(0) magic format(2) magic format(1) 15/15 page:b t1 c
a ***a ##a page:b ***b ##b c ***c ##c magic getTitle(0) magic format(2) magic format(1) 15/15 page:b t2 c
Error: Call to undefined method NativeVariableMethod\Post::missing()
--a
Error: Unknown named parameter $x
