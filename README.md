<div align="center">
    <a href="https://www.php.net">
        <img
            alt="PHP"
            src="https://www.php.net/images/logos/new-php-logo.svg"
            width="150">
    </a>
</div>

# PHP on TPDE

This branch is a port of php-src to a native execution engine built on
[TPDE](https://github.com/tpde2/tpde), a fast compiler back-end framework. The
Zend VM does not execute PHP code here: every function is translated into
native machine code through a PHP-specific intermediate representation (ZNMIR)
and TPDE. There is no fallback to the VM.

It is an independent, experimental fork and not an official PHP release. The
goal is maximum warm-request performance for real PHP applications such as
WordPress, Symfony and Laravel, with PHP-observable behavior identical to stock
PHP.

- **Targets:** Linux/ELF x86-64 (primary) and Darwin arm64. Configure rejects
  every other host.
- **Execution model:** native registers and stack are canonical; Zend frames
  are reconstructed on demand wherever PHP state is observable (exceptions,
  warnings, observers, backtraces, generators, fibers). Compiled functions call
  each other through a native convention, and failed speculation deoptimizes
  into the generic native version of a function, never into the VM.
- **OPcache:** native code images are persisted in OPcache without process
  addresses and are relocated when another process maps them.
- **TPDE revision:** pinned in
  [`Zend/Native/TPDE/ThirdParty/tpde/REVISION`](Zend/Native/TPDE/ThirdParty/tpde/REVISION).

### Layout

- [`Zend/Native/`](Zend/Native): ZNMIR, lowering, the TPDE back-ends
  (`TPDE/LinuxX64`, `TPDE/DarwinA64`) and the native runtime.
- [`docs/native-engine/`](docs/native-engine/README.md): architecture decisions
  and semantic contracts, starting with
  [ADR 0025](docs/native-engine/adr/0025-native-canonical-execution.md).
- [`scripts/native/`](scripts/native/README.md): build profiles, build and test
  commands.

### Building and testing the native engine

The Linux profiles build with clang; GCC cannot compile the TPDE x64
templates.

```shell
scripts/native/configure-dev.sh
scripts/native/build.sh --profile linux-amd64-native-debug-nts --print-binary
scripts/native/test-phpt.sh --tier quick    # native PHPTs, seconds
scripts/native/test-phpt.sh --tier commit   # full debug suites
scripts/native/test-phpt.sh --tier full     # plus deopt stress, ASan and UBSan
```

The native CLI cannot yet run `run-tests.php` itself, so the test scripts
need a separate plain PHP CLI as the runner in `NATIVE_PHPT_RUNNER`; the
native binary is the binary under test. Behavior is checked differentially
against stock PHP; see
[`docs/native-engine/test-command-contract.md`](docs/native-engine/test-command-contract.md).

The rest of this file is the upstream php-src README.

# The PHP Interpreter

PHP is a popular general-purpose scripting language that is especially suited to
web development. Fast, flexible and pragmatic, PHP powers everything from your
blog to the most popular websites in the world.

PHP is distributed under the [Modified BSD License](LICENSE)
(SPDX-License-Identifier: `BSD-3-Clause`).

[![Test](https://github.com/php/php-src/actions/workflows/test.yml/badge.svg)](https://github.com/php/php-src/actions/workflows/test.yml)
[![Fuzzing Status](https://oss-fuzz-build-logs.storage.googleapis.com/badges/php.svg)](https://issues.oss-fuzz.com/issues?q=project:php)

## Documentation

The PHP manual is available at [php.net/docs](https://www.php.net/docs).

## Installation

### Prebuilt packages and binaries

Prebuilt packages and binaries can be used to get up and running fast with PHP.

For Windows, the PHP binaries can be obtained from
[windows.php.net](https://windows.php.net). After extracting the archive the
`*.exe` files are ready to use.

For other systems, see the [installation chapter](https://www.php.net/install).

### Building PHP source code

*For Windows, see [Build your own PHP on Windows](https://wiki.php.net/internals/windows/stepbystepbuild_sdk_2).*

For a minimal PHP build from Git, you will need autoconf, bison, and re2c. For
a default build, you will additionally need libxml2 and libsqlite3.

On Ubuntu, you can install these using:

```shell
sudo apt install -y pkg-config build-essential autoconf bison re2c libxml2-dev libsqlite3-dev
```

On Fedora, you can install these using:

```shell
sudo dnf install re2c bison autoconf make ccache libxml2-devel sqlite-devel
```

On MacOS, you can install these using `brew`:

```shell
brew install autoconf bison re2c libiconv libxml2 sqlite
```

or with `MacPorts`:

```shell
sudo port install autoconf bison re2c libiconv libxml2 sqlite3
```

Generate configure:

```shell
./buildconf
```

Configure your build. `--enable-debug` is recommended for development, see
`./configure --help` for a full list of options.

```shell
# For development
./configure --enable-debug
# For production
./configure
```

Build PHP. To speed up the build, specify the maximum number of jobs using the
`-j` argument:

```shell
make -j4
```

The number of jobs should usually match the number of available cores, which
can be determined using `nproc`.

## Testing PHP source code

PHP ships with an extensive test suite, the command `make test` is used after
successful compilation of the sources to run this test suite.

Tests run in parallel by default, using up to 10 detected logical processors.
Set `-jN` in `TEST_PHP_ARGS` or `TESTS` to override the worker count:

```shell
make TEST_PHP_ARGS=-j4 test
```

This runs `make test` with a maximum of 4 concurrent jobs. Alternatively,
use `-j1` to run tests sequentially.

Use the `TEST_PHP_ARGS` or `TESTS` variable to test only specific directories:

```shell
make TESTS=tests/lang/ test
```

The [qa.php.net](https://qa.php.net) site provides more detailed info about
testing and quality assurance.

## Installing PHP built from source

After a successful build (and test), PHP may be installed with:

```shell
make install
```

Depending on your permissions and prefix, `make install` may need superuser
permissions.

## PHP extensions

Extensions provide additional functionality on top of PHP. PHP consists of many
essential bundled extensions. Additional extensions can be found on the
[PIE Extensions](https://packagist.org/extensions) list, and installed with
[🥧 PIE, the PHP Installer for Extensions](https://github.com/php/pie) or the
deprecated PHP Extension Community Library - [PECL](https://pecl.php.net).

## Contributing

The PHP source code is located in the Git repository at
[github.com/php/php-src](https://github.com/php/php-src). Contributions are most
welcome by forking the repository and sending a pull request.

Discussions are done on GitHub, but depending on the topic can also be relayed
to the official PHP developer mailing list internals@lists.php.net.

New features require an RFC and must be accepted by the developers. See
[Request for comments - RFC](https://wiki.php.net/rfc) and
[Voting on PHP features](https://wiki.php.net/rfc/voting) for more information
on the process.

Bug fixes don't require an RFC. If the bug has a GitHub issue, reference it in
the commit message using `GH-NNNNNN`. Use `#NNNNNN` for tickets in the old
[bugs.php.net](https://bugs.php.net) bug tracker.

    Fix GH-7815: php_uname doesn't recognise latest Windows versions
    Fix #55371: get_magic_quotes_gpc() throws deprecation warning

See [Git workflow](https://wiki.php.net/vcs/gitworkflow) for details on how pull
requests are merged.

### Guidelines for contributors

See further documents in the repository for more information on how to
contribute:

- [Contributing to PHP](/CONTRIBUTING.md)
- [PHP coding standards](/CODING_STANDARDS.md)
- [Internal documentation](https://php.github.io/php-src/)
- [Mailing list rules](/docs/mailinglist-rules.md)
- [PHP release process](/docs/release-process.md)

## Credits

For the list of people who've put work into PHP, please see the
[PHP credits page](https://www.php.net/credits.php).
