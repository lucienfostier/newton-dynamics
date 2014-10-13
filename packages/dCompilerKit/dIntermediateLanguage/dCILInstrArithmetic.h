/* Copyright (c) <2009> <Newton Game Dynamics>
*
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
*
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely
*/

#ifndef _DCIL_INSTRUC_ARITHMETIC_H_
#define _DCIL_INSTRUC_ARITHMETIC_H_


#include "dCIL.h"
#include "dCILInstr.h"


class dCILInstrThreeArgArithmetic: public dCILThreeArgInstr
{
	public: 
	dCILInstrThreeArgArithmetic(dCIL& program, const dArg& arg0, const dArg& arg1, const dArg& arg2)
		:dCILThreeArgInstr(program, arg0, arg1, arg2)
	{
	}

	dCILInstrThreeArgArithmetic* GetAsThreeArgArithmetic()
	{
		return this;
	}

	virtual bool CanBeEliminated() const 
	{ 
		return true; 
	}
};

class dCILInstrIntergerLogical : public dCILInstrThreeArgArithmetic
{
	public:
	dCILInstrIntergerLogical (dCIL& program, dOperator operation, const dString& name0, const dArgType& type0, const dString& name1, const dArgType& type1, const dString& name2, const dArgType& type2);

	void Serialize(char* const textOut) const;

	virtual bool ApplySemanticReordering ();
	virtual void AddGeneratedAndUsedSymbols (dDataFlowPoint& datFloatPoint) const;
	virtual void AddDefinedVariable (dDefinedVariableDictionary& dictionary) const;
	virtual void AddKilledStatements (const dDefinedVariableDictionary& dictionary, dDataFlowPoint& datFloatPoint) const;

	virtual bool ApplyCopyPropagation(dCILInstrMove* const moveInst, dDataFlowGraph& dataFlow) const { dAssert(0);  return false; }
	dOperator m_operator;
};




#endif