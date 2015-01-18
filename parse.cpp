/* picoc parser - parses source and executes statements */

#include "picoc.h"
#include "interpreter.h"

/* deallocate any memory */
void ParseCleanup(Picoc *pc)
{
    while (pc->CleanupTokenList != NULL)
    {
        TCleanupNodePtr Next = pc->CleanupTokenList->Next;
        
        deallocMem(pc->CleanupTokenList->Tokens);
        if (pc->CleanupTokenList->SourceText != NULL)
        {
#if defined(USE_MALLOC_HEAP) && defined(UNIX_HOST) // FIX: PlatformReadFile on unix uses malloc
            free((void *)ptrUnwrap(pc->CleanupTokenList->SourceText));
#else
            deallocMem((TLexCharPtr)pc->CleanupTokenList->SourceText);
#endif
        }
        deallocMem(pc->CleanupTokenList);
        pc->CleanupTokenList = Next;
    }
}

/* parse a statement, but only run it if Condition is TRUE */
enum ParseResult ParseStatementMaybeRun(TParseStatePtr Parser, int Condition, int CheckTrailingSemicolon)
{
    if (Parser->Mode != RunModeSkip && !Condition)
    {
        enum RunMode OldMode = Parser->Mode;
        enum ParseResult Result;
        Parser->Mode = RunModeSkip;
        Result = ParseStatement(Parser, CheckTrailingSemicolon);
        Parser->Mode = OldMode;
        return Result;
    }
    else
        return ParseStatement(Parser, CheckTrailingSemicolon);
}

/* count the number of parameters to a function or macro */
int ParseCountParams(TParseStatePtr Parser)
{
    int ParamCount = 0;
    
    enum LexToken Token = LexGetToken(Parser, NILL, TRUE);
    if (Token != TokenCloseBracket && Token != TokenEOF)
    { 
        /* count the number of parameters */
        ParamCount++;
        while ((Token = LexGetToken(Parser, NILL, TRUE)) != TokenCloseBracket && Token != TokenEOF)
        { 
            if (Token == TokenComma)
                ParamCount++;
        } 
    }
    
    return ParamCount;
}

/* parse a function definition and store it for later */
TValuePtr ParseFunctionDefinition(TParseStatePtr Parser, TValueTypePtr ReturnType, TRegStringPtr Identifier)
{
    TValueTypePtr ParamType;
    TRegStringPtr ParamIdentifier;
    enum LexToken Token = TokenNone;
    struct ParseState ParamParser;
    TValuePtr FuncValue;
    TValuePtr OldFuncValue;
    TParseStatePtr FuncBody;
    int ParamCount = 0;
    Picoc *pc = Parser->pc;

    if (pc->TopStackFrame != NULL)
        ProgramFail(Parser, "nested function definitions are not allowed");
        
    LexGetToken(Parser, NILL, TRUE);  /* open bracket */
    ParserCopy(ptrWrap(&ParamParser), Parser);
    ParamCount = ParseCountParams(Parser);
    if (ParamCount > PARAMETER_MAX)
        ProgramFail(Parser, "too many parameters (%d allowed)", PARAMETER_MAX);
    
    FuncValue = VariableAllocValueAndData(pc, Parser, sizeof(struct FuncDef) + sizeof(TValueTypePtr) * ParamCount + sizeof(TRegStringPtr) * ParamCount, FALSE, NILL, TRUE);
    FuncValue->Typ = ptrWrap(&pc->FunctionType);
    FuncValue->Val->FuncDef.ReturnType = ReturnType;
    FuncValue->Val->FuncDef.NumParams = ParamCount;
    FuncValue->Val->FuncDef.VarArgs = FALSE;
    FuncValue->Val->FuncDef.ParamType = (TValueTypePtrPtr)((TValueCharPtr)(FuncValue->Val) + sizeof(struct FuncDef));
    FuncValue->Val->FuncDef.ParamName = (TRegStringPtrPtr)((TValueCharPtr)(FuncValue->Val->FuncDef.ParamType) + sizeof(TValueTypePtr) * ParamCount);
    FuncValue->Val->FuncDef.Body = NILL;
    FuncValue->Val->FuncDef.Intrinsic = NILL; // FIX: this was kept uninitialized
   
    for (ParamCount = 0; ParamCount < FuncValue->Val->FuncDef.NumParams; ParamCount++)
    { 
        /* harvest the parameters into the function definition */
        if (ParamCount == FuncValue->Val->FuncDef.NumParams-1 && LexGetToken(ptrWrap(&ParamParser), NILL, FALSE) == TokenEllipsis)
        { 
            /* ellipsis at end */
            FuncValue->Val->FuncDef.NumParams--;
            FuncValue->Val->FuncDef.VarArgs = TRUE;
            break;
        }
        else
        { 
            /* add a parameter */
            TypeParse(ptrWrap(&ParamParser), &ParamType, &ParamIdentifier, NULL);
            if (ParamType->Base == TypeVoid)
            {
                /* this isn't a real parameter at all - delete it */
                ParamCount--;
                FuncValue->Val->FuncDef.NumParams--;
            }
            else
            {
                FuncValue->Val->FuncDef.ParamType[ParamCount] = ParamType;
                FuncValue->Val->FuncDef.ParamName[ParamCount] = ParamIdentifier;
            }
        }
        
        Token = LexGetToken(ptrWrap(&ParamParser), NILL, TRUE);
        if (Token != TokenComma && ParamCount < FuncValue->Val->FuncDef.NumParams-1)
            ProgramFail(ptrWrap(&ParamParser), "comma expected");
    }
    
    if (FuncValue->Val->FuncDef.NumParams != 0 && Token != TokenCloseBracket && Token != TokenComma && Token != TokenEllipsis)
        ProgramFail(ptrWrap(&ParamParser), "bad parameter");
    
    if (strcmp(Identifier, "main") == 0)
    {
        /* make sure it's int main() */
        if ( FuncValue->Val->FuncDef.ReturnType != ptrWrap(&pc->IntType) &&
             FuncValue->Val->FuncDef.ReturnType != ptrWrap(&pc->VoidType) )
            ProgramFail(Parser, "main() should return an int or void");

        if (FuncValue->Val->FuncDef.NumParams != 0 &&
             (FuncValue->Val->FuncDef.NumParams != 2 || FuncValue->Val->FuncDef.ParamType[0] != ptrWrap(&pc->IntType)) )
            ProgramFail(Parser, "bad parameters to main()");
    }
    
    /* look for a function body */
    Token = LexGetToken(Parser, NILL, FALSE);
    if (Token == TokenSemicolon)
        LexGetToken(Parser, NILL, TRUE);    /* it's a prototype, absorb the trailing semicolon */
    else
    {
        /* it's a full function definition with a body */
        if (Token != TokenLeftBrace)
            ProgramFail(Parser, "bad function definition");
        
        FuncBody = allocMem<struct ParseState>(false);

        if (!FuncBody)
            ProgramFail(Parser, "out of memory");

        ParserCopy(FuncBody, Parser);
        if (ParseStatementMaybeRun(Parser, FALSE, TRUE) != ParseResultOk)
            ProgramFail(Parser, "function definition expected");

        FuncValue->Val->FuncDef.Body = FuncBody;
        FuncValue->Val->FuncDef.Body->Pos = LexCopyTokens(FuncBody, FuncBody->Pos, Parser->Pos);

        /* is this function already in the global table? */
        if (TableGet(ptrWrap(&pc->GlobalTable), Identifier, &OldFuncValue, NULL, NULL, NULL))
        {
            if (OldFuncValue->Val->FuncDef.Body == NULL)
            {
                /* override an old function prototype */
                VariableFree(pc, TableDelete(pc, ptrWrap(&pc->GlobalTable), Identifier));
            }
            else
                ProgramFail(Parser, "'%s' is already defined", Identifier);
        }
    }

    if (!TableSet(pc, ptrWrap(&pc->GlobalTable), Identifier, FuncValue, Parser->FileName, Parser->Line, Parser->CharacterPos))
        ProgramFail(Parser, "'%s' is already defined", Identifier);
        
    return FuncValue;
}

/* parse an array initialiser and assign to a variable */
int ParseArrayInitialiser(TParseStatePtr Parser, TValuePtr NewVariable, int DoAssignment)
{
    int ArrayIndex = 0;
    enum LexToken Token;
    TValuePtr CValue;
    
    /* count the number of elements in the array */
    if (DoAssignment && Parser->Mode == RunModeRun)
    {
        struct ParseState CountParser;
        int NumElements;
        
        ParserCopy(ptrWrap(&CountParser), Parser);
        NumElements = ParseArrayInitialiser(ptrWrap(&CountParser), NewVariable, FALSE);

        if (NewVariable->Typ->Base != TypeArray)
            AssignFail(Parser, "%t from array initializer", NewVariable->Typ, NILL, 0, 0, NILL, 0);

        if (NewVariable->Typ->ArraySize == 0)
        {
            NewVariable->Typ = TypeGetMatching(Parser->pc, Parser, NewVariable->Typ->FromType, NewVariable->Typ->Base, NumElements, NewVariable->Typ->Identifier, TRUE);
            VariableRealloc(Parser, NewVariable, TypeSizeValue(NewVariable, FALSE));
        }
        #ifdef DEBUG_ARRAY_INITIALIZER
        PRINT_SOURCE_POS;
        printf("array size: %d \n", NewVariable->Typ->ArraySize);
        #endif
    }
    
    /* parse the array initialiser */
    Token = LexGetToken(Parser, NILL, FALSE);
    while (Token != TokenRightBrace)
    {
        if (LexGetToken(Parser, NILL, FALSE) == TokenLeftBrace)
        {
            /* this is a sub-array initialiser */
            int SubArraySize = 0;
            TValuePtr SubArray = NewVariable;
            if (Parser->Mode == RunModeRun && DoAssignment)
            {
                SubArraySize = TypeSize(NewVariable->Typ->FromType, NewVariable->Typ->FromType->ArraySize, TRUE);
                SubArray = VariableAllocValueFromExistingData(Parser, NewVariable->Typ->FromType, (TAnyValuePtr)(&NewVariable->Val->ArrayMem[0] + SubArraySize * ArrayIndex), TRUE, NewVariable);

                #ifdef DEBUG_ARRAY_INITIALIZER
                int FullArraySize = TypeSize(NewVariable->Typ, NewVariable->Typ->ArraySize, TRUE);
                PRINT_SOURCE_POS;
                PRINT_TYPE(NewVariable->Typ)
                printf("[%d] subarray size: %d (full: %d,%d) \n", ArrayIndex, SubArraySize, FullArraySize, NewVariable->Typ->ArraySize);
                #endif
                if (ArrayIndex >= NewVariable->Typ->ArraySize)
                    ProgramFail(Parser, "too many array elements");
            }
            LexGetToken(Parser, NILL, TRUE);
            ParseArrayInitialiser(Parser, SubArray, DoAssignment);
        }
        else
        {
            TValuePtr ArrayElement = NILL;
        
            if (Parser->Mode == RunModeRun && DoAssignment)
            {
                TValueTypePtr  ElementType = NewVariable->Typ;
                int TotalSize = 1;
                int ElementSize = 0;
                
                /* int x[3][3] = {1,2,3,4} => handle it just like int x[9] = {1,2,3,4} */
                while (ElementType->Base == TypeArray)
                {
                    TotalSize *= ElementType->ArraySize;
                    ElementType = ElementType->FromType;
                    
                    /* char x[10][10] = {"abc", "def"} => assign "abc" to x[0], "def" to x[1] etc */
                    if (LexGetToken(Parser, NILL, FALSE) == TokenStringConstant && ElementType->FromType->Base == TypeChar)
                        break;
                }
                ElementSize = TypeSize(ElementType, ElementType->ArraySize, TRUE);

                #ifdef DEBUG_ARRAY_INITIALIZER
                PRINT_SOURCE_POS;
                printf("[%d/%d] element size: %d (x%d) \n", ArrayIndex, TotalSize, ElementSize, ElementType->ArraySize);
                #endif

                if (ArrayIndex >= TotalSize)
                    ProgramFail(Parser, "too many array elements");

                ArrayElement = VariableAllocValueFromExistingData(Parser, ElementType, (TAnyValuePtr)(&NewVariable->Val->ArrayMem[0] + ElementSize * ArrayIndex), TRUE, NewVariable);
            }

            /* this is a normal expression initialiser */
            if (!ExpressionParse(Parser, &CValue))
                ProgramFail(Parser, "expression expected");

            if (Parser->Mode == RunModeRun && DoAssignment)
            {
                ExpressionAssign(Parser, ArrayElement, CValue, FALSE, NILL, 0, FALSE);
                VariableStackPop(Parser, CValue);
                VariableStackPop(Parser, ArrayElement);
            }
        }
        
        ArrayIndex++;

        Token = LexGetToken(Parser, NILL, FALSE);
        if (Token == TokenComma)
        {
            LexGetToken(Parser, NILL, TRUE);
            Token = LexGetToken(Parser, NILL, FALSE);
        }   
        else if (Token != TokenRightBrace)
            ProgramFail(Parser, "comma expected");
    }
    
    if (Token == TokenRightBrace)
        LexGetToken(Parser, NILL, TRUE);
    else
        ProgramFail(Parser, "'}' expected");
    
    return ArrayIndex;
}

/* assign an initial value to a variable */
void ParseDeclarationAssignment(TParseStatePtr Parser, TValuePtr NewVariable, int DoAssignment)
{
    TValuePtr CValue;

    if (LexGetToken(Parser, NILL, FALSE) == TokenLeftBrace)
    {
        /* this is an array initialiser */
        LexGetToken(Parser, NILL, TRUE);
        ParseArrayInitialiser(Parser, NewVariable, DoAssignment);
    }
    else
    {
        /* this is a normal expression initialiser */
        if (!ExpressionParse(Parser, &CValue))
            ProgramFail(Parser, "expression expected");
            
        if (Parser->Mode == RunModeRun && DoAssignment)
        {
            ExpressionAssign(Parser, NewVariable, CValue, FALSE, NILL, 0, FALSE);
            VariableStackPop(Parser, CValue);
        }
    }
}

/* declare a variable or function */
int ParseDeclaration(TParseStatePtr Parser, enum LexToken Token)
{
    TRegStringPtr Identifier;
    TValueTypePtr BasicType;
    TValueTypePtr Typ;
    TValuePtr NewVariable = NILL;
    int IsStatic = FALSE;
    int FirstVisit = FALSE;
    Picoc *pc = Parser->pc;

    TypeParseFront(Parser, &BasicType, &IsStatic);
    do
    {
        TypeParseIdentPart(Parser, BasicType, &Typ, &Identifier);
        if ((Token != TokenVoidType && Token != TokenStructType && Token != TokenUnionType && Token != TokenEnumType) && Identifier == pc->StrEmpty)
            ProgramFail(Parser, "identifier expected");
            
        if (Identifier != pc->StrEmpty)
        {
            /* handle function definitions */
            if (LexGetToken(Parser, NILL, FALSE) == TokenOpenBracket)
            {
                ParseFunctionDefinition(Parser, Typ, Identifier);
                return FALSE;
            }
            else
            {
                if (Typ == ptrWrap(&pc->VoidType) && Identifier != pc->StrEmpty)
                    ProgramFail(Parser, "can't define a void variable");
                    
                if (Parser->Mode == RunModeRun || Parser->Mode == RunModeGoto)
                    NewVariable = VariableDefineButIgnoreIdentical(Parser, Identifier, Typ, IsStatic, &FirstVisit);
                
                if (LexGetToken(Parser, NILL, FALSE) == TokenAssign)
                {
                    /* we're assigning an initial value */
                    LexGetToken(Parser, NILL, TRUE);
                    ParseDeclarationAssignment(Parser, NewVariable, !IsStatic || FirstVisit);
                }
            }
        }
        
        Token = LexGetToken(Parser, NILL, FALSE);
        if (Token == TokenComma)
            LexGetToken(Parser, NILL, TRUE);
            
    } while (Token == TokenComma);
    
    return TRUE;
}

/* parse a #define macro definition and store it for later */
void ParseMacroDefinition(TParseStatePtr Parser)
{
    TValuePtr MacroName;
    TRegStringPtr MacroNameStr;
    TValuePtr ParamName;
    TValuePtr MacroValue;

    if (LexGetToken(Parser, &MacroName, TRUE) != TokenIdentifier)
        ProgramFail(Parser, "identifier expected");
    
    MacroNameStr = MacroName->Val->Identifier;
    
    if (LexRawPeekToken(Parser) == TokenOpenMacroBracket)
    {
        /* it's a parameterised macro, read the parameters */
        enum LexToken Token = LexGetToken(Parser, NILL, TRUE);
        struct ParseState ParamParser;
        int NumParams;
        int ParamCount = 0;
        
        ParserCopy(ptrWrap(&ParamParser), Parser);
        NumParams = ParseCountParams(ptrWrap(&ParamParser));
        MacroValue = VariableAllocValueAndData(Parser->pc, Parser, sizeof(struct MacroDef) + sizeof(TRegStringPtr) * NumParams, FALSE, NILL, TRUE);
        MacroValue->Val->MacroDef.NumParams = NumParams;
        MacroValue->Val->MacroDef.ParamName = (TRegStringPtrPtr)((TAnyValueCharPtr)(MacroValue->Val) + sizeof(struct MacroDef));

        Token = LexGetToken(Parser, &ParamName, TRUE);
        
        while (Token == TokenIdentifier)
        {
            /* store a parameter name */
            MacroValue->Val->MacroDef.ParamName[ParamCount++] = ParamName->Val->Identifier;
            
            /* get the trailing comma */
            Token = LexGetToken(Parser, NILL, TRUE);
            if (Token == TokenComma)
                Token = LexGetToken(Parser, &ParamName, TRUE);
                
            else if (Token != TokenCloseBracket)
                ProgramFail(Parser, "comma expected");
        }
        
        if (Token != TokenCloseBracket)
            ProgramFail(Parser, "close bracket expected");
    }
    else
    {
        /* allocate a simple unparameterised macro */
        MacroValue = VariableAllocValueAndData(Parser->pc, Parser, sizeof(struct MacroDef), FALSE, NILL, TRUE);
        MacroValue->Val->MacroDef.NumParams = 0;
    }
    
    /* copy the body of the macro to execute later */
    ParserCopy(getMembrPtr(MacroValue->Val, &AnyValue::MacroDef, &MacroDef::Body), Parser);
    MacroValue->Typ = ptrWrap(&Parser->pc->MacroType);
    LexToEndOfLine(Parser);
    MacroValue->Val->MacroDef.Body.Pos = LexCopyTokens(Parser, MacroValue->Val->MacroDef.Body.Pos, Parser->Pos); // UNDONE: switched MacroDef.Body to Parser, shouldn't matter
    
    if (!TableSet(Parser->pc, ptrWrap(&Parser->pc->GlobalTable), MacroNameStr, MacroValue, Parser->FileName, Parser->Line, Parser->CharacterPos))
        ProgramFail(Parser, "'%s' is already defined", MacroNameStr);
}

/* copy the entire parser state */
void ParserCopy(TParseStatePtr To, TParseStatePtr From)
{
    memcpy(To, From, sizeof(struct ParseState));
}

/* copy where we're at in the parsing */
void ParserCopyPos(TParseStatePtr To, TParseStatePtr From)
{
    To->Pos = From->Pos;
    To->Line = From->Line;
    To->HashIfLevel = From->HashIfLevel;
    To->HashIfEvaluateToLevel = From->HashIfEvaluateToLevel;
    To->CharacterPos = From->CharacterPos;
}

/* parse a "for" statement */
void ParseFor(TParseStatePtr Parser)
{
    int Condition;
    struct ParseState PreConditional;
    struct ParseState PreIncrement;
    struct ParseState PreStatement;
    struct ParseState After;
    
    enum RunMode OldMode = Parser->Mode;
    
    int16_t PrevScopeID = 0, ScopeID = VariableScopeBegin(Parser, &PrevScopeID);

    if (LexGetToken(Parser, NILL, TRUE) != TokenOpenBracket)
        ProgramFail(Parser, "'(' expected");
                        
    if (ParseStatement(Parser, TRUE) != ParseResultOk)
        ProgramFail(Parser, "statement expected");
    
    ParserCopyPos(ptrWrap(&PreConditional), Parser);
    if (LexGetToken(Parser, NILL, FALSE) == TokenSemicolon)
        Condition = TRUE;
    else
        Condition = ExpressionParseInt(Parser);
    
    if (LexGetToken(Parser, NILL, TRUE) != TokenSemicolon)
        ProgramFail(Parser, "';' expected");
    
    ParserCopyPos(ptrWrap(&PreIncrement), Parser);
    ParseStatementMaybeRun(Parser, FALSE, FALSE);
    
    if (LexGetToken(Parser, NILL, TRUE) != TokenCloseBracket)
        ProgramFail(Parser, "')' expected");
    
    ParserCopyPos(ptrWrap(&PreStatement), Parser);
    if (ParseStatementMaybeRun(Parser, Condition, TRUE) != ParseResultOk)
        ProgramFail(Parser, "statement expected");
    
    if (Parser->Mode == RunModeContinue && OldMode == RunModeRun)
        Parser->Mode = RunModeRun;
        
    ParserCopyPos(ptrWrap(&After), Parser);
        
    while (Condition && Parser->Mode == RunModeRun)
    {
        ParserCopyPos(Parser, ptrWrap(&PreIncrement));
        ParseStatement(Parser, FALSE);
                        
        ParserCopyPos(Parser, ptrWrap(&PreConditional));
        if (LexGetToken(Parser, NILL, FALSE) == TokenSemicolon)
            Condition = TRUE;
        else
            Condition = ExpressionParseInt(Parser);
        
        if (Condition)
        {
            ParserCopyPos(Parser, ptrWrap(&PreStatement));
            ParseStatement(Parser, TRUE);
            
            if (Parser->Mode == RunModeContinue)
                Parser->Mode = RunModeRun;                
        }
    }
    
    if (Parser->Mode == RunModeBreak && OldMode == RunModeRun)
        Parser->Mode = RunModeRun;

    VariableScopeEnd(Parser, ScopeID, PrevScopeID);

    ParserCopyPos(Parser, ptrWrap(&After));
}

/* parse a block of code and return what mode it returned in */
enum RunMode ParseBlock(TParseStatePtr Parser, int AbsorbOpenBrace, int Condition)
{
    int16_t PrevScopeID = 0, ScopeID = VariableScopeBegin(Parser, &PrevScopeID);

    if (AbsorbOpenBrace && LexGetToken(Parser, NILL, TRUE) != TokenLeftBrace)
        ProgramFail(Parser, "'{' expected");

    if (Parser->Mode == RunModeSkip || !Condition)
    { 
        /* condition failed - skip this block instead */
        enum RunMode OldMode = Parser->Mode;
        Parser->Mode = RunModeSkip;
        while (ParseStatement(Parser, TRUE) == ParseResultOk)
        {}
        Parser->Mode = OldMode;
    }
    else
    { 
        /* just run it in its current mode */
        while (ParseStatement(Parser, TRUE) == ParseResultOk)
        {}
    }
    
    if (LexGetToken(Parser, NILL, TRUE) != TokenRightBrace)
        ProgramFail(Parser, "'}' expected");

    VariableScopeEnd(Parser, ScopeID, PrevScopeID);

    return Parser->Mode;
}

/* parse a typedef declaration */
void ParseTypedef(TParseStatePtr Parser)
{
    TValueTypePtr Typ;
    TValueTypePtrPtr TypPtr;
    TRegStringPtr TypeName;
    struct Value InitValue;
    
    TypeParse(Parser, &Typ, &TypeName, NULL);
    
    if (Parser->Mode == RunModeRun)
    {
        TypPtr = &Typ;
        InitValue.Typ = ptrWrap(&Parser->pc->TypeType);
        InitValue.Val = (TAnyValuePtr)(TypPtr);
        VariableDefine(Parser->pc, Parser, TypeName, ptrWrap(&InitValue), NILL, FALSE);
    }
}

/* parse a statement */
enum ParseResult ParseStatement(TParseStatePtr Parser, int CheckTrailingSemicolon)
{
    TValuePtr CValue;
    TValuePtr LexerValue;
    TValuePtr VarValue;
    int Condition;
    struct ParseState PreState;
    enum LexToken Token;
    
    /* if we're debugging, check for a breakpoint */
    if (Parser->DebugMode && Parser->Mode == RunModeRun)
        DebugCheckStatement(Parser);
    
    /* take note of where we are and then grab a token to see what statement we have */   
    ParserCopy(ptrWrap(&PreState), Parser);
    Token = LexGetToken(Parser, &LexerValue, TRUE);

    switch (Token)
    {
        case TokenEOF:
            return ParseResultEOF;
            
        case TokenIdentifier:
            /* might be a typedef-typed variable declaration or it might be an expression */
            if (VariableDefined(Parser->pc, LexerValue->Val->Identifier))
            {
                VariableGet(Parser->pc, Parser, LexerValue->Val->Identifier, &VarValue);
                if (VarValue->Typ->Base == Type_Type)
                {
                    *Parser = PreState;
                    ParseDeclaration(Parser, Token);
                    break;
                }
            }
            else
            {
                /* it might be a goto label */
                enum LexToken NextToken = LexGetToken(Parser, NILL, FALSE);
                if (NextToken == TokenColon)
                {
                    /* declare the identifier as a goto label */
                    LexGetToken(Parser, NILL, TRUE);
                    if (Parser->Mode == RunModeGoto && LexerValue->Val->Identifier == Parser->SearchGotoLabel)
                        Parser->Mode = RunModeRun;
        
                    CheckTrailingSemicolon = FALSE;
                    break;
                }
#ifdef FEATURE_AUTO_DECLARE_VARIABLES
                else /* new_identifier = something */
                {    /* try to guess type and declare the variable based on assigned value */
                    if (NextToken == TokenAssign && !VariableDefinedAndOutOfScope(Parser->pc, LexerValue->Val->Identifier))
                    {
                        if (Parser->Mode == RunModeRun)
                        {
                            TValuePtr CValue;
                            char* Identifier = LexerValue->Val->Identifier;

                            LexGetToken(Parser, NILL, TRUE);
                            if (!ExpressionParse(Parser, &CValue))
                            {
                                ProgramFail(Parser, "expected: expression");
                            }
                            
                            #if 0
                            PRINT_SOURCE_POS;
                            PlatformPrintf(Parser->pc->CStdOut, "%t %s = %d;\n", CValue->Typ, Identifier, CValue->Val->Integer);
                            printf("%d\n", VariableDefined(Parser->pc, Identifier));
                            #endif
                            VariableDefine(Parser->pc, Parser, Identifier, CValue, CValue->Typ, TRUE);
                            break;
                        }
                    }
                }
#endif
            }
            /* else fallthrough to expression */
	    /* no break */
            
        case TokenAsterisk: 
        case TokenAmpersand: 
        case TokenIncrement: 
        case TokenDecrement: 
        case TokenOpenBracket: 
            *Parser = PreState;
            ExpressionParse(Parser, &CValue);
            if (Parser->Mode == RunModeRun) 
                VariableStackPop(Parser, CValue);
            break;
            
        case TokenLeftBrace:
            ParseBlock(Parser, FALSE, TRUE);
            CheckTrailingSemicolon = FALSE;
            break;
            
        case TokenIf:
            if (LexGetToken(Parser, NILL, TRUE) != TokenOpenBracket)
                ProgramFail(Parser, "'(' expected");
                
            Condition = ExpressionParseInt(Parser);
            
            if (LexGetToken(Parser, NILL, TRUE) != TokenCloseBracket)
                ProgramFail(Parser, "')' expected");

            if (ParseStatementMaybeRun(Parser, Condition, TRUE) != ParseResultOk)
                ProgramFail(Parser, "statement expected");
            
            if (LexGetToken(Parser, NILL, FALSE) == TokenElse)
            {
                LexGetToken(Parser, NILL, TRUE);
                if (ParseStatementMaybeRun(Parser, !Condition, TRUE) != ParseResultOk)
                    ProgramFail(Parser, "statement expected");
            }
            CheckTrailingSemicolon = FALSE;
            break;
        
        case TokenWhile:
            {
                struct ParseState PreConditional;
                enum RunMode PreMode = Parser->Mode;

                if (LexGetToken(Parser, NILL, TRUE) != TokenOpenBracket)
                    ProgramFail(Parser, "'(' expected");
                    
                ParserCopyPos(ptrWrap(&PreConditional), Parser);
                do
                {
                    ParserCopyPos(Parser, ptrWrap(&PreConditional));
                    Condition = ExpressionParseInt(Parser);
                    if (LexGetToken(Parser, NILL, TRUE) != TokenCloseBracket)
                        ProgramFail(Parser, "')' expected");
                    
                    if (ParseStatementMaybeRun(Parser, Condition, TRUE) != ParseResultOk)
                        ProgramFail(Parser, "statement expected");
                    
                    if (Parser->Mode == RunModeContinue)
                        Parser->Mode = PreMode;
                    
                } while (Parser->Mode == RunModeRun && Condition);
                
                if (Parser->Mode == RunModeBreak)
                    Parser->Mode = PreMode;

                CheckTrailingSemicolon = FALSE;
            }
            break;
                
        case TokenDo:
            {
                struct ParseState PreStatement;
                enum RunMode PreMode = Parser->Mode;
                ParserCopyPos(ptrWrap(&PreStatement), Parser);
                do
                {
                    ParserCopyPos(Parser, ptrWrap(&PreStatement));
                    if (ParseStatement(Parser, TRUE) != ParseResultOk)
                        ProgramFail(Parser, "statement expected");
                
                    if (Parser->Mode == RunModeContinue)
                        Parser->Mode = PreMode;

                    if (LexGetToken(Parser, NILL, TRUE) != TokenWhile)
                        ProgramFail(Parser, "'while' expected");
                    
                    if (LexGetToken(Parser, NILL, TRUE) != TokenOpenBracket)
                        ProgramFail(Parser, "'(' expected");
                        
                    Condition = ExpressionParseInt(Parser);
                    if (LexGetToken(Parser, NILL, TRUE) != TokenCloseBracket)
                        ProgramFail(Parser, "')' expected");
                    
                } while (Condition && Parser->Mode == RunModeRun);           
                
                if (Parser->Mode == RunModeBreak)
                    Parser->Mode = PreMode;
            }
            break;
                
        case TokenFor:
            ParseFor(Parser);
            CheckTrailingSemicolon = FALSE;
            break;

        case TokenSemicolon: 
            CheckTrailingSemicolon = FALSE; 
            break;

        case TokenIntType:
        case TokenShortType:
        case TokenCharType:
        case TokenLongType:
        case TokenFloatType:
        case TokenDoubleType:
        case TokenVoidType:
        case TokenStructType:
        case TokenUnionType:
        case TokenEnumType:
        case TokenSignedType:
        case TokenUnsignedType:
        case TokenStaticType:
        case TokenAutoType:
        case TokenRegisterType:
        case TokenExternType:
            *Parser = PreState;
            CheckTrailingSemicolon = ParseDeclaration(Parser, Token);
            break;
        
        case TokenHashDefine:
            ParseMacroDefinition(Parser);
            CheckTrailingSemicolon = FALSE;
            break;
            
#ifndef NO_FILE_SUPPORT
        case TokenHashInclude:
            if (LexGetToken(Parser, &LexerValue, TRUE) != TokenStringConstant)
                ProgramFail(Parser, "\"filename.h\" expected");
            
            IncludeFile(Parser->pc, (TRegStringPtr)LexerValue->Val->Pointer, (Parser->LineFilePointer || Parser->FileName == Parser->pc->StrEmpty)); // UNDONE
            CheckTrailingSemicolon = FALSE;
            break;
#endif

        case TokenSwitch:
            if (LexGetToken(Parser, NILL, TRUE) != TokenOpenBracket)
                ProgramFail(Parser, "'(' expected");
                
            Condition = ExpressionParseInt(Parser);
            
            if (LexGetToken(Parser, NILL, TRUE) != TokenCloseBracket)
                ProgramFail(Parser, "')' expected");
            
            if (LexGetToken(Parser, NILL, FALSE) != TokenLeftBrace)
                ProgramFail(Parser, "'{' expected");
            
            { 
                /* new block so we can store parser state */
                enum RunMode OldMode = Parser->Mode;
                int OldSearchLabel = Parser->SearchLabel;
                Parser->Mode = RunModeCaseSearch;
                Parser->SearchLabel = Condition;
                
                ParseBlock(Parser, TRUE, (OldMode != RunModeSkip) && (OldMode != RunModeReturn));
                
                if (Parser->Mode != RunModeReturn)
                    Parser->Mode = OldMode;

                Parser->SearchLabel = OldSearchLabel;
            }

            CheckTrailingSemicolon = FALSE;
            break;

        case TokenCase:
            if (Parser->Mode == RunModeCaseSearch)
            {
                Parser->Mode = RunModeRun;
                Condition = ExpressionParseInt(Parser);
                Parser->Mode = RunModeCaseSearch;
            }
            else
                Condition = ExpressionParseInt(Parser);
                
            if (LexGetToken(Parser, NILL, TRUE) != TokenColon)
                ProgramFail(Parser, "':' expected");
            
            if (Parser->Mode == RunModeCaseSearch && Condition == Parser->SearchLabel)
                Parser->Mode = RunModeRun;

            CheckTrailingSemicolon = FALSE;
            break;
            
        case TokenDefault:
            if (LexGetToken(Parser, NILL, TRUE) != TokenColon)
                ProgramFail(Parser, "':' expected");
            
            if (Parser->Mode == RunModeCaseSearch)
                Parser->Mode = RunModeRun;
                
            CheckTrailingSemicolon = FALSE;
            break;

        case TokenBreak:
            if (Parser->Mode == RunModeRun)
                Parser->Mode = RunModeBreak;
            break;
            
        case TokenContinue:
            if (Parser->Mode == RunModeRun)
                Parser->Mode = RunModeContinue;
            break;
            
        case TokenReturn:
            if (Parser->Mode == RunModeRun)
            {
                if (!Parser->pc->TopStackFrame || Parser->pc->TopStackFrame->ReturnValue->Typ->Base != TypeVoid)
                {
                    if (!ExpressionParse(Parser, &CValue))
                        ProgramFail(Parser, "value required in return");
                    
                    if (!Parser->pc->TopStackFrame) /* return from top-level program? */
                        PlatformExit(Parser->pc, ExpressionCoerceInteger(CValue));
                    else
                        ExpressionAssign(Parser, Parser->pc->TopStackFrame->ReturnValue, CValue, TRUE, NILL, 0, FALSE);

                    VariableStackPop(Parser, CValue);
                }
                else
                {
                    if (ExpressionParse(Parser, &CValue))
                        ProgramFail(Parser, "value in return from a void function");                    
                }
                
                Parser->Mode = RunModeReturn;
            }
            else
                ExpressionParse(Parser, &CValue);
            break;

        case TokenTypedef:
            ParseTypedef(Parser);
            break;
            
        case TokenGoto:
            if (LexGetToken(Parser, &LexerValue, TRUE) != TokenIdentifier)
                ProgramFail(Parser, "identifier expected");
            
            if (Parser->Mode == RunModeRun)
            { 
                /* start scanning for the goto label */
                Parser->SearchGotoLabel = LexerValue->Val->Identifier;
                Parser->Mode = RunModeGoto;
            }
            break;
                
        case TokenDelete:
        {
            /* try it as a function or variable name to delete */
            if (LexGetToken(Parser, &LexerValue, TRUE) != TokenIdentifier)
                ProgramFail(Parser, "identifier expected");
                
            if (Parser->Mode == RunModeRun)
            { 
                /* delete this variable or function */
                CValue = TableDelete(Parser->pc, ptrWrap(&Parser->pc->GlobalTable), LexerValue->Val->Identifier);

                if (CValue == NULL)
                    ProgramFail(Parser, "'%s' is not defined", LexerValue->Val->Identifier);
                
                VariableFree(Parser->pc, CValue);
            }
            break;
        }
        
        default:
            *Parser = PreState;
            return ParseResultError;
    }
    
    if (CheckTrailingSemicolon)
    {
        if (LexGetToken(Parser, NILL, TRUE) != TokenSemicolon)
            ProgramFail(Parser, "';' expected");
    }
    
    return ParseResultOk;
}

/* quick scan a source file for definitions */
void PicocParse(Picoc *pc, TConstRegStringPtr FileName, TLexConstCharPtr Source, int SourceLen, int RunIt, int CleanupNow, int CleanupSource, int EnableDebugger)
{
    struct ParseState Parser;
    enum ParseResult Ok;
    TCleanupNodePtr NewCleanupNode;
    TRegStringPtr RegFileName = TableStrRegister(pc, FileName);

    TLexBufPtr Tokens = LexAnalyse(pc, RegFileName, Source, SourceLen, NULL);

    /* allocate a cleanup node so we can clean up the tokens later */
    if (!CleanupNow)
    {
        NewCleanupNode = allocMem<CleanupTokenNode>(false);
        if (NewCleanupNode == NULL)
            ProgramFailNoParser(pc, "out of memory");
        
        NewCleanupNode->Tokens = Tokens;
        if (CleanupSource)
            NewCleanupNode->SourceText = Source;
        else
            NewCleanupNode->SourceText = NILL;
            
        NewCleanupNode->Next = pc->CleanupTokenList;
        pc->CleanupTokenList = NewCleanupNode;
    }
    
    /* do the parsing */
    LexInitParser(ptrWrap(&Parser), pc, Source, Tokens, RegFileName, NULL, RunIt, EnableDebugger);

    do {
        Ok = ParseStatement(ptrWrap(&Parser), TRUE);
    } while (Ok == ParseResultOk);
    
    if (Ok == ParseResultError)
        ProgramFail(ptrWrap(&Parser), "parse error");
    
    /* clean up */
    if (CleanupNow)
        deallocMem(Tokens);
}

/* parse interactively */
void PicocParseInteractiveNoStartPrompt(Picoc *pc, int EnableDebugger)
{
    struct ParseState Parser;
    enum ParseResult Ok;
    
    LexInitParser(ptrWrap(&Parser), pc, NILL, NILL, pc->StrEmpty, NULL, TRUE, EnableDebugger);
#ifdef ARDUINO_HOST
    if (PicocPlatformSetExitPoint(pc) && pc->PicocExitValue == ARDUINO_EXIT)
        return; // called exit(), get out
#else
    PicocPlatformSetExitPoint(pc);
#endif
    LexInteractiveClear(pc, ptrWrap(&Parser));

    do
    {
        LexInteractiveStatementPrompt(pc);
        Ok = ParseStatement(ptrWrap(&Parser), TRUE);
        LexInteractiveCompleted(pc, ptrWrap(&Parser));
    } while (Ok == ParseResultOk);
    
    if (Ok == ParseResultError)
        ProgramFail(ptrWrap(&Parser), "parse error");
    
    PlatformPrintf(pc->CStdOut, "\n");
}

/* parse interactively, showing a startup message */
void PicocParseInteractive(Picoc *pc)
{
    PlatformPrintf(pc->CStdOut, INTERACTIVE_PROMPT_START);
    PicocParseInteractiveNoStartPrompt(pc, TRUE);
}

void PicocParseLineByLine(Picoc *pc, const char *FileName, void *FilePointer, int EnableDebugger)
{
    struct ParseState Parser;
    enum ParseResult Ok;
    TRegStringPtr RegFileName = TableStrRegister(pc, FileName);

    LexInitParser(ptrWrap(&Parser), pc, NILL, NILL, RegFileName, FilePointer, TRUE, EnableDebugger);
    /*PicocPlatformSetExitPoint(pc);*/
    LexInteractiveClear(pc, ptrWrap(&Parser));

    do
    {
        Ok = ParseStatement(ptrWrap(&Parser), TRUE);
        LexInteractiveCompleted(pc, ptrWrap(&Parser));

    } while (Ok == ParseResultOk);

    if (Ok == ParseResultError)
        ProgramFail(ptrWrap(&Parser), "parse error");
}
