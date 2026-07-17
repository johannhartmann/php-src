/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the Modified BSD License that is      |
  | bundled with this package in the file LICENSE, and is available      |
  | through the World Wide Web at <https://www.php.net/license/>.        |
  |                                                                      |
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <type_traits>

#include "zend_mir_alias.h"
#include "zend_mir_effect_summary.h"
#include "zend_mir_ownership.h"

static_assert(std::is_standard_layout_v<zend_mir_effect_summary>);
static_assert(std::is_standard_layout_v<zend_mir_ownership_transition>);

int main()
{
	zend_mir_effect_summary summary{};
	zend_mir_effect_summary_empty(&summary);
	return summary.modeled ? 0 : 1;
}
