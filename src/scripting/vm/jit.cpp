
#include "jit.h"
#include "jitintern.h"

extern PString *TypeString;
extern PStruct *TypeVector2;
extern PStruct *TypeVector3;

static void OutputJitLog(const asmjit::StringLogger &logger);

JitFuncPtr JitCompile(VMScriptFunction *sfunc)
{
#if 0
	if (strcmp(sfunc->PrintableName.GetChars(), "StatusScreen.drawNum") != 0)
		return nullptr;
#endif

	using namespace asmjit;
	StringLogger logger;
	try
	{
		ThrowingErrorHandler errorHandler;
		CodeHolder code;
		code.init(GetHostCodeInfo());
		code.setErrorHandler(&errorHandler);
		code.setLogger(&logger);

		JitCompiler compiler(&code, sfunc);
		CCFunc *func = compiler.Codegen();

		return reinterpret_cast<JitFuncPtr>(AddJitFunction(&code, func));
	}
	catch (const std::exception &e)
	{
		OutputJitLog(logger);
		I_FatalError("Unexpected JIT error: %s\n", e.what());
		return nullptr;
	}
}

void JitDumpLog(FILE *file, VMScriptFunction *sfunc)
{
	using namespace asmjit;
	StringLogger logger;
	try
	{
		ThrowingErrorHandler errorHandler;
		CodeHolder code;
		code.init(GetHostCodeInfo());
		code.setErrorHandler(&errorHandler);
		code.setLogger(&logger);

		JitCompiler compiler(&code, sfunc);
		compiler.Codegen();

		fwrite(logger.getString(), logger.getLength(), 1, file);
	}
	catch (const std::exception &e)
	{
		fwrite(logger.getString(), logger.getLength(), 1, file);

		FString err;
		err.Format("Unexpected JIT error: %s\n", e.what());
		fwrite(err.GetChars(), err.Len(), 1, file);
		fclose(file);

		I_FatalError("Unexpected JIT error: %s\n", e.what());
	}
}

static void OutputJitLog(const asmjit::StringLogger &logger)
{
	// Write line by line since I_FatalError seems to cut off long strings
	const char *pos = logger.getString();
	const char *end = pos;
	while (*end)
	{
		if (*end == '\n')
		{
			FString substr(pos, (int)(ptrdiff_t)(end - pos));
			Printf("%s\n", substr.GetChars());
			pos = end + 1;
		}
		end++;
	}
	if (pos != end)
		Printf("%s\n", pos);
}

/////////////////////////////////////////////////////////////////////////////

static const char *OpNames[NUM_OPS] =
{
#define xx(op, name, mode, alt, kreg, ktype)	#op,
#include "vmops.h"
#undef xx
};

asmjit::CCFunc *JitCompiler::Codegen()
{
	Setup();

	pc = sfunc->Code;
	auto end = pc + sfunc->CodeSize;
	while (pc != end)
	{
		int i = (int)(ptrdiff_t)(pc - sfunc->Code);
		op = pc->op;

		if (op != OP_PARAM && op != OP_PARAMI && op != OP_VTBL)
		{
			FString lineinfo;
			lineinfo.Format("; line %d: %02x%02x%02x%02x %s", sfunc->PCToLine(pc), pc->op, pc->a, pc->b, pc->c, OpNames[op]);
			cc.comment("", 0);
			cc.comment(lineinfo.GetChars(), lineinfo.Len());
		}

		labels[i].cursor = cc.getCursor();
		ResetTemp();
		EmitOpcode();

		pc++;
	}

	BindLabels();

	cc.endFunc();
	cc.finalize();

	return func;
}

void JitCompiler::EmitOpcode()
{
	switch (op)
	{
		#define xx(op, name, mode, alt, kreg, ktype)	case OP_##op: Emit##op(); break;
		#include "vmops.h"
		#undef xx

	default:
		I_FatalError("JIT error: Unknown VM opcode %d\n", op);
		break;
	}
}

void JitCompiler::BindLabels()
{
	asmjit::CBNode *cursor = cc.getCursor();
	unsigned int size = labels.Size();
	for (unsigned int i = 0; i < size; i++)
	{
		const OpcodeLabel &label = labels[i];
		if (label.inUse)
		{
			cc.setCursor(label.cursor);
			cc.bind(label.label);
		}
	}
	cc.setCursor(cursor);
}

void JitCompiler::CheckVMFrame()
{
	if (!vmframeAllocated)
	{
		auto cursor = cc.getCursor();
		cc.setCursor(vmframeCursor);

		auto vmstack = cc.newStack(sfunc->StackSize, 16, "vmstack");
		vmframe = cc.newIntPtr("vmframe");
		cc.lea(vmframe, vmstack);

		cc.setCursor(cursor);
		vmframeAllocated = true;
	}
}

asmjit::X86Gp JitCompiler::GetCallReturns()
{
	if (!callReturnsAllocated)
	{
		auto cursor = cc.getCursor();
		cc.setCursor(callReturnsCursor);
		auto stackalloc = cc.newStack(sizeof(VMReturn) * MAX_RETURNS, alignof(VMReturn), "stackalloc");
		callReturns = cc.newIntPtr("callReturns");
		cc.lea(callReturns, stackalloc);
		cc.setCursor(cursor);
		callReturnsAllocated = true;
	}
	return callReturns;
}

void JitCompiler::Setup()
{
	using namespace asmjit;

	ResetTemp();

	static const char *marks = "=======================================================";
	cc.comment("", 0);
	cc.comment(marks, 56);

	FString funcname;
	funcname.Format("Function: %s", sfunc->PrintableName.GetChars());
	cc.comment(funcname.GetChars(), funcname.Len());

	cc.comment(marks, 56);
	cc.comment("", 0);

	auto unusedFunc = cc.newIntPtr("func"); // VMFunction*
	args = cc.newIntPtr("args"); // VMValue *params
	numargs = cc.newInt32("numargs"); // int numargs
	ret = cc.newIntPtr("ret"); // VMReturn *ret
	numret = cc.newInt32("numret"); // int numret

	func = cc.addFunc(FuncSignature5<int, VMFunction *, void *, int, void *, int>());
	cc.setArg(0, unusedFunc);
	cc.setArg(1, args);
	cc.setArg(2, numargs);
	cc.setArg(3, ret);
	cc.setArg(4, numret);

	callReturnsCursor = cc.getCursor();

	konstd = sfunc->KonstD;
	konstf = sfunc->KonstF;
	konsts = sfunc->KonstS;
	konsta = sfunc->KonstA;

	labels.Resize(sfunc->CodeSize);

	CreateRegisters();
	IncrementVMCalls();
	SetupFrame();
}

void JitCompiler::SetupFrame()
{
	// the VM version reads this from the stack, but it is constant data
	offsetParams = ((int)sizeof(VMFrame) + 15) & ~15;
	offsetF = offsetParams + (int)(sfunc->MaxParam * sizeof(VMValue));
	offsetS = offsetF + (int)(sfunc->NumRegF * sizeof(double));
	offsetA = offsetS + (int)(sfunc->NumRegS * sizeof(FString));
	offsetD = offsetA + (int)(sfunc->NumRegA * sizeof(void*));
	offsetExtra = (offsetD + (int)(sfunc->NumRegD * sizeof(int32_t)) + 15) & ~15;

	if (sfunc->SpecialInits.Size() == 0 && sfunc->NumRegS == 0)
	{
		SetupSimpleFrame();
	}
	else
	{
		SetupFullVMFrame();
	}
}

void JitCompiler::SetupSimpleFrame()
{
	using namespace asmjit;

	// This is a simple frame with no constructors or destructors. Allocate it on the stack ourselves.

	vmframeCursor = cc.getCursor();

	int argsPos = 0;
	int regd = 0, regf = 0, rega = 0;
	for (unsigned int i = 0; i < sfunc->Proto->ArgumentTypes.Size(); i++)
	{
		const PType *type = sfunc->Proto->ArgumentTypes[i];
		if (sfunc->ArgFlags[i] & (VARF_Out | VARF_Ref))
		{
			cc.mov(regA[rega++], x86::ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, a)));
		}
		else if (type == TypeVector2)
		{
			cc.movsd(regF[regf++], x86::qword_ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f)));
			cc.movsd(regF[regf++], x86::qword_ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f)));
		}
		else if (type == TypeVector3)
		{
			cc.movsd(regF[regf++], x86::qword_ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f)));
			cc.movsd(regF[regf++], x86::qword_ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f)));
			cc.movsd(regF[regf++], x86::qword_ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f)));
		}
		else if (type == TypeFloat64)
		{
			cc.movsd(regF[regf++], x86::qword_ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f)));
		}
		else if (type == TypeString)
		{
			I_FatalError("JIT: Strings are not supported yet for simple frames");
		}
		else if (type->isIntCompatible())
		{
			cc.mov(regD[regd++], x86::dword_ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, i)));
		}
		else
		{
			cc.mov(regA[rega++], x86::ptr(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, a)));
		}
	}

	if (sfunc->NumArgs != argsPos || regd > sfunc->NumRegD || regf > sfunc->NumRegF || rega > sfunc->NumRegA)
		I_FatalError("JIT: sfunc->NumArgs != argsPos || regd > sfunc->NumRegD || regf > sfunc->NumRegF || rega > sfunc->NumRegA");

	for (int i = regd; i < sfunc->NumRegD; i++)
		cc.xor_(regD[i], regD[i]);

	for (int i = regf; i < sfunc->NumRegF; i++)
		cc.xorpd(regF[i], regF[i]);

	for (int i = rega; i < sfunc->NumRegA; i++)
		cc.xor_(regA[i], regA[i]);
}

static VMFrameStack *CreateFullVMFrame(VMScriptFunction *func, VMValue *args, int numargs)
{
	try
	{
		VMFrameStack *stack = &GlobalVMStack;
		VMFrame *newf = stack->AllocFrame(func);
		CurrentJitExceptInfo->vmframes++;
		VMFillParams(args, newf, numargs);
		return stack;
	}
	catch (...)
	{
		VMThrowException(std::current_exception());
		return nullptr;
	}
}

void JitCompiler::SetupFullVMFrame()
{
	using namespace asmjit;

	stack = cc.newIntPtr("stack");
	auto allocFrame = CreateCall<VMFrameStack *, VMScriptFunction *, VMValue *, int>(CreateFullVMFrame);
	allocFrame->setRet(0, stack);
	allocFrame->setArg(0, imm_ptr(sfunc));
	allocFrame->setArg(1, args);
	allocFrame->setArg(2, numargs);

	vmframe = cc.newIntPtr("vmframe");
	cc.mov(vmframe, x86::ptr(stack)); // stack->Blocks
	cc.mov(vmframe, x86::ptr(vmframe, VMFrameStack::OffsetLastFrame())); // Blocks->LastFrame
	vmframeAllocated = true;

	for (int i = 0; i < sfunc->NumRegD; i++)
		cc.mov(regD[i], x86::dword_ptr(vmframe, offsetD + i * sizeof(int32_t)));

	for (int i = 0; i < sfunc->NumRegF; i++)
		cc.movsd(regF[i], x86::qword_ptr(vmframe, offsetF + i * sizeof(double)));

	for (int i = 0; i < sfunc->NumRegS; i++)
		cc.lea(regS[i], x86::ptr(vmframe, offsetS + i * sizeof(FString)));

	for (int i = 0; i < sfunc->NumRegA; i++)
		cc.mov(regA[i], x86::ptr(vmframe, offsetA + i * sizeof(void*)));
}

static void PopFullVMFrame(VMFrameStack *stack)
{
	try
	{
		stack->PopFrame();
		CurrentJitExceptInfo->vmframes--;
	}
	catch (...)
	{
		VMThrowException(std::current_exception());
	}
}

void JitCompiler::EmitPopFrame()
{
	if (sfunc->SpecialInits.Size() != 0 || sfunc->NumRegS != 0)
	{
		auto popFrame = CreateCall<void, VMFrameStack *>(PopFullVMFrame);
		popFrame->setArg(0, stack);
	}
}

void JitCompiler::IncrementVMCalls()
{
	// VMCalls[0]++
	auto vmcallsptr = newTempIntPtr();
	auto vmcalls = newTempInt32();
	cc.mov(vmcallsptr, asmjit::imm_ptr(VMCalls));
	cc.mov(vmcalls, asmjit::x86::dword_ptr(vmcallsptr));
	cc.add(vmcalls, (int)1);
	cc.mov(asmjit::x86::dword_ptr(vmcallsptr), vmcalls);
}

void JitCompiler::CreateRegisters()
{
	regD.Resize(sfunc->NumRegD);
	regF.Resize(sfunc->NumRegF);
	regA.Resize(sfunc->NumRegA);
	regS.Resize(sfunc->NumRegS);

	for (int i = 0; i < sfunc->NumRegD; i++)
	{
		regname.Format("regD%d", i);
		regD[i] = cc.newInt32(regname.GetChars());
	}

	for (int i = 0; i < sfunc->NumRegF; i++)
	{
		regname.Format("regF%d", i);
		regF[i] = cc.newXmmSd(regname.GetChars());
	}

	for (int i = 0; i < sfunc->NumRegS; i++)
	{
		regname.Format("regS%d", i);
		regS[i] = cc.newIntPtr(regname.GetChars());
	}

	for (int i = 0; i < sfunc->NumRegA; i++)
	{
		regname.Format("regA%d", i);
		regA[i] = cc.newIntPtr(regname.GetChars());
	}
}

void JitCompiler::EmitNullPointerThrow(int index, EVMAbortException reason)
{
	auto label = EmitThrowExceptionLabel(reason);
	cc.test(regA[index], regA[index]);
	cc.je(label);
}

void JitCompiler::ThrowException(VMScriptFunction *func, VMOP *line, int reason)
{
	try
	{
		ThrowAbortException(func, line, (EVMAbortException)reason, nullptr);
	}
	catch (...)
	{
		VMThrowException(std::current_exception());
	}
}

void JitCompiler::EmitThrowException(EVMAbortException reason)
{
	auto call = CreateCall<void, VMScriptFunction *, VMOP *, int>(&JitCompiler::ThrowException);
	call->setArg(0, asmjit::imm_ptr(sfunc));
	call->setArg(1, asmjit::imm_ptr(pc));
	call->setArg(2, asmjit::imm(reason));
}

asmjit::Label JitCompiler::EmitThrowExceptionLabel(EVMAbortException reason)
{
	auto label = cc.newLabel();
	auto cursor = cc.getCursor();
	cc.bind(label);
	EmitThrowException(reason);
	cc.setCursor(cursor);
	return label;
}

asmjit::X86Gp JitCompiler::CheckRegD(int r0, int r1)
{
	if (r0 != r1)
	{
		return regD[r0];
	}
	else
	{
		auto copy = newTempInt32();
		cc.mov(copy, regD[r0]);
		return copy;
	}
}

asmjit::X86Xmm JitCompiler::CheckRegF(int r0, int r1)
{
	if (r0 != r1)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempXmmSd();
		cc.movsd(copy, regF[r0]);
		return copy;
	}
}

asmjit::X86Xmm JitCompiler::CheckRegF(int r0, int r1, int r2)
{
	if (r0 != r1 && r0 != r2)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempXmmSd();
		cc.movsd(copy, regF[r0]);
		return copy;
	}
}

asmjit::X86Xmm JitCompiler::CheckRegF(int r0, int r1, int r2, int r3)
{
	if (r0 != r1 && r0 != r2 && r0 != r3)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempXmmSd();
		cc.movsd(copy, regF[r0]);
		return copy;
	}
}

asmjit::X86Gp JitCompiler::CheckRegS(int r0, int r1)
{
	if (r0 != r1)
	{
		return regS[r0];
	}
	else
	{
		auto copy = newTempIntPtr();
		cc.mov(copy, regS[r0]);
		return copy;
	}
}

asmjit::X86Gp JitCompiler::CheckRegA(int r0, int r1)
{
	if (r0 != r1)
	{
		return regA[r0];
	}
	else
	{
		auto copy = newTempIntPtr();
		cc.mov(copy, regA[r0]);
		return copy;
	}
}

void JitCompiler::EmitNOP()
{
	cc.nop();
}