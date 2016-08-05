
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wikrt.h"


/* NOTES:
 *
 * Representation:
 * 
 * A pending value is just a tagged (block * value) pair. During an
 * evaluation, however, I'll need to build a stack that I can return
 * to in O(1) time. I can rebuild (block * value) from the stack.
 *
 * The 'stack' in question could simply be a list of `ops` lists.
 *
 * Effort Quota:
 *
 * Infinite loops will be a problem. So I'll just halt any computation
 * that appears to take too long. Eventually (with parallelism) I may
 * need something sophisticated, like a shared pool for computation
 * effort. 
 *
 * Testing our quota currently occurs whenever we end a block call,
 * including for tail calls. So we might run a bit over. This isn't
 * a big deal.
 *
 * How much work is 'too much'? I might eventually tune this when
 * loading the environment. But for now, I'll use a simple estimate
 * based on 'so many GC ticks'.
 *
 * Tail Call Optimization and Loopy Code:
 *
 * The 'tail call optimization' or TCO involves recognizing a `$c`
 * operator sequence at the end of a function call. It allows a
 * loopy computation to continue without increasing the call stack.
 * Some accelerators include the TCO, if performed at the end of
 * a block. So I have a choice:
 *
 *  * recognize a TCO accelerator in the parser
 *  * recognize TCO `$c]` sequence in the evaluator
 *
 * I've decided to move this responsibility into the parser. 
 */

#define WIKRT_EVAL_COMPACTION_STEPS 4

static inline void wikrt_eval_push_op(wikrt_cx* cx, wikrt_op op) 
{
    if(!wikrt_mem_reserve(cx, WIKRT_CELLSIZE)) { return; }
    cx->pc = wikrt_alloc_cellval_r(cx, WIKRT_PL, wikrt_i2v(op), cx->pc);
}

// (v*e) → e, with `v` added to head of `pc` as an opval.
static inline void wikrt_eval_push_opval(wikrt_cx* cx)
{
    // quote `v` into an opval
    wikrt_wrap_otag(cx, WIKRT_OTAG_OPVAL);

    // shift opval over to cx->pc
    if(!wikrt_p(cx->val)) { return; }
    wikrt_addr const a = wikrt_vaddr(cx->val);
    wikrt_val* const pa = wikrt_paddr(cx, a);
    cx->val = pa[1];
    pa[1] = cx->pc;
    cx->pc = wikrt_tag_addr(WIKRT_PL, a);
}


static void _wikrt_nop(wikrt_cx* cx) {  /* NOP */  }


static void _wikrt_sum_intro0(wikrt_cx* cx) 
{ 
    wikrt_wrap_sum(cx, WIKRT_INL); 
}
static void _wikrt_sum_elim0(wikrt_cx* cx) 
{
    wikrt_sum_tag lr;
    wikrt_unwrap_sum(cx, &lr);
    if(WIKRT_INL != lr) { wikrt_set_error(cx, WIKRT_ETYPE); }
}
static void _wikrt_sum_merge(wikrt_cx* cx) 
{
    wikrt_sum_tag lr;
    wikrt_unwrap_sum(cx, &lr);
    // do nothing with lr result
}
static void _wikrt_sum_assert(wikrt_cx* cx)
{
    wikrt_sum_tag lr;
    wikrt_unwrap_sum(cx, &lr);
    if(WIKRT_INR != lr) { wikrt_set_error(cx, WIKRT_ETYPE); }
}
static void _wikrt_accel_intro_void_left(wikrt_cx* cx)
{
    wikrt_wrap_sum(cx, WIKRT_INR);
}


static void wikrt_dK(wikrt_cx* cx, int32_t k) 
{
    // I could probably do faster integer building.
    // But it shouldn't be especially relevant with simplification.
    wikrt_intro_i32(cx, 10);
    wikrt_int_mul(cx);
    wikrt_intro_i32(cx, k);
    wikrt_int_add(cx);
}
static void _wikrt_intro_num(wikrt_cx* cx) { wikrt_intro_i32(cx, 0); }
static void _wikrt_d0(wikrt_cx* cx) { wikrt_dK(cx, 0); }
static void _wikrt_d1(wikrt_cx* cx) { wikrt_dK(cx, 1); }
static void _wikrt_d2(wikrt_cx* cx) { wikrt_dK(cx, 2); }
static void _wikrt_d3(wikrt_cx* cx) { wikrt_dK(cx, 3); }
static void _wikrt_d4(wikrt_cx* cx) { wikrt_dK(cx, 4); }
static void _wikrt_d5(wikrt_cx* cx) { wikrt_dK(cx, 5); }
static void _wikrt_d6(wikrt_cx* cx) { wikrt_dK(cx, 6); }
static void _wikrt_d7(wikrt_cx* cx) { wikrt_dK(cx, 7); }
static void _wikrt_d8(wikrt_cx* cx) { wikrt_dK(cx, 8); }
static void _wikrt_d9(wikrt_cx* cx) { wikrt_dK(cx, 9); }
static void _wikrt_int_cmp_gt(wikrt_cx* cx) 
{
//  G :: N(x) * (N(y) * e) → ((N(y)*N(x))+(N(x)*N(y)) * e -- y > x
//       #4 #2 G -- observes 4 > 2. Returns (N(2)*N(4)) on right.
    wikrt_ord gt;
    wikrt_int_cmp(cx, &gt);

    if(WIKRT_GT == gt) {
        wikrt_assocl(cx);
        wikrt_wrap_sum(cx, WIKRT_INR);
    } else {
        wikrt_wswap(cx);
        wikrt_assocl(cx);
        wikrt_wrap_sum(cx, WIKRT_INL);
    }
}

static inline bool wikrt_block_is_flagged_lazy(wikrt_otag otag) { 
    return (0 != (WIKRT_BLOCK_LAZY & otag));
}

static void _wikrt_eval_step_inline(wikrt_cx* cx) 
{
    // ([a→b]*a) → b. Equivalent to ABC code `vr$c`.

    wikrt_val* const v = wikrt_pval(cx, cx->val);
    bool const okType = wikrt_p(cx->val) && wikrt_o(*v) && wikrt_p(cx->cc);
    if(!okType) { wikrt_set_error(cx, WIKRT_ETYPE); return; }
    wikrt_val* const obj = wikrt_pobj(cx, (*v));

    if(!wikrt_otag_block(*obj)) { 
        wikrt_set_error(cx, WIKRT_ETYPE); 
    } 
    else if(0 != (WIKRT_BLOCK_LAZY & *obj)) {
        // Lazy blocks produce pending values. 
        (*obj) &= ~(WIKRT_BLOCK_LAZY); // don't preserve laziness
        if(!wikrt_mem_reserve(cx, WIKRT_CELLSIZE)) { return; }
        cx->val = wikrt_alloc_cellval_r(cx, WIKRT_O, WIKRT_OTAG_PEND, cx->val);
    }
    else {
        wikrt_addr const obj_addr = wikrt_vaddr_obj(*v);
        wikrt_val* const pctr = &(cx->pc);
        wikrt_val* const cstk = wikrt_pval(cx, cx->cc);
        // For both tail calls and regular calls, I'll push an operations list
        // to the cx->cc stack. Tail call optimization simply involves swapping
        // the applied function with an identity function (an empty ops list) 
        // whenever possible.
        _Static_assert(!WIKRT_NEED_FREE_ACTION, "dropping a cell during evaluation");
        obj[0]  = (*pctr);
        (*pctr) = obj[1];
        obj[1]  = (*cstk);
        (*cstk) = wikrt_tag_addr(WIKRT_PL, obj_addr);
        cx->val = v[1]; 
        if(WIKRT_UNIT_INR == obj[0]) { 
            // the tail call optimization. 
            obj[0]  = (*pctr);
            (*pctr) = WIKRT_UNIT_INR;
        }
    }
}
static void _wikrt_eval_step_tailcall(wikrt_cx* cx) 
{
    // ([a→b]*(a*unit))→b. 
    // Translate to an ([a→b]*a) inline operation.
    wikrt_assocl(cx);
    wikrt_elim_unit_r(cx);
    _wikrt_eval_step_inline(cx);
}
static void _wikrt_eval_step_apply(wikrt_cx* cx) 
{
    // ([a→b]*(a*e)) → (b*e) 
    // For simplicity, I'll just route this through the `inline` code,
    // even though it will never be in tail call position.
    wikrt_assocl(cx); wikrt_accel_swap(cx);  // (e * ([a→b]*a))
    wikrt_eval_push_op(cx, ACCEL_PROD_SWAP); 
    wikrt_eval_push_opval(cx); // quote `e`
    _wikrt_eval_step_inline(cx);
}
static void _wikrt_eval_step_condap(wikrt_cx* cx) 
{
    wikrt_wswap(cx); // (block * (sum * e)) → (sum * (block * e))
    wikrt_sum_tag lr;
    wikrt_unwrap_sum(cx, &lr);
    if(WIKRT_INR == lr) {
        wikrt_wrap_sum(cx, lr); // preserve sum type
        wikrt_wswap(cx);  
        wikrt_drop(cx);  // drop block, fails if relevant.
    } else {
        wikrt_eval_push_op(cx, OP_SUM_INTRO0); // return argument to left after apply.
        wikrt_wswap(cx);
        _wikrt_eval_step_apply(cx); // normal application of a block.
    }
}

static void _wikrt_asynch(wikrt_cx* cx) 
{
    // The `{&asynch}` annotation is intended to mark a value as
    // asynchronous. For now, I'll just model it as a lazy value
    // to ensure access is via `{&join}`.
    wikrt_intro_id_block(cx);
    wikrt_block_lazy(cx);
    _wikrt_eval_step_apply(cx);
}

static void _wikrt_join(wikrt_cx* cx)
{
    // The {&join} annotation serves a role similar to `seq` in Haskell.
    // It tells our runtime to wait upon a pending computation.
    
    // At the moment, pending computations are all modeled as (block*value)
    // pairs (hidden behind the `pending` tag). This might change in the 
    // future, e.g. for efficient asynch values. 
    wikrt_open_pending(cx);
    wikrt_assocr(cx);
    _wikrt_eval_step_apply(cx);
}


typedef void (*wikrt_op_evalfn)(wikrt_cx*);
static const wikrt_op_evalfn wikrt_op_evalfn_table[OP_COUNT] = 
{ [OP_SP] = _wikrt_nop
, [OP_LF] = _wikrt_nop
, [OP_PROD_ASSOCL] = wikrt_assocl
, [OP_PROD_ASSOCR] = wikrt_assocr
, [OP_PROD_W_SWAP] = wikrt_wswap
, [OP_PROD_Z_SWAP] = wikrt_zswap
, [OP_PROD_INTRO1] = wikrt_intro_unit_r
, [OP_PROD_ELIM1] = wikrt_elim_unit_r
, [OP_SUM_ASSOCL] = wikrt_sum_assocl
, [OP_SUM_ASSOCR] = wikrt_sum_assocr
, [OP_SUM_W_SWAP] = wikrt_sum_wswap
, [OP_SUM_Z_SWAP] = wikrt_sum_zswap
, [OP_SUM_INTRO0] = _wikrt_sum_intro0
, [OP_SUM_ELIM0] = _wikrt_sum_elim0
, [OP_COPY] = wikrt_copy
, [OP_DROP] = wikrt_drop
, [OP_APPLY] = _wikrt_eval_step_apply
, [OP_COMPOSE] = wikrt_compose
, [OP_QUOTE] = wikrt_quote
, [OP_REL] = wikrt_block_rel
, [OP_AFF] = wikrt_block_aff
, [OP_NUM] = _wikrt_intro_num
, [OP_D0] = _wikrt_d0
, [OP_D1] = _wikrt_d1
, [OP_D2] = _wikrt_d2
, [OP_D3] = _wikrt_d3
, [OP_D4] = _wikrt_d4
, [OP_D5] = _wikrt_d5
, [OP_D6] = _wikrt_d6
, [OP_D7] = _wikrt_d7
, [OP_D8] = _wikrt_d8
, [OP_D9] = _wikrt_d9
, [OP_ADD] = wikrt_int_add
, [OP_MUL] = wikrt_int_mul
, [OP_NEG] = wikrt_int_neg
, [OP_DIV] = wikrt_int_div
, [OP_GT] = _wikrt_int_cmp_gt
, [OP_CONDAP] = _wikrt_eval_step_condap
, [OP_DISTRIB] = wikrt_sum_distrib
, [OP_FACTOR] = wikrt_sum_factor
, [OP_MERGE] = _wikrt_sum_merge
, [OP_ASSERT] = _wikrt_sum_assert

, [ACCEL_TAILCALL] = _wikrt_eval_step_tailcall
, [ACCEL_INLINE] = _wikrt_eval_step_inline
, [ACCEL_PROD_SWAP] = wikrt_accel_swap
, [ACCEL_INTRO_UNIT_LEFT] = wikrt_intro_unit
, [ACCEL_SUM_SWAP]  = wikrt_accel_sum_swap
, [ACCEL_INTRO_VOID_LEFT] = _wikrt_accel_intro_void_left
, [ACCEL_wrzw] = wikrt_accel_wrzw
, [ACCEL_wzlw] = wikrt_accel_wzlw
, [ACCEL_ANNO_TRACE] = wikrt_trace_write
, [ACCEL_ANNO_TRASH] = wikrt_trash
, [ACCEL_ANNO_LOAD] = wikrt_load
, [ACCEL_ANNO_STOW] = wikrt_stow
, [ACCEL_ANNO_LAZY] = wikrt_block_lazy
, [ACCEL_ANNO_FORK] = wikrt_block_fork
, [ACCEL_ANNO_JOIN] = _wikrt_join
, [ACCEL_ANNO_ASYNCH] = _wikrt_asynch
, [ACCEL_ANNO_TEXT] = wikrt_anno_text
, [ACCEL_ANNO_BINARY] = wikrt_anno_binary
}; 

_Static_assert((WIKRT_ACCEL_COUNT == 18), 
    "evaluator is missing accelerators");

/* Construct an evaluation. ((a→b)*(a*e)) → ((pending b) * e).
 *
 * At the moment, this constructs a (pending (block * value)) structure.
 * The function wikrt_step_eval will need to preserve this structure if
 * it returns `true`.
 */
void wikrt_apply(wikrt_cx* cx) 
{
    bool const okType = wikrt_p(cx->val) && wikrt_blockval(cx, *wikrt_pval(cx, cx->val));
    if(!okType) { wikrt_set_error(cx, WIKRT_ETYPE); return; }
    wikrt_assocl(cx);
    wikrt_wrap_otag(cx, WIKRT_OTAG_PEND);
}

void wikrt_open_pending(wikrt_cx* cx)
{
    if(wikrt_p(cx->val)) {
        wikrt_val* const pv = wikrt_pval(cx, cx->val);
        if(wikrt_o(*pv)) {
            wikrt_val* const pobj = wikrt_pval(cx, *pv);
            wikrt_otag const otag = *pobj;
            if(wikrt_otag_pend(otag)) {
                _Static_assert(!WIKRT_NEED_FREE_ACTION, "free the 'pend' tag");
                (*pv) = pobj[1];
                return;
            }
        }
    }
    wikrt_set_error(cx, WIKRT_ETYPE); 
}

static inline void wikrt_require_fresh_eval(wikrt_cx* cx)
{
    bool const is_fresh_eval = (WIKRT_REG_CC_INIT == cx->cc) && (WIKRT_REG_PC_INIT == cx->pc);
    if(!is_fresh_eval) { wikrt_set_error(cx, WIKRT_IMPL); }
}

static inline void wikrt_run_eval_anno(wikrt_cx* cx, char const* strAnno)
{
    // Ignoring annotations is safe so long as coupled annotations
    // are handled appropriately. For the most part, annotation tokens
    // that Wikilon runtime recognizes should be detected at the parser.
}

static inline void wikrt_run_eval_token(wikrt_cx* cx, char const* token)
{
    if('&' == *token) { 
        wikrt_run_eval_anno(cx, token);
    } else if('.' == *token) {
        char seal[WIKRT_TOK_BUFFSZ];
        wikrt_unwrap_seal(cx, seal);
        bool const match_seal = (':' == *seal) && (0 == strcmp(seal+1,token+1));
        if(!match_seal) { wikrt_set_error(cx, WIKRT_ETYPE); }
    } else if(':' == *token) {
        // big sealer tokens should be rare.
        wikrt_wrap_seal(cx, token);
    } else if('\'' == *token) {
        // Assume token is resource ID for a local stowed value. NOTE: this
        // is better handled by the parser than by the evaluator.
        wikrt_intro_sv(cx, token);
    } else {
        wikrt_set_error(cx, WIKRT_IMPL);
    }
    
}

static void wikrt_run_eval_object(wikrt_cx* cx) 
{
    // handle extended operators: tokens and opvals.
    // The operator should be on the cx->val stack.
    assert(wikrt_p(cx->val) && wikrt_o(*wikrt_pval(cx, cx->val)));
    wikrt_val* const pv = wikrt_pval(cx, cx->val);
    wikrt_val* const pobj = wikrt_pobj(cx, *pv);

    if(wikrt_otag_opval(*pobj)) {
        _Static_assert(!WIKRT_NEED_FREE_ACTION, "todo: free WIKRT_OTAG_OPVAL cell");
        pv[0] = pobj[1]; 
    } else if(wikrt_otag_seal_sm(*pobj)) {
        if(!wikrt_p(pv[1])) { wikrt_set_error(cx, WIKRT_ETYPE); return; }
        wikrt_pval_swap(pobj+1, wikrt_pval(cx, pv[1]));
        wikrt_wswap(cx);
        wikrt_elim_unit(cx);
    } else if(wikrt_otag_seal(*pobj)) {
        char tokbuff[WIKRT_TOK_BUFFSZ];
        wikrt_unwrap_seal(cx, tokbuff);
        wikrt_elim_unit(cx);
        wikrt_run_eval_token(cx, tokbuff);
    } else {
        wikrt_set_error(cx, WIKRT_IMPL);
        fprintf(stderr, "%s: unhandled operation (%d)\n", __FUNCTION__, (int) LOBYTE(*pobj));
        abort();
    }
}

static void wikrt_run_eval_operator(wikrt_cx* cx, wikrt_op op)
{
    assert((OP_INVAL < op) && (op < OP_COUNT));
    wikrt_op_evalfn_table[op](cx);
}
 
void wikrt_run_eval_step(wikrt_cx* cx, int tick_steps) 
{
    uint64_t const tick_stop = cx->compaction_count + tick_steps;
    // Loop: repeatedly: obtain an operation then execute it.
    // Eventually I'll need a compact, high performance variant. 
    do { // Obtain an operation from cx->pc.
        if(WIKRT_PL == wikrt_vtag(cx->pc)) {
            wikrt_addr const addr = wikrt_vaddr(cx->pc);
            wikrt_val* const node = wikrt_paddr(cx,addr);
            wikrt_val const  op   = node[0];
            if(wikrt_smallint(op)) {
                _Static_assert(!WIKRT_NEED_FREE_ACTION, "review and repair: free program list cells");
                cx->pc  = node[1];
                wikrt_run_eval_operator(cx, (wikrt_op) wikrt_v2i(op));
            } else {
                cx->pc  = node[1];
                node[1] = cx->val;
                cx->val = wikrt_tag_addr(WIKRT_P, addr);
                wikrt_run_eval_object(cx);
            }
        } else if(WIKRT_UNIT_INR == cx->pc) {
            bool const eval_abort = (cx->compaction_count > tick_stop) || wikrt_has_error(cx);
            if(eval_abort) { return; }

            wikrt_val* const pcc = wikrt_pval(cx, cx->cc);
            if(WIKRT_PL == wikrt_vtag(*pcc)) {  
                _Static_assert(!WIKRT_NEED_FREE_ACTION, "todo: free stack cons cell");
                wikrt_val* const pstack = wikrt_pval(cx, (*pcc));
                cx->pc = pstack[0]; // pop the call stack
                (*pcc) = pstack[1]; 
                continue;
            } else if(WIKRT_UNIT_INR == (*pcc)) {
                return; // execution complete! 
            } else {
                // This shouldn't be possible.
                fprintf(stderr, "%s: unhandled evaluation stack type\n", __FUNCTION__);
                abort();
            }
        } else {
            fprintf(stderr, "%s: unhandled (compact?) operations list type (%lld)\n", __FUNCTION__, (long long int) cx->pc);
            abort();
        }
    } while(true);
    
    #undef eval_timeout
}

/* Step through an evaluation.  
 *
 *    ((pending a) * e) → ((pending a) * e) on `true`
 *    ((pending a) * e) → (a * e) on `false` without errors
 *
 * The pending tag wraps a (block * value) pair. I'll keep a
 * stack of incomplete operations lists during evaluation.
 *
 * During evaluation, the `e` value is hidden and I need a
 * stack for performance reasons. Additionally, I want very
 * fast access to the operations list. So I'll use the two
 * eval registers as follows:
 *
 *    cx->pc will contain my operations list (program counter)
 *    cx->cc will contain a (stack, e) pair.
 *
 * The stack is simply a list of ops-lists.
 */ 
bool wikrt_step_eval(wikrt_cx* cx)
{
    // preliminary
    wikrt_require_fresh_eval(cx);
    wikrt_open_pending(cx); // ((block * value) * e)
    if(wikrt_has_error(cx)) { return false; }

    // tuck `e` and an empty continuation stack into `cx->cc`. 
    assert(WIKRT_REG_CC_INIT == cx->cc);
    cx->cc = WIKRT_UNIT_INR;
    wikrt_pval_swap(wikrt_pval(cx, cx->val), &(cx->cc));
    wikrt_pval_swap(&(cx->val), &(cx->cc));

    // initialize `cx->pc` with the block's operations list. 
    // Remove as much indirection as feasible.
    assert(WIKRT_REG_PC_INIT == cx->pc);
    wikrt_open_block_ops(cx);
    wikrt_pval_swap(wikrt_pval(cx, cx->val), &(cx->pc));
    _Static_assert((WIKRT_REG_PC_INIT == WIKRT_UNIT), "assuming elim_unit for pc");
    wikrt_elim_unit(cx);

    // At this point: cx->cc and cx->pc are initialized.
    wikrt_run_eval_step(cx, WIKRT_EVAL_COMPACTION_STEPS); // run main evaluation loop

    bool const finished = 
        (WIKRT_UNIT_INR == cx->pc) &&
        (WIKRT_UNIT_INR == wikrt_pval(cx, cx->cc)[0]);

    if(finished) {
        // recover the hidden `e` value from cx->cc
        wikrt_pval_swap(&(cx->val), wikrt_pval(cx, cx->cc));
        wikrt_pval_swap(&(cx->val), &(cx->cc));

        // restore the registers.
        cx->pc = WIKRT_REG_PC_INIT;
        cx->cc = WIKRT_REG_CC_INIT;

        return false;
    } else {
        // TODO: rebuild the `block` for the next evaluation step. Restore
        // the pending value structure and registers.
        wikrt_set_error(cx, WIKRT_IMPL);
        return !wikrt_has_error(cx);
    }
}



// (block * e) → (ops * e), returning otag
wikrt_otag wikrt_open_block_ops(wikrt_cx* cx) 
{
    if(wikrt_p(cx->val)) {
        wikrt_val* const pv = wikrt_pval(cx, cx->val);
        if(wikrt_o(*pv)) {
            wikrt_val* const pobj = wikrt_pval(cx, *pv);
            wikrt_otag const otag = *pobj;
            if(wikrt_otag_block(otag)) {
                _Static_assert(!WIKRT_NEED_FREE_ACTION, "free the 'block' tag");
                (*pv) = pobj[1];
                return otag;
            }
        }
    }
    wikrt_set_error(cx, WIKRT_ETYPE);
    return 0;
}
