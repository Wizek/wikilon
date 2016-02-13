
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <assert.h>

#include "wikrt.h"

void wikrt_cx_resetmem(wikrt_cx*); 

wikrt_err wikrt_env_create(wikrt_env** ppEnv, char const* dirPath, uint32_t dbMaxMB) {
    wikrt_env* const e = calloc(1, sizeof(wikrt_env));
    if(NULL == e) return WIKRT_NOMEM;

    e->mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;

    // use of key-value database and stowage is optional
    if((NULL == dirPath) || (0 == dbMaxMB)) { 
        e->db_env = NULL;
    } else if(!wikrt_db_init(e, dirPath, dbMaxMB)) {
        free(e);
        return WIKRT_DBERR;
    } 

    // maybe create thread pool or task list, etc.?

    (*ppEnv) = e;
    return WIKRT_OK;
}

void wikrt_env_destroy(wikrt_env* e) {
    assert(NULL == e->cxhd);
    wikrt_db_destroy(e);
    pthread_mutex_destroy(&(e->mutex));
    free(e);
}

void wikrt_env_lock(wikrt_env* e) {
    pthread_mutex_lock(&(e->mutex));
}

void wikrt_env_unlock(wikrt_env* e) {
    pthread_mutex_unlock(&(e->mutex));
}

// trivial implementation 
void wikrt_env_sync(wikrt_env* e) {
    if(e->db_enable) {
        int const force_flush = 1;
        mdb_env_sync(e->db_env, force_flush);
    }
}

size_t cx_size_bytes(uint32_t sizeMB) { 
    return ((size_t) sizeMB) * (1024 * 1024); 
}

wikrt_err wikrt_cx_create(wikrt_env* e, wikrt_cx** ppCX, uint32_t sizeMB) {
    bool const bSizeValid = (WIKRT_CX_SIZE_MIN <= sizeMB) 
                         && (sizeMB <= WIKRT_CX_SIZE_MAX);
    if(!bSizeValid) return WIKRT_INVAL;
    size_t const sizeBytes = cx_size_bytes(sizeMB);

    wikrt_cx* const cx = calloc(1,sizeof(wikrt_cx));
    if(NULL == cx) return WIKRT_NOMEM;


    static int const prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    static int const flags = MAP_ANONYMOUS | MAP_PRIVATE;

    errno = 0;
    void* const pMem = mmap(NULL, sizeBytes, prot, flags, -1, 0); 
    if(NULL == pMem) {
        free(cx);
        return WIKRT_NOMEM;
    }

    cx->env    = e;
    cx->sizeMB = sizeMB;
    cx->memory = (wikrt_val*) pMem;
    //cx->mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;

    // set initial memory before adding context to global list
    // (e.g. to ensure empty stowage lists)
    wikrt_cx_resetmem(cx);

    // add to global context list
    wikrt_env_lock(e); {
        wikrt_cx* const hd = e->cxhd;
        cx->next = hd;
        cx->prev = NULL;
        if(NULL != hd) { hd->prev = cx; }
        e->cxhd = cx;
    } wikrt_env_unlock(e);
    
    (*ppCX) = cx;
    return WIKRT_OK;
}

void wikrt_cx_destroy(wikrt_cx* cx) {
    wikrt_env* const e = cx->env;

    // remove context from global context list
    wikrt_env_lock(e); {
        if(NULL != cx->next) { 
            cx->next->prev = cx->prev; 
        }
        if(NULL != cx->prev) { 
            cx->prev->next = cx->next; 
        } else { 
            assert(e->cxhd == cx);
            e->cxhd = cx->next; 
        }
    } wikrt_env_unlock(e);

    // free memory associated with the context
    size_t const sizeBytes = cx_size_bytes(cx->sizeMB);
    bool const context_unmapped = (0 == munmap(cx->memory, sizeBytes));
    assert(context_unmapped);
    free(cx);
}

wikrt_env* wikrt_cx_env(wikrt_cx* cx) {
    return cx->env;
}

void wikrt_cx_reset(wikrt_cx* cx) {
    // At the moment, contexts don't have any external metadata.
    // I'd prefer to keep it that way, if feasible. Anyhow, this
    // means a reset is a trivial update to a context's internal
    // memory. 
    wikrt_env_lock(cx->env); {
        wikrt_cx_resetmem(cx);
    } wikrt_env_unlock(cx->env);
}

void wikrt_cx_resetmem(wikrt_cx* cx) {
    wikrt_cx_hdr* hdr = (wikrt_cx_hdr*) cx->memory;
    (*hdr) = (wikrt_cx_hdr){ 0 }; // clear root memory

    wikrt_addr const hdrEnd = (wikrt_val) WIKRT_PAGEBUFF(sizeof(wikrt_cx_hdr));
    wikrt_val const szRem = (wikrt_val) cx_size_bytes(cx->sizeMB) - hdrEnd;
    #undef HDRSZ

    // we'll simply 'free' our chunk of non-header memory.
    wikrt_free(cx, &(hdr->flmain), hdrEnd, szRem);
}

char const* wikrt_abcd_operators() {
    // currently just pure ABC...
    return u8"lrwzvcLRWZVC%^ \n$o'kf#1234567890+*-QG?DFMK";
}

char const* wikrt_abcd_expansion(uint32_t opcode) { switch(opcode) {
    case ABC_PROD_ASSOCL: return "l";
    case ABC_PROD_ASSOCR: return "r";
    case ABC_PROD_W_SWAP: return "w";
    case ABC_PROD_Z_SWAP: return "z";
    case ABC_PROD_INTRO1: return "v";
    case ABC_PROD_ELIM1:  return "c";
    case ABC_SUM_ASSOCL:  return "L";
    case ABC_SUM_ASSOCR:  return "R";
    case ABC_SUM_W_SWAP:  return "W";
    case ABC_SUM_Z_SWAP:  return "Z";
    case ABC_SUM_INTRO0:  return "V";
    case ABC_SUM_ELIM0:   return "C";
    case ABC_COPY:        return "^";
    case ABC_DROP:        return "%";
    case ABC_SP:          return " ";
    case ABC_LF:          return "\n";
    case ABC_APPLY:       return "$";
    case ABC_COMPOSE:     return "o";
    case ABC_QUOTE:       return "'";
    case ABC_REL:         return "k";
    case ABC_AFF:         return "f";
    case ABC_INEW:        return "#";
    case ABC_ID1:         return "1";
    case ABC_ID2:         return "2";
    case ABC_ID3:         return "3";
    case ABC_ID4:         return "4";
    case ABC_ID5:         return "5";
    case ABC_ID6:         return "6";
    case ABC_ID7:         return "7";
    case ABC_ID8:         return "8";
    case ABC_ID9:         return "9";
    case ABC_ID0:         return "0";
    case ABC_IADD:        return "+";
    case ABC_IMUL:        return "*";
    case ABC_INEG:        return "-";
    case ABC_IDIV:        return "Q";
    case ABC_IGT:         return "G";
    case ABC_CONDAP:      return "?";
    case ABC_DISTRIB:     return "D";
    case ABC_FACTOR:      return "F";
    case ABC_MERGE:       return "M";
    case ABC_ASSERT:      return "K";
    default: return NULL;
}}

char const* wikrt_strerr(wikrt_err e) { switch(e) {
    case WIKRT_OK:              return "no error";
    case WIKRT_INVAL:           return "invalid parameters, programmer error";
    case WIKRT_IMPL:            return "reached limit of current implementation";
    case WIKRT_DBERR:           return "filesystem or database layer error";
    case WIKRT_NOMEM:           return "out of memory (malloc or mmap failure)";
    case WIKRT_CXFULL:          return "context full, size quota reached";
    case WIKRT_BUFFSZ:          return "target buffer too small";
    case WIKRT_TXN_CONFLICT:    return "transaction conflict";
    case WIKRT_QUOTA_STOP:      return "evaluation effort quota reached";
    case WIKRT_TYPE_ERROR:      return "type mismatch";
    default:                    return "unrecognized error code";
}}

wikrt_err wikrt_peek_type(wikrt_cx* cx, wikrt_vtype* out, wikrt_val const v)
{
    if(wikrt_i(v)) { 
        (*out) = WIKRT_VTYPE_INTEGER; 
    } else {
        wikrt_tag const vtag = wikrt_vtag(v);
        wikrt_addr const vaddr = wikrt_vaddr(v);
        if(WIKRT_P == vtag) {
            if(0 == vaddr) { (*out) = WIKRT_VTYPE_UNIT; }
            else { (*out) = WIKRT_VTYPE_PRODUCT; }
        } else if((WIKRT_PL == vtag) || (WIKRT_PR == vtag)) {
            (*out) = WIKRT_VTYPE_SUM;
        } else if((WIKRT_O == vtag) && (0 != vaddr)) {
            wikrt_val const* const pv = wikrt_pval(cx, vaddr);
            wikrt_val const otag = pv[0];
            if(wikrt_otag_bigint(otag)) { (*out) = WIKRT_VTYPE_INTEGER; }
            else if(wikrt_otag_deepsum(otag) || wikrt_otag_array(otag)) { (*out) = WIKRT_VTYPE_SUM; }
            else if(wikrt_otag_block(otag)) { (*out) = WIKRT_VTYPE_BLOCK; }
            else if(wikrt_otag_stowage(otag)) { (*out) = WIKRT_VTYPE_STOWED; }
            else { return WIKRT_INVAL; }
        } else { return WIKRT_INVAL; }
    }
    return WIKRT_OK;
}

// assumes normal form utf-8 argument, NUL-terminated
bool wikrt_valid_token(char const* s) {
    // valid size is 1..63 bytes
    size_t len = strlen(s);
    bool const bValidSize = (0 < len) && (len < 64);
    if(!bValidSize) return false;

    uint32_t cp;
    while(len != 0) {
        if(!utf8_step(&s,&len,&cp) || !wikrt_token_char(cp))
            return false;
    }
    return true;
}

wikrt_err wikrt_alloc_text(wikrt_cx* cx, wikrt_val* v, char const* s) { 
    return wikrt_alloc_text_fl(cx, wikrt_flmain(cx), v, s);
}

/* Currently allocating as a normal list. This means we allocate one
 * full cell (WIKRT_CELLSIZE) per character, usually an 8x increase.
 * Yikes! But I plan to later tune this to a dedicated structure.
 */
wikrt_err wikrt_alloc_text_fl(wikrt_cx* const cx, wikrt_fl* const fl, wikrt_val* const txt, char const* s) {
    wikrt_err r = WIKRT_OK;
    size_t len = strlen(s);
    wikrt_val hd;
    wikrt_addr* tl = &hd;
    uint32_t cp;
    while((len != 0) && utf8_step(&s, &len, &cp)) {
        if(!wikrt_text_char(cp)) { 
            r = WIKRT_INVAL; 
            goto e;
        }
        wikrt_addr dst;
        if(!wikrt_alloc(cx, fl, &dst, WIKRT_CELLSIZE)) {
            r = WIKRT_CXFULL;
            goto e;
        }
        (*tl) = wikrt_tag_addr(WIKRT_PL, dst);
        wikrt_val* pv = wikrt_pval(cx, dst);
        pv[0] = wikrt_i2v(cp);
        tl = (pv + 1);
    }
    (*tl) = WIKRT_UNIT_INR;
    (*txt) = hd;
    return WIKRT_OK;
e: // error; need to free allocated data
    (*tl) = WIKRT_UNIT_INR;
    wikrt_drop_fl(cx, fl, hd, true);
    (*txt) = WIKRT_UNIT_INR;
    return r;
}

wikrt_err wikrt_alloc_i32(wikrt_cx* cx, wikrt_val* v, int32_t n) {
    return wikrt_alloc_i32_fl(cx, wikrt_flmain(cx), v, n);
}

wikrt_err wikrt_alloc_bigint(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* v, bool sign, uint32_t* digit, wikrt_size n) 
{
    if((n < 2) || (0 == digit[n-1])) {
        // highest digit must be non-zero!
        return WIKRT_INVAL;
    }

    if(n > WIKRT_BIGINT_MAX_DIGITS) {
        // reached limits of implementation
        return WIKRT_IMPL; 
    }

    wikrt_size const szBytes = sizeof(wikrt_val) 
                             + (n * sizeof(uint32_t));

    wikrt_addr dst;
    if(!wikrt_alloc(cx, fl, &dst, szBytes)) {
        return WIKRT_CXFULL;
    }
    (*v) = wikrt_tag_addr(WIKRT_O, dst);
    wikrt_val* const pv = wikrt_pval(cx, dst);
    pv[0] = (((n << 1) | (sign ? 1 : 0)) << 8) | WIKRT_OTAG_BIGINT;
    uint32_t* const d = (uint32_t*) (pv + 1);
    for(wikrt_size ix = 0; ix < n; ++ix) {
        d[ix] = digit[ix];
    }
    return WIKRT_OK;
}

wikrt_err wikrt_alloc_i32_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* v, int32_t n) 
{
    bool const isSmallInt = ((WIKRT_SMALLINT_MIN <= n) && (n <= WIKRT_SMALLINT_MAX));
    if(isSmallInt) { 
        (*v) = wikrt_i2v(n); 
        return WIKRT_OK; 
    }

    bool const sign = (n < 0);
    if(sign) { n = -n; }
    uint32_t d[2];
    d[0] = (uint32_t) (n % WIKRT_BIGINT_DIGIT);
    d[1] = (uint32_t) (n / WIKRT_BIGINT_DIGIT);
    return wikrt_alloc_bigint(cx, fl, v, sign, d, 2);
}

wikrt_err wikrt_peek_i32(wikrt_cx* cx, wikrt_val const v, int32_t* i32) 
{
    // small integers (normal case)
    if(wikrt_i(v)) {
        (*i32) = wikrt_v2i(v);
        return WIKRT_OK;
    }

    // TODO: big integers, overflow calculations.
    return WIKRT_IMPL;
}

wikrt_err wikrt_alloc_i64(wikrt_cx* cx, wikrt_val* v, int64_t n) {
    return wikrt_alloc_i64_fl(cx, wikrt_flmain(cx), v, n);
}

wikrt_err wikrt_alloc_i64_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* v, int64_t n)
{
    bool const isSmallInt = ((WIKRT_SMALLINT_MIN <= n) && (n <= WIKRT_SMALLINT_MAX));
    if(isSmallInt) {
        (*v) = wikrt_i2v((int32_t)n);
        return WIKRT_OK;
    }
    
    bool const sign = (n < 0);
    if(sign) { n = - n; }
    uint32_t d[3]; // ~90 bits
    
    d[0] = (uint32_t) (n % WIKRT_BIGINT_DIGIT);
    n /= WIKRT_BIGINT_DIGIT;
    d[1] = (uint32_t) (n % WIKRT_BIGINT_DIGIT);
    d[2] = (uint32_t) (n / WIKRT_BIGINT_DIGIT);
    wikrt_size const nDigits = (0 == d[2]) ? 3 : 2;
    return wikrt_alloc_bigint(cx, fl, v, sign, d, nDigits);
}

wikrt_err wikrt_peek_i64(wikrt_cx* cx, wikrt_val const v, int64_t* i64) 
{
    if(wikrt_i(v)) {
        (*i64) = (int64_t) wikrt_v2i(v);
        return WIKRT_OK;
    }

    // TODO: big integers, simple overflow calculations.
    //  this will wait until after spike solution.
    return WIKRT_IMPL;
}

wikrt_err wikrt_alloc_prod(wikrt_cx* cx, wikrt_val* p, wikrt_val fst, wikrt_val snd) {
    return wikrt_alloc_prod_fl(cx, wikrt_flmain(cx), p, fst, snd);
}

wikrt_err wikrt_alloc_prod_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* p, wikrt_val fst, wikrt_val snd) 
{
    wikrt_addr dst;
    if(!wikrt_alloc(cx, fl, &dst, WIKRT_CELLSIZE)) {
        return WIKRT_CXFULL;
    }
    (*p) = wikrt_tag_addr(WIKRT_P, dst);
    wikrt_val* const pv = wikrt_pval(cx, dst);
    pv[0] = fst;
    pv[1] = snd;
    return WIKRT_OK;
}

wikrt_err wikrt_split_prod(wikrt_cx* cx, wikrt_val p, wikrt_val* fst, wikrt_val* snd) {
    return wikrt_split_prod_fl(cx, wikrt_flmain(cx), p, fst, snd);
}

wikrt_err wikrt_split_prod_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val p, wikrt_val* fst, wikrt_val* snd) 
{
    wikrt_tag const ptag = wikrt_vtag(p);
    wikrt_addr const paddr = wikrt_vaddr(p);
    wikrt_val* const pv = wikrt_pval(cx, paddr);

    if((WIKRT_P == ptag) && (0 != paddr)) {
        (*fst) = pv[0];
        (*snd) = pv[1];
        wikrt_free(cx, fl, paddr, WIKRT_CELLSIZE);
        return WIKRT_OK;
    } else {
        return WIKRT_TYPE_ERROR;
    }
}


wikrt_err wikrt_alloc_sum(wikrt_cx* cx, wikrt_val* c, bool inRight, wikrt_val v) {
    return wikrt_alloc_sum_fl(cx, wikrt_flmain(cx), c, inRight, v);
}

wikrt_err wikrt_alloc_sum_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* c, bool inRight, wikrt_val const v)
{
    wikrt_tag const vtag = wikrt_vtag(v);
    wikrt_addr const vaddr = wikrt_vaddr(v);
    wikrt_val* const pv = wikrt_pval(cx, vaddr);
    if(WIKRT_P == vtag) {
        // shallow sum on product, pointer manipulation, no allocation
        wikrt_tag const newtag = inRight ? WIKRT_PR : WIKRT_PL;
        (*c) = wikrt_tag_addr(newtag, vaddr);
        return WIKRT_OK;
    } else if((WIKRT_O == vtag) && wikrt_otag_deepsum(*pv) && ((*pv) < (1 << 30))) {
        // deepsum has space if bits 30 and 31 are available, i.e. if tag less than (1 << 30).
        // In this case, no allocation is required. We can update the existing deep sum in place.
        wikrt_val const sumtag = ((*pv) >> 6) | (inRight ? WIKRT_DEEPSUMR : WIKRT_DEEPSUML);
        wikrt_val const otag = (sumtag << 8) | WIKRT_OTAG_DEEPSUM;
        (*pv) = otag;
        return WIKRT_OK;
    } else { // allocate deep sum
        wikrt_addr dst;
        if(!wikrt_alloc(cx, fl, &dst, WIKRT_CELLSIZE)) {
            return WIKRT_CXFULL;
        }
        wikrt_val const sumtag = (inRight ? WIKRT_DEEPSUMR : WIKRT_DEEPSUML);
        wikrt_val const otag = (sumtag << 8) | WIKRT_OTAG_DEEPSUM;
        wikrt_val* const pv = wikrt_pval(cx, dst);
        pv[0] = otag;
        pv[1] = v;
        (*c) = wikrt_tag_addr(WIKRT_O, dst);
        return WIKRT_OK;
    }
}

wikrt_err wikrt_split_sum(wikrt_cx* cx, wikrt_val c, bool* inRight, wikrt_val* v) {
    return wikrt_split_sum_fl(cx, wikrt_flmain(cx), c, inRight, v);
}

wikrt_err wikrt_split_sum_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val c, bool* inRight, wikrt_val* v)
{
    wikrt_tag const tag = wikrt_vtag(c);
    wikrt_addr const addr = wikrt_vaddr(c);
    if(WIKRT_PL == tag) {
        (*inRight) = false;
        (*v) = wikrt_tag_addr(WIKRT_P, addr);
        return WIKRT_OK;
    } else if(WIKRT_PR == tag) {
        (*inRight) = true;
        (*v) = wikrt_tag_addr(WIKRT_P, addr);
        return WIKRT_OK;
    } else if(WIKRT_O == tag) {
        wikrt_val* const pv = wikrt_pval(cx, addr);
        wikrt_val const otag = pv[0];
        if(wikrt_otag_deepsum(otag)) {
            wikrt_val const s0 = (otag >> 8);
            (*inRight) = (3 == (3 & s0));
            wikrt_val const sf = (s0 >> 2);
            if(0 == sf) { // dealloc deepsum
                (*v) = pv[1];
                wikrt_free(cx, fl, addr, WIKRT_CELLSIZE);
            } else { // keep value, reduce one level 
                (*v) = c;
                pv[0] = (sf << 8) | WIKRT_OTAG_DEEPSUM;
            }
            return WIKRT_OK;
        } else if(wikrt_otag_array(otag)) {
            // TODO: pop one value from array, alloc pair?
            //  that would probably work for now...
            return WIKRT_IMPL;
        } else { return WIKRT_TYPE_ERROR; }
    } else { return WIKRT_TYPE_ERROR; }
}

wikrt_err wikrt_alloc_block(wikrt_cx* cx, wikrt_val* v, char const* abc, wikrt_abc_opts opts) {
    return wikrt_alloc_block_fl(cx, wikrt_flmain(cx), v, abc, opts);
}



wikrt_err wikrt_alloc_block_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* v, char const* abc, wikrt_abc_opts opts) 
{
    // TODO: represent block of code
    return WIKRT_IMPL;
}


wikrt_err wikrt_alloc_binary(wikrt_cx* cx, wikrt_val* v, uint8_t const* buff, size_t elems) {
    return wikrt_alloc_binary_fl(cx, wikrt_flmain(cx), v, buff, elems);
}

/* For the moment, we'll allocate a binary as a plain old list.
 */
wikrt_err wikrt_alloc_binary_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* v, uint8_t const* buff, size_t nElems)
{
    // TODO: allocate a binary in the context    
    return WIKRT_IMPL;
}

wikrt_err wikrt_alloc_seal_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* sv, char const* s, wikrt_val v)
{
    return WIKRT_IMPL;
}


wikrt_err wikrt_copy(wikrt_cx* cx, wikrt_val* copy, wikrt_val const src, bool bCopyAff) {
    return wikrt_copy_fl(cx, wikrt_flmain(cx), copy, src, bCopyAff);
}

/** deep copy a structure
 *
 * It will be important to control how much space is used when copying,
 * i.e. to avoid busting the thread stack. I might need to model the
 * copy stack within the context itself, albeit with reasonably large
 * blocks to reduce fragmentation.
 */
wikrt_err wikrt_copy_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val* copy, wikrt_val const src, bool bCopyAff)
{
    // TODO
    return WIKRT_IMPL;
}


wikrt_err wikrt_drop(wikrt_cx* cx, wikrt_val v, bool bDropRel) {
    return wikrt_drop_fl(cx, wikrt_flmain(cx), v, bDropRel);
}

/** delete a large structure
 *
 * Similar to 'copy', I need some way to track progress for deletion of 
 * deep structures in constant extra space. 
 */
wikrt_err wikrt_drop_fl(wikrt_cx*, wikrt_fl*, wikrt_val, bool bDropRel);
wikrt_err wikrt_stow_fl(wikrt_cx*, wikrt_fl*, wikrt_val* out, wikrt_val);

wikrt_err wikrt_read(wikrt_cx* cx, wikrt_val binary, size_t buffSize, 
    size_t* bytesRead, uint8_t* buffer, wikrt_val* remainder) 
{
    return wikrt_read_fl(cx, wikrt_flmain(cx), binary, buffSize, bytesRead, buffer, remainder);
}

wikrt_err wikrt_read_fl(wikrt_cx* cx, wikrt_fl* fl, wikrt_val binary, size_t buffSize, 
    size_t* bytesRead, uint8_t* buffer, wikrt_val* remainder)
{
    return WIKRT_IMPL;
}




