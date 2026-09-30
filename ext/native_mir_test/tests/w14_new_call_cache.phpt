--TEST--
Native new, polymorphic method and reference receiver calls through the resolution cache
--DESCRIPTION--
A call site keeps entries for its most recent targets: method sites keyed by
the receiver class see alternating classes and inherited methods, global and
static receivers are references, and new keyed by its class creates each
object before its cached constructor call: literal, self, static and parent
classes, constructors that throw, private constructors, classes without a
constructor and internal classes with their own object handlers.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class W14Shape {
    public static $made = 0;
    public function __construct(public $size = 1) { static::$made++; }
    public function area() { return $this->size * $this->size; }
    public static function make($size) { return new static($size); }
    public static function plain($size) { return new self($size); }
}
class W14Circle extends W14Shape {
    public function area() { return (int) (3 * $this->size * $this->size); }
    public static function base($size) { return new parent($size); }
}
class W14Square extends W14Shape {}
class W14Fragile {
    public function __construct($ok) { if (!$ok) { throw new RuntimeException('fragile'); } }
}
class W14Closed {
    private function __construct(public $v) {}
    public static function open($v) { return new W14Closed($v); }
}
class W14Bare { public $v = 'bare'; }
$w14_global = new W14Square(4);

function w14_areas(array $shapes)
{
    $sum = 0;
    foreach ($shapes as $shape) {
        $sum += $shape->area();
    }
    return $sum;
}

function w14_global_area()
{
    global $w14_global;
    static $kept;
    $kept ??= new W14Circle(2);
    return $w14_global->area() + $kept->area();
}

function w14_run($round)
{
    $out = [];
    $shapes = [];
    for ($i = 1; $i <= 3; $i++) {
        $shapes[] = new W14Shape($i);
        $shapes[] = new W14Circle($i);
        $shapes[] = new W14Square($i);
    }
    $out[] = w14_areas($shapes);
    $out[] = get_class(W14Circle::make(2)) . ':' . W14Circle::make(2)->area();
    $out[] = get_class(W14Circle::plain(2));
    $out[] = get_class(W14Circle::base(3)) . ':' . W14Circle::base(3)->area();
    $out[] = w14_global_area();
    try {
        new W14Fragile($round !== 1);
        $out[] = 'built';
    } catch (RuntimeException $e) {
        $out[] = $e->getMessage();
    }
    $out[] = W14Closed::open($round)->v;
    try {
        new W14Closed($round);
    } catch (Error $e) {
        $out[] = 'closed';
    }
    $out[] = (new W14Bare)->v;
    $out[] = count(new ArrayObject([1, 2, $round]));
    $out[] = (new DateTimeImmutable('@86400'))->format('Y-m-d');
    return $out;
}
for ($r = 0; $r < 3; $r++) {
    echo json_encode(w14_run($r)), "\n";
}
echo W14Shape::$made, "\n";
?>
--EXPECT--
[70,"W14Circle:12","W14Shape","W14Shape:9",28,"built",0,"closed","bare",3,"1970-01-02"]
[70,"W14Circle:12","W14Shape","W14Shape:9",28,"fragile",1,"closed","bare",3,"1970-01-02"]
[70,"W14Circle:12","W14Shape","W14Shape:9",28,"built",2,"closed","bare",3,"1970-01-02"]
44
