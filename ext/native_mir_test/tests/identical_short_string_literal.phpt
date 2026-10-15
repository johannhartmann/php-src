--TEST--
Native === and !== against short string literals decide by type, length and bytes
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class P { public $v; function __construct($v) { $this->v = $v; } }
function check($x) {
    $r = '';
    if ($x === '') { $r .= 'E'; }
    if ($x === 'a') { $r .= 'A'; }
    if ($x === 'seven77') { $r .= '7'; }
    if ($x === 'eight888') { $r .= '8'; }
    if ($x === 'nine99999') { $r .= '9'; }
    if ($x === 'sixteen-16-bytes') { $r .= 'S'; }
    if ($x === 'seventeen-17bytes') { $r .= 'L'; }
    if ($x !== 'a') { $r .= 'n'; }
    return $r === '' ? '-' : $r;
}
function prop(P $p) {
    return ($p->v === 'eight888' ? 'P' : 'p') . ($p->v !== 'x' ? 'q' : 'Q');
}
function undefined_var() {
    if ($nothing === 'a') { return 'yes'; }
    return 'no';
}
function by_ref() {
    $v = 'seven77';
    $r = &$v;
    return $r === 'seven77' ? 'R' : 'r';
}
$values = ['', 'a', 'b', 'seven77', 'seven7', 'eight888', 'eight889', 'nine99999', 'nine9999x',
    'sixteen-16-bytes', 'sixteen-16-bytez', 'seventeen-17bytes', 1, 1.0, null, true, ['a'], str_repeat('a', 1)];
foreach ($values as $value) {
    echo json_encode($value), ' ', check($value), "\n";
}
echo prop(new P('eight888')), prop(new P(str_repeat('eight888', 1))), prop(new P(8)), "\n";
echo undefined_var(), "\n";
echo by_ref(), "\n";
?>
--EXPECTF--
"" En
"a" A
"b" n
"seven77" 7n
"seven7" n
"eight888" 8n
"eight889" n
"nine99999" 9n
"nine9999x" n
"sixteen-16-bytes" Sn
"sixteen-16-bytez" n
"seventeen-17bytes" Ln
1 n
1 n
null n
true n
["a"] n
"a" A
PqPqpq

Warning: Undefined variable $nothing in %s on line %d
no
R
