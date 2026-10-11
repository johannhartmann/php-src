--TEST--
Native defined() answers from the run-time cache
--DESCRIPTION--
ZEND_DEFINED caches a found constant and a miss keyed by the constant table's size, as the VM does: constants defined later in the request, in a loop or under another name invalidate a cached miss.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NS;

function probe() {
    return [\defined('A1'), \defined('B1'), \defined('A1'), \defined('PHP_VERSION') && true, \defined('E_ALL')];
}
function loop() {
    $out = [];
    for ($i = 0; $i < 4; $i++) {
        $out[] = \defined('L' . 0) ? 'L' : '-';
        $out[] = \defined('L0') ? 'y' : 'n';
        if ($i == 1) {
            define('L0', 1);
        }
        if ($i == 2) {
            define('OTHER' . $i, 1);
        }
    }
    return implode('', $out);
}
function cv() {
    $r = [];
    for ($i = 0; $i < 3; $i++) {
        $x = \defined('CV_C');
        $r[] = $x;
        if (!\defined('CV_C')) {
            define('CV_C', [1, 2]);
        }
    }
    return $r;
}
echo json_encode(probe()), "\n";
define('A1', 'a');
echo json_encode(probe()), "\n";
const B1 = 2;
echo json_encode(probe()), "\n";
echo loop(), "\n";
echo json_encode(cv()), "\n";
echo json_encode([\defined('A1'), \defined('L0'), \defined('Nope')]), "\n";
class K { const X = 1; }
echo json_encode([\defined('X1'), \defined('K::X')]), "\n";
for ($i = 0; $i < 3; $i++) {
    echo \defined('LATE') ? "late $i\n" : "no $i\n";
    if ($i == 0) { define('LATE', 1); }
}
--EXPECT--
[false,false,false,true,true]
[true,false,true,true,true]
[true,false,true,true,true]
-n-nLyLy
[false,true,true]
[true,true,false]
[false,false]
no 0
late 1
late 2
