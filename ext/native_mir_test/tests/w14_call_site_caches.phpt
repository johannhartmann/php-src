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
$library = __DIR__ . '/w14_call_site_caches_library.inc';
file_put_contents($library, <<<'PHP'
<?php
function w14_site_label(string $prefix, int $value): string { return $prefix . $value; }
PHP);

class W14SiteA { public function label($i) { return 'a' . $i; } }
class W14SiteB extends W14SiteA { public function label($i) { return 'b' . $i; } }
class W14SiteC { public function __call($name, $arguments) { return $name . ':' . $arguments[0]; } }
final class W14SiteD { public function label($i) { return w14_site_label('d', $i); } }

function w14_site_labels(array $objects): string
{
    $out = [];
    foreach ($objects as $i => $object) {
        $out[] = $object->label($i);
    }
    return implode(',', $out);
}

try {
    require $library;
    $objects = [new W14SiteA, new W14SiteB, new W14SiteA, new W14SiteC, new W14SiteC, new W14SiteD, new W14SiteB];
    for ($round = 0; $round < 3; $round++) {
        echo w14_site_labels($objects), "\n";
    }
} finally {
    unlink($library);
}
?>
--EXPECT--
a0,b1,a2,label:3,label:4,d5,b6
a0,b1,a2,label:3,label:4,d5,b6
a0,b1,a2,label:3,label:4,d5,b6
