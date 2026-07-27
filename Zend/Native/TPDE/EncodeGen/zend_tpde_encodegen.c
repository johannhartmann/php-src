// SPDX-License-Identifier: PHP-3.01

/*
 * Target-neutral instruction-selection snippets for TPDE EncodeGen.
 *
 * Generate both checked-in headers with tpde_encodegen built from the TPDE
 * revision recorded in ThirdParty/tpde/REVISION:
 *
 * clang -c -emit-llvm -ffreestanding -fcf-protection=none -O3 \
 *   -fomit-frame-pointer -fno-math-errno \
 *   --target=x86_64-unknown-linux-gnu -march=x86-64 \
 *   -o zend_tpde_encodegen_x64.bc zend_tpde_encodegen.c
 * tpde_encodegen -o ../LinuxX64/zend_tpde_encodegen_x64.hpp \
 *   zend_tpde_encodegen_x64.bc
 *
 * clang -c -emit-llvm -ffreestanding -O3 -fomit-frame-pointer \
 *   -fno-math-errno --target=arm64-apple-darwin \
 *   -o zend_tpde_encodegen_a64.bc zend_tpde_encodegen.c
 * tpde_encodegen -o ../DarwinA64/zend_tpde_encodegen_a64.hpp \
 *   zend_tpde_encodegen_a64.bc
 */

typedef unsigned long long zend_native_u64;
typedef long long zend_native_i64;

zend_native_u64 zend_native_add_u64(
	zend_native_u64 left, zend_native_u64 right)
{
	return left + right;
}

zend_native_u64 zend_native_sub_u64(
	zend_native_u64 left, zend_native_u64 right)
{
	return left - right;
}

zend_native_u64 zend_native_mul_u64(
	zend_native_u64 left, zend_native_u64 right)
{
	return left * right;
}

zend_native_u64 zend_native_or_u64(
	zend_native_u64 left, zend_native_u64 right)
{
	return left | right;
}

zend_native_u64 zend_native_and_u64(
	zend_native_u64 left, zend_native_u64 right)
{
	return left & right;
}

zend_native_u64 zend_native_xor_u64(
	zend_native_u64 left, zend_native_u64 right)
{
	return left ^ right;
}

zend_native_u64 zend_native_eq_u64(
	zend_native_u64 left, zend_native_u64 right)
{
	return left == right;
}

zend_native_u64 zend_native_lt_i64(
	zend_native_i64 left, zend_native_i64 right)
{
	return left < right;
}

zend_native_u64 zend_native_le_i64(
	zend_native_i64 left, zend_native_i64 right)
{
	return left <= right;
}

double zend_native_add_f64(double left, double right)
{
	return left + right;
}

double zend_native_sub_f64(double left, double right)
{
	return left - right;
}

double zend_native_mul_f64(double left, double right)
{
	return left * right;
}

zend_native_u64 zend_native_load_u64(const zend_native_u64 *address)
{
	return *address;
}

unsigned int zend_native_load_u32(const unsigned int *address)
{
	return *address;
}

void zend_native_store_u64(
	zend_native_u64 *address, zend_native_u64 value)
{
	*address = value;
}
