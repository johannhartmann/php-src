<?php
class SBase {
    public $v = 1;
    static function who($x = 0) { return static::class . $x; }
    static function make() { return new static; }
    function inst($x) { return get_class($this) . ":" . ($this->v + $x); }
    static function viaSelf($x) { return self::who($x); }
    static function viaNamed($x) { return SBase::who($x); }
    function instViaSelf($x) { return self::who($x) . '|' . self::inst($x); }
    function typed(int $a): int { return $a + $this->v; }
}
class SChild extends SBase {
    public $v = 2;
    static function viaParent($x) { return parent::who($x); }
    function instViaParent($x) { return parent::inst($x) . '|' . parent::who($x); }
    function inst($x) { return "child" . $x; }
    function callTyped($x) { return parent::typed($x); }
}
