/*
 * Main interpreter loop.
 *
 * This was written with an ARM implementation in mind.
 * portable类型的解释器的解释循环入口
 */
void dvmInterpretPortable(Thread* self)
{
#if defined(EASY_GDB)
    // 这里仅仅是方便调试，保存下栈帧的地址
    StackSaveArea* debugSaveArea = SAVEAREA_FROM_FP(self->interpSave.curFrame);
#endif
    DvmDex* methodClassDex;     // curMethod->clazz->pDvmDex
    JValue retval;

    /* core state */
    const Method* curMethod;    // method we're interpreting 当前我们要解释的方法
    const u2* pc;               // program counter 程序计数器
    u4* fp;                     // frame pointer 帧指针
    u2 inst;                    // current instruction 当前指令

	
    /* instruction decoding */
    u4 ref;                     // 16 or 32-bit quantity fetched directly
    u2 vsrc1, vsrc2, vdst;      // usually used for register indexes
    
    /* method call setup */
    const Method* methodToCall;
    bool methodCallRange;

    /* 
    * static computed goto table
    * 静态计算好的跳转表
    * 实际上就是定义好的一个数�
    * 该静态跳转表在libdex的dexopcode.h中定义
    * [需要注意的叔：该表只提供给纯c实现的解释器来使用]
    * 解开宏后是这样的
    *  static const void* handlerTable[0x100] = {                      \
    *    H(OP_NOP),                                                            \
    *    H(OP_MOVE),                                                           \
    *    ....
    *  }
    * 这个表是由opcode-gen这个工具动态生成的，具体说这个工具依据什么来生成的，需要参看该工具的实现
    *
    * # define H(_op)             &&op_##_op
    * 实际上这个表里面存放了&&op_OP_NOP 这样的地址
    */
    DEFINE_GOTO_TABLE(handlerTable);

    /* copy state in 
    * 初始化一些状态值
    */
    curMethod = self->interpSave.method;
    pc = self->interpSave.pc;
    fp = self->interpSave.curFrame;
    retval = self->interpSave.retval;   /* only need for kInterpEntryReturn? */

    methodClassDex = curMethod->clazz->pDvmDex; //获取dex相关的数据(这里需要参考vm\DvmDex.cpp里面的相关实现)

    LOGVV("threadid=%d: %s.%s pc=%#x fp=%p",
        self->threadId, curMethod->clazz->descriptor, curMethod->name,
        pc - curMethod->insns, fp);

    /*
     * Handle any ongoing profiling and prep for debugging.
     * 下面主要是方便调试，表明进入了一个具体方法的解析
     */
    if (self->interpBreak.ctl.subMode != 0) {
        TRACE_METHOD_ENTER(self, curMethod);
        self->debugIsMethodEntry = true;   // Always true on startup
    }
    /*
     * DEBUG: scramble this to ensure we're not relying on it.
     */
    methodToCall = (const Method*) -1;

#if 0
    if (self->debugIsMethodEntry) {
        ILOGD("|-- Now interpreting %s.%s", curMethod->clazz->descriptor,
                curMethod->name);
        DUMP_REGS(curMethod, self->interpSave.curFrame, false);
    }
#endif

    //从这里开始进入取指令，执行，返回的阶段
    // 从这里实际上进入了一个do - while的循环，直到执行完毕返回
    FINISH(0);                  /* fetch and execute first instruction */
    //这里往下就没有东西了，但是也没有} ，说这个地方后面还有东西
    //由于这个文件中的代码都是将来要通过配置文件复制拼接到最终的解释器代码文件中的
    //所以可想而知这里后面就是具体的字节码的解释例程
    //上面的这一句只是从第一条指令开始，不断的调整pc指向，直到找到要解释的方法所包含的指令
    

/*--- start of opcodes ---*/
