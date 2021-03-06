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
 * Annotations.
 *
 * We're not expecting to make much use of runtime annotations, so speed vs.
 * space choices are weighted heavily toward small size.
 *
 * It would have been nice to treat "system" annotations in the same way
 * we do "real" annotations, but that doesn't work.  The chief difficulty
 * is that some of them have member types that are not legal in annotations,
 * such as Method and Annotation.  Another source of pain comes from the
 * AnnotationDefault annotation, which by virtue of being an annotation
 * could itself have default values, requiring some additional checks to
 * prevent recursion.
 *
 * It's simpler, and more efficient, to handle the system annotations
 * entirely inside the VM.  There are empty classes defined for the system
 * annotation types, but their only purpose is to allow the system
 * annotations to share name space with standard annotations.
 */
#include "Dalvik.h"

// fwd
static Object* processEncodedAnnotation(const ClassObject* clazz,\
    const u1** pPtr);
static bool skipEncodedAnnotation(const ClassObject* clazz, const u1** pPtr);

/*
 * System annotation descriptors.
 */
static const char* kDescrAnnotationDefault
                                    = "Ldalvik/annotation/AnnotationDefault;";
static const char* kDescrEnclosingClass
                                    = "Ldalvik/annotation/EnclosingClass;";
static const char* kDescrEnclosingMethod
                                    = "Ldalvik/annotation/EnclosingMethod;";
static const char* kDescrInnerClass = "Ldalvik/annotation/InnerClass;";
static const char* kDescrMemberClasses
                                    = "Ldalvik/annotation/MemberClasses;";
static const char* kDescrSignature  = "Ldalvik/annotation/Signature;";
static const char* kDescrThrows     = "Ldalvik/annotation/Throws;";

/*
 * Read an unsigned LEB128 value from a buffer.  Advances "pBuf".
 */
/*
 * 从'pBuf'中读取无符号LEB128值
 */
static u4 readUleb128(const u1** pBuf)
{
    u4 result = 0;
    int shift = 0;
    const u1* buf = *pBuf;
    u1 val;

    do {
        /*
         * Worst-case on bad data is we read too much data and return a bogus
         * result.  Safe to assume that we will encounter a byte with its
         * high bit clear before the end of the mapped file.
         */
        assert(shift < 32);

        val = *buf++;
        result |= (val & 0x7f) << shift;
        shift += 7;
    } while ((val & 0x80) != 0);

    *pBuf = buf;
    return result;
}

/*
 * Get the annotations directory item.
 */
/*
 * 获取注解目录项
 */

static const DexAnnotationsDirectoryItem* getAnnoDirectory(DexFile* pDexFile,
    const ClassObject* clazz)
{
    const DexClassDef* pClassDef;

    /*
     * Find the class def in the DEX file.  For better performance we should
     * stash this in the ClassObject.
     */
    /*在DEX文件中查找class索引,为了有更好的性能，我们应该储存这在ClassObject对象中*/
    pClassDef = dexFindClass(pDexFile, clazz->descriptor);
    assert(pClassDef != NULL);
    return dexGetAnnotationsDirectoryItem(pDexFile, pClassDef);
}

/*
 * Return a zero-length array of Annotation objects.
 *
 * TODO: this currently allocates a new array each time, but I think we
 * can get away with returning a canonical copy.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
/*
 * 返回一个空的注解数组
 */
static ArrayObject* emptyAnnoArray()
{
    return dvmAllocArrayByClass(
        gDvm.classJavaLangAnnotationAnnotationArray, 0, ALLOC_DEFAULT);
}

/*
 * Return an array of empty arrays of Annotation objects.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
/*
 * 返回一个注解对象数组的空数组  
 */
static ArrayObject* emptyAnnoArrayArray(int numElements)
{
    Thread* self = dvmThreadSelf();
    ArrayObject* arr;
    int i;

    arr = dvmAllocArrayByClass(gDvm.classJavaLangAnnotationAnnotationArrayArray,
            numElements, ALLOC_DEFAULT);
    if (arr != NULL) {
        ArrayObject** elems = (ArrayObject**)(void*)arr->contents;
        for (i = 0; i < numElements; i++) {
            elems[i] = emptyAnnoArray();
            dvmReleaseTrackedAlloc((Object*)elems[i], self);
        }
    }

    return arr;
}

/*
 * Read a signed integer.  "zwidth" is the zero-based byte count.
 */
/*
 * 读取一个有符号的整形，'zwidth'是从零开始的字节数
 */
static s4 readSignedInt(const u1* ptr, int zwidth)
{
    s4 val = 0;
    int i;

    for (i = zwidth; i >= 0; --i)
        val = ((u4)val >> 8) | (((s4)*ptr++) << 24);
    val >>= (3 - zwidth) * 8;

    return val;
}

/*
 * Read an unsigned integer.  "zwidth" is the zero-based byte count,
 * "fillOnRight" indicates which side we want to zero-fill from.
 */
/*
 * 读取一个无符号的整形,'zwidth'是从零开始的字节数,'fillOnRight'标示右边我们需要零来填充
 */
static u4 readUnsignedInt(const u1* ptr, int zwidth, bool fillOnRight)
{
    u4 val = 0;
    int i;

    if (!fillOnRight) {
        for (i = zwidth; i >= 0; --i)
            val = (val >> 8) | (((u4)*ptr++) << 24);
        val >>= (3 - zwidth) * 8;
    } else {
        for (i = zwidth; i >= 0; --i)
            val = (val >> 8) | (((u4)*ptr++) << 24);
    }
    return val;
}

/*
 * Read a signed long.  "zwidth" is the zero-based byte count.
 */
/*
 * 读取一个有符号的长整形.'zwidth'是从零开始的字节数
 */
static s8 readSignedLong(const u1* ptr, int zwidth)
{
    s8 val = 0;
    int i;

    for (i = zwidth; i >= 0; --i)
        val = ((u8)val >> 8) | (((s8)*ptr++) << 56);
    val >>= (7 - zwidth) * 8;

    return val;
}

/*
 * Read an unsigned long.  "zwidth" is the zero-based byte count,
 * "fillOnRight" indicates which side we want to zero-fill from.
 */
/*
 * 读取一个无符号的长整形,'zwidth'是从零开始的字节数,'fillOnRight'标示右边需要我们用零来填充
 */
static u8 readUnsignedLong(const u1* ptr, int zwidth, bool fillOnRight)
{
    u8 val = 0;
    int i;

    if (!fillOnRight) {
        for (i = zwidth; i >= 0; --i)
            val = (val >> 8) | (((u8)*ptr++) << 56);
        val >>= (7 - zwidth) * 8;
    } else {
        for (i = zwidth; i >= 0; --i)
            val = (val >> 8) | (((u8)*ptr++) << 56);
    }
    return val;
}


/*
 * ===========================================================================
 *      Element extraction
 * ===========================================================================
 */

/*
 * An annotation in "clazz" refers to a method by index.  This just gives
 * us the name of the class and the name and signature of the method.  We
 * need to find the method's class, and then find the method within that
 * class.  If the method has been resolved before, we can just use the
 * results of the previous lookup.
 *
 * Normally we do this as part of method invocation in the interpreter, which
 * provides us with a bit of context: is it virtual or direct, do we need
 * to initialize the class because it's a static method, etc.  We don't have
 * that information here, so we have to do a bit of searching.
 *
 * Returns NULL if the method was not found (exception may be pending).
 */
/*
 * 通过索引关联到一个方法.这仅仅给定我们类的名字和方法名称和方法签名，我们需要去找方法的类，
 * 然后找类里面的方法.如果之前方法还没有被解析，我们可以使用以前查询的结果
 */
static Method* resolveAmbiguousMethod(const ClassObject* referrer, u4 methodIdx)
{
    DexFile* pDexFile;
    ClassObject* resClass;
    Method* resMethod;
    const DexMethodId* pMethodId;
    const char* name;

    /* if we've already resolved this method, return it */
    resMethod = dvmDexGetResolvedMethod(referrer->pDvmDex, methodIdx);
    if (resMethod != NULL)
        return resMethod;

    pDexFile = referrer->pDvmDex->pDexFile;
    pMethodId = dexGetMethodId(pDexFile, methodIdx);
    resClass = dvmResolveClass(referrer, pMethodId->classIdx, true);
    if (resClass == NULL) {
        /* note exception will be pending */
        ALOGD("resolveAmbiguousMethod: unable to find class %d", methodIdx);
        return NULL;
    }
    if (dvmIsInterfaceClass(resClass)) {
        /* method is part of an interface -- not expecting that */
        ALOGD("resolveAmbiguousMethod: method in interface?");
        return NULL;
    }

    // TODO - consider a method access flag that indicates direct vs. virtual
    name = dexStringById(pDexFile, pMethodId->nameIdx);

    DexProto proto;
    dexProtoSetFromMethodId(&proto, pDexFile, pMethodId);

    if (name[0] == '<') {
        /*
         * Constructor or class initializer.  Only need to examine the
         * "direct" list, and don't need to look up the class hierarchy.
         */
        resMethod = dvmFindDirectMethod(resClass, name, &proto);
    } else {
        /*
         * Do a hierarchical scan for direct and virtual methods.
         *
         * This uses the search order from the VM spec (v2 5.4.3.3), which
         * seems appropriate here.
         */
        resMethod = dvmFindMethodHier(resClass, name, &proto);
    }

    return resMethod;
}

/*
 * constants for processAnnotationValue indicating what style of
 * result is wanted
 */
enum AnnotationResultStyle {
    kAllObjects,         /* return everything as an object */
    kAllRaw,             /* return everything as a raw value or index */
    kPrimitivesOrObjects /* return primitives as-is but the rest as objects */
};

/*
 * Recursively process an annotation value.
 * 递归的处理注解值
 *
 * "clazz" is the class on which the annotations are defined.  It may be
 * NULL when "resultStyle" is "kAllRaw".
 * 注解被定义在'clazz'的类中，当'resultStyle'是'kAllRaw'时，它可能是NULL
 *
 * If "resultStyle" is "kAllObjects", the result will always be an Object of an
 * appropriate type (in pValue->value.l).  For primitive types, the usual
 * wrapper objects will be created.
 * 如果'resultStyle'是'kAllObjects'，结果将总是一个合适类型的对象.对于基本数据
 * 类型，通常以包装后对象将被创建
 *
 * If "resultStyle" is "kAllRaw", numeric constants are stored directly into
 * "pValue", and indexed values like String and Method are returned as
 * indexes.  Complex values like annotations and arrays are not handled.
 * 如果'resultStyle'是'kAllRaw',那么数字常量将直接被保存到'pValue',并且
 * 被编入索引的值(如:Stringt和Method)将作为索引被返回，复杂的值(如:annotations 和 arrays)
 * 不去处理
 *
 * If "resultStyle" is "kPrimitivesOrObjects", numeric constants are stored
 * directly into "pValue", and everything else is constructed as an Object
 * of appropriate type (in pValue->value.l).
 * 如果'resultStyle'是'kPrimitivesOrObjects'，常量池被直接保存到'pValue',
 * 并且其他一切作为合适类型对象被构造
 *
 * The caller must call dvmReleaseTrackedAlloc on returned objects, when
 * using "kAllObjects" or "kPrimitivesOrObjects".
 * 当使用'kAllObjects'或者'kPrimitivesOrObjects'时，调用者必须调用dvmReleaseTrackedAlloc函数
 * 返回对象
 *
 * Returns "true" on success, "false" if the value could not be processed
 * or an object could not be allocated.  On allocation failure an exception
 * will be raised.
 * ‘true’则代表成功，如果为'false',值将不能被处理或者对象不能被分配
 * 如果分配失败将引发异常
 */
static bool processAnnotationValue(const ClassObject* clazz,
    const u1** pPtr, AnnotationValue* pValue,
    AnnotationResultStyle resultStyle)
{
    Thread* self = dvmThreadSelf();
    Object* elemObj = NULL;
    bool setObject = false;
    const u1* ptr = *pPtr;
    u1 valueType, valueArg;
    int width;
    u4 idx;

    valueType = *ptr++;
    valueArg = valueType >> kDexAnnotationValueArgShift;
    width = valueArg + 1;       /* assume, correct later */

    ALOGV("----- type is 0x%02x %d, ptr=%p [0x%06x]",
        valueType & kDexAnnotationValueTypeMask, valueArg, ptr-1,
        (ptr-1) - (u1*)clazz->pDvmDex->pDexFile->baseAddr);

    pValue->type = valueType & kDexAnnotationValueTypeMask;

    switch (valueType & kDexAnnotationValueTypeMask) {
    case kDexAnnotationByte:
        pValue->value.i = (s1) readSignedInt(ptr, valueArg);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('B'));
            setObject = true;
        }
        break;
    case kDexAnnotationShort:
        pValue->value.i = (s2) readSignedInt(ptr, valueArg);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('S'));
            setObject = true;
        }
        break;
    case kDexAnnotationChar:
        pValue->value.i = (u2) readUnsignedInt(ptr, valueArg, false);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('C'));
            setObject = true;
        }
        break;
    case kDexAnnotationInt:
        pValue->value.i = readSignedInt(ptr, valueArg);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('I'));
            setObject = true;
        }
        break;
    case kDexAnnotationLong:
        pValue->value.j = readSignedLong(ptr, valueArg);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('J'));
            setObject = true;
        }
        break;
    case kDexAnnotationFloat:
        pValue->value.i = readUnsignedInt(ptr, valueArg, true);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('F'));
            setObject = true;
        }
        break;
    case kDexAnnotationDouble:
        pValue->value.j = readUnsignedLong(ptr, valueArg, true);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('D'));
            setObject = true;
        }
        break;
    case kDexAnnotationBoolean:
        pValue->value.i = (valueArg != 0);
        if (resultStyle == kAllObjects) {
            elemObj = (Object*) dvmBoxPrimitive(pValue->value,
                        dvmFindPrimitiveClass('Z'));
            setObject = true;
        }
        width = 0;
        break;

    case kDexAnnotationString:
        idx = readUnsignedInt(ptr, valueArg, false);
        if (resultStyle == kAllRaw) {
            pValue->value.i = idx;
        } else {
            elemObj = (Object*) dvmResolveString(clazz, idx);
            setObject = true;
            if (elemObj == NULL)
                return false;
            dvmAddTrackedAlloc(elemObj, self);      // balance the Release
        }
        break;
    case kDexAnnotationType:
        idx = readUnsignedInt(ptr, valueArg, false);
        if (resultStyle == kAllRaw) {
            pValue->value.i = idx;
        } else {
            elemObj = (Object*) dvmResolveClass(clazz, idx, true);
            setObject = true;
            if (elemObj == NULL) {
                /* we're expected to throw a TypeNotPresentException here */
                DexFile* pDexFile = clazz->pDvmDex->pDexFile;
                const char* desc = dexStringByTypeIdx(pDexFile, idx);
                dvmClearException(self);
                dvmThrowTypeNotPresentException(desc);
                return false;
            } else {
                dvmAddTrackedAlloc(elemObj, self);      // balance the Release
            }
        }
        break;
    case kDexAnnotationMethod:
        idx = readUnsignedInt(ptr, valueArg, false);
        if (resultStyle == kAllRaw) {
            pValue->value.i = idx;
        } else {
            Method* meth = resolveAmbiguousMethod(clazz, idx);
            if (meth == NULL)
                return false;
            elemObj = dvmCreateReflectObjForMethod(clazz, meth);
            setObject = true;
            if (elemObj == NULL)
                return false;
        }
        break;
    case kDexAnnotationField:
        idx = readUnsignedInt(ptr, valueArg, false);
        assert(false);      // TODO
        break;
    case kDexAnnotationEnum:
        /* enum values are the contents of a static field */
        idx = readUnsignedInt(ptr, valueArg, false);
        if (resultStyle == kAllRaw) {
            pValue->value.i = idx;
        } else {
            StaticField* sfield;

            sfield = dvmResolveStaticField(clazz, idx);
            if (sfield == NULL) {
                return false;
            } else {
                assert(sfield->clazz->descriptor[0] == 'L');
                elemObj = sfield->value.l;
                setObject = true;
                dvmAddTrackedAlloc(elemObj, self);      // balance the Release
            }
        }
        break;
    case kDexAnnotationArray:
        /*
         * encoded_array format, which is a size followed by a stream
         * of annotation_value.
         *
         * We create an array of Object, populate it, and return it.
         */
        if (resultStyle == kAllRaw) {
            return false;
        } else {
            ArrayObject* newArray;
            u4 size, count;

            size = readUleb128(&ptr);
            LOGVV("--- annotation array, size is %u at %p", size, ptr);
            newArray = dvmAllocArrayByClass(gDvm.classJavaLangObjectArray,
                size, ALLOC_DEFAULT);
            if (newArray == NULL) {
                ALOGE("annotation element array alloc failed (%d)", size);
                return false;
            }

            AnnotationValue avalue;
            for (count = 0; count < size; count++) {
                if (!processAnnotationValue(clazz, &ptr, &avalue,
                                kAllObjects)) {
                    dvmReleaseTrackedAlloc((Object*)newArray, self);
                    return false;
                }
                Object* obj = (Object*)avalue.value.l;
                dvmSetObjectArrayElement(newArray, count, obj);
                dvmReleaseTrackedAlloc(obj, self);
            }

            elemObj = (Object*) newArray;
            setObject = true;
        }
        width = 0;
        break;
    case kDexAnnotationAnnotation:
        /* encoded_annotation format */
        if (resultStyle == kAllRaw)
            return false;
        elemObj = processEncodedAnnotation(clazz, &ptr);
        setObject = true;
        if (elemObj == NULL)
            return false;
        dvmAddTrackedAlloc(elemObj, self);      // balance the Release
        width = 0;
        break;
    case kDexAnnotationNull:
        if (resultStyle == kAllRaw) {
            pValue->value.i = 0;
        } else {
            assert(elemObj == NULL);
            setObject = true;
        }
        width = 0;
        break;
    default:
        ALOGE("Bad annotation element value byte 0x%02x (0x%02x)",
            valueType, valueType & kDexAnnotationValueTypeMask);
        assert(false);
        return false;
    }

    ptr += width;

    *pPtr = ptr;
    if (setObject)
        pValue->value.l = elemObj;
    return true;
}


/*
 * For most object types, we have nothing to do here, and we just return
 * "valueObj".
 * 对于大多数的对象类型，我们在这儿没有做太多的事情，只仅仅返回了'valueObj'
 * 
 * For an array annotation, the type of the extracted object will always
 * be java.lang.Object[], but we want it to match the type that the
 * annotation member is expected to return.  In some cases this may
 * involve un-boxing primitive values.
 * 对于一个数组注解, 提取的对象的类型总是java.lang.Object[]，但是，
 * 我们想要它的注解成员去返回相匹配类型，在某种情况下，这可能牵涉到拆箱的基本数据类型
 *
 * We allocate a second array with the correct type, then copy the data
 * over.  This releases the tracked allocation on "valueObj" and returns
 * a new, tracked object.
 *
 * On failure, this releases the tracking on "valueObj" and returns NULL
 * (allowing the call to say "foo = convertReturnType(foo, ..)").
 */
static Object* convertReturnType(Object* valueObj, ClassObject* methodReturn)
{
    if (valueObj == NULL ||
        !dvmIsArray((ArrayObject*)valueObj) || !dvmIsArrayClass(methodReturn))
    {
        return valueObj;
    }

    Thread* self = dvmThreadSelf();
    ClassObject* srcElemClass;
    ClassObject* dstElemClass;

    /*
     * We always extract kDexAnnotationArray into Object[], so we expect to
     * find that here.  This means we can skip the FindClass on
     * (valueObj->clazz->descriptor+1, valueObj->clazz->classLoader).
     */
    if (strcmp(valueObj->clazz->descriptor, "[Ljava/lang/Object;") != 0) {
        ALOGE("Unexpected src type class (%s)", valueObj->clazz->descriptor);
        return NULL;
    }
    srcElemClass = gDvm.classJavaLangObject;

    /*
     * Skip past the '[' to get element class name.  Note this is not always
     * the same as methodReturn->elementClass.
     */
    char firstChar = methodReturn->descriptor[1];
    if (firstChar == 'L' || firstChar == '[') {
        dstElemClass = dvmFindClass(methodReturn->descriptor+1,
            methodReturn->classLoader);
    } else {
        dstElemClass = dvmFindPrimitiveClass(firstChar);
    }
    ALOGV("HEY: converting valueObj from [%s to [%s",
        srcElemClass->descriptor, dstElemClass->descriptor);

    ArrayObject* srcArray = (ArrayObject*) valueObj;
    u4 length = srcArray->length;
    ArrayObject* newArray;

    newArray = dvmAllocArrayByClass(methodReturn, length, ALLOC_DEFAULT);
    if (newArray == NULL) {
        ALOGE("Failed creating duplicate annotation class (%s %d)",
            methodReturn->descriptor, length);
        goto bail;
    }

    bool success;
    if (dstElemClass->primitiveType == PRIM_NOT) {
        success = dvmCopyObjectArray(newArray, srcArray, dstElemClass);
    } else {
        success = dvmUnboxObjectArray(newArray, srcArray, dstElemClass);
    }
    if (!success) {
        ALOGE("Annotation array copy failed");
        dvmReleaseTrackedAlloc((Object*)newArray, self);
        newArray = NULL;
        goto bail;
    }

bail:
    /* replace old, return new */
    dvmReleaseTrackedAlloc(valueObj, self);
    return (Object*) newArray;
}

/*
 * Create a new AnnotationMember.
 * 创建一个新的注解成员
 *
 * "clazz" is the class on which the annotations are defined.  "pPtr"
 * points to a pointer into the annotation data.  "annoClass" is the
 * annotation's class.
 * 注解被定义在'clazz'类中. 'pPtr'指向一个指针的注解数据区.'annoClass'是一个注解类
 *
 * We extract the annotation's value, create a new AnnotationMember object,
 * and construct it.
 * 我们抽取注解值，创建一个新的注解成员对象并且构造它,'pPtr'指针指向了
 *
 * Returns NULL on failure; an exception may or may not be raised.
 */
static Object* createAnnotationMember(const ClassObject* clazz,
    const ClassObject* annoClass, const u1** pPtr)
{
    Thread* self = dvmThreadSelf();
    const DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    StringObject* nameObj = NULL;
    Object* valueObj = NULL;
    Object* newMember = NULL;
    Object* methodObj = NULL;
    ClassObject* methodReturn = NULL;
    u4 elementNameIdx;
    const char* name;
    AnnotationValue avalue;
    JValue unused;
    bool failed = true;

    elementNameIdx = readUleb128(pPtr);

    if (!processAnnotationValue(clazz, pPtr, &avalue, kAllObjects)) {
        ALOGW("Failed processing annotation value");
        goto bail;
    }
    valueObj = (Object*)avalue.value.l;

    /* new member to hold the element */
    newMember =
        dvmAllocObject(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationMember,
        ALLOC_DEFAULT);
    name = dexStringById(pDexFile, elementNameIdx);
    nameObj = dvmCreateStringFromCstr(name);

    /* find the method in the annotation class, given only the name */
    if (name != NULL) {
        Method* annoMeth = dvmFindVirtualMethodByName(annoClass, name);
        if (annoMeth == NULL) {
            ALOGW("WARNING: could not find annotation member %s in %s",
                name, annoClass->descriptor);
        } else {
            methodObj = dvmCreateReflectObjForMethod(annoClass, annoMeth);
            methodReturn = dvmGetBoxedReturnType(annoMeth);
        }
    }
    if (newMember == NULL || nameObj == NULL || methodObj == NULL ||
        methodReturn == NULL)
    {
        ALOGE("Failed creating annotation element (m=%p n=%p a=%p r=%p)",
            newMember, nameObj, methodObj, methodReturn);
        goto bail;
    }

    /* convert the return type, if necessary */
    valueObj = convertReturnType(valueObj, methodReturn);
    if (valueObj == NULL)
        goto bail;

    /* call 4-argument constructor */
    dvmCallMethod(self, gDvm.methOrgApacheHarmonyLangAnnotationAnnotationMember_init,
        newMember, &unused, nameObj, valueObj, methodReturn, methodObj);
    if (dvmCheckException(self)) {
        ALOGD("Failed constructing annotation element");
        goto bail;
    }

    failed = false;

bail:
    /* release tracked allocations */
    dvmReleaseTrackedAlloc(newMember, self);
    dvmReleaseTrackedAlloc((Object*)nameObj, self);
    dvmReleaseTrackedAlloc(valueObj, self);
    dvmReleaseTrackedAlloc(methodObj, self);
    if (failed)
        return NULL;
    else
        return newMember;
}

/*
 * Create a new Annotation object from what we find in the annotation item.
 *
 * "clazz" is the class on which the annotations are defined.  "pPtr"
 * points to a pointer into the annotation data.
 *
 * We use the AnnotationFactory class to create the annotation for us.  The
 * method we call is:
 *
 *  public static Annotation createAnnotation(
 *      Class<? extends Annotation> annotationType,
 *      AnnotationMember[] elements)
 *
 * Returns a new Annotation, which will NOT be in the local ref table and
 * not referenced elsewhere, so store it away soon.  On failure, returns NULL
 * with an exception raised.
 */
/*
 * 我们从注解项中创建一个新的注解对象.
 *
 * 注解被定义在'clazz'类中，'pPtr'指向了一个指针的注解数据区
 * 
 * 我们使用AnnotationFactory类去创建注解,下面这个方法将被调用
 *  public static Annotation createAnnotation(
 *      Class<? extends Annotation> annotationType,
 *      AnnotationMember[] elements)
 */
static Object* processEncodedAnnotation(const ClassObject* clazz,
    const u1** pPtr)
{
    Thread* self = dvmThreadSelf();
    Object* newAnno = NULL;
    ArrayObject* elementArray = NULL;
    const ClassObject* annoClass;
    const u1* ptr;
    u4 typeIdx, size, count;

    ptr = *pPtr;
    typeIdx = readUleb128(&ptr);
    size = readUleb128(&ptr);

    LOGVV("----- processEnc ptr=%p type=%d size=%d", ptr, typeIdx, size);

    annoClass = dvmDexGetResolvedClass(clazz->pDvmDex, typeIdx);
    if (annoClass == NULL) {
        annoClass = dvmResolveClass(clazz, typeIdx, true);
        if (annoClass == NULL) {
            ALOGE("Unable to resolve %s annotation class %d",
                clazz->descriptor, typeIdx);
            assert(dvmCheckException(self));
            return NULL;
        }
    }

    ALOGV("----- processEnc ptr=%p [0x%06x]  typeIdx=%d size=%d class=%s",
        *pPtr, *pPtr - (u1*) clazz->pDvmDex->pDexFile->baseAddr,
        typeIdx, size, annoClass->descriptor);

    /*
     * Elements are parsed out and stored in an array.  The Harmony
     * constructor wants an array with just the declared elements --
     * default values get merged in later.
     */
    JValue result;

    if (size > 0) {
        elementArray = dvmAllocArrayByClass(
            gDvm.classOrgApacheHarmonyLangAnnotationAnnotationMemberArray,
            size, ALLOC_DEFAULT);
        if (elementArray == NULL) {
            ALOGE("failed to allocate annotation member array (%d elements)",
                size);
            goto bail;
        }
    }

    /*
     * "ptr" points to a byte stream with "size" occurrences of
     * annotation_element.
     */
    for (count = 0; count < size; count++) {
        Object* newMember = createAnnotationMember(clazz, annoClass, &ptr);
        if (newMember == NULL)
            goto bail;

        /* add it to the array */
        dvmSetObjectArrayElement(elementArray, count, newMember);
    }

    dvmCallMethod(self,
        gDvm.methOrgApacheHarmonyLangAnnotationAnnotationFactory_createAnnotation,
        NULL, &result, annoClass, elementArray);
    if (dvmCheckException(self)) {
        ALOGD("Failed creating an annotation");
        //dvmLogExceptionStackTrace();
        goto bail;
    }

    newAnno = (Object*)result.l;

bail:
    dvmReleaseTrackedAlloc((Object*) elementArray, NULL);
    *pPtr = ptr;
    if (newAnno == NULL && !dvmCheckException(self)) {
        /* make sure an exception is raised */
        dvmThrowRuntimeException("failure in processEncodedAnnotation");
    }
    return newAnno;
}

/*
 * Run through an annotation set and convert each entry into an Annotation
 * object.
 * 遍历注解集合并转换每一项成注解对象
 *
 * Returns an array of Annotation objects, or NULL with an exception raised
 * on alloc failure.
 * 返回一个注解数组对象，或者是NULL,分配失败将引发异常
 */
static ArrayObject* processAnnotationSet(const ClassObject* clazz,
    const DexAnnotationSetItem* pAnnoSet, int visibility)
{
    DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    const DexAnnotationItem* pAnnoItem;
    ArrayObject* annoArray;
    int i, count;
    u4 dstIndex;

    /* we need these later; make sure they're initialized */
    if (!dvmIsClassInitialized(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationFactory))
        dvmInitClass(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationFactory);
    if (!dvmIsClassInitialized(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationMember))
        dvmInitClass(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationMember);

    /* count up the number of visible elements */
    for (i = count = 0; i < (int) pAnnoSet->size; i++) {
        pAnnoItem = dexGetAnnotationItem(pDexFile, pAnnoSet, i);
        if (pAnnoItem->visibility == visibility)
            count++;
    }

    annoArray =
        dvmAllocArrayByClass(gDvm.classJavaLangAnnotationAnnotationArray,
                             count, ALLOC_DEFAULT);
    if (annoArray == NULL)
        return NULL;

    /*
     * Generate Annotation objects.  We must put them into the array
     * immediately (or add them to the tracked ref table).
     */
    dstIndex = 0;
    for (i = 0; i < (int) pAnnoSet->size; i++) {
        pAnnoItem = dexGetAnnotationItem(pDexFile, pAnnoSet, i);
        if (pAnnoItem->visibility != visibility)
            continue;
        const u1* ptr = pAnnoItem->annotation;
        Object *anno = processEncodedAnnotation(clazz, &ptr);
        if (anno == NULL) {
            dvmReleaseTrackedAlloc((Object*) annoArray, NULL);
            return NULL;
        }
        dvmSetObjectArrayElement(annoArray, dstIndex, anno);
        ++dstIndex;
    }

    return annoArray;
}

/*
 * Return the annotation item of the specified type in the annotation set, or
 * NULL if the set contains no annotation of that type.
 */
/*
 * 在注解集合('pAnnoSet')中返回一个指定类型的注解项; 如果如果注解集合中不包括指定类型的注解，则返回NULL
 */
static const DexAnnotationItem* getAnnotationItemFromAnnotationSet(
        const ClassObject* clazz, const DexAnnotationSetItem* pAnnoSet,
        int visibility, const ClassObject* annotationClazz)
{
    DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    const DexAnnotationItem* pAnnoItem;
    int i;
    const ClassObject* annoClass;
    const u1* ptr;
    u4 typeIdx;

    /* we need these later; make sure they're initialized */
    if (!dvmIsClassInitialized(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationFactory))
        dvmInitClass(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationFactory);
    if (!dvmIsClassInitialized(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationMember))
        dvmInitClass(gDvm.classOrgApacheHarmonyLangAnnotationAnnotationMember);

    for (i = 0; i < (int) pAnnoSet->size; i++) {
        pAnnoItem = dexGetAnnotationItem(pDexFile, pAnnoSet, i);
        if (pAnnoItem->visibility != visibility)
            continue;

        ptr = pAnnoItem->annotation;
        typeIdx = readUleb128(&ptr);

        annoClass = dvmDexGetResolvedClass(clazz->pDvmDex, typeIdx);
        if (annoClass == NULL) {
            annoClass = dvmResolveClass(clazz, typeIdx, true);
            if (annoClass == NULL) {
                return NULL; // an exception is pending
            }
        }

        if (annoClass == annotationClazz) {
            return pAnnoItem;
        }
    }

    return NULL;
}

/*
 * Return the Annotation object of the specified type in the annotation set, or
 * NULL if the set contains no annotation of that type.
 */
/*
 * 在注解集合中返回一个指定类型的注解对象，如果该集合没有包涵指定类型的注解，则返回NULL
 */
static Object* getAnnotationObjectFromAnnotationSet(const ClassObject* clazz,
        const DexAnnotationSetItem* pAnnoSet, int visibility,
        const ClassObject* annotationClazz)
{
    const DexAnnotationItem* pAnnoItem = getAnnotationItemFromAnnotationSet(
            clazz, pAnnoSet, visibility, annotationClazz);
    if (pAnnoItem == NULL) {
        return NULL;
    }
    const u1* ptr = pAnnoItem->annotation;
    return processEncodedAnnotation(clazz, &ptr);
}

/*
 * ===========================================================================
 *      Skipping and scanning
 * ===========================================================================
 */

/*
 * Skip past an annotation value.
 *
 * "clazz" is the class on which the annotations are defined.
 *
 * Returns "true" on success, "false" on parsing failure.
 */
/*
 * 跳过一个注解值 . 注解被定义在'clazz'类中
 * 'true'代表成功，'false'代表解析失败
 */
static bool skipAnnotationValue(const ClassObject* clazz, const u1** pPtr)
{
    const u1* ptr = *pPtr;
    u1 valueType, valueArg;
    int width;

    valueType = *ptr++;
    valueArg = valueType >> kDexAnnotationValueArgShift;
    width = valueArg + 1;       /* assume */

    ALOGV("----- type is 0x%02x %d, ptr=%p [0x%06x]",
        valueType & kDexAnnotationValueTypeMask, valueArg, ptr-1,
        (ptr-1) - (u1*)clazz->pDvmDex->pDexFile->baseAddr);

    switch (valueType & kDexAnnotationValueTypeMask) {
    case kDexAnnotationByte:        break;
    case kDexAnnotationShort:       break;
    case kDexAnnotationChar:        break;
    case kDexAnnotationInt:         break;
    case kDexAnnotationLong:        break;
    case kDexAnnotationFloat:       break;
    case kDexAnnotationDouble:      break;
    case kDexAnnotationString:      break;
    case kDexAnnotationType:        break;
    case kDexAnnotationMethod:      break;
    case kDexAnnotationField:       break;
    case kDexAnnotationEnum:        break;

    case kDexAnnotationArray:
        /* encoded_array format */
        {
            u4 size = readUleb128(&ptr);
            while (size--) {
                if (!skipAnnotationValue(clazz, &ptr))
                    return false;
            }
        }
        width = 0;
        break;
    case kDexAnnotationAnnotation:
        /* encoded_annotation format */
        if (!skipEncodedAnnotation(clazz, &ptr))
            return false;
        width = 0;
        break;
    case kDexAnnotationBoolean:
    case kDexAnnotationNull:
        width = 0;
        break;
    default:
        ALOGE("Bad annotation element value byte 0x%02x", valueType);
        assert(false);
        return false;
    }

    ptr += width;

    *pPtr = ptr;
    return true;
}

/*
 * Skip past an encoded annotation.  Mainly useful for annotations embedded
 * in other annotations.
 */
/*
 * 跳过一个加密注解.
 * 在其他的注解中，主要用于可嵌入注解
 */
static bool skipEncodedAnnotation(const ClassObject* clazz, const u1** pPtr)
{
    const u1* ptr;
    u4 size;

    ptr = *pPtr;
    (void) readUleb128(&ptr);
    size = readUleb128(&ptr);

    /*
     * "ptr" points to a byte stream with "size" occurrences of
     * annotation_element.
     */
    while (size--) {
        (void) readUleb128(&ptr);

        if (!skipAnnotationValue(clazz, &ptr))
            return false;
    }

    *pPtr = ptr;
    return true;
}


/*
 * Compare the name of the class in the DEX file to the supplied descriptor.
 * Return value is equivalent to strcmp.
 */
/*
 * 比较DEX文件所提供的描述符中的类的名称。
 */
static int compareClassDescriptor(DexFile* pDexFile, u4 typeIdx,
    const char* descriptor)
{
    const char* str = dexStringByTypeIdx(pDexFile, typeIdx);

    return strcmp(str, descriptor);
}

/*
 * Search through the annotation set for an annotation with a matching
 * descriptor.
 * 通过注解集搜索与匹配的描述符的注解
 *
 * Comparing the string descriptor is slower than comparing an integer class
 * index.  If annotation lists are expected to be long, we could look up
 * the class' index by name from the DEX file, rather than doing a class
 * lookup and string compare on each entry.  (Note the index will be
 * different for each DEX file, so we can't cache annotation class indices
 * globally.)
 */
static const DexAnnotationItem* searchAnnotationSet(const ClassObject* clazz,
    const DexAnnotationSetItem* pAnnoSet, const char* descriptor,
    int visibility)
{
    DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    const DexAnnotationItem* result = NULL;
    u4 typeIdx;
    int i;

    //printf("##### searchAnnotationSet %s %d\n", descriptor, visibility);

    for (i = 0; i < (int) pAnnoSet->size; i++) {
        const DexAnnotationItem* pAnnoItem;

        pAnnoItem = dexGetAnnotationItem(pDexFile, pAnnoSet, i);
        if (pAnnoItem->visibility != visibility)
            continue;
        const u1* ptr = pAnnoItem->annotation;
        typeIdx = readUleb128(&ptr);

        if (compareClassDescriptor(pDexFile, typeIdx, descriptor) == 0) {
            //printf("#####  match on %x/%p at %d\n", typeIdx, pDexFile, i);
            result = pAnnoItem;
            break;
        }
    }

    return result;
}

/*
 * Find an annotation value in the annotation_item whose name matches "name".
 * A pointer to the annotation_value is returned, or NULL if it's not found.
 */
/*
 * 在注解项中查找一个注解值，其名称匹配'name'
 */
static const u1* searchEncodedAnnotation(const ClassObject* clazz,
    const u1* ptr, const char* name)
{
    DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    u4 typeIdx, size;

    typeIdx = readUleb128(&ptr);
    size = readUleb128(&ptr);
    //printf("#####   searching ptr=%p type=%u size=%u\n", ptr, typeIdx, size);

    while (size--) {
        u4 elementNameIdx;
        const char* elemName;

        elementNameIdx = readUleb128(&ptr);
        elemName = dexStringById(pDexFile, elementNameIdx);
        if (strcmp(name, elemName) == 0) {
            //printf("#####   item match on %s\n", name);
            return ptr;     /* points to start of value */
        }

        skipAnnotationValue(clazz, &ptr);
    }

    //printf("#####   no item match on %s\n", name);
    return NULL;
}

#define GAV_FAILED  ((Object*) 0x10000001)

/*
 * Extract an encoded annotation value from the field specified by "annoName".
 *
 * "expectedType" is an annotation value type, e.g. kDexAnnotationString.
 * "debugAnnoName" is only used in debug messages.
 *
 * Returns GAV_FAILED on failure.  If an allocation failed, an exception
 * will be raised.
 */
/*
 * 从指定字段值'annoName'提取一个注解编码值(这个值可能是数组对象，普通对象,异常等...)
 *
 * 'expectedType'是一个注解值类型, 例如 kDexAnnotationString.
 * 'debugAnnoName'仅仅被使用在调试消息
 */
static Object* getAnnotationValue(const ClassObject* clazz,
    const DexAnnotationItem* pAnnoItem, const char* annoName,
    int expectedType, const char* debugAnnoName)
{
    const u1* ptr;
    AnnotationValue avalue;

    /* find the annotation */
    ptr = searchEncodedAnnotation(clazz, pAnnoItem->annotation, annoName);
    if (ptr == NULL) {
        ALOGW("%s annotation lacks '%s' member", debugAnnoName, annoName);
        return GAV_FAILED;
    }

    if (!processAnnotationValue(clazz, &ptr, &avalue, kAllObjects))
        return GAV_FAILED;

    /* make sure it has the expected format */
    if (avalue.type != expectedType) {
        ALOGW("%s %s has wrong type (0x%02x, expected 0x%02x)",
            debugAnnoName, annoName, avalue.type, expectedType);
        return GAV_FAILED;
    }

    return (Object*)avalue.value.l;
}


/*
 * Find the Signature attribute and extract its value.  (Signatures can
 * be found in annotations on classes, constructors, methods, and fields.)
 *
 * Caller must call dvmReleaseTrackedAlloc().
 *
 * Returns NULL if not found.  On memory alloc failure, returns NULL with an
 * exception raised.
 */
 /*
 * 查找一个签名属性并抽取它的值.(签名能被找到在注解类，构造函数，方法和字段中)
 */
static ArrayObject* getSignatureValue(const ClassObject* clazz,
    const DexAnnotationSetItem* pAnnoSet)
{
    const DexAnnotationItem* pAnnoItem;
    Object* obj;

    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrSignature,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL)
        return NULL;

    /*
     * The Signature annotation has one member, "String value".
     */
    obj = getAnnotationValue(clazz, pAnnoItem, "value", kDexAnnotationArray,
            "Signature");
    if (obj == GAV_FAILED)
        return NULL;
    assert(obj->clazz == gDvm.classJavaLangObjectArray);

    return (ArrayObject*)obj;
}


/*
 * ===========================================================================
 *      Class
 * ===========================================================================
 */

/*
 * Find the DexAnnotationSetItem for this class.
 */
/*
 * 查找这个类的DexAnnotationSetItem
 */
static const DexAnnotationSetItem* findAnnotationSetForClass(
    const ClassObject* clazz)
{
    DexFile* pDexFile;
    const DexAnnotationsDirectoryItem* pAnnoDir;

    if (clazz->pDvmDex == NULL)         /* generated class (Proxy, array) */
        return NULL;

    pDexFile = clazz->pDvmDex->pDexFile;
    pAnnoDir = getAnnoDirectory(pDexFile, clazz);
    if (pAnnoDir != NULL)
        return dexGetClassAnnotationSet(pDexFile, pAnnoDir);
    else
        return NULL;
}

/*
 * Return an array of Annotation objects for the class.  Returns an empty
 * array if there are no annotations.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 *
 * On allocation failure, this returns NULL with an exception raised.
 */
/*
 * 返回这个类的注解数组对象.如果这儿没有注解值，则返回空
 */
ArrayObject* dvmGetClassAnnotations(const ClassObject* clazz)
{
    ArrayObject* annoArray;
    const DexAnnotationSetItem* pAnnoSet = NULL;

    pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL) {
        /* no annotations for anything in class, or no class annotations */
        annoArray = emptyAnnoArray();
    } else {
        annoArray = processAnnotationSet(clazz, pAnnoSet,
                        kDexVisibilityRuntime);
    }

    return annoArray;
}

/*
 * Returns the annotation or NULL if it doesn't exist.
 */
 /* 
 * 获取本类的注解
 */
Object* dvmGetClassAnnotation(const ClassObject* clazz,
        const ClassObject* annotationClazz)
{
    const DexAnnotationSetItem* pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL) {
        return NULL;
    }
    return getAnnotationObjectFromAnnotationSet(clazz, pAnnoSet,
            kDexVisibilityRuntime, annotationClazz);
}

/*
 * Returns true if the annotation exists.
 */
/*
 * 判断目前类中是否有指定类型的注解
 */
bool dvmIsClassAnnotationPresent(const ClassObject* clazz,
        const ClassObject* annotationClazz)
{
    const DexAnnotationSetItem* pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL) {
        return NULL;
    }
    const DexAnnotationItem* pAnnoItem = getAnnotationItemFromAnnotationSet(
            clazz, pAnnoSet, kDexVisibilityRuntime, annotationClazz);
    return (pAnnoItem != NULL);
}

/*
 * Retrieve the Signature annotation, if any.  Returns NULL if no signature
 * exists.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
 /*
  * 获取注解签名
  */
ArrayObject* dvmGetClassSignatureAnnotation(const ClassObject* clazz)
{
    ArrayObject* signature = NULL;
    const DexAnnotationSetItem* pAnnoSet;

    pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet != NULL)
        signature = getSignatureValue(clazz, pAnnoSet);

    return signature;
}

/*
 * Get the EnclosingMethod attribute from an annotation.  Returns a Method
 * object, or NULL.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
�/*
 * 从注解中得到EnclosingMethod属性.返回一个Method对象或NULL
 */
Object* dvmGetEnclosingMethod(const ClassObject* clazz)
{
    const DexAnnotationItem* pAnnoItem;
    const DexAnnotationSetItem* pAnnoSet;
    Object* obj;

    pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL)
        return NULL;

    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrEnclosingMethod,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL)
        return NULL;

    /*
     * The EnclosingMethod annotation has one member, "Method value".
     */
    obj = getAnnotationValue(clazz, pAnnoItem, "value", kDexAnnotationMethod,
            "EnclosingMethod");
    if (obj == GAV_FAILED)
        return NULL;
    assert(obj->clazz == gDvm.classJavaLangReflectConstructor ||
           obj->clazz == gDvm.classJavaLangReflectMethod);

    return obj;
}

/*
 * Find a class' enclosing class.  We return what we find in the
 * EnclosingClass attribute.
 * 
 * Returns a Class object, or NULL.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
/*
 * 找到一个封闭类,返回EnclosingClass的属性值
 */
ClassObject* dvmGetDeclaringClass(const ClassObject* clazz)
{
    const DexAnnotationItem* pAnnoItem;
    const DexAnnotationSetItem* pAnnoSet;
    Object* obj;

    pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL)
        return NULL;

    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrEnclosingClass,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL)
        return NULL;

    /*
     * The EnclosingClass annotation has one member, "Class value".
     */
    /*
   * EnclosingClass注解有一个成员‘Class value’
   */
    obj = getAnnotationValue(clazz, pAnnoItem, "value", kDexAnnotationType,
            "EnclosingClass");
    if (obj == GAV_FAILED)
        return NULL;

    assert(dvmIsClassObject(obj));
    return (ClassObject*)obj;
}

/*
 * Find a class' enclosing class.  We first search for an EnclosingClass
 * attribute, and if that's not found we look for an EnclosingMethod.
 *
 * Returns a Class object, or NULL.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
 /*
 * 查找一个封闭类，我们首先查找一个EnclosingClass属性,并且如果没有被找到，则我们查找一个EnclosingMethod
 */
ClassObject* dvmGetEnclosingClass(const ClassObject* clazz)
{
    const DexAnnotationItem* pAnnoItem;
    const DexAnnotationSetItem* pAnnoSet;
    Object* obj;

    pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL)
        return NULL;

    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrEnclosingClass,
        kDexVisibilitySystem);
    if (pAnnoItem != NULL) {
        /*
         * The EnclosingClass annotation has one member, "Class value".
         */
        obj = getAnnotationValue(clazz, pAnnoItem, "value", kDexAnnotationType,
                "EnclosingClass");
        if (obj != GAV_FAILED) {
            assert(dvmIsClassObject(obj));
            return (ClassObject*)obj;
        }
    }

    /*
     * That didn't work.  Look for an EnclosingMethod.
     *
     * We could create a java.lang.reflect.Method object and extract the
     * declaringClass from it, but that's more work than we want to do.
     * Instead, we find the "value" item and parse the index out ourselves.
     */
    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrEnclosingMethod,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL)
        return NULL;

    /* find the value member */
    const u1* ptr;
    ptr = searchEncodedAnnotation(clazz, pAnnoItem->annotation, "value");
    if (ptr == NULL) {
        ALOGW("EnclosingMethod annotation lacks 'value' member");
        return NULL;
    }

    /* parse it, verify the type */
    AnnotationValue avalue;
    if (!processAnnotationValue(clazz, &ptr, &avalue, kAllRaw)) {
        ALOGW("EnclosingMethod parse failed");
        return NULL;
    }
    if (avalue.type != kDexAnnotationMethod) {
        ALOGW("EnclosingMethod value has wrong type (0x%02x, expected 0x%02x)",
            avalue.type, kDexAnnotationMethod);
        return NULL;
    }

    /* pull out the method index and resolve the method */
    Method* meth = resolveAmbiguousMethod(clazz, avalue.value.i);
    if (meth == NULL)
        return NULL;

    ClassObject* methClazz = meth->clazz;
    dvmAddTrackedAlloc((Object*) methClazz, NULL);      // balance the Release
    return methClazz;
}

/*
 * Get the EnclosingClass attribute from an annotation.  If found, returns
 * "true".  A String with the original name of the class and the original
 * access flags are returned through the arguments.  (The name will be NULL
 * for an anonymous inner class.)
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
/*
 * 从一个注解中获取Enclosing属性.如果找到了，则返回true.通过参数 类的原始名称字符串和
 * 原始访问标识将被返回(对于匿名内部类，这个名字将是NULL)
 */
bool dvmGetInnerClass(const ClassObject* clazz, StringObject** pName,
    int* pAccessFlags)
{
    const DexAnnotationItem* pAnnoItem;
    const DexAnnotationSetItem* pAnnoSet;

    pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL)
        return false;

    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrInnerClass,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL)
        return false;

    /*
     * The InnerClass annotation has two members, "String name" and
     * "int accessFlags".  We don't want to get the access flags as an
     * Integer, so we process that as a simple value.
     */
    const u1* ptr;
    ptr = searchEncodedAnnotation(clazz, pAnnoItem->annotation, "name");
    if (ptr == NULL) {
        ALOGW("InnerClass annotation lacks 'name' member");
        return false;
    }

    /* parse it into an Object */
    AnnotationValue avalue;
    if (!processAnnotationValue(clazz, &ptr, &avalue, kAllObjects)) {
        ALOGD("processAnnotationValue failed on InnerClass member 'name'");
        return false;
    }

    /* make sure it has the expected format */
    if (avalue.type != kDexAnnotationNull &&
        avalue.type != kDexAnnotationString)
    {
        ALOGW("InnerClass name has bad type (0x%02x, expected STRING or NULL)",
            avalue.type);
        return false;
    }

    *pName = (StringObject*) avalue.value.l;
    assert(*pName == NULL || (*pName)->clazz == gDvm.classJavaLangString);

    ptr = searchEncodedAnnotation(clazz, pAnnoItem->annotation, "accessFlags");
    if (ptr == NULL) {
        ALOGW("InnerClass annotation lacks 'accessFlags' member");
        return false;
    }

    /* parse it, verify the type */
    if (!processAnnotationValue(clazz, &ptr, &avalue, kAllRaw)) {
        ALOGW("InnerClass accessFlags parse failed");
        return false;
    }
    if (avalue.type != kDexAnnotationInt) {
        ALOGW("InnerClass value has wrong type (0x%02x, expected 0x%02x)",
            avalue.type, kDexAnnotationInt);
        return false;
    }

    *pAccessFlags = avalue.value.i;

    return true;
}

/*
 * Extract an array of Class objects from the MemberClasses annotation
 * for this class.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 *
 * Returns NULL if we don't find any member classes.
 */
 /*
  * 从MemberClasses注解中抽取Class[]，MemberClasses注解有一个成员'Class[] value'
  * 
  */
ArrayObject* dvmGetDeclaredClasses(const ClassObject* clazz)
{
    const DexAnnotationSetItem* pAnnoSet;
    const DexAnnotationItem* pAnnoItem;
    Object* obj;

    pAnnoSet = findAnnotationSetForClass(clazz);
    if (pAnnoSet == NULL)
        return NULL;

    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrMemberClasses,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL)
        return NULL;

    /*
   * The MemberClasses annotation has one member, "Class[] value".
   */
    obj = getAnnotationValue(clazz, pAnnoItem, "value",
            kDexAnnotationArray, "MemberClasses");
    if (obj == GAV_FAILED)
        return NULL;
    assert(dvmIsArray((ArrayObject*)obj));
    obj = convertReturnType(obj, gDvm.classJavaLangClassArray);
    return (ArrayObject*)obj;
}


/*
 * ===========================================================================
 *      Method (and Constructor)
 * ===========================================================================
 */

/*
 * Compare the attributes (class name, method name, method signature) of
 * the specified method to "method".
 */
/*
 * 指定方法到'method'比较属性（类名，方法名，方法签名）
 */
static int compareMethodStr(DexFile* pDexFile, u4 methodIdx,
    const Method* method)
{
    const DexMethodId* pMethodId = dexGetMethodId(pDexFile, methodIdx);
    const char* str = dexStringByTypeIdx(pDexFile, pMethodId->classIdx);
    int result = strcmp(str, method->clazz->descriptor);

    if (result == 0) {
        str = dexStringById(pDexFile, pMethodId->nameIdx);
        result = strcmp(str, method->name);
        if (result == 0) {
            DexProto proto;
            dexProtoSetFromMethodId(&proto, pDexFile, pMethodId);
            result = dexProtoCompare(&proto, &method->prototype);
        }
    }

    return result;
}

/*
 * Given a method, determine the method's index.
 *
 * We could simply store this in the Method*, but that would cost 4 bytes
 * per method.  Instead we plow through the DEX data.
 *
 * We have two choices: look through the class method data, or look through
 * the global method_ids table.  The former is awkward because the method
 * could have been defined in a superclass or interface.  The latter works
 * out reasonably well because it's in sorted order, though we're still left
 * doing a fair number of string comparisons.
 */
 �/*
  * 给定一个方法来确定方法的索引
  */
static u4 getMethodIdx(const Method* method)
{
    DexFile* pDexFile = method->clazz->pDvmDex->pDexFile;
    u4 hi = pDexFile->pHeader->methodIdsSize -1;
    u4 lo = 0;
    u4 cur;

    while (hi >= lo) {
        int cmp;
        cur = (lo + hi) / 2;

        cmp = compareMethodStr(pDexFile, cur, method);
        if (cmp < 0) {
            lo = cur + 1;
        } else if (cmp > 0) {
            hi = cur - 1;
        } else {
            break;
        }
    }

    if (hi < lo) {
        /* this should be impossible -- the method came out of this DEX */
        char* desc = dexProtoCopyMethodDescriptor(&method->prototype);
        ALOGE("Unable to find method %s.%s %s in DEX file!",
            method->clazz->descriptor, method->name, desc);
        free(desc);
        dvmAbort();
    }

    return cur;
}

/*
 * Find the DexAnnotationSetItem for this method.
 *
 * Returns NULL if none found.
 */
/*
 * 根据'method'返回DexAnnotationSetItem
 */
static const DexAnnotationSetItem* findAnnotationSetForMethod(
    const Method* method)
{
    ClassObject* clazz = method->clazz;
    DexFile* pDexFile;
    const DexAnnotationsDirectoryItem* pAnnoDir;
    const DexMethodAnnotationsItem* pMethodList;
    const DexAnnotationSetItem* pAnnoSet = NULL;

    if (clazz->pDvmDex == NULL)         /* generated class (Proxy, array) */
        return NULL;
    pDexFile = clazz->pDvmDex->pDexFile;

    pAnnoDir = getAnnoDirectory(pDexFile, clazz);
    if (pAnnoDir != NULL) {
        pMethodList = dexGetMethodAnnotations(pDexFile, pAnnoDir);
        if (pMethodList != NULL) {
            /*
             * Run through the list and find a matching method.  We compare the
             * method ref indices in the annotation list with the method's DEX
             * method_idx value.
             *
             * TODO: use a binary search for long lists
             *
             * Alternate approach: for each entry in the annotations list,
             * find the method definition in the DEX file and perform string
             * comparisons on class name, method name, and signature.
             */
            u4 methodIdx = getMethodIdx(method);
            u4 count = dexGetMethodAnnotationsSize(pDexFile, pAnnoDir);
            u4 idx;

            for (idx = 0; idx < count; idx++) {
                if (pMethodList[idx].methodIdx == methodIdx) {
                    /* found! */
                    pAnnoSet = dexGetMethodAnnotationSetItem(pDexFile,
                                    &pMethodList[idx]);
                    break;
                }
            }
        }
    }

    return pAnnoSet;
}

/*
 * Return an array of Annotation objects for the method.  Returns an empty
 * array if there are no annotations.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 *
 * On allocation failure, this returns NULL with an exception raised.
 */
 /*
 * 根据'method'返回注解数组对象，如果这儿没有注解则返回空
 */
ArrayObject* dvmGetMethodAnnotations(const Method* method)
{
    ClassObject* clazz = method->clazz;
    const DexAnnotationSetItem* pAnnoSet;
    ArrayObject* annoArray = NULL;

    pAnnoSet = findAnnotationSetForMethod(method);
    if (pAnnoSet == NULL) {
        /* no matching annotations found */
        annoArray = emptyAnnoArray();
    } else {
        annoArray = processAnnotationSet(clazz, pAnnoSet,kDexVisibilityRuntime);
    }

    return annoArray;
}

/*
 * Returns the annotation or NULL if it doesn't exist.
 */
/*
 * 获取注解集'pAnnoSet'中kDexVisibilityRuntime类型的注解对象
 */
Object* dvmGetMethodAnnotation(const ClassObject* clazz, const Method* method,
        const ClassObject* annotationClazz)
{
    const DexAnnotationSetItem* pAnnoSet = findAnnotationSetForMethod(method);
    if (pAnnoSet == NULL) {
        return NULL;
    }
    return getAnnotationObjectFromAnnotationSet(clazz, pAnnoSet,
            kDexVisibilityRuntime, annotationClazz);
}

/*
 * Returns true if the annotation exists.
 */
bool dvmIsMethodAnnotationPresent(const ClassObject* clazz,
        const Method* method, const ClassObject* annotationClazz)
{
    const DexAnnotationSetItem* pAnnoSet = findAnnotationSetForMethod(method);
    if (pAnnoSet == NULL) {
        return NULL;
    }
    const DexAnnotationItem* pAnnoItem = getAnnotationItemFromAnnotationSet(
            clazz, pAnnoSet, kDexVisibilityRuntime, annotationClazz);
    return (pAnnoItem != NULL);
}

/*
 * Retrieve the Signature annotation, if any.  Returns NULL if no signature
 * exists.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
/*
 * 获取注解签名，如果没有签名存在则返回NULL
 */
ArrayObject* dvmGetMethodSignatureAnnotation(const Method* method)
{
    ClassObject* clazz = method->clazz;
    const DexAnnotationSetItem* pAnnoSet;
    ArrayObject* signature = NULL;

    pAnnoSet = findAnnotationSetForMethod(method);
    if (pAnnoSet != NULL)
        signature = getSignatureValue(clazz, pAnnoSet);

    return signature;
}

/*
 * Extract an array of exception classes from the "system" annotation list
 * for this method.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 *
 * Returns NULL if we don't find any exceptions for this method.
 */
/*
 * 从'system'注解列表中提取异常数组对象
 *
 * Throws注解有一个成员'Class[] value'
 */
ArrayObject* dvmGetMethodThrows(const Method* method)
{
    ClassObject* clazz = method->clazz;
    const DexAnnotationSetItem* pAnnoSet;
    const DexAnnotationItem* pAnnoItem;

    /* find the set for this method */
    pAnnoSet = findAnnotationSetForMethod(method);
    if (pAnnoSet == NULL)
        return NULL;        /* nothing for this method */

    /* find the "Throws" annotation, if any */
    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrThrows,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL)
        return NULL;        /* no Throws */

    /*
     * The Throws annotation has one member, "Class[] value".
     */
    Object* obj = getAnnotationValue(clazz, pAnnoItem, "value",
        kDexAnnotationArray, "Throws");
    if (obj == GAV_FAILED)
        return NULL;
    assert(dvmIsArray((ArrayObject*)obj));
    obj = convertReturnType(obj, gDvm.classJavaLangClassArray);
    return (ArrayObject*)obj;
}

/*
 * Given an Annotation's method, find the default value, if any.
 *
 * If this is a CLASS annotation, and we can't find a match for the
 * default class value, we need to throw a TypeNotPresentException.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
Object* dvmGetAnnotationDefaultValue(const Method* method)
{
    const ClassObject* clazz = method->clazz;
    DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    const DexAnnotationsDirectoryItem* pAnnoDir;
    const DexAnnotationSetItem* pAnnoSet = NULL;

    /*
     * The method's declaring class (the annotation) will have an
     * AnnotationDefault "system" annotation associated with it if any
     * of its methods have default values.  Start by finding the
     * DexAnnotationItem associated with the class.
     */
    pAnnoDir = getAnnoDirectory(pDexFile, clazz);
    if (pAnnoDir != NULL)
        pAnnoSet = dexGetClassAnnotationSet(pDexFile, pAnnoDir);
    if (pAnnoSet == NULL) {
        /* no annotations for anything in class, or no class annotations */
        return NULL;
    }

    /* find the "AnnotationDefault" annotation, if any */
    const DexAnnotationItem* pAnnoItem;
    pAnnoItem = searchAnnotationSet(clazz, pAnnoSet, kDescrAnnotationDefault,
        kDexVisibilitySystem);
    if (pAnnoItem == NULL) {
        /* no default values for any member in this annotation */
        //printf("##### no default annotations for %s.%s\n",
        //    method->clazz->descriptor, method->name);
        return NULL;
    }

    /*
     * The AnnotationDefault annotation has one member, "Annotation value".
     * We need to pull that out.
     */
    const u1* ptr;
    ptr = searchEncodedAnnotation(clazz, pAnnoItem->annotation, "value");
    if (ptr == NULL) {
        ALOGW("AnnotationDefault annotation lacks 'value'");
        return NULL;
    }
    if ((*ptr & kDexAnnotationValueTypeMask) != kDexAnnotationAnnotation) {
        ALOGW("AnnotationDefault value has wrong type (0x%02x)",
            *ptr & kDexAnnotationValueTypeMask);
        return NULL;
    }

    /*
     * The value_type byte for VALUE_ANNOTATION is followed by
     * encoded_annotation data.  We want to scan through it to find an
     * entry whose name matches our method name.
     */
    ptr++;
    ptr = searchEncodedAnnotation(clazz, ptr, method->name);
    if (ptr == NULL)
        return NULL;        /* no default annotation for this method */

    /* got it, pull it out */
    AnnotationValue avalue;
    if (!processAnnotationValue(clazz, &ptr, &avalue, kAllObjects)) {
        ALOGD("processAnnotationValue failed on default for '%s'",
            method->name);
        return NULL;
    }

    /* convert the return type, if necessary */
    ClassObject* methodReturn = dvmGetBoxedReturnType(method);
    Object* obj = (Object*)avalue.value.l;
    obj = convertReturnType(obj, methodReturn);

    return obj;
}


/*
 * ===========================================================================
 *      Field
 * ===========================================================================
 */

/*
 * Compare the attributes (class name, field name, field signature) of
 * the specified field to "field".
 */
/*
 * 指定字段'field',比较属性(类名，字段名，字段签名)
 */
static int compareFieldStr(DexFile* pDexFile, u4 idx, const Field* field)
{
    const DexFieldId* pFieldId = dexGetFieldId(pDexFile, idx);
    const char* str = dexStringByTypeIdx(pDexFile, pFieldId->classIdx);
    int result = strcmp(str, field->clazz->descriptor);

    if (result == 0) {
        str = dexStringById(pDexFile, pFieldId->nameIdx);
        result = strcmp(str, field->name);
        if (result == 0) {
            str = dexStringByTypeIdx(pDexFile, pFieldId->typeIdx);
            result = strcmp(str, field->signature);
        }
    }

    return result;
}

/*
 * Given a field, determine the field's index.
 *
 * This has the same tradeoffs as getMethodIdx.
 */
/*
 * 给定一个字段来确定字段的索引
 */
static u4 getFieldIdx(const Field* field)
{
    DexFile* pDexFile = field->clazz->pDvmDex->pDexFile;
    u4 hi = pDexFile->pHeader->fieldIdsSize -1;
    u4 lo = 0;
    u4 cur;

    while (hi >= lo) {
        int cmp;
        cur = (lo + hi) / 2;

        cmp = compareFieldStr(pDexFile, cur, field);
        if (cmp < 0) {
            lo = cur + 1;
        } else if (cmp > 0) {
            hi = cur - 1;
        } else {
            break;
        }
    }

    if (hi < lo) {
        /* this should be impossible -- the field came out of this DEX */
        ALOGE("Unable to find field %s.%s %s in DEX file!",
            field->clazz->descriptor, field->name, field->signature);
        dvmAbort();
    }

    return cur;
}

/*
 * Find the DexAnnotationSetItem for this field.
 *
 * Returns NULL if none found.
 */
/*
 * 根据'field'返回DexAnnotationSetItem
 */
static const DexAnnotationSetItem* findAnnotationSetForField(const Field* field)
{
    ClassObject* clazz = field->clazz;
    DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    const DexAnnotationsDirectoryItem* pAnnoDir;
    const DexFieldAnnotationsItem* pFieldList;

    pAnnoDir = getAnnoDirectory(pDexFile, clazz);
    if (pAnnoDir == NULL)
        return NULL;

    pFieldList = dexGetFieldAnnotations(pDexFile, pAnnoDir);
    if (pFieldList == NULL)
        return NULL;

    /*
     * Run through the list and find a matching field.  We compare the
     * field ref indices in the annotation list with the field's DEX
     * field_idx value.
     *
     * TODO: use a binary search for long lists
     *
     * Alternate approach: for each entry in the annotations list,
     * find the field definition in the DEX file and perform string
     * comparisons on class name, field name, and signature.
     */
    u4 fieldIdx = getFieldIdx(field);
    u4 count = dexGetFieldAnnotationsSize(pDexFile, pAnnoDir);
    u4 idx;

    for (idx = 0; idx < count; idx++) {
        if (pFieldList[idx].fieldIdx == fieldIdx) {
            /* found! */
            return dexGetFieldAnnotationSetItem(pDexFile, &pFieldList[idx]);
        }
    }

    return NULL;
}

/*
 * Return an array of Annotation objects for the field.  Returns an empty
 * array if there are no annotations.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 *
 * On allocation failure, this returns NULL with an exception raised.
 */
 /*
 * 根据'field'返回注解数组对象，如果这儿没有注解则返回空
 */
ArrayObject* dvmGetFieldAnnotations(const Field* field)
{
    ClassObject* clazz = field->clazz;
    ArrayObject* annoArray = NULL;
    const DexAnnotationSetItem* pAnnoSet = NULL;

    pAnnoSet = findAnnotationSetForField(field);
    if (pAnnoSet == NULL) {
        /* no matching annotations found */
        annoArray = emptyAnnoArray();
    } else {
        annoArray = processAnnotationSet(clazz, pAnnoSet,
                        kDexVisibilityRuntime);
    }

    return annoArray;
}

/*
 * Returns the annotation or NULL if it doesn't exist.
 */
/*
 * 获取注解集'pAnnoSet'中kDexVisibilityRuntime类型的注解对象
 */
Object* dvmGetFieldAnnotation(const ClassObject* clazz, const Field* field,
        const ClassObject* annotationClazz)
{
    const DexAnnotationSetItem* pAnnoSet = findAnnotationSetForField(field);
    if (pAnnoSet == NULL) {
        return NULL;
    }
    return getAnnotationObjectFromAnnotationSet(clazz, pAnnoSet,
            kDexVisibilityRuntime, annotationClazz);
}

/*
 * Returns true if the annotation exists.
 */
bool dvmIsFieldAnnotationPresent(const ClassObject* clazz,
        const Field* field, const ClassObject* annotationClazz)
{
    const DexAnnotationSetItem* pAnnoSet = findAnnotationSetForField(field);
    if (pAnnoSet == NULL) {
        return NULL;
    }
    const DexAnnotationItem* pAnnoItem = getAnnotationItemFromAnnotationSet(
            clazz, pAnnoSet, kDexVisibilityRuntime, annotationClazz);
    return (pAnnoItem != NULL);
}

/*
 * Retrieve the Signature annotation, if any.  Returns NULL if no signature
 * exists.
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
/*
 * 获取注解签名，如果没有签名存在则返回NULL
 */
ArrayObject* dvmGetFieldSignatureAnnotation(const Field* field)
{
    ClassObject* clazz = field->clazz;
    const DexAnnotationSetItem* pAnnoSet;
    ArrayObject* signature = NULL;

    pAnnoSet = findAnnotationSetForField(field);
    if (pAnnoSet != NULL)
        signature = getSignatureValue(clazz, pAnnoSet);

    return signature;
}


/*
 * ===========================================================================
 *      Parameter
 * ===========================================================================
 */

/*
 * We have an annotation_set_ref_list, which is essentially a list of
 * entries that we pass to processAnnotationSet().
 *
 * The returned object must be released with dvmReleaseTrackedAlloc.
 */
/*
 * 我们有一个annotation_set_ref_list，这本质上是一个条目列表，
 * 我们传递给processAnnotationSet()
 *
 * 返回的对象必须被释放dvmReleaseTrackedAlloc
 */
static ArrayObject* processAnnotationSetRefList(const ClassObject* clazz,
    const DexAnnotationSetRefList* pAnnoSetList, u4 count)
{
    DexFile* pDexFile = clazz->pDvmDex->pDexFile;
    ArrayObject* annoArrayArray = NULL;
    u4 idx;

    /* allocate an array of Annotation arrays to hold results */
    annoArrayArray = dvmAllocArrayByClass(
        gDvm.classJavaLangAnnotationAnnotationArrayArray, count, ALLOC_DEFAULT);
    if (annoArrayArray == NULL) {
        ALOGW("annotation set ref array alloc failed");
        goto bail;
    }

    for (idx = 0; idx < count; idx++) {
        Thread* self = dvmThreadSelf();
        const DexAnnotationSetRefItem* pItem;
        const DexAnnotationSetItem* pAnnoSet;
        Object *annoSet;
        DexAnnotationSetItem emptySet;
        emptySet.size = 0;

        pItem = dexGetParameterAnnotationSetRef(pAnnoSetList, idx);
        pAnnoSet = dexGetSetRefItemItem(pDexFile, pItem);
        if (pAnnoSet == NULL) {
            pAnnoSet = &emptySet;
        }

        annoSet = (Object *)processAnnotationSet(clazz,
                                                 pAnnoSet,
                                                 kDexVisibilityRuntime);
        if (annoSet == NULL) {
            ALOGW("processAnnotationSet failed");
            annoArrayArray = NULL;
            goto bail;
        }
        dvmSetObjectArrayElement(annoArrayArray, idx, annoSet);
        dvmReleaseTrackedAlloc((Object*) annoSet, self);
    }

bail:
    return annoArrayArray;
}

/*
 * Find the DexAnnotationSetItem for this parameter.
 *
 * Returns NULL if none found.
 */
/*
 * 通过'method'查找 DexAnnotationSetItem
 */
static const DexParameterAnnotationsItem* findAnnotationsItemForMethod(
    const Method* method)
{
    ClassObject* clazz = method->clazz;
    DexFile* pDexFile;
    const DexAnnotationsDirectoryItem* pAnnoDir;
    const DexParameterAnnotationsItem* pParameterList;

    if (clazz->pDvmDex == NULL)         /* generated class (Proxy, array) */
        return NULL;

    pDexFile = clazz->pDvmDex->pDexFile;
    pAnnoDir = getAnnoDirectory(pDexFile, clazz);
    if (pAnnoDir == NULL)
        return NULL;

    pParameterList = dexGetParameterAnnotations(pDexFile, pAnnoDir);
    if (pParameterList == NULL)
        return NULL;

    /*
     * Run through the list and find a matching method.  We compare the
     * method ref indices in the annotation list with the method's DEX
     * method_idx value.
     *
     * TODO: use a binary search for long lists
     *
     * Alternate approach: for each entry in the annotations list,
     * find the method definition in the DEX file and perform string
     * comparisons on class name, method name, and signature.
     */
    u4 methodIdx = getMethodIdx(method);
    u4 count = dexGetParameterAnnotationsSize(pDexFile, pAnnoDir);
    u4 idx;

    for (idx = 0; idx < count; idx++) {
        if (pParameterList[idx].methodIdx == methodIdx) {
            /* found! */
            return &pParameterList[idx];
        }
    }

    return NULL;
}


/*
 * Count up the number of arguments the method takes.  The "this" pointer
 * doesn't count.
 */
/*
 * 统计方法的参数数
 */
static int countMethodArguments(const Method* method)
{
    /* method->shorty[0] is the return type */
    return strlen(method->shorty + 1);
}

/*
 * Return an array of arrays of Annotation objects.  The outer array has
 * one entry per method parameter, the inner array has the list of annotations
 * associated with that parameter.
 * 返回一个的注解对象数组的数组,外部数组有方法参数条目，内部数组有与该参数有关的注释列表
 *
 * If the method has no parameters, we return an array of length zero.  If
 * the method has one or more parameters, we return an array whose length
 * is equal to the number of parameters; if a given parameter does not have
 * an annotation, the corresponding entry will be null.
 * 如果方法没有参数,我们返回一个零长度的数组.如果该方法有一个或者多个参数,
 * 那么返回一个数组，其长度等于参数的数目，如果一个给定的参数不具有一个注释，
 * 相应的条目将是空。
 *
 * Caller must call dvmReleaseTrackedAlloc().
 */
ArrayObject* dvmGetParameterAnnotations(const Method* method)
{
    ClassObject* clazz = method->clazz;
    const DexParameterAnnotationsItem* pItem;
    ArrayObject* annoArrayArray = NULL;

    pItem = findAnnotationsItemForMethod(method);
    if (pItem != NULL) {
        DexFile* pDexFile = clazz->pDvmDex->pDexFile;
        const DexAnnotationSetRefList* pAnnoSetList;
        u4 size;

        size = dexGetParameterAnnotationSetRefSize(pDexFile, pItem);
        pAnnoSetList = dexGetParameterAnnotationSetRefList(pDexFile, pItem);
        annoArrayArray = processAnnotationSetRefList(clazz, pAnnoSetList, size);
    } else {
        /* no matching annotations found */
        annoArrayArray = emptyAnnoArrayArray(countMethodArguments(method));
    }

    return annoArrayArray;
}


/*
 * ===========================================================================
 *      DexEncodedArray interpretation
 * ===========================================================================
 */

/**
 * Initializes an encoded array iterator.
 *
 * @param iterator iterator to initialize
 * @param encodedArray encoded array to iterate over
 * @param clazz class to use when resolving strings and types
 */
/*
 * 初始化一个加密数组迭代器
 */
void dvmEncodedArrayIteratorInitialize(EncodedArrayIterator* iterator,
        const DexEncodedArray* encodedArray, const ClassObject* clazz) {
    iterator->encodedArray = encodedArray;
    iterator->cursor = encodedArray->array;
    iterator->size = readUleb128(&iterator->cursor);
    iterator->elementsLeft = iterator->size;
    iterator->clazz = clazz;
}

/**
 * Returns whether there are more elements to be read.
 */
/*
 * 返回是否有更多的数据将被读取
 */
bool dvmEncodedArrayIteratorHasNext(const EncodedArrayIterator* iterator) {
    return (iterator->elementsLeft != 0);
}

/**
 * Returns the next decoded value from the iterator, advancing its
 * cursor. This returns primitive values in their corresponding union
 * slots, and returns everything else (including nulls) as object
 * references in the "l" union slot.
 *
 * The caller must call dvmReleaseTrackedAlloc() on any returned reference.
 *
 * @param value pointer to store decoded value into
 * @returns true if a value was decoded and the cursor advanced; false if
 * the last value had already been decoded or if there was a problem decoding
 */
/*
 * 移动它的索引.从迭代器中返回下一个解密值
 */
bool dvmEncodedArrayIteratorGetNext(EncodedArrayIterator* iterator,
        AnnotationValue* value) {
    bool processed;

    if (iterator->elementsLeft == 0) {
        return false;
    }

    processed = processAnnotationValue(iterator->clazz, &iterator->cursor,
            value, kPrimitivesOrObjects);

    if (! processed) {
        ALOGE("Failed to process array element %d from %p",
                iterator->size - iterator->elementsLeft,
                iterator->encodedArray);
        iterator->elementsLeft = 0;
        return false;
    }

    iterator->elementsLeft--;
    return true;
}
