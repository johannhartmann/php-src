--TEST--
Cached property resolutions stay correct across requests while a class file changes
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
catch_workers_output = yes
php_admin_flag[display_errors] = On
EOT;

$lib = sys_get_temp_dir() . '/fpm-property-sites-lib.php';
$state = sys_get_temp_dir() . '/fpm-property-sites-state.txt';
/* Each version moves the properties and changes their visibility. */
$sources = [
    "<?php class PropLib { private \$a = 'a0'; public \$b = 'b0'; }",
    "<?php class PropLib { public \$z = 0; public \$b = 'b2'; public \$a = 'a2'; }",
    "<?php class PropLib { public \$b = 'b4'; protected \$a = 'a4'; }",
];
file_put_contents($lib, $sources[0]);
@unlink($state);
$sourceList = var_export($sources, true);

$code = <<<EOT
<?php
\$lib = '$lib';
\$state = '$state';
\$sources = $sourceList;
EOT . <<<'EOT'

$n = (int) @file_get_contents($state);
file_put_contents($state, $n + 1);
require_once $lib;

class Holder {
    private $priv;
    protected $prot = 'q';
    public $pub;
    function __construct($v) { $this->priv = "p$v"; $this->pub = "u$v"; }
    function readOther(Holder $h) { return $h->priv . $h->prot; }
}
class SubHolder extends Holder {
    function readProt() { return $this->prot; }
    function readPriv() { return isset($this->priv) ? 'set' : 'unset'; }
}
trait T { function tp() { return $this->tv; } }
class TA { use T; private $tv = 'ta'; }
class TB { use T; public $x = 0; private $tv = 'tb'; }
class Typed { public int $n = 0; }
class RO { function __construct(public readonly int $v) {} }
class M { private $hidden = 'h'; function __get($k) { return "magic:$k"; } }
class Poly1 { public $name = 'one'; }
class Poly2 { public $pad = 0; public $name = 'two'; }

function peek($o) { try { return $o->priv; } catch (Error $e) { return 'E'; } }
function pub($o) { $o->pub .= '!'; return $o->pub; }
function name($o) { return $o->name; }
function setN($o, $v) { try { $o->n = $v; return $o->n; } catch (TypeError $e) { return 'T'; } }
function bump($o) { try { $o->v++; return $o->v; } catch (Error $e) { return 'R'; } }
function hidden($o) { return $o->hidden; }
function readLib($o) {
    try { $a = $o->a; } catch (Error $e) { $a = 'E'; }
    return $a . '/' . $o->b;
}

$h = new Holder($n);
$s = new SubHolder($n + 10);
$out = [];
$out[] = $h->readOther($s);
$out[] = $s->readProt();
$out[] = $s->readPriv();
$out[] = peek($h);
$out[] = pub($h);
$out[] = (new TA)->tp() . (new TB)->tp();
$f = function () { return $this->priv; };
$out[] = Closure::bind($f, $h, Holder::class)();
try { $out[] = Closure::bind($f, $h, SubHolder::class)(); } catch (Error $e) { $out[] = 'CE'; }
$out[] = name(new Poly1) . name(new Poly2) . name(new Poly1);
$t = new Typed;
$out[] = setN($t, "5") . setN($t, "x");
$out[] = bump(new RO($n));
$out[] = hidden(new M);
$out[] = readLib(new PropLib);
if ($n % 2 == 1 && isset($sources[($n + 1) / 2])) {
    file_put_contents($lib, $sources[($n + 1) / 2]);
    opcache_invalidate($lib, true);
}
echo $n, ' ', implode(',', $out), "\n";
EOT;

$tester = new FPM\Tester($cfg, $code);
$tester->start(iniEntries: [
    'opcache.enable' => '1',
    'opcache.file_update_protection' => '0',
    'opcache.validate_timestamps' => '0',
]);
$tester->expectLogStartNotices();
for ($i = 0; $i < 6; $i++) {
    echo $tester->request()->getBody(), "\n";
}
$tester->terminate();
$tester->close();

?>
--EXPECT--
0 p10q,q,unset,E,u0!,tatb,p0,CE,onetwoone,5T,R,magic:hidden,E/b0
1 p11q,q,unset,E,u1!,tatb,p1,CE,onetwoone,5T,R,magic:hidden,E/b0
2 p12q,q,unset,E,u2!,tatb,p2,CE,onetwoone,5T,R,magic:hidden,a2/b2
3 p13q,q,unset,E,u3!,tatb,p3,CE,onetwoone,5T,R,magic:hidden,a2/b2
4 p14q,q,unset,E,u4!,tatb,p4,CE,onetwoone,5T,R,magic:hidden,E/b4
5 p15q,q,unset,E,u5!,tatb,p5,CE,onetwoone,5T,R,magic:hidden,E/b4
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
@unlink(sys_get_temp_dir() . '/fpm-property-sites-lib.php');
@unlink(sys_get_temp_dir() . '/fpm-property-sites-state.txt');
?>
