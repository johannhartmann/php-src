#include "Zend/Native/MIR/zend_mir.h"
#include "Zend/Native/MIR/zend_mir_private.h"
#include "Zend/Native/MIR/Core/zend_mir_arena.h"
#include "Zend/Native/MIR/CFG/zend_mir_cfg.h"
#include "Zend/Native/MIR/CFG/zend_mir_dominance.h"
#include "Zend/Native/MIR/CFG/zend_mir_phi.h"
#include "Zend/Native/MIR/Semantics/zend_mir_alias.h"
#include "Zend/Native/MIR/Semantics/zend_mir_effect_summary.h"
#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"
#include "Zend/Native/MIR/Frame/zend_mir_frame_state.h"
#include "Zend/Native/MIR/Frame/zend_mir_source_map.h"
#include "Zend/Native/MIR/Text/zend_mir_dump.h"
#include "Zend/Native/MIR/Verify/zend_mir_verify.h"

int main(void)
{
	return zend_mir_contract_is_compatible(ZEND_MIR_CONTRACT_VERSION) ? 0 : 1;
}
