--TEST--
Native Engine runs functions and suspended generators under phpdbg
--INI--
opcache.enable=1
--PHPDBG--
r
q
--EXPECTF--
[Successful compilation of %s]
prompt> value:2
value:5
[Script ended normally]
prompt>
--FILE--
<?php
function native_engine_phpdbg_leaf(int $value): int { return $value + 1; }
function native_engine_phpdbg_generator(): Generator {
    yield native_engine_phpdbg_leaf(1);
    yield native_engine_phpdbg_leaf(4);
}
foreach (native_engine_phpdbg_generator() as $value) {
    echo "value:", $value, "\n";
}
