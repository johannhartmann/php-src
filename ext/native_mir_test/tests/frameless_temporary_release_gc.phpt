--TEST--
Native frameless calls release temporary arguments without buffering GC roots
--DESCRIPTION--
A frameless call frees its temporary arguments as the VM's FREE_OP does,
with zval_ptr_dtor_nogc(): an array a temporary shares with a property is
not buffered as a possible cycle root, also on the general path that warns
about an undefined CV argument.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Holder {
    public array $values;
    public string $text;

    public function __construct() {
        $this->values = range(1, 5);
        $this->text = str_repeat('ab', 8);
    }

    public function values(): array {
        return $this->values;
    }

    public function text(): string {
        return $this->text;
    }
}

function defined_arguments(Holder $holder): int {
    $hits = 0;
    for ($i = 0; $i < 20; $i++) {
        $hits += in_array(3, $holder->values()) ? 1 : 0;
        $hits += in_array('3', $holder->values(), true) ? 1 : 0;
        $hits += array_key_exists(2, $holder->values()) ? 1 : 0;
        $hits += str_contains($holder->text(), 'ba') ? 1 : 0;
        $hits += in_array(1, $holder->values) ? 1 : 0;
    }
    return $hits;
}

function undefined_two(Holder $holder): int {
    $hits = 0;
    for ($i = 0; $i < 2; $i++) {
        $hits += in_array($undef, $holder->values()) ? 1 : 0;
    }
    return $hits;
}

function undefined_three(Holder $holder): int {
    $hits = 0;
    for ($i = 0; $i < 2; $i++) {
        $hits += in_array($undef, $holder->values(), true) ? 1 : 0;
    }
    return $hits;
}

foreach (['defined_arguments', 'undefined_two', 'undefined_three'] as $test) {
    gc_collect_cycles();
    $holder = new Holder;
    $before = gc_status()['roots'];
    var_dump($test($holder));
    var_dump(gc_status()['roots'] - $before);
}
?>
--EXPECTF--
int(80)
int(0)

Warning: Undefined variable $undef in %s on line %d

Warning: Undefined variable $undef in %s on line %d
int(0)
int(0)

Warning: Undefined variable $undef in %s on line %d

Warning: Undefined variable $undef in %s on line %d
int(0)
int(0)
