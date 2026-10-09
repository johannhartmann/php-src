--TEST--
Native: A callable|array parameter takes closures through a call from a C boundary
--DESCRIPTION--
callable lies outside MAY_BE_ANY: callable|array is no array-only local
ABI, so its entry guards nothing and a closure argument must not make the
function return the private retry status to a universal call.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = __DIR__ . '/callable_union_argument';
@mkdir($dir);
file_put_contents("$dir/NativeDispatcher.php", <<<'PHP'
<?php
class NativeDispatcher
{
    private array $listeners = [];

    public function listenerCount(?string $name = null): int
    {
        if (null !== $name) {
            if (empty($this->listeners[$name])) {
                return 0;
            }
        }
        return count($this->listeners[$name] ?? []);
    }

    public function priority(string $name, callable|array $listener): ?int
    {
        if (empty($this->listeners[$name])) {
            return null;
        }
        foreach ($this->listeners[$name] as $priority => &$listeners) {
            foreach ($listeners as &$v) {
                if ($v !== $listener && \is_array($v) && isset($v[0]) && $v[0] instanceof \Closure && 2 >= \count($v)) {
                    $v[0] = $v[0]();
                }
            }
        }
        return null;
    }

    public function add(string $name, callable|array $listener, int $priority = 0)
    {
        $this->listeners[$name][$priority][] = $listener;
    }

    public function remove(string $name, callable|array $listener)
    {
        if (empty($this->listeners[$name])) {
            return;
        }
        if (\is_array($listener) && isset($listener[0]) && $listener[0] instanceof \Closure && 2 >= \count($listener)) {
            $listener[0] = $listener[0]();
        }
        foreach ($this->listeners[$name] as $priority => &$listeners) {
            foreach ($listeners as $k => &$v) {
                if ($v === $listener || ($listener instanceof \Closure && $v == $listener)) {
                    unset($listeners[$k]);
                }
            }
        }
    }
}
PHP);
spl_autoload_register(function ($class) use ($dir) { require "$dir/$class.php"; });
class Subscriber {
    public $dispatcher;
    public $registered = true;
    function __construct($d) { $this->dispatcher = $d; }
    public function onEvent($event) {
        $this->dispatcher?->remove('event', $this->onEvent(...));
        $this->registered = false;
        return 'removed';
    }
}
$d = new NativeDispatcher;
$s = new Subscriber($d);
$d->add('event', $s->onEvent(...));
$d->add('event', 'strlen', 5);
var_dump($s->onEvent(1), $s->registered, $d->listenerCount('event'));
--EXPECT--
string(7) "removed"
bool(false)
int(2)
--CLEAN--
<?php
@unlink(__DIR__ . '/callable_union_argument/NativeDispatcher.php');
@rmdir(__DIR__ . '/callable_union_argument');
