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

  AC_DEFINE([HAVE_NATIVE_MIR_TEST], [1],
    [Define to 1 if the test-only native MIR bridge is enabled.])

  PHP_NEW_EXTENSION([native_mir_test],
    [native_mir_test.c],
    [no],,
    [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1])

  PHP_ADD_BUILD_DIR([
    Zend/Native/MIR/CFG
    Zend/Native/MIR/Core
    Zend/Native/MIR/Frame
    Zend/Native/MIR/Scalar
    Zend/Native/MIR/Semantics
    Zend/Native/MIR/Text
    Zend/Native/MIR/Verify
    Zend/Native/Lowering/Core
    Zend/Native/Lowering/Frontend
    Zend/Native/Lowering/Scalar/Logic
    Zend/Native/Lowering/Scalar/Numeric
    Zend/Native/Lowering/StraightLine
  ])

  PHP_ADD_SOURCES([Zend/Native/MIR/CFG],
    [zend_mir_cfg.c
     zend_mir_dominance.c
     zend_mir_phi.c])
  PHP_ADD_SOURCES([Zend/Native/MIR/Core],
    [zend_mir_arena.c
     zend_mir_ids.c
     zend_mir_module.c
     zend_mir_view.c])
  PHP_ADD_SOURCES([Zend/Native/MIR/Frame],
    [zend_mir_frame_intern.c
     zend_mir_frame_state.c
     zend_mir_source_map.c])
  PHP_ADD_SOURCES([Zend/Native/MIR/Scalar],
    [zend_mir_scalar_descriptors.c
     zend_mir_value_facts.c
     zend_mir_verify_scalar.c])
  PHP_ADD_SOURCES([Zend/Native/MIR/Semantics],
    [zend_mir_alias.c
     zend_mir_effect_summary.c
     zend_mir_ownership.c
     zend_mir_semantic_catalog.c])
  PHP_ADD_SOURCES([Zend/Native/MIR/Text],
    [zend_mir_dump.c])
  PHP_ADD_SOURCES([Zend/Native/MIR/Verify],
    [zend_mir_verify.c
     zend_mir_verify_cfg.c
     zend_mir_verify_dominance.c
     zend_mir_verify_frames.c
     zend_mir_verify_ids.c
     zend_mir_verify_semantics.c])

  PHP_ADD_SOURCES([Zend/Native/Lowering/Core],
    [zend_mir_lowering.c
     zend_mir_lowering_context.c
     zend_mir_lowering_diagnostics.c
     zend_mir_lowering_providers.c
     zend_mir_lowering_registry.c])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Frontend],
    [zend_mir_literal_pool.c
     zend_mir_operand_map.c
     zend_mir_slot_map.c
     zend_mir_source_positions.c
     zend_mir_value_facts.c
     zend_mir_zend_source.c])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Logic],
    [zend_mir_logic_proofs.c
     zend_mir_logic_provider.c
     zend_mir_lower_boolean.c
     zend_mir_lower_cast.c
     zend_mir_lower_compare.c])
  PHP_ADD_SOURCES([Zend/Native/Lowering/Scalar/Numeric],
    [zend_mir_lower_numeric.c
     zend_mir_numeric_proofs.c
     zend_mir_numeric_provider.c])
  PHP_ADD_SOURCES([Zend/Native/Lowering/StraightLine],
    [zend_mir_lifetime_provider.c
     zend_mir_lower_copy_move.c
     zend_mir_lower_entry_state.c
     zend_mir_lower_return.c
     zend_mir_lower_structural.c])
])
