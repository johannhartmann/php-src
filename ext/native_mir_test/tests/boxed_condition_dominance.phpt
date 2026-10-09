--TEST--
Native: A register boolean condition keeps its defining value only where it dominates
--DESCRIPTION--
A CV tested in a finally block after an earlier try/finally resolves
through a source binding without its own definition. The register value
the global override chain names is defined on another path and must not
feed the condition; the slot does.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Loader
{
    public $container;
    public $debug = false;
    private $cachePath = '/nonexistent/native-dominance/cache.php';

    public function load()
    {
        $cachePath = $this->cachePath;
        $lock = null;
        $oldContainer = null;
        $backtrace = [];
        try {
            if (is_file($cachePath) && \is_object($this->container = include $cachePath)) {
                return 'cached';
            }
            if ($lock = @fopen($cachePath . '.lock', 'w')) {
                if (!flock($lock, \LOCK_EX | \LOCK_NB, $wouldBlock) && !flock($lock, $wouldBlock ? \LOCK_SH : \LOCK_EX)) {
                    $lock = null;
                } elseif (!$oldContainer || \get_class($this->container) !== $oldContainer->name) {
                    $lock = null;
                }
            }
        } finally {
        }
        if ($collect = $this->debug && !\defined('NATIVE_DOMINANCE_UNDEFINED')) {
            $previous = set_error_handler(function ($type, $message, $file, $line) use (&$logs, &$previous) {
                if (\E_USER_DEPRECATED !== $type && \E_DEPRECATED !== $type) {
                    return false;
                }
                for ($i = 0; isset($backtrace[$i]); ++$i) {
                    if (isset($backtrace[$i]['file'], $backtrace[$i]['line']) && $backtrace[$i]['line'] === $line && $backtrace[$i]['file'] === $file) {
                        break;
                    }
                }
                $logs[$message] = [$type];
                return null;
            });
        }
        $container = null;
        try {
            $container = new ArrayObject([1, 2]);
        } finally {
            if ($collect) {
                restore_error_handler();
                $summary = null !== $container ? implode(',', $container->getArrayCopy()) : '';
                echo "collected $summary\n";
            }
        }
        if ($oldContainer && \get_class($this->container) !== $oldContainer->name) {
            return 'changed';
        }
        if ($lock && method_exists(Loader::class, 'load') && file_exists($cachePath . '.preload')) {
            return 'preload';
        }
        return count($container);
    }
}
$loader = new Loader;
var_dump($loader->load());
$loader->debug = true;
var_dump($loader->load());
--EXPECT--
int(2)
collected 1,2
int(2)
