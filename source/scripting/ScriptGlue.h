
#ifndef _SCRIPTGLUE_H_
#define _SCRIPTGLUE_H_

#include "ScriptingHost.h"

// Functions to be called from Javascript:

JSBool WriteLog(JSContext * context, JSObject * globalObject, unsigned int argc, jsval *argv, jsval *rval);

JSBool getEntityByHandle( JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval );
JSBool getEntityTemplate( JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval );
JSBool setTimeout( JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval );
JSBool setInterval( JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval );
JSBool cancelInterval( JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval );

// Returns the sort-of-global object associated with the current GUI
JSBool getGUIGlobal(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

// Returns the global object, e.g. for setting global variables.
JSBool getGlobal(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

JSBool setCursor(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

JSBool startServer(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);
JSBool joinGame(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);
JSBool startGame(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);
JSBool endGame(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

// Replaces the current language (locale) with a new one
JSBool loadLanguage(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

// Tells the main loop to stop looping
JSBool exitProgram(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

// Crashes.
JSBool crash(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

// Tries to print the amount of remaining video memory. (I don't like starting functions with underscores).
JSBool js_mem(JSContext* context, JSObject* globalObject, unsigned int argc, jsval* argv, jsval* rval);

extern JSFunctionSpec ScriptFunctionTable[];
extern JSPropertySpec ScriptGlobalTable[];

#endif
