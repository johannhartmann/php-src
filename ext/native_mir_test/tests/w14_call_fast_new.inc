<?php
class NPlain { public $a; public $b; function __construct($a, $b = 'dflt') { $this->a = $a; $this->b = $b; } }
class NChild extends NPlain {}
class NNoCtor { public $x = 1; }
class NThrow { public $n; function __construct($n) { $this->n = $n; if ($n % 2) throw new RuntimeException("ctor$n"); } function __destruct() { echo "~NThrow{$this->n} "; } }
abstract class NAbstract { function __construct() {} }
class NDtor { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~NDtor{$this->n} "; } }
function nthrow_arg($r) { throw new LogicException("arg$r"); }
