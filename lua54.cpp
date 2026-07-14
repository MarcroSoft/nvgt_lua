/* lua54.cpp - compiles the entire vendored Lua 5.4 distribution as a single C++ translation unit.
 * Building Lua as C++ makes lua_error use C++ exceptions instead of longjmp, which is required for it to safely
 * unwind through the bridge's C++ stack frames. The include order mirrors upstream's onelua.c.
 */

#define LUA_CORE
#define LUA_LIB

#include "lua54/lprefix.h"

// core
#include "lua54/lzio.c"
#include "lua54/lctype.c"
#include "lua54/lopcodes.c"
#include "lua54/lmem.c"
#include "lua54/lundump.c"
#include "lua54/ldump.c"
#include "lua54/lstate.c"
#include "lua54/lgc.c"
#include "lua54/llex.c"
#include "lua54/lcode.c"
#include "lua54/lparser.c"
#include "lua54/ldebug.c"
#include "lua54/lfunc.c"
#include "lua54/lobject.c"
#include "lua54/ltm.c"
#include "lua54/lstring.c"
#include "lua54/ltable.c"
#include "lua54/ldo.c"
#include "lua54/lvm.c"
#include "lua54/lapi.c"

// auxiliary library
#include "lua54/lauxlib.c"

// standard libraries
#include "lua54/lbaselib.c"
#include "lua54/lcorolib.c"
#include "lua54/ldblib.c"
#include "lua54/liolib.c"
#include "lua54/lmathlib.c"
#include "lua54/loadlib.c"
#include "lua54/loslib.c"
#include "lua54/lstrlib.c"
#include "lua54/ltablib.c"
#include "lua54/lutf8lib.c"
#include "lua54/linit.c"
