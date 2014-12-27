/* picoc interactive debugger */

#ifndef NO_DEBUGGER

#include "interpreter.h"

#define BREAKPOINT_HASH(p) ( ((unsigned long)getNumPtr((p)->FileName)) ^ (((p)->Line << 16) | ((p)->CharacterPos << 16)) )

/* initialise the debugger by clearing the breakpoint table */
void DebugInit(Picoc *pc)
{
    TableInitTable(ptrWrap(&pc->BreakpointTable), &pc->BreakpointHashTable[0], BREAKPOINT_TABLE_SIZE, TRUE);
    pc->BreakpointCount = 0;
}

/* free the contents of the breakpoint table */
void DebugCleanup(Picoc *pc)
{
    TTableEntryPtr Entry;
    TTableEntryPtr NextEntry;
    int Count;
    
    for (Count = 0; Count < pc->BreakpointTable.Size; Count++)
    {
        for (Entry = pc->BreakpointHashTable[Count]; Entry != NULL; Entry = NextEntry)
        {
            NextEntry = Entry->Next;
            deallocMem(Entry);
        }
    }
}

/* search the table for a breakpoint */
static TTableEntryPtr DebugTableSearchBreakpoint(TParseStatePtr Parser, int *AddAt)
{
    TTableEntryPtr Entry;
    Picoc *pc = Parser->pc;
    int HashValue = BREAKPOINT_HASH(Parser) % pc->BreakpointTable.Size;
    
    for (Entry = pc->BreakpointHashTable[HashValue]; Entry != NULL; Entry = Entry->Next)
    {
        if (Entry->p.b.FileName == Parser->FileName && Entry->p.b.Line == Parser->Line && Entry->p.b.CharacterPos == Parser->CharacterPos)
            return Entry;   /* found */
    }
    
    *AddAt = HashValue;    /* didn't find it in the chain */
    return NILL;
}

/* set a breakpoint in the table */
void DebugSetBreakpoint(TParseStatePtr Parser)
{
    int AddAt;
    TTableEntryPtr FoundEntry = DebugTableSearchBreakpoint(Parser, &AddAt);
    Picoc *pc = Parser->pc;
    
    if (FoundEntry == NULL)
    {   
        /* add it to the table */
        TTableEntryPtr NewEntry = allocMem<struct TableEntry>(false);
        if (NewEntry == NULL)
            ProgramFailNoParser(pc, "out of memory");
            
        NewEntry->p.b.FileName = Parser->FileName;
        NewEntry->p.b.Line = Parser->Line;
        NewEntry->p.b.CharacterPos = Parser->CharacterPos;
        NewEntry->Next = pc->BreakpointHashTable[AddAt];
        pc->BreakpointHashTable[AddAt] = NewEntry;
        pc->BreakpointCount++;
    }
}

/* delete a breakpoint from the hash table */
int DebugClearBreakpoint(TParseStatePtr Parser)
{
    TTableEntryPtrPtr EntryPtr;
    Picoc *pc = Parser->pc;
    int HashValue = BREAKPOINT_HASH(Parser) % pc->BreakpointTable.Size;
    
    for (EntryPtr = &pc->BreakpointHashTable[HashValue]; *EntryPtr != NILL; EntryPtr = getMembrPtr((TTableEntryPtr)(*EntryPtr), &TableEntry::Next))
    {
        TTableEntryPtr DeleteEntry = *EntryPtr;
        if (DeleteEntry->p.b.FileName == Parser->FileName && DeleteEntry->p.b.Line == Parser->Line && DeleteEntry->p.b.CharacterPos == Parser->CharacterPos)
        {
            *EntryPtr = DeleteEntry->Next;
            deallocMem(DeleteEntry);
            pc->BreakpointCount--;

            return TRUE;
        }
    }

    return FALSE;
}

/* before we run a statement, check if there's anything we have to do with the debugger here */
void DebugCheckStatement(TParseStatePtr Parser)
{
    int DoBreak = FALSE;
    int AddAt;
    Picoc *pc = Parser->pc;
    
    /* has the user manually pressed break? */
    if (pc->DebugManualBreak)
    {
        PlatformPrintf(pc->CStdOut, "break\n");
        DoBreak = TRUE;
        pc->DebugManualBreak = FALSE;
    }
    
    /* is this a breakpoint location? */
    if (Parser->pc->BreakpointCount != 0 && DebugTableSearchBreakpoint(Parser, &AddAt) != NULL)
        DoBreak = TRUE;
    
    /* handle a break */
    if (DoBreak)
    {
        PlatformPrintf(pc->CStdOut, "Handling a break\n");
        PicocParseInteractiveNoStartPrompt(pc, FALSE);
    }
}

void DebugStep()
{
}
#endif /* !NO_DEBUGGER */
