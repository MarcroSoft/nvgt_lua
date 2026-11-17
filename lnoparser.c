/* lnoparser.c - front-end stubs for the bytecode-only Lua build
 * Added for NVGT, not part of upstream Lua; adapted from Oidua's lnoparser.c (https://github.com/MarcroSoft/Oidua).
 *
 * Compiled games only need to run Lua code that was precompiled with lua_compile or lua_compile_file, so when the
 * plugin is linked statically, NVGT's stubs get a Lua without its front-end: lua55.cpp leaves out llex.c, lcode.c,
 * lparser.c and ldump.c when NVGT_LUA_BYTECODE_ONLY is defined and includes this file in their place. Every file in
 * lua55/ stays the official release, byte for byte; the whole delta is here and in that #if in lua55.cpp.
 *
 * Only three symbols tie the rest of Lua to the front-end:
 *
 *   luaX_init    called once from f_luaopen in lstate.c to intern the reserved words; the only reference into llex.c
 *                from outside the parser
 *   luaY_parser  called from f_parser in ldo.c to compile source text; the only reference into lparser.c and lcode.c
 *   luaU_dump    called from lua_dump in lapi.c to write bytecode; the only reference into ldump.c
 *
 * The standard libraries all stay, unlike in Oidua: require, package.path and set_pack's module lookup work as they do
 * in nvgt, they just find precompiled chunks.
 */

/* Interning the reserved words only ever serves the lexer, which is not in this build, so there is nothing to intern. */
void luaX_init (lua_State *L) {
  UNUSED(L);
}


/* Compiling source text is precisely what this build exists to prevent. Fail the way the lexer would have:
 * LUA_ERRSYNTAX with a message left on the stack, so lua_load, load, loadfile, require and exec report it through
 * their normal path. */
LClosure *luaY_parser (lua_State *L, ZIO *z, Table *anchor, Mbuffer *buff, Dyndata *dyd, const char *name,
                       int firstchar) {
  char buffid[LUA_IDSIZE];
  UNUSED(z); UNUSED(anchor); UNUSED(buff); UNUSED(dyd); UNUSED(firstchar);
  luaO_chunkid(buffid, name, strlen(name));
  luaO_pushfstring(L, "%s: compiled games can only load precompiled Lua code (see lua_compile and lua_compile_file)",
                   buffid);
  luaD_throw(L, LUA_ERRSYNTAX);
  return NULL;  /* to avoid warnings; luaD_throw does not return */
}


/* Writing bytecode needs ldump.c. Raise an error rather than return a failure status: Lua 5.5's string.dump ignores
 * lua_dump's result and would hand back the function itself instead of a string. lua_compile never gets here, as it
 * checks nvgt_lua_has_compiler first. */
int luaU_dump (lua_State *L, const Proto *f, lua_Writer w, void *data, int strip) {
  UNUSED(f); UNUSED(w); UNUSED(data); UNUSED(strip);
  luaG_runerror(L, "compiled games cannot dump Lua functions to bytecode");
  return 1;  /* to avoid warnings; luaG_runerror does not return */
}
