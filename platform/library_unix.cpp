#include "../interpreter.h"

void UnixSetupFunc(Picoc *)
{    
}

void Ctest (TParseStatePtr Parser, TValuePtr ReturnValue, TValuePtrPtr Param, int NumArgs)
{
    printf("test(%d)\n", Param[0]->Val->Integer);
    Param[0]->Val->Integer = 1234;
}

void Clineno (TParseStatePtr Parser, TValuePtr ReturnValue, TValuePtrPtr Param, int NumArgs)
{
    ReturnValue->Val->Integer = Parser->Line;
}

/* list of all library functions and their prototypes */
struct LibraryFunction UnixFunctions[] =
{
    { Ctest,        "void test(int);" },
    { Clineno,      "int lineno();" },
    { NULL,         NULL }
};

void PlatformLibraryInit(Picoc *pc)
{
    IncludeRegister(pc, "picoc_unix.h", &UnixSetupFunc, &UnixFunctions[0], NULL);
}
