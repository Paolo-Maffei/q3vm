/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// vm.c -- virtual machine

/*


intermix code and data
symbol table

a dll has one imported function: VM_SystemCall
and one exported function: Perform

*/

#include <string.h>
#include "vm_local.h"

vm_t    *currentVM = NULL;
vm_t    *lastVM    = NULL;
int     vm_debugLevel;

// used by Com_Error to get rid of running vm's before longjmp
// static int forced_unload;

#define MAX_VM      3
vm_t    vmTable[MAX_VM];


void VM_VmInfo_f( void );
void VM_VmProfile_f( void );


void VM_Debug( int level ) {
    vm_debugLevel = level;
}

intptr_t SV_GameSystemCalls( intptr_t *args );

/*
=============
Q_strncpyz
 
Safe strncpy that ensures a trailing zero
=============
*/
void Q_strncpyz( char *dest, const char *src, int destsize ) {
    if ( !dest ) {
        return;
    }
    if ( !src ) {
        return;
    }
    if ( destsize < 1 ) {
        return;
    }

    strncpy( dest, src, destsize-1 );
    dest[destsize-1] = 0;
}

void Com_Error( int level, const char *error, ... )
{
    fprintf(stderr, "%s\n", error);
    exit(-1);
}

/*
==============
VM_Init
==============
*/
void VM_Init( void ) {

    memset( vmTable, 0, sizeof( vmTable ) );
}


/*
===============
VM_ValueToSymbol

Assumes a program counter value
===============
*/
const char *VM_ValueToSymbol( vm_t *vm, int value ) {
    vmSymbol_t  *sym;
    static char     text[MAX_TOKEN_CHARS];

    sym = vm->symbols;
    if ( !sym ) {
        return "NO SYMBOLS";
    }

    // find the symbol
    while ( sym->next && sym->next->symValue <= value ) {
        sym = sym->next;
    }

    if ( value == sym->symValue ) {
        return sym->symName;
    }

    snprintf( text, sizeof( text ), "%s+%i", sym->symName, value - sym->symValue );

    return text;
}

/*
===============
VM_ValueToFunctionSymbol

For profiling, find the symbol behind this value
===============
*/
vmSymbol_t *VM_ValueToFunctionSymbol( vm_t *vm, int value ) {
    vmSymbol_t  *sym;
    static vmSymbol_t   nullSym;

    sym = vm->symbols;
    if ( !sym ) {
        return &nullSym;
    }

    while ( sym->next && sym->next->symValue <= value ) {
        sym = sym->next;
    }

    return sym;
}


/*
===============
VM_SymbolToValue
===============
*/
int VM_SymbolToValue( vm_t *vm, const char *symbol ) {
    vmSymbol_t  *sym;

    for ( sym = vm->symbols ; sym ; sym = sym->next ) {
        if ( !strcmp( symbol, sym->symName ) ) {
            return sym->symValue;
        }
    }
    return 0;
}

/*
===============
ParseHex
===============
*/
static int ParseHex( const char *text ) {
    int     value;
    int     c;

    value = 0;
    while ( ( c = *text++ ) != 0 ) {
        if ( c >= '0' && c <= '9' ) {
            value = value * 16 + c - '0';
            continue;
        }
        if ( c >= 'a' && c <= 'f' ) {
            value = value * 16 + 10 + c - 'a';
            continue;
        }
        if ( c >= 'A' && c <= 'F' ) {
            value = value * 16 + 10 + c - 'A';
            continue;
        }
    }

    return value;
}

/*
===============
VM_LoadSymbols
===============
*/
#if 0
void VM_LoadSymbols( vm_t *vm ) {
    union {
        char    *c;
        void    *v;
    } mapfile;
    char *text_p, *token;
    char    name[MAX_QPATH];
    char    symbols[MAX_QPATH];
    vmSymbol_t  **prev, *sym;
    int     count;
    int     value;
    int     chars;
    int     segment;
    int     numInstructions;

    // don't load symbols if not developer
    if ( !com_developer->integer ) {
        return;
    }

    COM_StripExtension(vm->name, name, sizeof(name));
    sprintf( symbols, sizeof( symbols ), "vm/%s.map", name );
    FS_ReadFile( symbols, &mapfile.v );
    if ( !mapfile.c ) {
        Com_Printf( "Couldn't load symbol file: %s\n", symbols );
        return;
    }

    numInstructions = vm->instructionCount;

    // parse the symbols
    text_p = mapfile.c;
    prev = &vm->symbols;
    count = 0;

    while ( 1 ) {
        token = COM_Parse( &text_p );
        if ( !token[0] ) {
            break;
        }
        segment = ParseHex( token );
        if ( segment ) {
            COM_Parse( &text_p );
            COM_Parse( &text_p );
            continue;       // only load code segment values
        }

        token = COM_Parse( &text_p );
        if ( !token[0] ) {
            Com_Printf( "WARNING: incomplete line at end of file\n" );
            break;
        }
        value = ParseHex( token );

        token = COM_Parse( &text_p );
        if ( !token[0] ) {
            Com_Printf( "WARNING: incomplete line at end of file\n" );
            break;
        }
        chars = strlen( token );
        sym = Hunk_Alloc( sizeof( *sym ) + chars, h_high );
        *prev = sym;
        prev = &sym->next;
        sym->next = NULL;

        // convert value from an instruction number to a code offset
        if ( value >= 0 && value < numInstructions ) {
            value = vm->instructionPointers[value];
        }

        sym->symValue = value;
        Q_strncpyz( sym->symName, token, chars + 1 );

        count++;
    }

    vm->numSymbols = count;
    Com_Printf( "%i symbols parsed from %s\n", count, symbols );
    FS_FreeFile( mapfile.v );
}
#endif

/*
============
VM_DllSyscall

Dlls will call this directly

 rcg010206 The horror; the horror.

  The syscall mechanism relies on stack manipulation to get its args.
   This is likely due to C's inability to pass "..." parameters to
   a function in one clean chunk. On PowerPC Linux, these parameters
   are not necessarily passed on the stack, so while (&arg[0] == arg)
   is true, (&arg[1] == 2nd function parameter) is not necessarily
   accurate, as arg's value might have been stored to the stack or
   other piece of scratch memory to give it a valid address, but the
   next parameter might still be sitting in a register.

  Quake's syscall system also assumes that the stack grows downward,
   and that any needed types can be squeezed, safely, into a signed int.

  This hack below copies all needed values for an argument to a
   array in memory, so that Quake can get the correct values. This can
   also be used on systems where the stack grows upwards, as the
   presumably standard and safe stdargs.h macros are used.

  As for having enough space in a signed int for your datatypes, well,
   it might be better to wait for DOOM 3 before you start porting.  :)

  The original code, while probably still inherently dangerous, seems
   to work well enough for the platforms it already works on. Rather
   than add the performance hit for those platforms, the original code
   is still in use there.

  For speed, we just grab 15 arguments, and don't worry about exactly
   how many the syscall actually needs; the extra is thrown away.

============
*/
intptr_t QDECL VM_DllSyscall( intptr_t arg, ... ) {
/*
#if !id386 || defined __clang__
  // rcg010206 - see commentary above
  intptr_t args[MAX_VMSYSCALL_ARGS];
  int i;
  va_list ap;

  args[0] = arg;

  va_start(ap, arg);
  for (i = 1; i < ARRAY_LEN (args); i++)
    args[i] = va_arg(ap, intptr_t);
  va_end(ap);

  return currentVM->systemCall( args );
#else // original id code
*/
    return currentVM->systemCall( &arg );
/*
#endif
*/
}


/*
=================
VM_LoadQVM

Load a .qvm file
=================
*/
static uint8_t imageTemp[5*1024*1024];
vmHeader_t *VM_LoadQVM(vm_t *vm, qboolean alloc, qboolean unpure)
{
    int  dataLength;
    int  i;
    char filename[MAX_QPATH];
    union {
        vmHeader_t *h;
        void *v;
    } header;
    FILE* f;
    size_t sz;

    // load the image
    snprintf( filename, sizeof(filename), "%s", vm->name );
    Com_Printf( "Loading vm file %s...\n", filename );

    f = fopen(filename, "rb");
    if (!f)
    {
        Com_Printf("Failed to open file %s.\n", filename);
        return NULL;
    }

    fseek(f, 0L, SEEK_END);
    sz = ftell(f);
    rewind(f);
    if (sz > sizeof(imageTemp))
    {
        fclose(f);
        return NULL;
    }
    size_t result = fread(imageTemp, 1, sz, f);
    if (result != sz)
    {
        fclose(f);
        return NULL;
    }
    fclose(f);
    header.v = imageTemp;

    if ( !header.h ) {
        Com_Printf( "Failed.\n" );
        VM_Free( vm );

        Com_Printf("Warning: Couldn't open VM file %s\n", filename);

        return NULL;
    }

    if( LittleLong( header.h->vmMagic ) == VM_MAGIC ) {
        // byte swap the header
        // sizeof( vmHeader_t ) - sizeof( int ) is the 1.32b vm header size
        for ( i = 0 ; i < ( sizeof( vmHeader_t ) - sizeof( int ) ) / 4 ; i++ ) {
            ((int *)header.h)[i] = LittleLong( ((int *)header.h)[i] );
        }

        // validate
        if ( header.h->bssLength < 0
            || header.h->dataLength < 0
            || header.h->litLength < 0
            || header.h->codeLength <= 0 )
        {
            Com_Printf("Warning: %s has bad header\n", filename);
            return NULL;
        }
    } else {
        Com_Printf("Warning: %s does not have a recognisable "
                "magic number in its header\n", filename);
        return NULL;
    }

    // round up to next power of 2 so all data operations can
    // be mask protected
    dataLength = header.h->dataLength + header.h->litLength +
        header.h->bssLength;
    for ( i = 0 ; dataLength > ( 1 << i ) ; i++ ) {
    }
    dataLength = 1 << i;

    if(alloc)
    {
        // allocate zero filled space for initialized and uninitialized data
        // leave some space beyond data mask so we can secure all mask operations
        vm->dataAlloc = dataLength + 4;
        vm->dataBase = (uint8_t*)malloc(vm->dataAlloc);
        vm->dataMask = dataLength - 1;
    }
    else
    {
        // clear the data, but make sure we're not clearing more than allocated
        if(vm->dataAlloc != dataLength + 4)
        {
            Com_Printf("Warning: Data region size of %s not matching after "
                    "VM_Restart()\n", filename);
            return NULL;
        }

        Com_Memset(vm->dataBase, 0, vm->dataAlloc);
    }

    // copy the intialized data
    Com_Memcpy( vm->dataBase, (uint8_t *)header.h + header.h->dataOffset,
        header.h->dataLength + header.h->litLength );

    // byte swap the longs
    for ( i = 0 ; i < header.h->dataLength ; i += 4 ) {
        *(int *)(vm->dataBase + i) = LittleLong( *(int *)(vm->dataBase + i ) );
    }


    return header.h;
}

/*
=================
VM_Restart

Reload the data, but leave everything else in place
This allows a server to do a map_restart without changing memory allocation

We need to make sure that servers can access unpure QVMs (not contained in any pak)
even if the client is pure, so take "unpure" as argument.
=================
*/
vm_t *VM_Restart(vm_t *vm, qboolean unpure)
{
    vmHeader_t  *header;

    // load the image
    Com_Printf("VM_Restart()\n");

    if(!(header = VM_LoadQVM(vm, qfalse, unpure)))
    {
        Com_Error(ERR_DROP, "VM_Restart failed");
    }

    // free the original file
    // FS_FreeFile(header);

    return vm;
}

/*
================
VM_Create

If image ends in .qvm it will be interpreted, otherwise
it will attempt to load as a system dll
================
*/
vm_t *VM_Create( const char *module, vmInterpret_t interpret ) {
    vm_t        *vm;
    vmHeader_t  *header;
    // int i;
    // char filename[MAX_OSPATH];

    if ( !module || !module[0] ) {
        Com_Error( ERR_FATAL, "VM_Create: bad parms" );
    }

    vm = &vmTable[0];

    Q_strncpyz(vm->name, module, sizeof(vm->name));
    header = VM_LoadQVM(vm, qtrue, qfalse);
    if (!header)
    {
        Com_Error( ERR_FATAL, "Failed to load bytecode.\n");
    }

    // VM_Free overwrites the name on failed load
    Q_strncpyz(vm->name, module, sizeof(vm->name));

    vm->systemCall = SV_GameSystemCalls;

    // allocate space for the jump targets, which will be filled in by the compile/prep functions
    vm->instructionCount = header->instructionCount;
    vm->instructionPointers = (intptr_t*)malloc(vm->instructionCount * sizeof(*vm->instructionPointers));

    // copy or compile the instructions
    vm->codeLength = header->codeLength;

    vm->compiled = qfalse;

    // VM_Compile may have reset vm->compiled if compilation failed
    if (!vm->compiled)
    {
        VM_PrepareInterpreter( vm, header );
    }

    // load the map file
    // VM_LoadSymbols( vm );

    // the stack is implicitly at the end of the image
    vm->programStack = vm->dataMask + 1;
    vm->stackBottom = vm->programStack - PROGRAM_STACK_SIZE;

    return vm;
}

#if 0
/*
==============
VM_Free
==============
*/
void VM_Free( vm_t *vm ) {

    if(!vm) {
        return;
    }

    if(vm->callLevel) {
        if(!forced_unload) {
            Com_Error( ERR_FATAL, "VM_Free(%s) on running vm", vm->name );
            return;
        } else {
            Com_Printf( "forcefully unloading %s vm\n", vm->name );
        }
    }

    if(vm->destroy)
        vm->destroy(vm);

    if ( vm->dllHandle ) {
        Sys_UnloadDll( vm->dllHandle );
        Com_Memset( vm, 0, sizeof( *vm ) );
    }
#if 0   // now automatically freed by hunk
    if ( vm->codeBase ) {
        Z_Free( vm->codeBase );
    }
    if ( vm->dataBase ) {
        Z_Free( vm->dataBase );
    }
    if ( vm->instructionPointers ) {
        Z_Free( vm->instructionPointers );
    }
#endif
    Com_Memset( vm, 0, sizeof( *vm ) );

    currentVM = NULL;
    lastVM = NULL;
}

void VM_Clear(void) {
    int i;
    for (i=0;i<MAX_VM; i++) {
        VM_Free(&vmTable[i]);
    }
}

void VM_Forced_Unload_Start(void) {
    forced_unload = 1;
}

void VM_Forced_Unload_Done(void) {
    forced_unload = 0;
}
#endif

void *VM_ArgPtr( intptr_t intValue ) {
    if ( !intValue ) {
        return NULL;
    }
    // currentVM is missing on reconnect
    if ( currentVM==NULL )
      return NULL;

    if ( currentVM->entryPoint ) {
        return (void *)(currentVM->dataBase + intValue);
    }
    else {
        return (void *)(currentVM->dataBase + (intValue & currentVM->dataMask));
    }
}
#if 0

void *VM_ExplicitArgPtr( vm_t *vm, intptr_t intValue ) {
    if ( !intValue ) {
        return NULL;
    }

    // currentVM is missing on reconnect here as well?
    if ( currentVM==NULL )
      return NULL;

    //
    if ( vm->entryPoint ) {
        return (void *)(vm->dataBase + intValue);
    }
    else {
        return (void *)(vm->dataBase + (intValue & vm->dataMask));
    }
}
#endif


/*
==============
VM_Call


Upon a system call, the stack will look like:

sp+32   parm1
sp+28   parm0
sp+24   return value
sp+20   return address
sp+16   local1
sp+14   local0
sp+12   arg1
sp+8    arg0
sp+4    return stack
sp      return address

An interpreted function will immediately execute
an OP_ENTER instruction, which will subtract space for
locals from sp
==============
*/

intptr_t QDECL VM_Call( vm_t *vm, int callnum, ... )
{
    vm_t    *oldVM;
    intptr_t r;

    if(!vm || !vm->name[0])
    {
        Com_Error(ERR_FATAL, "VM_Call with NULL vm");
    }

    oldVM = currentVM;
    currentVM = vm;
    lastVM = vm;

    if ( vm_debugLevel ) {
      Com_Printf( "VM_Call( %d )\n", callnum );
    }

    ++vm->callLevel;
    r = VM_CallInterpreted( vm, (int*)&callnum );
    --vm->callLevel;

    if ( oldVM != NULL )
      currentVM = oldVM;
    return r;
}

//=================================================================

static int QDECL VM_ProfileSort( const void *a, const void *b ) {
    vmSymbol_t  *sa, *sb;

    sa = *(vmSymbol_t **)a;
    sb = *(vmSymbol_t **)b;

    if ( sa->profileCount < sb->profileCount ) {
        return -1;
    }
    if ( sa->profileCount > sb->profileCount ) {
        return 1;
    }
    return 0;
}

/*
==============
VM_VmProfile_f

==============
*/
#if 0
void VM_VmProfile_f( void ) {
    vm_t        *vm;
    vmSymbol_t  **sorted, *sym;
    int         i;
    double      total;

    if ( !lastVM ) {
        return;
    }

    vm = lastVM;

    if ( !vm->numSymbols ) {
        return;
    }

    sorted = Z_Malloc( vm->numSymbols * sizeof( *sorted ) );
    sorted[0] = vm->symbols;
    total = sorted[0]->profileCount;
    for ( i = 1 ; i < vm->numSymbols ; i++ ) {
        sorted[i] = sorted[i-1]->next;
        total += sorted[i]->profileCount;
    }

    qsort( sorted, vm->numSymbols, sizeof( *sorted ), VM_ProfileSort );

    for ( i = 0 ; i < vm->numSymbols ; i++ ) {
        int     perc;

        sym = sorted[i];

        perc = 100 * (float) sym->profileCount / total;
        Com_Printf( "%2i%% %9i %s\n", perc, sym->profileCount, sym->symName );
        sym->profileCount = 0;
    }

    Com_Printf("    %9.0f total\n", total );

    Z_Free( sorted );
}
#endif

/*
==============
VM_VmInfo_f

==============
*/
void VM_VmInfo_f( void ) {
    vm_t    *vm;
    int     i;

    Com_Printf( "Registered virtual machines:\n" );
    for ( i = 0 ; i < MAX_VM ; i++ ) {
        vm = &vmTable[i];
        if ( !vm->name[0] ) {
            break;
        }
        Com_Printf( "%s : ", vm->name );
        if ( vm->dllHandle ) {
            Com_Printf( "native\n" );
            continue;
        }
        if ( vm->compiled ) {
            Com_Printf( "compiled on load\n" );
        } else {
            Com_Printf( "interpreted\n" );
        }
        Com_Printf( "    code length : %7i\n", vm->codeLength );
        Com_Printf( "    table length: %7i\n", vm->instructionCount*4 );
        Com_Printf( "    data length : %7i\n", vm->dataMask + 1 );
    }
}

/*
===============
VM_LogSyscalls

Insert calls to this while debugging the vm compiler
===============
*/
void VM_LogSyscalls( int *args ) {
    static  int     callnum;
    static  FILE    *f;

    if ( !f ) {
        f = fopen("syscalls.log", "w" );
    }
    callnum++;
    fprintf(f, "%i: %p (%i) = %i %i %i %i\n", callnum, (void*)(args - (int *)currentVM->dataBase),
        args[0], args[1], args[2], args[3], args[4] );
}

/*
=================
VM_BlockCopy
Executes a block copy operation within currentVM data space
=================
*/

void VM_BlockCopy(unsigned int dest, unsigned int src, size_t n)
{
    unsigned int dataMask = currentVM->dataMask;

    if ((dest & dataMask) != dest
    || (src & dataMask) != src
    || ((dest + n) & dataMask) != dest + n
    || ((src + n) & dataMask) != src + n)
    {
        Com_Error(ERR_DROP, "OP_BLOCK_COPY out of range!");
    }

    Com_Memcpy(currentVM->dataBase + dest, currentVM->dataBase + src, n);
}

void CopyShortSwap(void *dest, void *src)
{
    uint8_t *to = dest, *from = src;

    to[0] = from[1];
    to[1] = from[0];
}

void CopyLongSwap(void *dest, void *src)
{
    uint8_t *to = dest, *from = src;

    to[0] = from[3];
    to[1] = from[2];
    to[2] = from[1];
    to[3] = from[0];
}

short   ShortSwap (short l)
{
    uint8_t    b1,b2;

    b1 = l&255;
    b2 = (l>>8)&255;

    return (b1<<8) + b2;
}

short   ShortNoSwap (short l)
{
    return l;
}

int    LongSwap (int l)
{
    uint8_t    b1,b2,b3,b4;

    b1 = l&255;
    b2 = (l>>8)&255;
    b3 = (l>>16)&255;
    b4 = (l>>24)&255;

    return ((int)b1<<24) + ((int)b2<<16) + ((int)b3<<8) + b4;
}

int LongNoSwap (int l)
{
    return l;
}

float FloatSwap (const float *f) {
    floatint_t out;

    out.f = *f;
    out.ui = LongSwap(out.ui);

    return out.f;
}

float FloatNoSwap (const float *f)
{
    return *f;
}


intptr_t SV_GameSystemCalls( intptr_t *args ) {
    switch( args[0] ) {
    case G_PRINT:
        Com_Printf( "%s", (const char*)VMA(1) );
        return 0;
    case G_ERROR:
        Com_Error( ERR_DROP, "%s", (const char*)VMA(1) );
        return 0;
    case G_MILLISECONDS:
        return 0;
    case G_CVAR_REGISTER:
        return 0;
    case G_CVAR_UPDATE:
        return 0;
    case G_CVAR_SET:
        return 0;
    case G_CVAR_VARIABLE_INTEGER_VALUE:
        return 0;
    case G_CVAR_VARIABLE_STRING_BUFFER:
        return 0;
    case G_ARGC:
        return 0;
    case G_ARGV:
        return 0;
    case G_SEND_CONSOLE_COMMAND:
        return 0;
    case G_FS_FOPEN_FILE:
        return 0;
    case G_FS_READ:
        return 0;
    case G_FS_WRITE:
        return 0;
    case G_FS_FCLOSE_FILE:
        return 0;
    case G_FS_GETFILELIST:
        return 0;
    case G_FS_SEEK:
        return 0;
    case G_LOCATE_GAME_DATA:
        return 0;
    case G_DROP_CLIENT:
        return 0;
    case G_SEND_SERVER_COMMAND:
        return 0;
    case G_LINKENTITY:
        return 0;
    case G_UNLINKENTITY:
        return 0;
    case G_ENTITIES_IN_BOX:
        return 0;
    case G_ENTITY_CONTACT:
        return 0;
    case G_ENTITY_CONTACTCAPSULE:
        return 0;
    case G_TRACE:
        return 0;
    case G_TRACECAPSULE:
        return 0;
    case G_POINT_CONTENTS:
        return 0;
    case G_SET_BRUSH_MODEL:
        return 0;
    case G_IN_PVS:
        return 0;
    case G_IN_PVS_IGNORE_PORTALS:
        return 0;
    case G_SET_CONFIGSTRING:
        return 0;
    case G_GET_CONFIGSTRING:
        return 0;
    case G_SET_USERINFO:
        return 0;
    case G_GET_USERINFO:
        return 0;
    case G_GET_SERVERINFO:
        return 0;
    case G_ADJUST_AREA_PORTAL_STATE:
        return 0;
    case G_AREAS_CONNECTED:
        return 0;

    case TRAP_MEMSET:
        Com_Memset( VMA(1), args[2], args[3] );
        return 0;

    case TRAP_MEMCPY:
        Com_Memcpy( VMA(1), VMA(2), args[3] );
        return 0;

    case TRAP_STRNCPY:
        strncpy( VMA(1), VMA(2), args[3] );
        return args[1];

    case TRAP_SIN:
        return 0;

    case TRAP_COS:
        return 0;

    case TRAP_ATAN2:
        return 0;

    case TRAP_SQRT:
        return 0;

    case TRAP_MATRIXMULTIPLY:
        return 0;

    case TRAP_ANGLEVECTORS:
        return 0;

    default:
        Com_Error( ERR_DROP, "Bad game system trap: %ld", (long int) args[0] );
    }
    return 0;
}