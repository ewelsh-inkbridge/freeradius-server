/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file unlang/try.c
 * @brief Unlang "try" keyword evaluation.
 *
 * @copyright 2023 Network RADIUS SAS (legal@networkradius.com)
 */
RCSID("$Id$")

#include "unlang_priv.h"
#include "try_priv.h"
#include "catch_priv.h"

static unlang_action_t unlang_try(UNUSED unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	/*
	 *	When this frame finishes, jump ahead to the appropriate "catch".
	 *
	 *	All of the magic is done in the compile phase.
	 */
	frame_repeat(frame, unlang_interpret_skip_to_catch);

	return unlang_interpret_push_children(NULL, request, RLM_MODULE_NOT_SET, UNLANG_NEXT_SIBLING);
}

void unlang_try_init(void)
{
	unlang_register(UNLANG_TYPE_TRY,
			   &(unlang_op_t){
				.name = "try",
				.type = UNLANG_TYPE_TRY,
				.flag = UNLANG_OP_FLAG_DEBUG_BRACES,

				.interpret = unlang_try,

				.unlang_size = sizeof(unlang_try_t),
				.unlang_name = "unlang_try_t",
			   });
}
