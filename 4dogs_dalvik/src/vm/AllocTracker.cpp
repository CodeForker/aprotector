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
 * Allocation tracking and reporting.  We maintain a circular buffer with
 * the most recent allocations.  The data can be viewed through DDMS.
 *
 * There are two basic approaches: manage the buffer with atomic updates
 * and do a system-wide suspend when DDMS requests it, or protect all
 * accesses with a mutex.  The former is potentially more efficient, but
 * the latter is much simpler and more reliable.
 *
 * Ideally we'd just use the object heap allocation mutex to guard this
 * structure, but at the point we grab that (under dvmMalloc()) we're just
 * allocating a collection of bytes and no longer have the class reference.
 * Because this is an optional feature it's best to leave the existing
 * code undisturbed and just use an additional lock.
 *
 * We don't currently track allocations of class objects.  We could, but
 * with the possible exception of Proxy objects they're not that interesting.
 *
 * TODO: if we add support for class unloading, we need to add the class
 * references here to the root set (or just disable class unloading while
 * this is active).
 *
 * TODO: consider making the parameters configurable, so DDMS can decide
 * how many allocations it wants to see and what the stack depth should be.
 * Changing the window size is easy, changing the max stack depth is harder
 * because we go from an array of fixed-size structs to variable-sized data.
 */
#include "Dalvik.h"

#define kMaxAllocRecordStackDepth   16      /* max 255 */
#define kNumAllocRecords            512     /* MUST be power of 2 */

/*
 * Record the details of an allocation.
 */

/*
 *breif:记录分配内容.包括类内存块，要求的总大小，可回收的简单线程id，函数栈信息（正在执行的方法,当前执行的偏移，以16字节为单位）.
*/
struct AllocRecord {
    ClassObject*    clazz;      /* class allocated in this block */
    u4              size;       /* total size requested */
    u2              threadId;   /* simple thread ID; could be recycled */

    /* stack trace elements; unused entries have method==NULL */
    struct {
        const Method* method;   /* which method we're executing in */
        int         pc;         /* current execution offset, in 16-bit units */
    } stackElem[kMaxAllocRecordStackDepth];

    /*
     * This was going to be either wall-clock time in seconds or monotonic
     * time in milliseconds since the VM started, to give a rough sense for
     * how long ago an allocation happened.  This adds a system call per
     * allocation, which is too much overhead.
     */
    //u4      timestamp;
};

/*
 * Initialize a few things.  This gets called early, so keep activity to
 * a minimum.
 */

/*
 *breif:初始化操作，实际是初始化allocTracker的互斥.
*/
bool dvmAllocTrackerStartup()
{
    /* prep locks */
    dvmInitMutex(&gDvm.allocTrackerLock);

    /* initialized when enabled by DDMS */
    assert(gDvm.allocRecords == NULL);

    return true;
}

/*
 * Release anything we're holding on to.
 */

/*
 *breif:清理操作，释放互斥.
*/
void dvmAllocTrackerShutdown()
{
    free(gDvm.allocRecords);
    dvmDestroyMutex(&gDvm.allocTrackerLock);
}


/*
 * ===========================================================================
 *      Collection
 * ===========================================================================
 */

/*
 * Enable allocation tracking.  Does nothing if tracking is already enabled.
 *
 * Returns "true" on success.
 */

/*
 *breif:开启内存分配跟踪器.实际是为gDvm的allocrecords分配一块内存.
*/
bool dvmEnableAllocTracker()
{
    bool result = true;
    dvmLockMutex(&gDvm.allocTrackerLock);

    if (gDvm.allocRecords == NULL) {
        ALOGI("Enabling alloc tracker (%d entries, %d frames --> %d bytes)",
            kNumAllocRecords, kMaxAllocRecordStackDepth,
            sizeof(AllocRecord) * kNumAllocRecords);
        gDvm.allocRecordHead = gDvm.allocRecordCount = 0;
        gDvm.allocRecords =
            (AllocRecord*) malloc(sizeof(AllocRecord) * kNumAllocRecords);

        if (gDvm.allocRecords == NULL)
            result = false;
    }

    dvmUnlockMutex(&gDvm.allocTrackerLock);
    return result;
}

/*
 * Disable allocation tracking.  Does nothing if tracking is not enabled.
 */

/*
 *breif:禁用内存分配跟踪器.实际释放gDvm的allocRecords.
*/
void dvmDisableAllocTracker()
{
    dvmLockMutex(&gDvm.allocTrackerLock);

    if (gDvm.allocRecords != NULL) {
        free(gDvm.allocRecords);
        gDvm.allocRecords = NULL;
    }

    dvmUnlockMutex(&gDvm.allocTrackerLock);
}

/*
 * Get the last few stack frames.
 */

/*
 *breif:获取函数信息栈的信息.
*/
static void getStackFrames(Thread* self, AllocRecord* pRec)
{
    int stackDepth = 0;
    void* fp;

    fp = self->interpSave.curFrame;

    while ((fp != NULL) && (stackDepth < kMaxAllocRecordStackDepth)) {
        const StackSaveArea* saveArea = SAVEAREA_FROM_FP(fp);
        const Method* method = saveArea->method;

        if (!dvmIsBreakFrame((u4*) fp)) {
            pRec->stackElem[stackDepth].method = method;
            if (dvmIsNativeMethod(method)) {
                pRec->stackElem[stackDepth].pc = 0;
            } else {
                assert(saveArea->xtra.currentPc >= method->insns &&
                        saveArea->xtra.currentPc <
                        method->insns + dvmGetMethodInsnsSize(method));
                pRec->stackElem[stackDepth].pc =
                    (int) (saveArea->xtra.currentPc - method->insns);
            }
            stackDepth++;
        }

        assert(fp != saveArea->prevFrame);
        fp = saveArea->prevFrame;
    }

    /* clear out the rest (normally there won't be any) */
    while (stackDepth < kMaxAllocRecordStackDepth) {
        pRec->stackElem[stackDepth].method = NULL;
        pRec->stackElem[stackDepth].pc = 0;
        stackDepth++;
    }
}

/*
 * Add a new allocation to the set.
 *
 */

/*
 *breif:向gDvm.allocRecords添加新的allocrecord并且将分配信息初始化.
 *param[clazz]:类对象.
 *param[size]:需要的大小.
*/
void dvmDoTrackAllocation(ClassObject* clazz, size_t size)
{
    Thread* self = dvmThreadSelf();
    if (self == NULL) {
        ALOGW("alloc tracker: no thread");
        return;
    }

    dvmLockMutex(&gDvm.allocTrackerLock);
    if (gDvm.allocRecords == NULL) {
        dvmUnlockMutex(&gDvm.allocTrackerLock);
        return;
    }

    /* advance and clip */
    if (++gDvm.allocRecordHead == kNumAllocRecords)
        gDvm.allocRecordHead = 0;

    AllocRecord* pRec = &gDvm.allocRecords[gDvm.allocRecordHead];

    pRec->clazz = clazz;
    pRec->size = size;
    pRec->threadId = self->threadId;
    getStackFrames(self, pRec);

    if (gDvm.allocRecordCount < kNumAllocRecords)
        gDvm.allocRecordCount++;

    dvmUnlockMutex(&gDvm.allocTrackerLock);
}


/*
 * ===========================================================================
 *      Reporting
 * ===========================================================================
 */

/*
The data we send to DDMS contains everything we have recorded.

Message header (all values big-endian):
  (1b) message header len (to allow future expansion); includes itself
  (1b) entry header len
  (1b) stack frame len
  (2b) number of entries
  (4b) offset to string table from start of message
  (2b) number of class name strings
  (2b) number of method name strings
  (2b) number of source file name strings
  For each entry:
    (4b) total allocation size
    (2b) threadId
    (2b) allocated object's class name index
    (1b) stack depth
    For each stack frame:
      (2b) method's class name
      (2b) method name
      (2b) method source file
      (2b) line number, clipped to 32767; -2 if native; -1 if no source
  (xb) class name strings
  (xb) method name strings
  (xb) source file strings

  As with other DDM traffic, strings are sent as a 4-byte length
  followed by UTF-16 data.

We send up 16-bit unsigned indexes into string tables.  In theory there
can be (kMaxAllocRecordStackDepth * kNumAllocRecords) unique strings in
each table, but in practice there should be far fewer.

The chief reason for using a string table here is to keep the size of
the DDMS message to a minimum.  This is partly to make the protocol
efficient, but also because we have to form the whole thing up all at
once in a memory buffer.

We use separate string tables for class names, method names, and source
files to keep the indexes small.  There will generally be no overlap
between the contents of these tables.
*/

/*
我们在字符串表存储16位无符号索引,从理论上可以将字符串分开存储在每个表(kMaxAllocRecordStackDepth * kNumAllocRecords)，实际上用到的少的多.
在这里使用字符串表主要原因是为了保持ddms的消息长度最小.
我们使用单独的字符串表保存类名，方法名，源文件.通常在这些表之间内容不重叠.
*/

const int kMessageHeaderLen = 15;
const int kEntryHeaderLen = 9;
const int kStackFrameLen = 8;

/*
 * Return the index of the head element.
 *
 * We point at the most-recently-written record, so if allocRecordCount is 1
 * we want to use the current element.  Take "head+1" and subtract count
 * from it.
 *
 * We need to handle underflow in our circular buffer, so we add
 * kNumAllocRecords and then mask it back down.
 */

/*
 *breif:返回头元素的索引.头元素指向最近写入的记录，所以如果allocRecordCount是1,要使用当前元素，则需要一系列运算来计算.我们添加kNumAllocRecords作为掩码用来处理循环缓冲区的溢出.
*/
inline static int headIndex()
{
    return (gDvm.allocRecordHead+1 + kNumAllocRecords - gDvm.allocRecordCount)
        & (kNumAllocRecords-1);
}

/*
 * Dump the contents of a PointerSet full of character pointers.
 */

/*
 *breif:一个PointerSet的字符指针的内容转储.
 *param[strings]:PointerSet类型的字符串.
*/
static void dumpStringTable(PointerSet* strings)
{
    int count = dvmPointerSetGetCount(strings);
    int i;

    for (i = 0; i < count; i++)
        printf("  %s\n", (const char*) dvmPointerSetGetEntry(strings, i));
}

/*
 * Get the method's source file.  If we don't know it, return "" instead
 * of a NULL pointer.
 */

/*
 *breif:返回方法所在类的源文件.
 *param[method]:方法.
*/
static const char* getMethodSourceFile(const Method* method)
{
    const char* fileName = dvmGetMethodSourceFile(method);
    if (fileName == NULL)
        fileName = "";
    return fileName;
}

/*
 * Generate string tables.
 *
 * Our source material is UTF-8 string constants from DEX files.  If we
 * want to be thorough we can generate a hash value for each string and
 * use the VM hash table implementation, or we can do a quick & dirty job
 * by just maintaining a list of unique pointers.  If the same string
 * constant appears in multiple DEX files we'll end up with duplicates,
 * but in practice this shouldn't matter (and if it does, we can uniq-sort
 * the result in a second pass).
 */

/*
 *breif:将allocrecords信息存储到PointerSet指针集合里.
 *param[classNames]:存储类信息的集合.
 *param[methodNames]:存储方法名的集合.
 *param[fileNames]:存储文件的集合.
*/
static bool populateStringTables(PointerSet* classNames,
    PointerSet* methodNames, PointerSet* fileNames)
{
    int count = gDvm.allocRecordCount;
    int idx = headIndex();
    int classCount, methodCount, fileCount;         /* debug stats */

    classCount = methodCount = fileCount = 0;

    while (count--) {
        AllocRecord* pRec = &gDvm.allocRecords[idx];

        dvmPointerSetAddEntry(classNames, pRec->clazz->descriptor);
        classCount++;

        int i;
        for (i = 0; i < kMaxAllocRecordStackDepth; i++) {
            if (pRec->stackElem[i].method == NULL)
                break;

            const Method* method = pRec->stackElem[i].method;
            dvmPointerSetAddEntry(classNames, method->clazz->descriptor);
            classCount++;
            dvmPointerSetAddEntry(methodNames, method->name);
            methodCount++;
            dvmPointerSetAddEntry(fileNames, getMethodSourceFile(method));
            fileCount++;
        }

        idx = (idx + 1) & (kNumAllocRecords-1);
    }

    ALOGI("class %d/%d, method %d/%d, file %d/%d",
        dvmPointerSetGetCount(classNames), classCount,
        dvmPointerSetGetCount(methodNames), methodCount,
        dvmPointerSetGetCount(fileNames), fileCount);

    return true;
}

/*
 * Generate the base info (i.e. everything but the string tables).
 *
 * This should be called twice.  On the first call, "ptr" is NULL and
 * "baseLen" is zero.  The return value is used to allocate a buffer.
 * On the second call, "ptr" points to a data buffer, and "baseLen"
 * holds the value from the result of the first call.
 *
 * The size of the output data is returned.
 */

/*
 *breif:生成基本信息存储在(ptr)中.包括类名，方法名，源文件，函数栈等信息.
 *param[ptr]:存储的内存地址.
 *param[baseLen]:base长度.
 *param[classNames]:类名集合.
 *param[methodNames]:方法名集合.
 *param[fileNames]:文件名集合.
*/
static size_t generateBaseOutput(u1* ptr, size_t baseLen,
    const PointerSet* classNames, const PointerSet* methodNames,
    const PointerSet* fileNames)
{
    u1* origPtr = ptr;
    int count = gDvm.allocRecordCount;
    int idx = headIndex();

    if (origPtr != NULL) {
        set1(&ptr[0], kMessageHeaderLen);
        set1(&ptr[1], kEntryHeaderLen);
        set1(&ptr[2], kStackFrameLen);
        set2BE(&ptr[3], count);
        set4BE(&ptr[5], baseLen);
        set2BE(&ptr[9], dvmPointerSetGetCount(classNames));
        set2BE(&ptr[11], dvmPointerSetGetCount(methodNames));
        set2BE(&ptr[13], dvmPointerSetGetCount(fileNames));
    }
    ptr += kMessageHeaderLen;

    while (count--) {
        AllocRecord* pRec = &gDvm.allocRecords[idx];

        /* compute depth */
        int  depth;
        for (depth = 0; depth < kMaxAllocRecordStackDepth; depth++) {
            if (pRec->stackElem[depth].method == NULL)
                break;
        }

        /* output header */
        if (origPtr != NULL) {
            set4BE(&ptr[0], pRec->size);
            set2BE(&ptr[4], pRec->threadId);
            set2BE(&ptr[6],
                dvmPointerSetFind(classNames, pRec->clazz->descriptor));
            set1(&ptr[8], depth);
        }
        ptr += kEntryHeaderLen;

        /* convert stack frames */
        int i;
        for (i = 0; i < depth; i++) {
            if (origPtr != NULL) {
                const Method* method = pRec->stackElem[i].method;
                int lineNum;

                lineNum = dvmLineNumFromPC(method, pRec->stackElem[i].pc);
                if (lineNum > 32767)
                    lineNum = 32767;

                set2BE(&ptr[0], dvmPointerSetFind(classNames,
                        method->clazz->descriptor));
                set2BE(&ptr[2], dvmPointerSetFind(methodNames,
                        method->name));
                set2BE(&ptr[4], dvmPointerSetFind(fileNames,
                        getMethodSourceFile(method)));
                set2BE(&ptr[6], (u2)lineNum);
            }
            ptr += kStackFrameLen;
        }

        idx = (idx + 1) & (kNumAllocRecords-1);
    }

    return ptr - origPtr;
}

/*
 * Compute the size required to store a string table.  Includes the length
 * word and conversion to UTF-16.
 */

/*
 *breif:计算需要存储字符串表的大小.包括字  转换为utf-16的长度.
 *param[strings]:字符串指针集合.
*/
static size_t computeStringTableSize(PointerSet* strings)
{
    int count = dvmPointerSetGetCount(strings);
    size_t size = 0;
    int i;

    for (i = 0; i < count; i++) {
        const char* str = (const char*) dvmPointerSetGetEntry(strings, i);

        size += 4 + dvmUtf8Len(str) * 2;
    }

    return size;
}

/*
 * Convert a UTF-8 string to UTF-16.  We also need to byte-swap the values
 * to big-endian, and we can't assume even alignment on the target.
 *
 * Returns the string's length, in characters.
 */

/*
 *breif:utf-8字符串转换为utf-16字符串并且将字节序换成大端.
 *param[utf16Str]:utf-16的字符串.
 *param[utf8Str]:utf-8的字符串.
*/
static int convertUtf8ToUtf16BEUA(u1* utf16Str, const char* utf8Str)
{
    u1* origUtf16Str = utf16Str;

    while (*utf8Str != '\0') {
        u2 utf16 = dexGetUtf16FromUtf8(&utf8Str);       /* advances utf8Str */
        set2BE(utf16Str, utf16);
        utf16Str += 2;
    }

    return (utf16Str - origUtf16Str) / 2;
}

/*
 * Output a string table serially.
 */

/*
 *breif:将字符串表连续输出到(ptr)指向的地址，字符串从utf-8转换为utf-16大端.
 *param[strings]:字符串集合.
 *param[ptr]:输出地址.
*/
static size_t outputStringTable(PointerSet* strings, u1* ptr)
{
    int count = dvmPointerSetGetCount(strings);
    u1* origPtr = ptr;
    int i;

    for (i = 0; i < count; i++) {
        const char* str = (const char*) dvmPointerSetGetEntry(strings, i);
        int charLen;

        /* copy UTF-8 string to big-endian unaligned UTF-16 */
        charLen = convertUtf8ToUtf16BEUA(&ptr[4], str);
        set4BE(&ptr[0], charLen);

        ptr += 4 + charLen * 2;
    }

    return ptr - origPtr;
}

/*
 * Generate a DDM packet with all of the tracked allocation data.
 *
 * On success, returns "true" with "*pData" and "*pDataLen" set.
 */

/*
 *breif:生成ddm所有跟踪分配的数据包.
 *param[pData]:输出的数据包.
 *param[pDataLen]:数据包长度.
*/
bool dvmGenerateTrackedAllocationReport(u1** pData, size_t* pDataLen)
{
    bool result = false;
    u1* buffer = NULL;

    dvmLockMutex(&gDvm.allocTrackerLock);

    /*
     * Part 1: generate string tables.
     */
    PointerSet* classNames = NULL;
    PointerSet* methodNames = NULL;
    PointerSet* fileNames = NULL;

    /*
     * Allocate storage.  Usually there's 60-120 of each thing (sampled
     * when max=512), but it varies widely and isn't closely bound to
     * the number of allocations we've captured.  The sets expand quickly
     * if needed.
     */
    classNames = dvmPointerSetAlloc(128);
    methodNames = dvmPointerSetAlloc(128);
    fileNames = dvmPointerSetAlloc(128);
    if (classNames == NULL || methodNames == NULL || fileNames == NULL) {
        ALOGE("Failed allocating pointer sets");
        goto bail;
    }

    if (!populateStringTables(classNames, methodNames, fileNames))
        goto bail;

    if (false) {
        printf("Classes:\n");
        dumpStringTable(classNames);
        printf("Methods:\n");
        dumpStringTable(methodNames);
        printf("Files:\n");
        dumpStringTable(fileNames);
    }

    /*
     * Part 2: compute the size of the output.
     *
     * (Could also just write to an expanding buffer.)
     */
    size_t baseSize, totalSize;
    baseSize = generateBaseOutput(NULL, 0, classNames, methodNames, fileNames);
    assert(baseSize > 0);
    totalSize = baseSize;
    totalSize += computeStringTableSize(classNames);
    totalSize += computeStringTableSize(methodNames);
    totalSize += computeStringTableSize(fileNames);
    ALOGI("Generated AT, size is %zd/%zd", baseSize, totalSize);

    /*
     * Part 3: allocate a buffer and generate the output.
     */
    u1* strPtr;

    buffer = (u1*) malloc(totalSize);
    strPtr = buffer + baseSize;
    generateBaseOutput(buffer, baseSize, classNames, methodNames, fileNames);
    strPtr += outputStringTable(classNames, strPtr);
    strPtr += outputStringTable(methodNames, strPtr);
    strPtr += outputStringTable(fileNames, strPtr);
    if (strPtr - buffer != (int)totalSize) {
        ALOGE("size mismatch (%d vs %zd)", strPtr - buffer, totalSize);
        dvmAbort();
    }
    //dvmPrintHexDump(buffer, totalSize);

    *pData = buffer;
    *pDataLen = totalSize;
    buffer = NULL;          // don't free -- caller will own
    result = true;

bail:
    dvmPointerSetFree(classNames);
    dvmPointerSetFree(methodNames);
    dvmPointerSetFree(fileNames);
    free(buffer);
    dvmUnlockMutex(&gDvm.allocTrackerLock);
    //dvmDumpTrackedAllocations(false);
    return result;
}

/*
 * Dump the tracked allocations to the log file.
 *
 * If "enable" is set, we try to enable the feature if it's not already
 * active.
 */

/*
 *breif:转储跟踪分配到日志文件.
 *param[enable]:启用同步.
*/
void dvmDumpTrackedAllocations(bool enable)
{
    if (enable)
        dvmEnableAllocTracker();

    dvmLockMutex(&gDvm.allocTrackerLock);
    if (gDvm.allocRecords == NULL) {
        dvmUnlockMutex(&gDvm.allocTrackerLock);
        return;
    }

    /*
     * "idx" is the head of the list.  We want to start at the end of the
     * list and move forward to the tail.
     */
    int idx = headIndex();
    int count = gDvm.allocRecordCount;

    ALOGI("Tracked allocations, (head=%d count=%d)",
        gDvm.allocRecordHead, count);
    while (count--) {
        AllocRecord* pRec = &gDvm.allocRecords[idx];
        ALOGI(" T=%-2d %6d %s",
            pRec->threadId, pRec->size, pRec->clazz->descriptor);

        if (true) {
            for (int i = 0; i < kMaxAllocRecordStackDepth; i++) {
                if (pRec->stackElem[i].method == NULL)
                    break;

                const Method* method = pRec->stackElem[i].method;
                if (dvmIsNativeMethod(method)) {
                    ALOGI("    %s.%s (Native)",
                        method->clazz->descriptor, method->name);
                } else {
                    ALOGI("    %s.%s +%d",
                        method->clazz->descriptor, method->name,
                        pRec->stackElem[i].pc);
                }
            }
        }

        /* pause periodically to help logcat catch up */
        if ((count % 5) == 0)
            usleep(40000);

        idx = (idx + 1) & (kNumAllocRecords-1);
    }

    dvmUnlockMutex(&gDvm.allocTrackerLock);
    if (false) {
        u1* data;
        size_t dataLen;
        if (dvmGenerateTrackedAllocationReport(&data, &dataLen))
            free(data);
    }
}
