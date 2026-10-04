--TEST--
Native call-site caches for method and named function calls
--DESCRIPTION--
A method call site caches the receiver class and method like the VM and
misses when the class changes; __call trampolines are never cached. A call
by name to a function of another file caches the resolved function.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$library = __DIR__ . '/call_site_caches_library.inc';
file_put_contents($library, <<<'PHP'
<?php
function native_site_label(string $prefix, int $value): string { return $prefix . $value; }
PHP);

class NativeSiteA { public function label($i) { return 'a' . $i; } }
class NativeSiteB extends NativeSiteA { public function label($i) { return 'b' . $i; } }
class NativeSiteC { public function __call($name, $arguments) { return $name . ':' . $arguments[0]; } }
final class NativeSiteD { public function label($i) { return native_site_label('d', $i); } }

function native_site_labels(array $objects): string
{
    $out = [];
    foreach ($objects as $i => $object) {
        $out[] = $object->label($i);
    }
    return implode(',', $out);
}

try {
    require $library;
    $objects = [new NativeSiteA, new NativeSiteB, new NativeSiteA, new NativeSiteC, new NativeSiteC, new NativeSiteD, new NativeSiteB];
    for ($round = 0; $round < 3; $round++) {
        echo native_site_labels($objects), "\n";
    }
} finally {
    unlink($library);
}
?>
--EXPECT--
a0,b1,a2,label:3,label:4,d5,b6
a0,b1,a2,label:3,label:4,d5,b6
a0,b1,a2,label:3,label:4,d5,b6
