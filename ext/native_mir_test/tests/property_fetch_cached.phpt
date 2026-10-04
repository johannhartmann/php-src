--TEST--
Native cached property fetches for RW, UNSET and by-value FUNC_ARG
--DESCRIPTION--
Read-write, unset and write fetches address an untyped declared property
through the run-time cache, and a property passed to a by-value parameter
reads like FETCH_OBJ_R. Typed and readonly properties, by-reference
parameters and dynamic properties keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function byval($x) { return $x; }
function byref(&$x) { $x = 'set'; }
class P { public $a = ['x' => 1, 'y' => 2]; public $n = 1; public int $t = 1; public readonly array $ro; public $u;
  function __construct() { $this->ro = [1]; }
  function run() {
    unset($this->a['x']); $this->a['z']['w'] = 3; $this->n++; $this->a['y'] .= 'b';
    $r = [byval($this->n), byval($this->a), call_user_func('byval', $this->n)];
    byref($this->u); byref($this->a['q']);
    try { unset($this->ro[0]); } catch (Error $e) { $r[] = get_class($e); }
    $this->t++; unset($this->missing['k']);
    return [$r, $this->a, $this->u, $this->t];
  }
}
for ($i = 0; $i < 2; $i++) echo json_encode((new P)->run()), "\n";
?>
--EXPECTF--

Deprecated: Creation of dynamic property P::$missing is deprecated in %s on line %d
[[2,{"y":"2b","z":{"w":3}},2,"Error"],{"y":"2b","z":{"w":3},"q":"set"},"set",2]

Deprecated: Creation of dynamic property P::$missing is deprecated in %s on line %d
[[2,{"y":"2b","z":{"w":3}},2,"Error"],{"y":"2b","z":{"w":3},"q":"set"},"set",2]
