/* lua55.cpp - compiles the entire vendored Lua 5.5 distribution as a single C++ translation unit.
 * Building Lua as C++ makes lua_error use C++ exceptions instead of longjmp, which is required for it to safely
 * unwind through the bridge's C++ stack frames. The include order mirrors upstream's onelua.c.
 */

#define LUA_CORE
#define LUA_LIB

#include "lua55/lprefix.h"

// core
#include "lua55/lzio.c"
#include "lua55/lctype.c"
#include "lua55/lopcodes.c"
#include "lua55/lmem.c"
#include "lua55/lundump.c"
#include "lua55/ldump.c"
#include "lua55/lstate.c"
#include "lua55/lgc.c"
#include "lua55/llex.c"
// llex.c defines a next(ls) macro for its own use; undefine it so it cannot clash with std::next when later files
// pull in standard library headers compiled as C++ (breaks the build with libstdc++).
#undef next
#include "lua55/lcode.c"
#include "lua55/lparser.c"
#include "lua55/ldebug.c"
#include "lua55/lfunc.c"
#include "lua55/lobject.c"
#include "lua55/ltm.c"
#include "lua55/lstring.c"
#include "lua55/ltable.c"
#include "lua55/ldo.c"
#include "lua55/lvm.c"
#include "lua55/lapi.c"

// auxiliary library
#include "lua55/lauxlib.c"

// standard libraries
#include "lua55/lbaselib.c"
#include "lua55/lcorolib.c"
#include "lua55/ldblib.c"
#include "lua55/liolib.c"
#include "lua55/lmathlib.c"
#include "lua55/loadlib.c"
#include "lua55/loslib.c"
#include "lua55/lstrlib.c"
#include "lua55/ltablib.c"
#include "lua55/lutf8lib.c"
#include "lua55/linit.c"
