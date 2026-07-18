/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <string.h>

#include "zend_mir_lowering_internal.h"

/*
 * Compile-time projection of docs/native-engine/lowering/w03-opcode-profile.json.
 * Reserved opcode numbers 45 and 79 intentionally have no entry.
 */
static const zend_mir_lowering_profile_entry zend_mir_w03_profile_entries[] = {
	{UINT32_C(0), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(1), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(2), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(3), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(4), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(5), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(6), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(7), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(8), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(9), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(10), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(11), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(12), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(13), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(14), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(15), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(16), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(17), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(18), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(19), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(20), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(21), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(22), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(23), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(24), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(25), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(26), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(27), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(28), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(29), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(30), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(31), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(32), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(33), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(34), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(35), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(36), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(37), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(38), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(39), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(40), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(41), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(42), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(43), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(44), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(46), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(47), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(48), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(49), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(50), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(51), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(52), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(53), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(54), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(55), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(56), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(57), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(58), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(59), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(60), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(61), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(62), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(63), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(64), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(65), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(66), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(67), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(68), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(69), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(70), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(71), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(72), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(73), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(74), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(75), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(76), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(77), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(78), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(80), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(81), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(82), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(83), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(84), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(85), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(86), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(87), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(88), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(89), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(90), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(91), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(92), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(93), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(94), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(95), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(96), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(97), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(98), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(99), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(100), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(101), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(102), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(103), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(104), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(105), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(106), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(107), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(108), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(109), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(110), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(111), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(112), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(113), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(114), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(115), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(116), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(117), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(118), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(119), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(120), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(121), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(122), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(123), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(124), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(125), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(126), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(127), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(128), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(129), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(130), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(131), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(132), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(133), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(134), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(135), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(136), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(137), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(138), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(139), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(140), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(141), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(142), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(143), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(144), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(145), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(146), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(147), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(148), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(149), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(150), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(151), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(152), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(153), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(154), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(155), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(156), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(157), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(158), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(159), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(160), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(161), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(162), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(163), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(164), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(165), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(166), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(167), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(168), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(169), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(170), ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{UINT32_C(171), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(172), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(173), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(174), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(175), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(176), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(177), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(178), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(179), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(180), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(181), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(182), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(183), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(184), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(185), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(186), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(187), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(188), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(189), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(190), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(191), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(192), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(193), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(194), ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{UINT32_C(195), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(196), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(197), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(198), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(199), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(200), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(201), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(202), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(203), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(204), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(205), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(206), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(207), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(208), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{UINT32_C(209), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(210), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(211), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{UINT32_C(212), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{UINT32_C(213), ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05}
};

static const zend_mir_lowering_profile zend_mir_w03_profile = {
	zend_mir_w03_profile_entries,
	(uint32_t) (sizeof(zend_mir_w03_profile_entries)
		/ sizeof(zend_mir_w03_profile_entries[0]))
};

const zend_mir_lowering_profile *zend_mir_lowering_w03_profile(void)
{
	return &zend_mir_w03_profile;
}

static void zend_mir_lowering_set_code(
	zend_mir_lowering_diagnostic_code *diagnostic_out,
	zend_mir_lowering_diagnostic_code code)
{
	if (diagnostic_out != NULL) {
		*diagnostic_out = code;
	}
}

static bool zend_mir_lowering_disposition_is_valid(
	zend_mir_lowering_profile_disposition disposition)
{
	return disposition >= ZEND_MIR_LOWERING_PROFILE_ACCEPTED
		&& disposition <= ZEND_MIR_LOWERING_PROFILE_REJECTED;
}

const zend_mir_lowering_profile_entry *zend_mir_lowering_profile_find(
	const zend_mir_lowering_profile *profile, uint32_t zend_opcode_number)
{
	uint32_t low = 0;
	uint32_t high;

	if (profile == NULL || profile->entries == NULL) {
		return NULL;
	}
	high = profile->entry_count;
	while (low < high) {
		uint32_t middle = low + (high - low) / 2;
		const zend_mir_lowering_profile_entry *entry = &profile->entries[middle];

		if (entry->zend_opcode_number < zend_opcode_number) {
			low = middle + 1;
		} else if (entry->zend_opcode_number > zend_opcode_number) {
			high = middle;
		} else {
			return entry;
		}
	}
	return NULL;
}

bool zend_mir_lowering_registry_init(zend_mir_lowering_registry *registry,
	const zend_mir_lowering_profile *profile,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	uint32_t index;
	uint32_t accepted_count = 0;

	zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_INVALID_SOURCE);
	if (registry == NULL || profile == NULL || profile->entries == NULL
			|| profile->entry_count == 0) {
		return false;
	}
	for (index = 0; index < profile->entry_count; index++) {
		const zend_mir_lowering_profile_entry *entry = &profile->entries[index];

		if (!zend_mir_lowering_disposition_is_valid(entry->disposition)
				|| (index != 0
					&& profile->entries[index - 1].zend_opcode_number
						>= entry->zend_opcode_number)) {
			return false;
		}
		if (entry->disposition == ZEND_MIR_LOWERING_PROFILE_ACCEPTED) {
			accepted_count++;
		}
	}
	if (accepted_count == 0 || accepted_count > ZEND_MIR_LOWERING_MAX_CLAIMS) {
		return false;
	}

	memset(registry, 0, sizeof(*registry));
	registry->profile = profile;
	zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_OK);
	return true;
}

bool zend_mir_lowering_registry_construct(zend_mir_lowering_registry *registry,
	const zend_mir_lowering_profile *profile,
	const zend_mir_lowering_provider_array *provider_arrays,
	uint32_t provider_array_count,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_lowering_registry candidate;
	uint32_t array_index;

	zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_UNKNOWN_PROVIDER);
	if (registry == NULL || provider_arrays == NULL || provider_array_count == 0
			|| provider_array_count > ZEND_MIR_LOWERING_MAX_PROVIDERS
			|| !zend_mir_lowering_registry_init(
				&candidate, profile, diagnostic_out)) {
		return false;
	}
	for (array_index = 0; array_index < provider_array_count; array_index++) {
		const zend_mir_lowering_provider_array *array =
			&provider_arrays[array_index];
		uint32_t provider_index;

		if (array->providers == NULL || array->provider_count == 0
				|| array->provider_count > ZEND_MIR_LOWERING_MAX_PROVIDERS) {
			zend_mir_lowering_set_code(
				diagnostic_out, ZEND_MIRL_UNKNOWN_PROVIDER);
			return false;
		}
		for (provider_index = 0; provider_index < array->provider_count;
				provider_index++) {
			if (!zend_mir_lowering_registry_register(
					&candidate, &array->providers[provider_index],
					diagnostic_out)) {
				return false;
			}
		}
	}
	if (!zend_mir_lowering_registry_validate(&candidate, diagnostic_out)) {
		return false;
	}
	*registry = candidate;
	return true;
}

static bool zend_mir_lowering_provider_precedes(
	const zend_mir_lowering_provider *left,
	const zend_mir_lowering_provider *right)
{
	if (left->semantic_family_id != right->semantic_family_id) {
		return left->semantic_family_id < right->semantic_family_id;
	}
	return left->provider_id < right->provider_id;
}

static void zend_mir_lowering_sort_dispatch(zend_mir_lowering_registry *registry)
{
	uint32_t index;

	for (index = 1; index < registry->dispatch_count; index++) {
		zend_mir_lowering_dispatch_entry value = registry->dispatch[index];
		uint32_t position = index;

		while (position != 0
				&& registry->dispatch[position - 1].claim.zend_opcode_number
					> value.claim.zend_opcode_number) {
			registry->dispatch[position] = registry->dispatch[position - 1];
			position--;
		}
		registry->dispatch[position] = value;
	}
}

bool zend_mir_lowering_registry_register(zend_mir_lowering_registry *registry,
	const zend_mir_lowering_provider *provider,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_lowering_claim claims[ZEND_MIR_LOWERING_MAX_CLAIMS];
	uint32_t claim_count;
	uint32_t provider_position;
	uint32_t index;

	zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_UNKNOWN_PROVIDER);
	if (registry == NULL || registry->profile == NULL || provider == NULL
			|| provider->claim_count == NULL || provider->claim_at == NULL
			|| provider->lower == NULL || registry->complete
			|| registry->provider_count >= ZEND_MIR_LOWERING_MAX_PROVIDERS) {
		return false;
	}
	for (index = 0; index < registry->provider_count; index++) {
		if (registry->providers[index].provider_id == provider->provider_id) {
			zend_mir_lowering_set_code(
				diagnostic_out, ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM);
			return false;
		}
	}

	claim_count = provider->claim_count(provider->context);
	if (claim_count == 0 || claim_count > ZEND_MIR_LOWERING_MAX_CLAIMS
			|| claim_count > ZEND_MIR_LOWERING_MAX_CLAIMS - registry->dispatch_count) {
		return false;
	}
	for (index = 0; index < claim_count; index++) {
		const zend_mir_lowering_profile_entry *profile_entry;
		uint32_t previous;

		if (!provider->claim_at(provider->context, index, &claims[index])
				|| claims[index].semantic_family_id != provider->semantic_family_id) {
			return false;
		}
		profile_entry = zend_mir_lowering_profile_find(
			registry->profile, claims[index].zend_opcode_number);
		if (profile_entry == NULL
				|| profile_entry->disposition != ZEND_MIR_LOWERING_PROFILE_ACCEPTED) {
			return false;
		}
		for (previous = 0; previous < index; previous++) {
			if (claims[previous].zend_opcode_number
					== claims[index].zend_opcode_number) {
				zend_mir_lowering_set_code(
					diagnostic_out, ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM);
				return false;
			}
		}
		for (previous = 0; previous < registry->dispatch_count; previous++) {
			if (registry->dispatch[previous].claim.zend_opcode_number
					== claims[index].zend_opcode_number) {
				zend_mir_lowering_set_code(
					diagnostic_out, ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM);
				return false;
			}
		}
	}

	provider_position = registry->provider_count;
	while (provider_position != 0
			&& zend_mir_lowering_provider_precedes(
				provider, &registry->providers[provider_position - 1])) {
		registry->providers[provider_position]
			= registry->providers[provider_position - 1];
		provider_position--;
	}
	for (index = 0; index < registry->dispatch_count; index++) {
		if (registry->dispatch[index].provider_index >= provider_position) {
			registry->dispatch[index].provider_index++;
		}
	}
	registry->providers[provider_position] = *provider;
	registry->provider_count++;

	for (index = 0; index < claim_count; index++) {
		zend_mir_lowering_dispatch_entry *entry =
			&registry->dispatch[registry->dispatch_count++];

		entry->claim = claims[index];
		entry->provider_index = provider_position;
	}
	zend_mir_lowering_sort_dispatch(registry);
	zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_OK);
	return true;
}

bool zend_mir_lowering_registry_validate(zend_mir_lowering_registry *registry,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	uint32_t profile_index;
	uint32_t dispatch_index = 0;

	zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_UNKNOWN_PROVIDER);
	if (registry == NULL || registry->profile == NULL) {
		return false;
	}
	if (registry->complete) {
		zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_OK);
		return true;
	}
	for (profile_index = 0; profile_index < registry->profile->entry_count;
			profile_index++) {
		const zend_mir_lowering_profile_entry *entry =
			&registry->profile->entries[profile_index];

		if (entry->disposition != ZEND_MIR_LOWERING_PROFILE_ACCEPTED) {
			continue;
		}
		if (dispatch_index >= registry->dispatch_count
				|| registry->dispatch[dispatch_index].claim.zend_opcode_number
					!= entry->zend_opcode_number) {
			return false;
		}
		dispatch_index++;
	}
	if (dispatch_index != registry->dispatch_count) {
		return false;
	}
	registry->complete = true;
	zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_OK);
	return true;
}

const zend_mir_lowering_provider *zend_mir_lowering_registry_find(
	const zend_mir_lowering_registry *registry, uint32_t zend_opcode_number)
{
	uint32_t low = 0;
	uint32_t high;

	if (registry == NULL || !registry->complete) {
		return NULL;
	}
	high = registry->dispatch_count;
	while (low < high) {
		uint32_t middle = low + (high - low) / 2;
		const zend_mir_lowering_dispatch_entry *entry = &registry->dispatch[middle];

		if (entry->claim.zend_opcode_number < zend_opcode_number) {
			low = middle + 1;
		} else if (entry->claim.zend_opcode_number > zend_opcode_number) {
			high = middle;
		} else if (entry->provider_index < registry->provider_count) {
			return &registry->providers[entry->provider_index];
		} else {
			return NULL;
		}
	}
	return NULL;
}

uint32_t zend_mir_lowering_registry_provider_count(
	const zend_mir_lowering_registry *registry)
{
	return registry != NULL ? registry->provider_count : 0;
}

bool zend_mir_lowering_registry_provider_at(
	const zend_mir_lowering_registry *registry,
	uint32_t index, zend_mir_lowering_provider *out)
{
	if (registry == NULL || out == NULL || index >= registry->provider_count) {
		return false;
	}
	*out = registry->providers[index];
	return true;
}

static uint32_t zend_mir_lowering_nop_claim_count(const void *context)
{
	return context != NULL ? 1 : 0;
}

static bool zend_mir_lowering_nop_claim_at(
	const void *context, uint32_t index, zend_mir_lowering_claim *out)
{
	if (context == NULL || out == NULL || index != 0) {
		return false;
	}
	*out = *(const zend_mir_lowering_claim *) context;
	return true;
}

static zend_mir_lowering_status zend_mir_lowering_nop(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator)
{
	(void) context;
	(void) source_opcode;
	(void) mutator;
	return ZEND_MIR_LOWERING_SUCCESS;
}

bool zend_mir_lowering_register_nop(zend_mir_lowering_registry *registry,
	uint32_t semantic_family_id, zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_lowering_provider provider;
	zend_mir_lowering_claim previous_claim;
	bool registered;

	if (registry == NULL) {
		zend_mir_lowering_set_code(diagnostic_out, ZEND_MIRL_UNKNOWN_PROVIDER);
		return false;
	}
	previous_claim = registry->builtin_nop_claim;
	registry->builtin_nop_claim.zend_opcode_number = 0;
	registry->builtin_nop_claim.semantic_family_id = semantic_family_id;
	provider.provider_id = 0;
	provider.semantic_family_id = semantic_family_id;
	provider.context = &registry->builtin_nop_claim;
	provider.claim_count = zend_mir_lowering_nop_claim_count;
	provider.claim_at = zend_mir_lowering_nop_claim_at;
	provider.lower = zend_mir_lowering_nop;
	registered = zend_mir_lowering_registry_register(
		registry, &provider, diagnostic_out);
	if (!registered) {
		registry->builtin_nop_claim = previous_claim;
	}
	return registered;
}
