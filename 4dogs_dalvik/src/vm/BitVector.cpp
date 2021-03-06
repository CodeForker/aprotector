/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Implementation of an expandable bit vector.
 */

/*
 * 实现一个扩展位向量操作
 */
#include "Dalvik.h"

#include <stdlib.h>
#include <strings.h>

/* 4位增长 */
#define kBitVectorGrowth    4   /* increase by 4 u4s when limit hit */


/*
 * Allocate a bit vector with enough space to hold at least the specified
 * number of bits.
 */

/*
 *breif:分配一个足够空间的位向量容纳最少的指定的位数.
 *param[startBits]:扩展的起始地址.
 *param[expandable]:扩展标记.
*/
BitVector* dvmAllocBitVector(unsigned int startBits, bool expandable)
{
    BitVector* bv;
    unsigned int count;

    assert(sizeof(bv->storage[0]) == 4);        /* assuming 32-bit units */

    bv = (BitVector*) malloc(sizeof(BitVector));

    count = (startBits + 31) >> 5;

    bv->storageSize = count;
    bv->expandable = expandable;
    bv->storage = (u4*) calloc(count, sizeof(u4));
    return bv;
}

/*
 * Free a BitVector.
 */

/*
 *breif:释放位向量器.
 *param[pBits]:位向量指针.
*/
void dvmFreeBitVector(BitVector* pBits)
{
    if (pBits == NULL)
        return;

    free(pBits->storage);
    free(pBits);
}

/*
 * "Allocate" the first-available bit in the bitmap.
 *
 * This is not synchronized.  The caller is expected to hold some sort of
 * lock that prevents multiple threads from executing simultaneously in
 * dvmAllocBit/dvmFreeBit.
 */

/*
 *breif:位图中第一个可分配的位.若有扩展标记则分配扩展.
 *param[pBits]:位向量.
*/
int dvmAllocBit(BitVector* pBits)
{
    unsigned int word, bit;

retry:
    for (word = 0; word < pBits->storageSize; word++) {
        if (pBits->storage[word] != 0xffffffff) {
            /*
             * There are unallocated bits in this word.  Return the first.
             */
            bit = ffs(~(pBits->storage[word])) -1;
            assert(bit < 32);
            pBits->storage[word] |= 1 << bit;
            return (word << 5) | bit;
        }
    }

    /*
     * Ran out of space, allocate more if we're allowed to.
     */
    if (!pBits->expandable)
        return -1;

    pBits->storage = (u4*)realloc(pBits->storage,
                    (pBits->storageSize + kBitVectorGrowth) * sizeof(u4));
    memset(&pBits->storage[pBits->storageSize], 0x00,
        kBitVectorGrowth * sizeof(u4));
    pBits->storageSize += kBitVectorGrowth;
    goto retry;
}

/*
 * Mark the specified bit as "set".
 */

/*
 *breif:标记指定的位"set".
 *param[pBits]:位向量.
 *param[num]:指定的位.
*/
void dvmSetBit(BitVector* pBits, unsigned int num)
{
    if (num >= pBits->storageSize * sizeof(u4) * 8) {
        if (!pBits->expandable) {
            ALOGE("Attempt to set bit outside valid range (%d, limit is %d)",
                num, pBits->storageSize * sizeof(u4) * 8);
            dvmAbort();
        }

        /* Round up to word boundaries for "num+1" bits */
        unsigned int newSize = (num + 1 + 31) >> 5;
        assert(newSize > pBits->storageSize);
        pBits->storage = (u4*)realloc(pBits->storage, newSize * sizeof(u4));
        if (pBits->storage == NULL) {
            ALOGE("BitVector expansion to %d failed", newSize * sizeof(u4));
            dvmAbort();
        }
        memset(&pBits->storage[pBits->storageSize], 0x00,
            (newSize - pBits->storageSize) * sizeof(u4));
        pBits->storageSize = newSize;
    }

    pBits->storage[num >> 5] |= 1 << (num & 0x1f);
}

/*
 * Mark the specified bit as "clear".
 */

/*
 *breif:标记指定位"clear".
 *pBits:位向量.
 *num:指定位.
*/
void dvmClearBit(BitVector* pBits, unsigned int num)
{
    assert(num < pBits->storageSize * sizeof(u4) * 8);

    pBits->storage[num >> 5] &= ~(1 << (num & 0x1f));
}

/*
 * Mark all bits bit as "clear".
 */

/*
 *breif:清理所有位向量的标记.
 *param[pBits]:位向量.
*/
void dvmClearAllBits(BitVector* pBits)
{
    unsigned int count = pBits->storageSize;
    memset(pBits->storage, 0, count * sizeof(u4));
}

/*
 * Mark specified number of bits as "set". Cannot set all bits like ClearAll
 * since there might be unused bits - setting those to one will confuse the
 * iterator.
 */

/*
 *breif:标记指定位数为"set".
 *param[pBits]:位向量.
 *param[numBits]:位数.
*/
void dvmSetInitialBits(BitVector* pBits, unsigned int numBits)
{
    unsigned int idx;
    assert(((numBits + 31) >> 5) <= pBits->storageSize);
    for (idx = 0; idx < (numBits >> 5); idx++) {
        pBits->storage[idx] = -1;
    }
    unsigned int remNumBits = numBits & 0x1f;
    if (remNumBits) {
        pBits->storage[idx] = (1 << remNumBits) - 1;
    }
}

/*
 * Determine whether or not the specified bit is set.
 */

/*
 *breif:判断指定的位是否设置.
 *param[pBits]:位向量.
 *param[num]:指定的位.
*/
bool dvmIsBitSet(const BitVector* pBits, unsigned int num)
{
    assert(num < pBits->storageSize * sizeof(u4) * 8);

    unsigned int val = pBits->storage[num >> 5] & (1 << (num & 0x1f));
    return (val != 0);
}

/*
 * Count the number of bits that are set.
 */

/*
 *breif:计算标记的位数.
 *param[pBits]:位向量.
*/
int dvmCountSetBits(const BitVector* pBits)
{
    unsigned int word;
    unsigned int count = 0;

    for (word = 0; word < pBits->storageSize; word++) {
        u4 val = pBits->storage[word];

        if (val != 0) {
            if (val == 0xffffffff) {
                count += 32;
            } else {
                /* count the number of '1' bits */
                while (val != 0) {
                    val &= val - 1;
                    count++;
                }
            }
        }
    }

    return count;
}

/*
 * If the vector sizes don't match, log an error and abort.
 */

/*
 *breif:如果矢量的大小不匹配，记录错误并中止.
 *param[bv1]:位向量.
 *param[bv2]:位向量.
*/
static void checkSizes(const BitVector* bv1, const BitVector* bv2)
{
    if (bv1->storageSize != bv2->storageSize) {
        ALOGE("Mismatched vector sizes (%d, %d)",
            bv1->storageSize, bv2->storageSize);
        dvmAbort();
    }
}

/*
 * Copy a whole vector to the other. Only do that when the both vectors have
 * the same size.
 */

/*
 *breif:当位向量大小相同时，将源位向量拷贝到目的位向量.
 *param[dest]:目的位向量.
 *param[src]:源位向量.
*/
void dvmCopyBitVector(BitVector *dest, const BitVector *src)
{
    /* if dest is expandable and < src, we could expand dest to match */
    checkSizes(dest, src);

    memcpy(dest->storage, src->storage, sizeof(u4) * dest->storageSize);
}

/*
 * Intersect two bit vectors and store the result to the dest vector.
 */

/*
 *breif:将大小相同扩展标记相同的两个位向量与操作后存储到同样大小与标记目标位向量.
 *param[dest]:目的位向量.
 *param[src1]:源位向量.
 *param[src2]:源位向量.
*/
bool dvmIntersectBitVectors(BitVector *dest, const BitVector *src1,
                            const BitVector *src2)
{
    if (dest->storageSize != src1->storageSize ||
        dest->storageSize != src2->storageSize ||
        dest->expandable != src1->expandable ||
        dest->expandable != src2->expandable)
        return false;

    unsigned int idx;
    for (idx = 0; idx < dest->storageSize; idx++) {
        dest->storage[idx] = src1->storage[idx] & src2->storage[idx];
    }
    return true;
}

/*
 * Unify two bit vectors and store the result to the dest vector.
 */

/*                                                                              
 *breif:将大小相同扩展标记相同的两个位向量或操作后存储到同样大小与标记目标位向\\
量.                                                                             
*param[dest]:目的位向量.                                                       
*param[src1]:源位向量.                                                         
*param[src2]:源位向量.                                                         
*/

bool dvmUnifyBitVectors(BitVector *dest, const BitVector *src1,
                        const BitVector *src2)
{
    if (dest->storageSize != src1->storageSize ||
        dest->storageSize != src2->storageSize ||
        dest->expandable != src1->expandable ||
        dest->expandable != src2->expandable)
        return false;

    unsigned int idx;
    for (idx = 0; idx < dest->storageSize; idx++) {
        dest->storage[idx] = src1->storage[idx] | src2->storage[idx];
    }
    return true;
}

/*
 * Compare two bit vectors and return true if difference is seen.
 */

/*
 *breif:判断两个位向量，若true表示不相同.
 *param[src1]:位向量.
 *param[src2]:位向量.
*/
bool dvmCompareBitVectors(const BitVector *src1, const BitVector *src2)
{
    if (src1->storageSize != src2->storageSize ||
        src1->expandable != src2->expandable)
        return true;

    unsigned int idx;
    for (idx = 0; idx < src1->storageSize; idx++) {
        if (src1->storage[idx] != src2->storage[idx]) return true;
    }
    return false;
}

/* Initialize the iterator structure */

/*
 *breif:初始化迭代器.
 *param[pBits]:位向量.
 *param[iterator]:迭代器.
*/
void dvmBitVectorIteratorInit(BitVector* pBits, BitVectorIterator* iterator)
{
    iterator->pBits = pBits;
    iterator->bitSize = pBits->storageSize * sizeof(u4) * 8;
    iterator->idx = 0;
}

/* Return the next position set to 1. -1 means end-of-element reached */

/*
 *breif:返回迭代器中下一个设置的位向量.若位-1则表示结束位置.
 *param[iterator]:迭代器.
*/
int dvmBitVectorIteratorNext(BitVectorIterator* iterator)
{
    const BitVector* pBits = iterator->pBits;
    u4 bitIndex = iterator->idx;

    assert(iterator->bitSize == pBits->storageSize * sizeof(u4) * 8);
    if (bitIndex >= iterator->bitSize) return -1;

    for (; bitIndex < iterator->bitSize; bitIndex++) {
        unsigned int wordIndex = bitIndex >> 5;
        unsigned int mask = 1 << (bitIndex & 0x1f);
        if (pBits->storage[wordIndex] & mask) {
            iterator->idx = bitIndex+1;
            return bitIndex;
        }
    }
    /* No more set bits */
    return -1;
}


/*
 * Merge the contents of "src" into "dst", checking to see if this causes
 * any changes to occur.  This is a logical OR.
 *
 * Returns "true" if the contents of the destination vector were modified.
 */

/*
 *breif:检查两个位向量变化，若有变化则返回true.
 *param[dst]:目标位向量.
 *param[src]:源位向量.
*/
bool dvmCheckMergeBitVectors(BitVector* dst, const BitVector* src)
{
    bool changed = false;

    checkSizes(dst, src);

    unsigned int idx;
    for (idx = 0; idx < dst->storageSize; idx++) {
        u4 merged = src->storage[idx] | dst->storage[idx];
        if (dst->storage[idx] != merged) {
            dst->storage[idx] = merged;
            changed = true;
        }
    }

    return changed;
}
