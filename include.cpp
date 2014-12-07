/* picoc include system - can emulate system includes from built-in libraries
 * or it can include and parse files if the system has files */
 
#include "picoc.h"
#include "interpreter.h"

#ifndef NO_HASH_INCLUDE


/* initialise the built-in include libraries */
void IncludeInit(Picoc *pc)
{
#ifndef BUILTIN_MINI_STDLIB
    IncludeRegister(pc, "ctype.h", NULL, &StdCtypeFunctions[0], NULL);
    IncludeRegister(pc, "errno.h", &StdErrnoSetupFunc, NULL, NULL);
# ifndef NO_FP
    IncludeRegister(pc, "math.h", &MathSetupFunc, &MathFunctions[0], NULL);
# endif
    IncludeRegister(pc, "stdbool.h", &StdboolSetupFunc, NULL, StdboolDefs);
    IncludeRegister(pc, "stdio.h", &StdioSetupFunc, &StdioFunctions[0], StdioDefs);
    IncludeRegister(pc, "stdlib.h", &StdlibSetupFunc, &StdlibFunctions[0], NULL);
    IncludeRegister(pc, "string.h", &StringSetupFunc, &StringFunctions[0], NULL);
    IncludeRegister(pc, "time.h", &StdTimeSetupFunc, &StdTimeFunctions[0], StdTimeDefs);
# ifndef WIN32
    IncludeRegister(pc, "unistd.h", &UnistdSetupFunc, &UnistdFunctions[0], UnistdDefs);
# endif
#endif
}

/* clean up space used by the include system */
void IncludeCleanup(Picoc *pc)
{
    TIncludeLibraryPtr ThisInclude = pc->IncludeLibList;
    TIncludeLibraryPtr NextInclude;
    
    while (ThisInclude != NULL)
    {
        NextInclude = ThisInclude->NextLib;
        deallocMem(ThisInclude);
        ThisInclude = NextInclude;
    }

    pc->IncludeLibList = NILL;
}

/* register a new build-in include file */
void IncludeRegister(Picoc *pc, const char *IncludeName, void (*SetupFunction)(Picoc *pc), struct LibraryFunction *FuncList, const char *SetupCSource)
{
    TIncludeLibraryPtr NewLib = allocMem<struct IncludeLibrary>(false);
    NewLib->IncludeName = TableStrRegister(pc, IncludeName);
    NewLib->SetupFunction = SetupFunction;
    NewLib->FuncList = FuncList;
    NewLib->SetupCSource = SetupCSource;
    NewLib->NextLib = pc->IncludeLibList;
    pc->IncludeLibList = NewLib;
}

/* include all of the system headers */
void PicocIncludeAllSystemHeaders(Picoc *pc)
{
    TIncludeLibraryPtr ThisInclude = pc->IncludeLibList;
    
    for (; ThisInclude != NULL; ThisInclude = ThisInclude->NextLib)
        IncludeFile(pc, ThisInclude->IncludeName, FALSE); /* NOTE: LineByLine arg doesn't affect system libs */
}

/* include one of a number of predefined libraries, or perhaps an actual file */
void IncludeFile(Picoc *pc, TRegStringPtr FileName, int LineByLine)
{
    TIncludeLibraryPtr LInclude;
    
    /* scan for the include file name to see if it's in our list of predefined includes */
    for (LInclude = pc->IncludeLibList; LInclude != NULL; LInclude = LInclude->NextLib)
    {
        if (strcmp(LInclude->IncludeName, FileName) == 0)
        {
            /* found it - protect against multiple inclusion */
            if (!VariableDefined(pc, FileName))
            {
                VariableDefine(pc, NILL, FileName, NILL, ptrWrap(&pc->VoidType), FALSE);
                
                /* run an extra startup function if there is one */
                if (LInclude->SetupFunction != NULL)
                    (*LInclude->SetupFunction)(pc);
                
                /* parse the setup C source code - may define types etc. */
                if (LInclude->SetupCSource != NULL)
                    PicocParse(pc, FileName, LInclude->SetupCSource, strlen(LInclude->SetupCSource), TRUE, TRUE, FALSE, FALSE);
                
                /* set up the library functions */
                if (LInclude->FuncList != NULL)
                    LibraryAdd(pc, &pc->GlobalTable, FileName, LInclude->FuncList);
            }
            
            return;
        }
    }
    
#ifdef BUILTIN_MINI_STDLIB
    if (!strcmp(FileName, "stdio.h") || !strcmp(FileName, "stdlib.h") || !strcmp(FileName, "string.h") ||
        !strcmp(FileName, "math.h"))
        return; // avoid errors with standard headers
#endif

    /* not a predefined file, read a real file */

#ifdef WRAP_REGSTRINGS
    char buf[MAX_INC_FILENAME + 1];
    strncpy(buf, FileName, MAX_INC_FILENAME);
    buf[MAX_INC_FILENAME] = 0;

    if (LineByLine)
        PicocPlatformScanFileByLine(pc, buf);
    else
        PicocPlatformScanFile(pc, buf);
#else
    if (LineByLine)
        PicocPlatformScanFileByLine(pc, FileName);
    else
        PicocPlatformScanFile(pc, FileName);
#endif
}

#endif /* NO_HASH_INCLUDE */
