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
 * @brief map and unlang integration.
 * @brief Unlang "map" keyword evaluation.
 *
 * @ingroup AVP
 *
 * @copyright 2018 The FreeRADIUS server project
 * @copyright 2018 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/map.h>
#include <freeradius-devel/unlang/tmpl.h>
#include <freeradius-devel/unlang/map.h>

#include "map_priv.h"

typedef enum {
	UNLANG_UPDATE_MAP_INIT = 0,				//!< Start processing a map.
	UNLANG_UPDATE_MAP_EXPANDED_LHS,				//!< Expand the LHS xlat or exec (if needed).
	UNLANG_UPDATE_MAP_EXPANDED_RHS				//!< Expand the RHS xlat or exec (if needed).
} unlang_update_state_t;

/** State of an update block
 *
 */
typedef struct {
	fr_dcursor_t			maps;			//!< Cursor of maps to evaluate.

	fr_dlist_head_t			vlm_head;		//!< Head of list of VP List Mod.

	fr_value_box_list_t		lhs_result;		//!< Result of expanding the LHS
	fr_value_box_list_t		rhs_result;		//!< Result of expanding the RHS.

	unlang_update_state_t		state;			//!< What we're currently doing.
} unlang_frame_state_update_t;

/** State of a map block
 *
 */
typedef struct {
	fr_value_box_list_t		src_result;		//!< Result of expanding the map source.

	/** @name Resumption and signalling
	 * @{
 	 */
	void				*rctx;			//!< for resume / signal
	map_proc_func_t			resume;			//!< resumption handler
	unlang_map_signal_t		signal;			//!< for signal handlers
	fr_signal_t			sigmask;		//!< Signals to block.

	/** @} */
} unlang_frame_state_map_proc_t;

/** Wrapper to create a map_ctx_t as a compound literal
 *
 * @param[in] _mod_inst	of the module being called.
 * @param[in] _map_inst	of the map being called.
 * @param[in] _rctx	Resume ctx (if any).
 */
#define MAP_CTX(_mod_inst, _map_inst, _rctx) &(map_ctx_t){ .moi = _mod_inst, .mpi = _map_inst, .rctx = _rctx }

/** Apply a list of modifications on one or more fr_pair_t lists.
 *
 * @param[in] request	The current request.
 * @param[out] p_result	The rcode indicating what the result
 *      		of the operation was.
 * @return
 *	- UNLANG_ACTION_CALCULATE_RESULT changes were applied.
 *	- UNLANG_ACTION_PUSHED_CHILD async execution of an expansion is required.
 */
static unlang_action_t list_mod_apply(unlang_result_t *p_result, request_t *request)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_update_t	*update_state = frame->state;
	vp_list_mod_t const		*vlm = NULL;

	/*
	 *	No modifications...
	 */
	if (fr_dlist_empty(&update_state->vlm_head)) {
		RDEBUG2("Nothing to update");
		goto done;
	}

	/*
	 *	Apply the list of modifications.  This should not fail
	 *	except on memory allocation error.
	 */
	while ((vlm = fr_dlist_next(&update_state->vlm_head, vlm))) {
		int ret;

		ret = map_list_mod_apply(request, vlm);
		if (!fr_cond_assert(ret == 0)) {
			TALLOC_FREE(frame->state);

			return UNLANG_ACTION_FAIL;
		}
	}

done:
	RETURN_UNLANG_NOOP;
}

/** Create a list of modifications to apply to one or more fr_pair_t lists
 *
 * @param[out] p_result	The rcode indicating what the result
 *      		of the operation was.
 * @param[in] request	The current request.
 * @param[in] frame	Current stack frame.
 * @return
 *	- UNLANG_ACTION_CALCULATE_RESULT changes were applied.
 *	- UNLANG_ACTION_PUSHED_CHILD async execution of an expansion is required.
 */
static unlang_action_t list_mod_create(unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_frame_state_update_t	*update_state = talloc_get_type_abort(frame->state, unlang_frame_state_update_t);
	map_t				*map;

	/*
	 *	Iterate over the maps producing a set of modifications to apply.
	 */
	for (map = fr_dcursor_current(&update_state->maps);
	     map;
	     map = fr_dcursor_next(&update_state->maps)) {
	     	repeatable_set(frame);	/* Call us again when done */

		switch (update_state->state) {
		case UNLANG_UPDATE_MAP_INIT:
			update_state->state = UNLANG_UPDATE_MAP_EXPANDED_LHS;

			fr_assert(fr_value_box_list_empty(&update_state->lhs_result));	/* Should have been consumed */
			fr_assert(fr_value_box_list_empty(&update_state->rhs_result));	/* Should have been consumed */

			switch (map->lhs->type) {
			default:
				break;

			case TMPL_TYPE_EXEC:
				if (unlang_tmpl_push(update_state, &update_state->lhs_result,
						     request, map->lhs,
						     NULL) < 0) {
					return UNLANG_ACTION_STOP_PROCESSING;
				}
				return UNLANG_ACTION_PUSHED_CHILD;

			case TMPL_TYPE_XLAT:
				if (unlang_xlat_push(update_state, NULL, &update_state->lhs_result,
						     request, tmpl_xlat(map->lhs), false) < 0) {
					return UNLANG_ACTION_STOP_PROCESSING;
				}
				return UNLANG_ACTION_PUSHED_CHILD;

			case TMPL_TYPE_REGEX_XLAT_UNRESOLVED:
			case TMPL_TYPE_REGEX:
			case TMPL_TYPE_REGEX_UNCOMPILED:
			case TMPL_TYPE_REGEX_XLAT:
			case TMPL_TYPE_XLAT_UNRESOLVED:
				fr_assert(0);
			error:
				TALLOC_FREE(frame->state);
				repeatable_clear(frame);
				return UNLANG_ACTION_FAIL;
			}
			FALL_THROUGH;

		case UNLANG_UPDATE_MAP_EXPANDED_LHS:
			/*
			 *	map_to_list_mod() already concatenates the LHS, so we don't need to do it here.
			 */
			if (!map->rhs) goto next;

			update_state->state = UNLANG_UPDATE_MAP_EXPANDED_RHS;

			switch (map->rhs->type) {
			default:
				break;

			case TMPL_TYPE_EXEC:
				if (unlang_tmpl_push(update_state, &update_state->rhs_result,
						     request, map->rhs, NULL) < 0) {
					return UNLANG_ACTION_STOP_PROCESSING;
				}
				return UNLANG_ACTION_PUSHED_CHILD;

			case TMPL_TYPE_XLAT:
				if (unlang_xlat_push(update_state, NULL, &update_state->rhs_result,
						     request, tmpl_xlat(map->rhs), false) < 0) {
					return UNLANG_ACTION_STOP_PROCESSING;
				}
				return UNLANG_ACTION_PUSHED_CHILD;

			case TMPL_TYPE_REGEX:
			case TMPL_TYPE_REGEX_UNCOMPILED:
			case TMPL_TYPE_REGEX_XLAT:
			case TMPL_TYPE_REGEX_XLAT_UNRESOLVED:
			case TMPL_TYPE_XLAT_UNRESOLVED:
				fr_assert(0);
				goto error;
			}
			FALL_THROUGH;

		case UNLANG_UPDATE_MAP_EXPANDED_RHS:
		{
			vp_list_mod_t *new_mod;
			/*
			 *	Concat the top level results together
			 */
			if (!fr_value_box_list_empty(&update_state->rhs_result) &&
			    (fr_value_box_list_concat_in_place(update_state,
			    				       fr_value_box_list_head(&update_state->rhs_result), &update_state->rhs_result, FR_TYPE_STRING,
			    				       FR_VALUE_BOX_LIST_FREE, true,
			    				       SIZE_MAX) < 0)) {
				RPEDEBUG("Failed concatenating RHS expansion results");
				goto error;
			}

			if (map_to_list_mod(update_state, &new_mod,
					    request, map,
					    &update_state->lhs_result, &update_state->rhs_result) < 0) goto error;
			if (new_mod) fr_dlist_insert_tail(&update_state->vlm_head, new_mod);

			fr_value_box_list_talloc_free(&update_state->rhs_result);
		}

		next:
			update_state->state = UNLANG_UPDATE_MAP_INIT;
			fr_value_box_list_talloc_free(&update_state->lhs_result);

			break;
		}
	}

	return list_mod_apply(p_result, request);
}


/** Execute an update block
 *
 * Update blocks execute in two phases, first there's an evaluation phase where
 * each input map is evaluated, outputting one or more modification maps. The modification
 * maps detail a change that should be made to a list in the current request.
 * The request is not modified during this phase.
 *
 * The second phase applies those modification maps to the current request.
 * This re-enables the atomic functionality of update blocks provided in v2.x.x.
 * If one map fails in the evaluation phase, no more maps are processed, and the current
 * result is discarded.
 */
static unlang_action_t unlang_update_state_init(unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_group_t			*g = unlang_generic_to_group(frame->instruction);
	unlang_map_t			*gext = unlang_group_to_map(g);
	unlang_frame_state_update_t	*update_state;

	/*
	 *	Initialise the frame state
	 */
	MEM(frame->state = update_state = talloc_zero_pooled_object(request->stack, unlang_frame_state_update_t,
								    (sizeof(map_t) +
								    (sizeof(tmpl_t) * 2) + 128),
								    g->num_children));	/* 128 is for string buffers */

	fr_dcursor_init(&update_state->maps, &gext->map.head);
	fr_value_box_list_init(&update_state->lhs_result);
	fr_value_box_list_init(&update_state->rhs_result);
	fr_dlist_init(&update_state->vlm_head, vp_list_mod_t, entry);

	/*
	 *	Call list_mod_create
	 */
	frame_repeat(frame, list_mod_create);
	return list_mod_create(p_result, request, frame);
}

static unlang_action_t map_proc_resume(unlang_result_t *p_result, request_t *request,
#ifdef WITH_VERIFY_PTR
				       unlang_stack_frame_t *frame
#else
				       UNUSED unlang_stack_frame_t *frame
#endif
				      )
{
	unlang_frame_state_map_proc_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_map_proc_t);
	unlang_frame_state_map_proc_t	*map_proc_state = talloc_get_type_abort(frame->state, unlang_frame_state_map_proc_t);
	map_proc_func_t			resume;
	unlang_group_t			*g = unlang_generic_to_group(frame->instruction);
	unlang_map_t			*gext = unlang_group_to_map(g);
	map_proc_inst_t			*inst = gext->proc_inst;
	unlang_action_t			ua = UNLANG_ACTION_CALCULATE_RESULT;

#ifdef WITH_VERIFY_PTR
	VALUE_BOX_LIST_VERIFY(&map_proc_state->src_result);
#endif
	resume = state->resume;
	state->resume = NULL;

	/*
	 *	Call any map resume function
	 */
	if (resume) ua = resume(p_result, MAP_CTX(inst->proc->mod_inst, inst->data, state->rctx),
				request, &map_proc_state->src_result, inst->maps);
	return ua;
}

/** Yield a request back to the interpreter from within a module
 *
 * This passes control of the request back to the unlang interpreter, setting
 * callbacks to execute when the request is 'signalled' asynchronously, or whatever
 * timer or I/O event the module was waiting for occurs.
 *
 * @note The module function which calls #unlang_module_yield should return control
 *	of the C stack to the unlang interpreter immediately after calling #unlang_module_yield.
 *	A common pattern is to use ``return unlang_module_yield(...)``.
 *
 * @param[in] request		The current request.
 * @param[in] resume		Called on unlang_interpret_mark_runnable().
 * @param[in] signal		Called on unlang_action().
 * @param[in] sigmask		Set of signals to block.
 * @param[in] rctx		to pass to the callbacks.
 * @return
 *	- UNLANG_ACTION_YIELD.
 */
unlang_action_t unlang_map_yield(request_t *request,
				 map_proc_func_t resume, unlang_map_signal_t signal, fr_signal_t sigmask, void *rctx)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_map_proc_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_map_proc_t);

	REQUEST_VERIFY(request);	/* Check the yielded request is sane */

	state->rctx = rctx;
	state->resume = resume;
	state->signal = signal;
	state->sigmask = sigmask;

	/*
	 *	We set the repeatable flag here,
	 *	so that the resume function is always
	 *	called going back up the stack.
	 */
	frame_repeat(frame, map_proc_resume);

	return UNLANG_ACTION_YIELD;
}

static unlang_action_t map_proc_apply(unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_group_t			*g = unlang_generic_to_group(frame->instruction);
	unlang_map_t			*gext = unlang_group_to_map(g);

	map_proc_inst_t			*inst = gext->proc_inst;
	unlang_frame_state_map_proc_t	*map_proc_state = talloc_get_type_abort(frame->state, unlang_frame_state_map_proc_t);

	RDEBUG2("MAP %s \"%pM\"", inst->proc->name, &map_proc_state->src_result);

	VALUE_BOX_LIST_VERIFY(&map_proc_state->src_result);
	frame_repeat(frame, map_proc_resume);

	return inst->proc->evaluate(p_result, MAP_CTX(inst->proc->mod_inst, inst->data, NULL),
				    request, &map_proc_state->src_result, inst->maps);
}

static unlang_action_t unlang_map_state_init(unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_group_t			*g = unlang_generic_to_group(frame->instruction);
	unlang_map_t			*gext = unlang_group_to_map(g);
	map_proc_inst_t			*inst = gext->proc_inst;
	unlang_frame_state_map_proc_t	*map_proc_state = talloc_get_type_abort(frame->state, unlang_frame_state_map_proc_t);

	/*
	 *	Initialise the frame state
	 */
	repeatable_set(frame);

	fr_value_box_list_init(&map_proc_state->src_result);
	/*
	 *	Set this BEFORE doing anything else, as we will be
	 *	called again after unlang_xlat_push() returns.
	 */
	frame->process = map_proc_apply;

	/*
	 *	Expand the map source
	 */
	if (inst->src) switch (inst->src->type) {
	default:
	{
		fr_value_box_t *src_result = NULL;
		if (tmpl_aexpand(frame->state, &src_result,
				 request, inst->src, NULL, NULL) < 0) {
			REDEBUG("Failed expanding map src");
		error:
			RETURN_UNLANG_FAIL;
		}
		fr_value_box_list_insert_head(&map_proc_state->src_result, src_result);
		break;
	}
	case TMPL_TYPE_EXEC:
		if (unlang_tmpl_push(map_proc_state, &map_proc_state->src_result,
				     request, inst->src, NULL) < 0) {
			return UNLANG_ACTION_STOP_PROCESSING;
		}
		return UNLANG_ACTION_PUSHED_CHILD;

	case TMPL_TYPE_XLAT:
		if (unlang_xlat_push(map_proc_state, NULL, &map_proc_state->src_result,
				     request, tmpl_xlat(inst->src), false) < 0) {
			return UNLANG_ACTION_STOP_PROCESSING;
		}
		return UNLANG_ACTION_PUSHED_CHILD;


	case TMPL_TYPE_REGEX:
	case TMPL_TYPE_REGEX_UNCOMPILED:
	case TMPL_TYPE_REGEX_XLAT:
	case TMPL_TYPE_REGEX_XLAT_UNRESOLVED:
	case TMPL_TYPE_XLAT_UNRESOLVED:
		fr_assert(0);
		goto error;
	}

	return map_proc_apply(p_result, request, frame);
}

void unlang_map_init(void)
{
	unlang_register(UNLANG_TYPE_UPDATE,
			   &(unlang_op_t){
				.name = "update",
				.type = UNLANG_TYPE_UPDATE,
				.flag = UNLANG_OP_FLAG_DEBUG_BRACES,

				.interpret = unlang_update_state_init,

				.unlang_size = sizeof(unlang_map_t),
				.unlang_name = "unlang_map_t",
			   });

	unlang_register(UNLANG_TYPE_MAP,
			   &(unlang_op_t){
				.name = "map",
				.type = UNLANG_TYPE_MAP,
				.flag = UNLANG_OP_FLAG_RCODE_SET,

				.interpret = unlang_map_state_init,

				.unlang_size = sizeof(unlang_map_t),
				.unlang_name = "unlang_map_t",

				.frame_state_size = sizeof(unlang_frame_state_map_proc_t),
				.frame_state_type = "unlang_frame_state_map_proc_t",
			   });
}
