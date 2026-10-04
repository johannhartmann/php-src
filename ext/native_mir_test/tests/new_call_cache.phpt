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
class NativeShape {
    public static $made = 0;
    public function __construct(public $size = 1) { static::$made++; }
    public function area() { return $this->size * $this->size; }
    public static function make($size) { return new static($size); }
    public static function plain($size) { return new self($size); }
}
class NativeCircle extends NativeShape {
    public function area() { return (int) (3 * $this->size * $this->size); }
    public static function base($size) { return new parent($size); }
}
class NativeSquare extends NativeShape {}
class NativeFragile {
    public function __construct($ok) { if (!$ok) { throw new RuntimeException('fragile'); } }
}
class NativeClosed {
    private function __construct(public $v) {}
    public static function open($v) { return new NativeClosed($v); }
}
class NativeBare { public $v = 'bare'; }
$native_global = new NativeSquare(4);

function native_areas(array $shapes)
{
    $sum = 0;
    foreach ($shapes as $shape) {
        $sum += $shape->area();
    }
    return $sum;
}

function native_global_area()
{
    global $native_global;
    static $kept;
    $kept ??= new NativeCircle(2);
    return $native_global->area() + $kept->area();
}

function native_run($round)
{
    $out = [];
    $shapes = [];
    for ($i = 1; $i <= 3; $i++) {
        $shapes[] = new NativeShape($i);
        $shapes[] = new NativeCircle($i);
        $shapes[] = new NativeSquare($i);
    }
    $out[] = native_areas($shapes);
    $out[] = get_class(NativeCircle::make(2)) . ':' . NativeCircle::make(2)->area();
    $out[] = get_class(NativeCircle::plain(2));
    $out[] = get_class(NativeCircle::base(3)) . ':' . NativeCircle::base(3)->area();
    $out[] = native_global_area();
    try {
        new NativeFragile($round !== 1);
        $out[] = 'built';
    } catch (RuntimeException $e) {
        $out[] = $e->getMessage();
    }
    $out[] = NativeClosed::open($round)->v;
    try {
        new NativeClosed($round);
    } catch (Error $e) {
        $out[] = 'closed';
    }
    $out[] = (new NativeBare)->v;
    $out[] = count(new ArrayObject([1, 2, $round]));
    $out[] = (new DateTimeImmutable('@86400'))->format('Y-m-d');
    return $out;
}
for ($r = 0; $r < 3; $r++) {
    echo json_encode(native_run($r)), "\n";
}
echo NativeShape::$made, "\n";
?>
--EXPECT--
[70,"NativeCircle:12","NativeShape","NativeShape:9",28,"built",0,"closed","bare",3,"1970-01-02"]
[70,"NativeCircle:12","NativeShape","NativeShape:9",28,"fragile",1,"closed","bare",3,"1970-01-02"]
[70,"NativeCircle:12","NativeShape","NativeShape:9",28,"built",2,"closed","bare",3,"1970-01-02"]
44
