
#pragma once
#ifndef WIKRT_H

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <lmdb.h>
#include "wikrt.h"
#include "wikrt_eph.h"

/** NOTES
 * 
 * Pointers: We'll use native pointers internally. This performs well, and 
 * is necessary for wikrt_cx_freeze to support references into the parent
 * context.
 * 
 * Dictionary Names: valid Awelon words up to so many bytes are accepted.
 * Anything else is aliased via secure hash. This should be invisible to 
 * our API clients. 
 *
 * Timing Attacks: Secure hashes must resist timing attacks. Expose only
 * first 60 bits or so to timing. This might be achieved by performing a
 * partial key search using comparisons, and comparing the rest via scan.
 *
 * Copy on Write: I can introduce wikrt_cx_freeze action to the API such that
 * subsequent copies of a frozen context are logical, shallow, copy-on-write
 * in nature. I'll try this as an experimental API. (Now added.) Writeable
 *
 * Write-Exec: Can I just make it so contexts are fully read-write-exec?
 * I might lose the ability to work on SE-linux, but that isn't a huge 
 * problem for me. Look into mprotect(2) and personality(2).
 *
 * GC of Secure Hash Resources: I'll need some environment-level counting
 * bloom filters. For now, I could probably just use a fixed size filter.
 *
 * Multi-Process Utilities: use shm_open to create and manage the ephemeron
 * table. Ephemerons will be tracked via simple counting hashtable. We'll
 * need to assign a unique ID to each runtime database for this to work.
 */

// Using native sized words.
typedef uintptr_t wikrt_v;

// Aliases for internal documentation.
typedef wikrt_v  wikrt_o;           // tagged object header
typedef wikrt_v  wikrt_n;           // small natural number
typedef intptr_t wikrt_i;           // small integer number
typedef wikrt_v  wikrt_z;           // arbitrary size value
typedef wikrt_v  wikrt_a;           // location in memory
#define WIKRT_V_MAX  UINTPTR_MAX
#define WIKRT_Z_MAX  WIKRT_V_MAX

#define WIKRT_LNBUFF(SZ,LN) ((((SZ)+((LN)-1))/(LN))*(LN))
#define WIKRT_LNBUFF_POW2(SZ,LN) (((SZ) + ((LN) - 1)) & ~((LN) - 1))
#define WIKRT_CELLSIZE (2 * sizeof(wikrt_v))
#define WIKRT_CELLBUFF(sz) WIKRT_LNBUFF_POW2(sz, WIKRT_CELLSIZE)
#define WIKRT_FILE_MODE (mode_t)(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#define WIKRT_DIR_MODE (mode_t)(WIKRT_FILE_MODE | S_IXUSR | S_IXGRP)

#define WIKRT_WORKER_STACK_MIN (64 * 1024)
#define WIKRT_WORKER_STACK_SIZE WIKRT_LNBUFF(WIKRT_WORKER_STACK_MIN, PTHREAD_STACK_MIN)

static inline wikrt_z wikrt_cellbuff(wikrt_z n) { return WIKRT_CELLBUFF(n); }

/** Bit Representations
 *
 *      b00     small constants, actions, tags
 *      b01     tagged objects or actions (header ..data..)
 *      b10     composition cell (B, A) => [B A]
 *      b11     constructor cell (H, T) => [H T :]
 *      `b` bit is 1 for blocks or values words, 0 for inline actions
 *
 * Common Small Constants (2 bits + b00)
 *
 *      00      extended
 *      10      naturals        
 *      x1      integers
 *
 *   Naturals have a range up to 2^27-1 on a 32-bit system or 2^59-1
 *   on a 64-bit system before the 'big' tagged object encoding must
 *   be used. Integers use the same range plus or minus so we can 
 *   trivially convert between small naturals and small integers.
 *
 * Extended Small Constants (3 bits + 00b00)
 *
 *      000     built-in primitives, accelerators, etc.
 *      (small rationals, decimals, labels, texts, etc.)
 *
 *   I'm assuming 64-bit systems will be the common option. Decimals 
 *   with an 8 bit exponent (base 10) and 48-bit mantissa would cover
 *   a lot of practical values. On a 32-bit system, extended small
 *   constants won't cover nearly as much but are still useful.
 *
 * Note: one reason for constants to be arranged the way they are is
 * to ensure a zero-filled memory corresponds to a sequence of OP_NOP
 * inaction.
 *
 * Aside: Awelon and Wikilon are unlikely to support floating point
 * data, excepting very careful acceleration of linear algebras. CPUs
 * may vary in how much internal precision they provide, etc..
 */
#define WIKRT_SMALL 0
#define WIKRT_OBJ   1
#define WIKRT_COMP  2
#define WIKRT_CONS  3
#define WIKRT_VAL   4

#define WIKRT_SMV   (WIKRT_VAL | WIKRT_SMALL)
#define WIKRT_VOBJ  (WIKRT_VAL | WIKRT_OBJ)

#define WIKRT_REF_MASK_TYPE     3
#define WIKRT_REF_MASK_ADDR     (~((wikrt_v)7))

#define WIKRT_SMALL_INT_OP    8  /* _1000; int behavior */
#define WIKRT_SMALL_INT_VAL  (WIKRT_SMALL_INT_OP | WIKRT_VAL)
#define WIKRT_SMALL_NAT_OP   16  /* 10000; nat behavior */
#define WIKRT_SMALL_NAT_VAL  (WIKRT_SMALL_NAT_OP | WIKRT_VAL)

#define WIKRT_SMALLNAT_MAX (WIKRT_V_MAX >> 5)
#define WIKRT_SMALLINT_MAX WIKRT_SMALLNAT_MAX
#define WIKRT_SMALLINT_MIN (- WIKRT_SMALLINT_MAX)

// bit-level utility functions
static inline wikrt_v wikrt_vtag(wikrt_v v) { return (WIKRT_REF_MASK_TYPE & v); }
static inline wikrt_a wikrt_v2a(wikrt_v v) { return (WIKRT_REF_MASK_ADDR & v); }
static inline bool wikrt_action(wikrt_v v) { return !(WIKRT_VAL & v); }
static inline bool wikrt_value(wikrt_v v) { return !wikrt_action(v); }

static inline bool wikrt_val_in_ref(wikrt_v v) { return !wikrt_vtag(v); }
static inline bool wikrt_is_basic_op(wikrt_v v) { return !(v & 0xFF); }
static inline wikrt_v* wikrt_a2p(wikrt_a a) { return (wikrt_v*)a; }
static inline wikrt_v* wikrt_v2p(wikrt_v v) { return wikrt_a2p(wikrt_v2a(v)); }
static inline bool wikrt_is_ptr(wikrt_v v) { return !wikrt_val_in_ref(v); }

static inline bool wikrt_is_small_nat_op(wikrt_v v)  { return (WIKRT_SMALL_NAT_OP  == (v & 0x1F)); }
static inline bool wikrt_is_small_nat_val(wikrt_v v) { return (WIKRT_SMALL_NAT_VAL == (v & 0x1F)); }
static inline bool wikrt_is_small_int_op(wikrt_v v)  { return (WIKRT_SMALL_INT_OP  == (v & 0x0F)); }
static inline bool wikrt_is_small_int_val(wikrt_v v) { return (WIKRT_SMALL_INT_VAL == (v & 0x0F)); }

static inline wikrt_n wikrt_from_small_nat(wikrt_v v) { return (((wikrt_n)v) >> 5); }
static inline wikrt_v wikrt_to_small_nat_val(wikrt_n n) { return (((wikrt_v)(n << 5))|WIKRT_SMALL_NAT_VAL); }
static inline wikrt_v wikrt_to_small_nat_op(wikrt_n n) { return (((wikrt_v)(n << 5))|WIKRT_SMALL_NAT_OP); }

static inline wikrt_i wikrt_from_small_int(wikrt_v v) { return (((wikrt_i)v) >> 4); }
static inline wikrt_v wikrt_to_small_int_val(wikrt_i i) { return (((wikrt_v)(i << 4))|WIKRT_SMALL_INT_VAL); }
static inline wikrt_v wikrt_to_small_int_op(wikrt_i i) { return (((wikrt_v)(i << 4))|WIKRT_SMALL_INT_OP); }

/** Secure Random Data
 *
 * Wikilon Runtime doesn't need entropy for much. But one place it
 * does need some random data is allocation of the shared memory
 * ephemeron tables.
 */
void wikrt_get_entropy(size_t amt, uint8_t* out);

/** Tagged Objects
 *
 * Wikilon runtime shouldn't need more than 32 common object types,
 * especially if I consolidate common pairs and quadruples. Beyond
 * the structural type, each object tracks a little substructure.
 *
 *   - shared:  reference shared, must copy on write
 *   - no-copy: the (nc) annotation has been applied
 *   - no-drop: the (nd) annotation has been applied
 * 
 * Objects are unique by default, enabling in-place update. But
 * in-place update is only possible with specific accelerators,
 * such as fast, indexed update of a list (via an array).
 */
typedef enum wikrt_otype
{ WIKRT_OTYPE_PAIR = 0  // trivial (header, value) pairs
, WIKRT_OTYPE_QUAD      // (header, value, value, value) quadruples

, WIKRT_OTYPE_RAW       // raw binary IO data
, WIKRT_OTYPE_BINARY    // array of byte values
, WIKRT_OTYPE_BLOCK     // flat sequence of code
, WIKRT_OTYPE_ARRAY     // compact list value

, WIKRT_OTYPE_BIGNUM    // natural numbers
, WIKRT_OTYPE_TASK      // a fragment of code under evaluation
, WIKRT_OTYPE_WORD      // interned, and includes annotations
} wikrt_otype;

#define WIKRT_O_TYPE  (0x1f)
#define WIKRT_O_SHARE (1<<5)
#define WIKRT_O_NC    (1<<6)
#define WIKRT_O_ND    (1<<7)
#define WIKRT_O_DATA_OFF  8
#define WIKRT_O_DATA_MAX (WIKRT_Z_MAX >> WIKRT_O_DATA_OFF)

/** Array and Block Structure
 *
 * Arrays are simple (header, val, val, val, ...) where the number of
 * values is in the header. The interpretation might be for a block of
 * code or a list of values. This gives array fragments a maximum size
 * in Awelon, but we can logically append two fragments.
 * 
 * It is also feasible to support logical slices and logical reversal
 * of array data. If we append two slices adjacent in memory, we can
 * combine them into one large slice to optimize divide-and-conquer
 * tactics on large arrays. 
 *
 * Array buffers aren't supported implicitly, but one can explicitly
 * allocate a large buffer then model allocations and growth and ring
 * buffers and so on within that buffer, using zeroes to fill unused
 * data slots. This technique is used within tasks to model data and
 * call stacks.
 *
 * Lists in Awelon are heterogeneous. Thus, so are arrays.
 *
 */
typedef struct wikrt_array { wikrt_o o; wikrt_v d[]; } wikrt_array;
typedef wikrt_array wikrt_block;

/** Binary and Raw Data
 *
 * Binaries are just arrays of bytes, while raw data models unparsed
 * code or other IO resources. All initial input to a Wikilon context
 * is treated as raw data until parsed, and we'll generally translate
 * back to raw data upon output.
 */
typedef struct wikrt_binary { wikrt_o o; uint8_t b[]; } wikrt_binary;
typedef wikrt_binary wikrt_raw;

/** Big Numbers
 *
 * Awelon primarily supports natural numbers, and works with them
 * always in base 10. Under the hood, we'll use a variation on the
 * binary coded decimals: a big number is represented by sequence
 * of 32-bit words, each ranging 0..999999999, little-endian (low
 * words first). This representation shall be used at parse time 
 * for number words.
 *
 * Big integers, decimals, or rationals will be modeled explicitly
 * above big natural numbers, whereas small integers and useful
 * decimals (on a 64-bit system) can be modeled via small values.
 */
typedef struct wikrt_bignum { wikrt_o o; uint32_t w[]; } wikrt_bignum;

/* Pairs and Quads
 *
 * logical list reversal: pair
 * logical list append: quad (size if known, left, right)
 * logical array slice: quad (offset, count, data)
 *
 * fixpoint wrapper: composition or pair
 *   Using composition would require an allocation per loop.
 *   But that isn't bad for guaranteeing GC-driven actions.
 * (error) values: a pair
 * text values: pair, wrap a binary
 * JIT code: quad wrapping a block?
 *
 * Maybe seal/unseal actions. 
 */

/** Words table.
 *
 * Excepting numbers, words are interned and kept in a hashtable.
 * Each word will need some metadata.
 *
 *     word's definition (JIT, block, task, unread)
 *     word's input-output arity if known (0-1 for value words)
 *     track update of a word since most recent commit
 *     read/write status since last commit
 *     breakpoint state
 *
 * Almost any access to our word table will be synchronized, but
 * ideally the words themselves may be used with minimal synch.
 * 
 * Since words are mostly present as a cache, it's okay to GC words
 * that aren't used and reload them from dictionary as needed. To
 * track caching, we might want to model a `recently used` field in
 * each word, clearing it when we perform full GC then setting it
 * when first link a word after GC. This would simplify decisions
 * to GC data that isn't used much (including stowage resources).
 *
 * The exception is written definitions, which cannot be collected
 * until we commit. So we must auto-mark words whose definitions 
 * have yet to be written. 
 */

/** Built-in Operations (Primitives, Accelerators, Annotations)
 *
 * Awelon relies on accelerators as a primary performance technique,
 * both for functions and data. The "built ins" are just the set of
 * basic or accelerated fixed-form functions.
 *
 * Accelerators cannot be referenced directly by user code, instead
 * being accessed indirectly by matching. Define function "w" to the
 * program "(a2) [] b a" and you'll use OP_w. This method is fragile
 * in general, but that can be mitigated with wikrt_write_prelude
 * and de-facto standardization.
 * 
 * Annotations are included in this list, excepting sealers and other
 * annotations that have a partially user-defined symbol. Unrecognized
 * annotations will be dropped upon parsing.
 */
typedef enum wikrt_op 
{ OP_NOP = 0    // empty program (identity behavior)
, OP_a          // apply; [B][A]a == A[B]
, OP_b          // bind;  [B][A]b == [[B]A]
, OP_c          // copy;  [A]c == [A][A]
, OP_d          // drop;  [A]d ==

// Arity Annotations
, OP_ANNO_a2    // [B][A](a2) == [B][A]
, OP_ANNO_a3    // [C][B][A](a3) == [C][B][A]
, OP_ANNO_a4    // ...
, OP_ANNO_a5
, OP_ANNO_a6
, OP_ANNO_a7
, OP_ANNO_a8
, OP_ANNO_a9

// Substructural Types
, OP_ANNO_nc    // (nc) no-copy, forbid value copy
, OP_ANNO_nd    // (nd) no-drop, forbid value drop

// Active Debugging (Preliminary)
, OP_ANNO_error // (error) marks a value
, OP_ANNO_trace // (trace) writes debug output

// Performance Annotations
, OP_ANNO_par   // (par) evaluates block in parallel
, OP_ANNO_eval  // (eval) evaluate before continuing
, OP_ANNO_memo  // (memo) memoize computation of block
, OP_ANNO_stow  // [large value](stow) => [$secureHash]
                // [small value](stow) => [small value]
, OP_ANNO_trash // (trash) replace block with error value
    // todo: annotations for link, optimize, compile

// Extensions for Compiled code
, OP_EXT_RETURN // represents end of block
 , OP_EXT_RETURN_ad // tail call via `... a d]`
 , OP_EXT_RETURN_i  // tail call via `... i]`
, OP_EXT_RPUSH // push data to return stack
, OP_EXT_RPOP // /pop data from return stack

// Annotations to control Optimization, Compilation?

// Simple Accelerators
 // future: permutations of data plumbing. Common loops.
 // Note: I should probably guide this via actual usage.
, OP_w          // swap;   [B][A]w == [A][B]; w = (a2) [] b a
, OP_rot        // [C][B][A]rot == [A][C][B]; rot = (a3) [] b b a
, OP_i          // inline; [A]i == A; i = [] w a d
, OP_z          // fixpoint Z combinator; [X][F]z == [X][[F]z]F
                // z = [[(a3) c i] b (=z) [c] a b w i](a3) c i

// Conditional Behaviors
, OP_true       // [B][A]true i     == A;    true = [a d]
, OP_false      // [B][A]false i    == B;    false = [d i] (= 0)
, OP_L          // [B][A][[V]L] i   == [V]B; L = (a3) w d w i
, OP_R          // [B][A][[V]R] i   == [V]A; R = (a3) w b a d
, OP_ANNO_bool  // (bool) type assertion  [F] or [T]
, OP_ANNO_opt   // (opt) type assertion   [F] or [[V]R]
, OP_ANNO_sum   // (sum) type assertion   [[V]L] or [[V]R]
, OP_ANNO_cond  // (cond) type assertion  (sum or boolean)
, OP_if         // if = rot (cond) i

// Natural Number Arithmetic
//  I need at least add, multiply, difference, and division.
//  Diff and div should be lossless. Like 7 11 diff might be 0 4
//  to record the latter was larger, and division has remainder.
//  An accelerated GCD might also be nice.
//
//  These operations are only accelerated if our 0 and S are
//  defined appropriately, along with the specific operations.
//
//  Conveniently, I don't need any divide-by-zero errors at the
//  API layer. That would become a "divide-by-zero"(error) and
//  freeze the relevant portion of the evaluation.
, OP_ANNO_nat   // (nat) type assertion
, OP_S          // essentially `[(nat)R]b`
, OP_nat_add    
, OP_nat_mul
, OP_nat_diff
, OP_nat_divmod

// Integer Arithmetic
//  add, mul, div, sub, abs, neg

// List and Array Operations
} wikrt_op;

/** The Database
 * 
 * Other than named roots, Wikilon uses secure hashes to reference
 * binary values. Thus most keys are 60 byte secure hashes encoded
 * in base64url. This has non-trivial storage and lookup overheads.
 * But it simplifies several problems related to structure sharing
 * and stability of identifiers for import/export. 
 *
 * For security reasons, lookups with a secure hash (db->memory etc.)
 * will limit exposure for timing attacks to at most 60 bits. 
 *
 * Lazy reference counting GC is used for secure hash resources. The
 * laziness is achieved by separating stable reference counts from
 * pending updates. Objects with zero references are tracked via the
 * shared memory wikrt_eph ephemeron table.
 *
 * The 'roots' table is just arbitrary data and updates to it must
 * be manually reference counted as part of a transactional update.
 * Roots are not themselves reference counted.
 */ 
typedef struct wikrt_db {
    // LMDB layer resources
    MDB_env            *mdb;
    MDB_dbi             roots;  // name → binary data
    MDB_dbi             memory; // hash → binary data
    MDB_dbi             refcts; // hash → reference counts and pending deltas
    MDB_dbi             refupd; // list of partial hashes with pending deltas
} wikrt_db;

void wikrt_db_close(wikrt_env*);

/** The Environment
 *
 * This models the physical machine resources shared by contexts,
 * including the persistence layer and virtual 'CPUs' in the form
 * of worker threads.
 * 
 * All contexts are tracked. I use two circular linked lists, with
 * one being the list with 'work available' so worker threads don't
 * need to repeatedly scan passive contexts when searching for work.
 * The other is essentially a list of passive or single-threaded
 * contexts. A context may also abort work, forcing it back to the
 * 
 * I might need an additional, separate thread to manage GC of the
 * database. Or I could keep some heuristics and perform this from
 * whichever threads are working with the database at the time.
 *
 * Worker threads will cycle through available contexts, those marked
 * as having work available.
 */
struct wikrt_env {
    // every context is exclusively in one list
    wikrt_cx        *cxs;   // single-threaded or passive contexts
    wikrt_cx        *cxw;   // contexts with obvious work available
    pthread_mutex_t mutex;  // mutex for environment manipulations

    // database and shared memory ephemeron table
    wikrt_db        *db;    // persistent storage
    wikrt_eph       *eph;   // ephemeral reference tracking


    // workers thread pool and work signaling
    uint32_t        workers_alloc;  // for increasing thread count
    uint32_t        workers_max;    // for reducing thread count
    pthread_cond_t  work_available; // work in cxw or if max<alloc
    pthread_cond_t  workers_halted; // for safe shutdown
};
// for static assertions, if we break this assumption remove the def
#define WIKRT_ENV_HAS_TWO_CONTEXT_LISTS 1

void* wikrt_worker_behavior(void* e); 
void wikrt_halt_threads(wikrt_env* e);

/** Write Set for Generational GC
 *
 * Generational GC requires tracking references from the old generation
 * to the young generation. I intend to track this at the field level so
 * we can work with parts of a large array. My current plan is to use a
 * hashtable indexing small 'pages' of fields to a bitfield. 
 *
 * Assuming the table is half filled, this has worst case 12.5% overhead
 * on a 32-bit system or 6.25% overhead on a 64-bit system. The normal 
 * case is considerably better because we expect most old objects to be
 * unmodified. All of these tables can be cleared every time we perform
 * a full context GC, so this also is recoverable space.
 *
 * A write set is represented within a WIKRT_OTYPE_BINARY object, and
 * may further be wrapped as a composable list of write sets. 
 */
typedef struct wikrt_wsd {
    wikrt_n     page;
    wikrt_n     bits;
} wikrt_wsd;
typedef struct wikrt_ws {
    wikrt_z     size;       // buffer size
    wikrt_z     fill;       // element count
    wikrt_wsd   data[];     // data, adjacent to the header.
} wikrt_ws;

/** Multi-Threading and Garbage Collection
 * 
 * Threads operate within a context, evaluating parallel tasks. To minimize
 * synchronization, each thread has its own 'nursery' - a volume of memory
 * for lightweight allocations that is independently garbage collected. Due
 * to compaction of objects within a nursery, it's important that a thread
 * never directly references another thread's nursery.
 *
 * Communication between threads requires 'stable' memory - objects that are
 * promoted from the nursery to the shared survivor space. This includes both
 * receiving tasks and results from a parallel operation. This adds latency
 * to parallelism, and requires careful attention to task 'state'.
 *
 * In any case, each thread must track
 *
 *   - memory and local generations
 *   - trace and profile logging
 *   - ownership of tasks, task status
 * 
 * I'm also interested in support for fine-grained control of parallelism
 * within a context, but it isn't a critical feature at this time. For now,
 * each thread will simply track a local effort quota.
 *
 * Effort tracking: I'd like to generally preallocate the effort for a few
 * GC cycles. We can likely estimate based on a previous cycle time, and
 * prepay for a few GC cycles. The 'cycle' in question might be a survivor
 * GC cycle rather than the youngest generation.
 */
typedef struct wikrt_thread { 
    wikrt_cx* cx;   // for large allocations, coordination

    // Memory Management (no free lists)
    wikrt_a start;  // first reserved address
    wikrt_a end;    // last reserved address
    wikrt_a elder;  // end of prior young generation
    wikrt_a young;  // end of young generation (alloc start)
    wikrt_a stop;   // allocation cap (for GC, reserves space for marking)
    wikrt_a alloc;  // current allocator

    // Write Set for Generational GC.
    wikrt_v write_set;

    // Tasks to Perform.
    wikrt_v ready;      // tasks we can work on now
    wikrt_v ready_r;    // tasks recently allocated
    wikrt_v waiting;    // tasks awaiting promotion

    // Debug Logs - thread local; moved to cx->memory when stable
    wikrt_v trace;      // (trace) messages 
    wikrt_v prof;       // stack profile

    // Local Memory Statistics
    uint64_t gc_bytes_processed;
    uint64_t gc_bytes_collected;

    // effort tracking
    uint64_t time_last;  // wikrt_thread_time at last allocation
    uint32_t effort;     // pre-allocated effort for this cycle

    // Idea: I could add an mdb_txn here via mdb_txn_reset and
    //  mdb_txn_renew to reduce malloc/free overheads within a
    //  thread.
} wikrt_thread;

/** current timestamp in microseconds */
uint64_t wikrt_thread_time(); // microseconds

/** Registers Table
 *
 * Registers are kept in a simple hashtable, using a binary for the
 * register names and an array for register data. Register data is
 * simply interpreted as a toplevel program, leveraging OTYPE_RAW
 * to represent intermediate outputs. 


 * The table is written only via API-level actions.
 * 
 * 
 * 
 * 
 */
typedef struct wikrt_rtb { 
    wikrt_n size;
    wikrt_n fill;
    wikrt_v ids;
    wikrt_v data;
} wikrt_rtb;

bool wikrt_register_addend_temp(wikrt_cx*, wikrt_r);

 
/** A context.
 *
 * A context is represented by a contiguous volume of memory, and has
 * a corresponding dictionary. Memory is filled via 'streams', which
 * represent externally accessible binary data. 
 *
 * Additionally, each context tracks words loaded from the dictionary.
 * This supports the transaction modela nd allows for compilation of 
 * words, and partial GC as the context fills.
 *
 * A context is associated with a dictionary in persistent storage.
 * If a dictionary name is an invalid word or is larger than its
 * secure hash, we'll rewrite it to the secure hash of the name.
 *
 * Worker threads will operate in a context until either no work is
 * available or until interrupted via workers_halt. Each thread has 
 * its own wikrt_thread, with shared allocations from cx->memory
 * synchronized via cx->mutex. The main thread is preserved so it
 * can be used across many API calls.
 */
struct wikrt_cx {
    wikrt_env      *env;
    
    // note: following fields are protected by env->mutex
    // to support worker threads, etc.
    wikrt_cx       *cxn;                // circular list of contexts
    wikrt_cx       *cxp;
    bool            in_env_worklist;    // in env->cxw (as opposed to cxs)
    bool            workers_halt;       // request active workers to halt
    uint32_t        worker_count;       // count of workers in context
    pthread_cond_t  workers_done;       // signal when (0 == worker_count)

    // mutex for content within context
    pthread_mutex_t mutex;              // to protect local allocator

    // to support frozen contexts
    wikrt_n         refct;              // references as a frozen context 
    wikrt_cx*       proto;              // a frozen prototype context 
    bool            frozen;             // whether this context is frozen

    // Debug Flags
    bool            trace_enable;       // (trace) messages
    bool            prof_enable;        // stack profiling
    
    // parallel computations in shared memory
    size_t          size;               // initial allocation
    wikrt_thread    memory;             // shared context memory

    // Dictionary Data
    size_t          dict_name_len;      // 0..WIKRT_HASH_SIZE
    uint8_t         dict_name[WIKRT_HASH_SIZE + 4]; // unique name of dictionary (NUL terminated) 
    uint8_t         dict_ver[WIKRT_HASH_SIZE + 4];  // an import/export hash val (NUL terminated)
    wikrt_v         words;              // words table in context memory

    // Registers
    wikrt_v         temp;               // temporary data register
    wikrt_rtb       reg;                // primary registers table

    // todo:
    // Stowage tracking: need to know all stowage roots
    // Dictionary indexing?

};

// a sufficient minimum size that we won't have too many problems
#define WIKRT_CX_MIN_SIZE (1<<14)

// default effort is about 100ms labor
#define WIKRT_CX_DEFAULT_EFFORT (100 * 1000)

wikrt_z wikrt_gc_bitfield_size(wikrt_z alloc_space);
wikrt_z wikrt_compute_alloc_space(wikrt_z space_total); // include GC reserve space

void wikrt_add_cx(wikrt_cx** plist, wikrt_cx* cx);
void wikrt_rem_cx(wikrt_cx** plist, wikrt_cx* cx);
bool wikrt_cx_has_work(wikrt_cx*);
void wikrt_cx_signal_work_available(wikrt_cx*);
void wikrt_cx_interrupt_work(wikrt_cx*);
bool wikrt_cx_has_work(wikrt_cx*);
size_t wikrt_word_len(uint8_t const* const src, size_t maxlen);

static inline bool wikrt_cx_unshared(wikrt_cx* cx) 
{
    return (0 == cx->worker_count) 
        && !(cx->in_env_worklist);
}

// test availability of thread-local memory 
static inline bool wikrt_thread_mem_available(wikrt_thread const* t, wikrt_z amt)
{
    return ((t->stop - t->alloc) >= amt);
}
bool wikrt_thread_mem_gc_then_reserve(wikrt_thread* t, wikrt_z amt);

// attempt to reserve some thread-local memory
static inline bool wikrt_thread_mem_reserve(wikrt_thread* const t, wikrt_z amt)
{
    return wikrt_thread_mem_available(t, amt) ? true 
         : wikrt_thread_mem_gc_then_reserve(t, amt);
}

// Allocate from thread memory. Assumes `amt` is buffered to wikrt_cellbuff,
// and that our thread has sufficient space. 
static inline wikrt_a wikrt_thread_alloc(wikrt_thread* const t, wikrt_z amt)
{
    wikrt_a const r = t->alloc;
    t->alloc += amt;
    return r;
}

// Function for large allocations outside the current thread.
bool wikrt_alloc(wikrt_cx*, wikrt_a*, wikrt_z amt);


#define WIKRT_H
#endif

