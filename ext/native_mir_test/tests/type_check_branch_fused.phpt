--TEST--
Native TYPE_CHECK of a CV fused into its branch
--DESCRIPTION--
An is_*() type check of a CV whose only consumer is the following
JMPZ/JMPNZ is evaluated by the branch, like the VM's smart branch: the
mask bit of the type or of a reference's referent decides; an undefined CV
still warns through the helper, and is_resource() keeps its own form.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function kinds($v) {
    $o = '';
    if (is_array($v)) $o .= 'a';
    if (is_string($v)) $o .= 's';
    if (!is_int($v)) $o .= '!i';
    if (is_bool($v)) $o .= 'b';
    if (is_null($v)) $o .= 'n';
    if (is_float($v)) $o .= 'f';
    if (is_object($v)) $o .= 'o';
    if (!is_scalar($v)) $o .= '!sc';
    if (is_iterable($v)) $o .= 'it';
    if (is_numeric($v)) $o .= '#';
    return $o;
}
function through_reference($x) {
    $r = &$x;
    $o = is_array($r) ? 'A' : '-';
    while (is_string($r) && strlen($r) < 3) { $r .= 'x'; }
    return $o . (is_string($x) ? $x : gettype($x));
}
function undefined_cv() {
    $o = is_null($missing) ? 'null' : 'set';
    if (!is_array($other)) $o .= '/notarray';
    return $o;
}
function resource_check($h) { return is_resource($h) ? 'res' : 'nores'; }
$values = [[1], 'str', 3, 2.5, true, null, new ArrayObject([]), '42'];
for ($i = 0; $i < 3; $i++) {
    foreach ($values as $v) { echo kinds($v), ' '; }
    echo through_reference('a'), ' ', through_reference([1]), ' ', undefined_cv(), ' ';
    $f = fopen('php://memory', 'r'); echo resource_check($f); fclose($f); echo ' ', resource_check($f), "\n";
}
function concat_ternary($items, $lim) {
    $out = '';
    foreach ($items as $i => $s) {
        $out .= ($i < $lim) ? $s : strtoupper($s);
        $out .= is_string($s) ? $s : '-';
    }
    return $out;
}
function serialize_block($block) {
    $content = '';
    $index = 0;
    foreach ($block['innerContent'] as $chunk) {
        $content .= is_string($chunk) ? $chunk : serialize_block($block['innerBlocks'][$index++]);
    }
    return "<{$block['name']}>$content</{$block['name']}>";
}
$leaf = ['name' => 'p', 'innerContent' => [str_repeat('x', 2), 'y'], 'innerBlocks' => []];
$tree = ['name' => 'g', 'innerContent' => [str_repeat('<', 2), null, '|', null, '>'], 'innerBlocks' => [$leaf, $leaf]];
for ($r = 0; $r < 4; $r++) {
    echo concat_ternary([str_repeat('a', 2), 'b' . $r, 7, str_repeat('c', 3)], 2), ' ', serialize_block($tree), "\n";
}
?>
--EXPECTF--
a!i!scit s!i # !if# !ib !in!sc !io!scit s!i# -axx Aarray 
Warning: Undefined variable $missing in %s on line 23

Warning: Undefined variable $other in %s on line 24
null/notarray res nores
a!i!scit s!i # !if# !ib !in!sc !io!scit s!i# -axx Aarray 
Warning: Undefined variable $missing in %s on line 23

Warning: Undefined variable $other in %s on line 24
null/notarray res nores
a!i!scit s!i # !if# !ib !in!sc !io!scit s!i# -axx Aarray 
Warning: Undefined variable $missing in %s on line 23

Warning: Undefined variable $other in %s on line 24
null/notarray res nores
aaaab0b07-CCCccc <g><<<p>xxy</p>|<p>xxy</p>></g>
aaaab1b17-CCCccc <g><<<p>xxy</p>|<p>xxy</p>></g>
aaaab2b27-CCCccc <g><<<p>xxy</p>|<p>xxy</p>></g>
aaaab3b37-CCCccc <g><<<p>xxy</p>|<p>xxy</p>></g>
