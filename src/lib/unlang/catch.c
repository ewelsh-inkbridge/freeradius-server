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
 * @file unlang/catch.c
 * @brief Unlang "catch" keyword evaluation.
 *
 * @copyright 2023 Network RADIUS SAS (legal@networkradius.com)
 */
RCSID("$Id$")

#include <freeradius-devel/server/rcode.h>
#include "unlang_priv.h"
#include "catch_priv.h"

static unlang_action_t catch_skip_to_next(UNUSED unlang_result_t *p_result, UNUSED request_t *request, unlang_stack_frame_t *frame)
{
	unlang_t		*unlang;

	fr_assert(frame->instruction->type == UNLANG_TYPE_CATCH);

	for (unlang = frame->instruction->next;
	     unlang != NULL;
	     unlang = unlang->next) {
		if (unlang->type == UNLANG_TYPE_CATCH) continue;

		break;
	}

	return frame_set_next(frame, unlang);
}

static unlang_action_t unlang_catch(UNUSED unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
#ifndef NDEBUG
	unlang_catch_t const *c = unlang_generic_to_catch(frame->instruction);

	fr_assert(!c->catching[p_result->rcode]);
#endif

	/*
	 *	Skip over any "catch" statementa after this one.
	 */
	frame_repeat(frame, catch_skip_to_next);

	return unlang_interpret_push_children(NULL, request, RLM_MODULE_NOT_SET, UNLANG_NEXT_SIBLING);
}


/** Skip ahead to a particular "catch" instruction.
 *
 */
unlang_action_t unlang_interpret_skip_to_catch(UNUSED unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_t		*unlang;
	rlm_rcode_t		rcode = unlang_interpret_rcode(request);

	fr_assert(frame->instruction->type == UNLANG_TYPE_TRY);

	/*
	 *	'try' at the end of a block without 'catch' should have been caught by the compiler.
	 */
	fr_assert(frame->instruction->next);

	for (unlang = frame->instruction->next;
	     unlang != NULL;
	     unlang = unlang->next) {
		unlang_catch_t const *c;

		if (unlang->type != UNLANG_TYPE_CATCH) {
		not_caught:
			RDEBUG3("No catch section for %s",
				fr_table_str_by_value(mod_rcode_table, rcode, "<invalid>"));
			return frame_set_next(frame, unlang);
		}

		if (rcode >= RLM_MODULE_NUMCODES) continue;

		c = unlang_generic_to_catch(unlang);
		if (c->catching[rcode]) break;
	}
	if (!unlang) goto not_caught;

	return frame_set_next(frame, unlang);
}

void unlang_catch_init(void)
{
	unlang_register(UNLANG_TYPE_CATCH,
			   &(unlang_op_t){
				.name = "catch",
				.type = UNLANG_TYPE_CATCH,
				.flag = UNLANG_OP_FLAG_DEBUG_BRACES,

				.interpret = unlang_catch,

				.unlang_size = sizeof(unlang_catch_t),
				.unlang_name = "unlang_catch_t",
			   });
}
