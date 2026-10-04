PHP_ARG_ENABLE([native-engine],
  [whether to enable the native userland execution engine],
  [AS_HELP_STRING([--enable-native-engine],
    [Enable the native userland execution engine])],
  [no],
  [no])

PHP_ARG_ENABLE([native-mir-test],
  [whether to enable the native MIR compile/dump test bridge],
  [AS_HELP_STRING([--enable-native-mir-test],
    [Enable the test-only native MIR compile/dump bridge])],
  [no],
  [no])

AS_VAR_IF([PHP_NATIVE_MIR_TEST], [no], [], [
  AS_CASE([$PHP_NATIVE_MIR_TEST],
    [yes], [],
    [AC_MSG_ERROR([native_mir_test is test-only and cannot be built shared])])

  PHP_NATIVE_ENGINE=yes
  AC_DEFINE([HAVE_NATIVE_MIR_TEST], [1],
    [Define to 1 if the test-only native MIR bridge is enabled.])
])

AS_VAR_IF([PHP_NATIVE_ENGINE], [no], [], [
  AS_CASE([$PHP_NATIVE_ENGINE],
    [yes], [],
    [AC_MSG_ERROR([native engine is a core component and cannot be built shared])])

  AC_DEFINE([HAVE_NATIVE_ENGINE], [1],
    [Define to 1 if the native userland execution engine is enabled.])

  PHP_REQUIRE_CXX()
  PHP_CXX_COMPILE_STDCXX([20], [mandatory], [PHP_NATIVE_MIR_TEST_STDCXX])
  PHP_NATIVE_MIR_TEST_CXXFLAGS="$PHP_NATIVE_MIR_TEST_STDCXX -fno-exceptions -fno-rtti"
  dnl PHP enables C++ after it has set CXXFLAGS, so AC_PROG_CXX adds no
  dnl default optimization. The native compiler is C++; optimize it in every
  dnl non-debug build that does not choose its own level.
  AS_VAR_IF([ZEND_DEBUG], [no], [
    AS_CASE([" $CXXFLAGS "],
      [*" -O"*], [],
      [PHP_NATIVE_MIR_TEST_CXXFLAGS="$PHP_NATIVE_MIR_TEST_CXXFLAGS -O2"])
  ])
  dnl Zend API changes that C would only warn about, such as a zval * passed
  dnl where a zend_object * is now expected, must fail the native build.
  PHP_NATIVE_MIR_TEST_CFLAGS="-Werror=incompatible-pointer-types -Werror=int-conversion"

  dnl Each host builds only its own target backend (ADR 0006).
  AS_CASE([$host_cpu-$host_os],
    [x86_64-linux*], [PHP_NATIVE_TARGET=linux-x64],
    [aarch64-darwin*|arm64-darwin*], [PHP_NATIVE_TARGET=darwin-a64],
    [AC_MSG_ERROR([the native engine supports Linux x86-64 and Darwin arm64 hosts])])

  PHP_ADD_INCLUDE([$abs_srcdir/Zend/Native/TPDE/ThirdParty/tpde/include])
  AS_VAR_IF([PHP_NATIVE_TARGET], [linux-x64], [
    AC_PATH_PROG([NATIVE_MIR_TEST_PYTHON], [python3])
    AS_IF([test -z "$NATIVE_MIR_TEST_PYTHON"], [
      AC_MSG_ERROR([python3 is required to generate the vendored TPDE/Fadec encoder])
    ])
    NATIVE_MIR_TEST_FADEC_BUILD_DIR="$abs_builddir/Zend/Native/TPDE/ThirdParty/tpde/fadec/generated"
    AS_MKDIR_P([$NATIVE_MIR_TEST_FADEC_BUILD_DIR])
    AC_MSG_NOTICE([generating the pinned TPDE/Fadec x86-64 encoder])
    AS_IF([! "$NATIVE_MIR_TEST_PYTHON" \
        "$abs_srcdir/Zend/Native/TPDE/ThirdParty/tpde/fadec/parseinstrs.py" \
        encode2 \
        "$abs_srcdir/Zend/Native/TPDE/ThirdParty/tpde/fadec/instrs.txt" \
        "$NATIVE_MIR_TEST_FADEC_BUILD_DIR/fadec-encode2-public.inc" \
        "$NATIVE_MIR_TEST_FADEC_BUILD_DIR/fadec-encode2-private.inc" \
        --64], [
      AC_MSG_ERROR([failed to generate the pinned TPDE/Fadec x86-64 encoder])
    ])
    PHP_ADD_INCLUDE([$abs_srcdir/Zend/Native/TPDE/ThirdParty/tpde/fadec])
    PHP_ADD_INCLUDE([$NATIVE_MIR_TEST_FADEC_BUILD_DIR])
  ], [
    PHP_ADD_INCLUDE([$abs_srcdir/Zend/Native/TPDE/ThirdParty/tpde/disarm])
  ])

  AS_VAR_IF([PHP_NATIVE_MIR_TEST], [no], [], [
    PHP_NEW_EXTENSION([native_mir_test],
      [native_mir_test.c],
      [no],,
      [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1],
      [cxx])
  ])

  PHP_ADD_BUILD_DIR([Zend/Native/TPDE/Common])
  PHP_ADD_SOURCES_X([Zend/Native/TPDE/Common], [zend_tpde_backend.cpp],
    [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/TPDE/ThirdParty/tpde/src])
  PHP_ADD_SOURCES_X([Zend/Native/TPDE/ThirdParty/tpde/src],
    [Assembler.cpp AssemblerElf.cpp FunctionWriter.cpp StringTable.cpp ValueAssignment.cpp base.cpp],
    [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/TPDE/ThirdParty/tpde/src/util])
  PHP_ADD_SOURCES_X([Zend/Native/TPDE/ThirdParty/tpde/src/util], [SmallVector.cpp],
    [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
  AS_VAR_IF([PHP_NATIVE_TARGET], [linux-x64], [
    PHP_ADD_BUILD_DIR([Zend/Native/TPDE/LinuxX64])
    PHP_ADD_SOURCES_X([Zend/Native/TPDE/LinuxX64], [zend_tpde_linux_x64.cpp],
      [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
    PHP_ADD_BUILD_DIR([Zend/Native/TPDE/ThirdParty/tpde/fadec])
    PHP_ADD_SOURCES([Zend/Native/TPDE/ThirdParty/tpde/fadec], [encode2.c],
      [-Wno-overlength-strings])
    PHP_ADD_BUILD_DIR([Zend/Native/TPDE/ThirdParty/tpde/src/x64])
    PHP_ADD_SOURCES_X([Zend/Native/TPDE/ThirdParty/tpde/src/x64], [FunctionWriterX64.cpp],
      [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
    PHP_ADD_BUILD_DIR([Zend/Native/Runtime/LinuxX64])
    PHP_ADD_SOURCES_X([Zend/Native/Runtime/LinuxX64], [zend_native_publish_linux_x64.cpp],
      [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
  ], [
    PHP_ADD_BUILD_DIR([Zend/Native/TPDE/DarwinA64])
    PHP_ADD_SOURCES_X([Zend/Native/TPDE/DarwinA64],
      [zend_tpde_darwin_arm64.cpp zend_tpde_darwin_assembler.cpp],
      [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
    PHP_ADD_BUILD_DIR([Zend/Native/TPDE/ThirdParty/tpde/disarm])
    PHP_ADD_SOURCES([Zend/Native/TPDE/ThirdParty/tpde/disarm], [encode.c])
    PHP_ADD_BUILD_DIR([Zend/Native/TPDE/ThirdParty/tpde/src/arm64])
    PHP_ADD_SOURCES_X([Zend/Native/TPDE/ThirdParty/tpde/src/arm64], [FunctionWriterA64.cpp],
      [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
    PHP_ADD_BUILD_DIR([Zend/Native/Runtime/DarwinA64])
    PHP_ADD_SOURCES_X([Zend/Native/Runtime/DarwinA64], [zend_native_publish_darwin_arm64.cpp],
      [$PHP_NATIVE_MIR_TEST_CXXFLAGS], [PHP_GLOBAL_OBJS])
  ])
  PHP_ADD_BUILD_DIR([Zend/Native/Runtime/Common])
  PHP_ADD_SOURCES([Zend/Native/Runtime/Common],
    [zend_native_bindings.c zend_native_calls.c zend_native_execute.c zend_native_generators.c zend_native_internal_call.c zend_native_objects.c zend_native_runtime.c zend_native_values.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Compiler])
  PHP_ADD_SOURCES([Zend/Native/Compiler], [zend_native_dynamic_code.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_SOURCES([Zend/Native/Compiler], [zend_native_compiler.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_SOURCES([Zend/Native/Compiler], [zend_native_executor.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/CFG])
  PHP_ADD_SOURCES([Zend/Native/MIR/CFG], [zend_mir_cfg.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/CFG])
  PHP_ADD_SOURCES([Zend/Native/MIR/CFG], [zend_mir_dominance.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/CFG])
  PHP_ADD_SOURCES([Zend/Native/MIR/CFG], [zend_mir_phi.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Core])
  PHP_ADD_SOURCES([Zend/Native/MIR/Core], [zend_mir_arena.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Core])
  PHP_ADD_SOURCES([Zend/Native/MIR/Core], [zend_mir_ids.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Core])
  PHP_ADD_SOURCES([Zend/Native/MIR/Core], [zend_mir_module.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Core])
  PHP_ADD_SOURCES([Zend/Native/MIR/Core], [zend_mir_view.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Frame])
  PHP_ADD_SOURCES([Zend/Native/MIR/Frame], [zend_mir_frame_intern.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Frame])
  PHP_ADD_SOURCES([Zend/Native/MIR/Frame], [zend_mir_frame_state.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Frame])
  PHP_ADD_SOURCES([Zend/Native/MIR/Frame], [zend_mir_source_map.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Scalar])
  PHP_ADD_SOURCES([Zend/Native/MIR/Scalar], [zend_mir_scalar_descriptors.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Scalar])
  PHP_ADD_SOURCES([Zend/Native/MIR/Scalar], [zend_mir_value_facts.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Scalar])
  PHP_ADD_SOURCES([Zend/Native/MIR/Scalar], [zend_mir_verify_scalar.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Semantics])
  PHP_ADD_SOURCES([Zend/Native/MIR/Semantics], [zend_mir_alias.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Semantics])
  PHP_ADD_SOURCES([Zend/Native/MIR/Semantics], [zend_mir_effect_summary.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Semantics])
  PHP_ADD_SOURCES([Zend/Native/MIR/Semantics], [zend_mir_ownership.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Semantics])
  PHP_ADD_SOURCES([Zend/Native/MIR/Semantics], [zend_mir_semantic_catalog.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Text])
  PHP_ADD_SOURCES([Zend/Native/MIR/Text], [zend_mir_dump.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Verify])
  PHP_ADD_SOURCES([Zend/Native/MIR/Verify], [zend_mir_verify.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Verify])
  PHP_ADD_SOURCES([Zend/Native/MIR/Verify], [zend_mir_verify_cfg.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Verify])
  PHP_ADD_SOURCES([Zend/Native/MIR/Verify], [zend_mir_verify_dominance.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Verify])
  PHP_ADD_SOURCES([Zend/Native/MIR/Verify], [zend_mir_verify_frames.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Verify])
  PHP_ADD_SOURCES([Zend/Native/MIR/Verify], [zend_mir_verify_ids.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Verify])
  PHP_ADD_SOURCES([Zend/Native/MIR/Verify], [zend_mir_verify_semantics.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Core])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Core], [zend_mir_lowering.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Core])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Core], [zend_mir_lowering_context.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Core])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Core], [zend_mir_lowering_diagnostics.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Core])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Core], [zend_mir_lowering_providers.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Core])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Core], [zend_mir_lowering_registry.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Frontend])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Frontend], [zend_mir_literal_pool.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Frontend])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Frontend], [zend_mir_operand_map.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Frontend])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Frontend], [zend_mir_slot_map.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Frontend])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Frontend], [zend_mir_source_positions.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Frontend])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Frontend], [zend_mir_value_facts.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Frontend])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Frontend], [zend_mir_zend_source.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Logic])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Logic], [zend_mir_logic_proofs.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Logic])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Logic], [zend_mir_logic_provider.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Logic])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Logic], [zend_mir_lower_boolean.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Logic])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Logic], [zend_mir_lower_cast.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Logic])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Logic], [zend_mir_lower_compare.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Numeric])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Numeric], [zend_mir_lower_numeric.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Numeric])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Numeric], [zend_mir_numeric_proofs.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/Scalar/Numeric])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Numeric], [zend_mir_numeric_provider.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/StraightLine])
  PHP_ADD_SOURCES([Zend/Native/Lowering/StraightLine], [zend_mir_lifetime_provider.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/StraightLine])
  PHP_ADD_SOURCES([Zend/Native/Lowering/StraightLine], [zend_mir_lower_copy_move.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/StraightLine])
  PHP_ADD_SOURCES([Zend/Native/Lowering/StraightLine], [zend_mir_lower_entry_state.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/StraightLine])
  PHP_ADD_SOURCES([Zend/Native/Lowering/StraightLine], [zend_mir_lower_return.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/StraightLine])
  PHP_ADD_SOURCES([Zend/Native/Lowering/StraightLine], [zend_mir_lower_structural.c],
    [$PHP_NATIVE_MIR_TEST_CFLAGS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/ControlFlow])
  PHP_ADD_SOURCES_X([Zend/Native/Lowering/ControlFlow], [zend_mir_control_flow_proofs.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/ControlFlow])
  PHP_ADD_SOURCES_X([Zend/Native/Lowering/ControlFlow], [zend_mir_control_flow_provider.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/Lowering/ControlFlow])
  PHP_ADD_SOURCES_X([Zend/Native/Lowering/ControlFlow], [zend_mir_lower_control_flow.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/ControlFlow])
  PHP_ADD_SOURCES_X([Zend/Native/MIR/ControlFlow], [zend_mir_control_flow_map.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/ControlFlow])
  PHP_ADD_SOURCES_X([Zend/Native/MIR/ControlFlow], [zend_mir_verify_control_flow.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/Calls/Model])
  PHP_ADD_SOURCES_X([Zend/Native/Calls/Model], [zend_mir_call_model.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/MIR/Values])
  PHP_ADD_SOURCES_X([Zend/Native/MIR/Values], [zend_mir_verify_values.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/Values/Core])
  PHP_ADD_SOURCES_X([Zend/Native/Values/Core], [zend_mir_value_core.c],, [PHP_GLOBAL_OBJS])
  PHP_ADD_BUILD_DIR([Zend/Native/Values/Lowering])
  PHP_ADD_SOURCES_X([Zend/Native/Values/Lowering], [zend_mir_value_lowering.c],, [PHP_GLOBAL_OBJS])
])
