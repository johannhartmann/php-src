--TEST--
String interpolation: literal pieces stored inline, scalar pieces converted without decoding, ROPE_END into a reused temporary
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class T { function __toString(): string { return 'T!'; } }
function r($a, $b, $c) {
    $s = "[$a|$b]";
    $t = "x{$a}y{$b}z{$c}w";
    $u = "<div class=\"{$a}\" id=\"" . $b . "\">{$c}</div>";
    $v = "$a";
    return "$s $t $u $v";
}
$ref = 'refd';
$alias = &$ref;
foreach ([['s', 'q', 3], [1, -2, 0], [null, true, false], [1.5, 'f', 2.25], [new T, 'o', PHP_INT_MAX], [$alias, $ref, 'r']] as [$a, $b, $c]) {
    echo r($a, $b, $c), "\n";
}
function arr() { $x = [1]; return "a{$x}b"; }
echo arr(), "\n";
function undef() { return "u{$nope}v"; }
echo undef(), "\n";
for ($i = 0; $i < 3; $i++) { $k = $i * 10; echo "loop {$i}:{$k}", "\n"; }
--EXPECTF--
[s|q] xsyqz3w <div class="s" id="q">3</div> s
[1|-2] x1y-2z0w <div class="1" id="-2">0</div> 1
[|1] xy1zw <div class="" id="1"></div> 
[1.5|f] x1.5yfz2.25w <div class="1.5" id="f">2.25</div> 1.5
[T!|o] xT!yoz9223372036854775807w <div class="T!" id="o">9223372036854775807</div> T!
[refd|refd] xrefdyrefdzrw <div class="refd" id="refd">r</div> refd

Warning: Array to string conversion in %s on line 15
aArrayb

Warning: Undefined variable $nope in %s on line 17
uv
loop 0:0
loop 1:10
loop 2:20
