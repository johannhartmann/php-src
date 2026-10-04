<?php
class LBase {
    public static function name() { return 'B:' . static::class; }
    public static function tag($i) { return static::name() . $i; }
    public static function each($n) { $r = ''; for ($i = 0; $i < $n; $i++) { $r .= static::tag($i); } return $r; }
    public function viaStatic() { return static::name(); }
}
class LChild extends LBase {
    public static function name() { return 'C:' . static::class; }
}
class LOther extends LBase {}

class PBase {
    private $n = 0;
    private function step(): void { $this->n++; }
    private function peek() { return static::class . ':' . $this->n; }
    public function run($k) { for ($i = 0; $i < $k; $i++) { $this->step(); } return $this->peek(); }
    public function bound() { return function () { return $this->peek(); }; }
}
class PChild extends PBase {}
class PShadow extends PBase {
    public $shadowed = 0;
    public function step(): void { $this->shadowed++; }
    public function peek() { return 'shadow'; }
}
