/**
 * =========================================================================
 * File        : dll_ver.cpp
 * Project     : 0 A.D.
 * Description : return DLL version information.
 *
 * @author Jan.Wassenberg@stud.uni-karlsruhe.de
 * =========================================================================
 */

/*
 * Copyright (c) 2004 Jan Wassenberg
 *
 * Redistribution and/or modification are also permitted under the
 * terms of the GNU General Public License as published by the
 * Free Software Foundation (version 2 or later, at your option).
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "precompiled.h"

#include <stdio.h>
#include <stdlib.h>

#include "win_internal.h"
#include "dll_ver.h"

#if MSC_VERSION
#pragma comment(lib, "version.lib")		// DLL version
#endif

// get version information for the specified DLL.
static LibError get_ver(const char* module_path, char* out_ver, size_t out_ver_len)
{
	WIN_SAVE_LAST_ERROR;	// GetFileVersion*, Ver*

	// determine size of and allocate memory for version information.
	DWORD unused;
	const DWORD ver_size = GetFileVersionInfoSize(module_path, &unused);
	if(!ver_size)
		WARN_RETURN(ERR_FAIL);
	void* buf = malloc(ver_size);
	if(!buf)
		WARN_RETURN(ERR_NO_MEM);

	LibError ret = ERR_FAIL;	// single point of exit (for free())

	if(GetFileVersionInfo(module_path, 0, ver_size, buf))
	{
		u16* lang;	// -> 16 bit language ID, 16 bit codepage
		uint lang_len;
		const BOOL ok = VerQueryValue(buf, "\\VarFileInfo\\Translation", (void**)&lang, &lang_len);
		if(ok && lang && lang_len == 4)
		{
			char subblock[64];
			sprintf(subblock, "\\StringFileInfo\\%04X%04X\\FileVersion", lang[0], lang[1]);
			const char* in_ver;
			uint in_ver_len;
			if(VerQueryValue(buf, subblock, (void**)&in_ver, &in_ver_len))
			{
				strcpy_s(out_ver, out_ver_len, in_ver);
				ret = ERR_OK;
			}
		}
	}
	free(buf);

	WIN_RESTORE_LAST_ERROR;

	return ret;
}


//
// build a string containing DLL filename(s) and their version info.
//

static char* dll_list_buf;
static size_t dll_list_chars;
static char* dll_list_pos;

// set output buffer into which DLL names and their versions will be written.
void dll_list_init(char* buf, size_t chars)
{
	dll_list_pos = dll_list_buf = buf;
	dll_list_chars = chars;
}


// read DLL file version and append that and its name to the list.
// return 0 on success or a negative error code.
//
// name should preferably be the complete path to DLL, to make sure
// we don't inadvertently load another one on the library search path.
// we add the .dll extension if necessary.
LibError dll_list_add(const char* name)
{
	// not be called before dll_list_init or after failure
	if(!dll_list_pos)
		WARN_RETURN(ERR_LOGIC);

	// some driver names are stored in the registry without .dll extension.
	// if necessary, copy to new buffer and add it there.
	// note: do not change extension if present; some drivers have a
	// ".sys" extension, so always appending ".dll" is incorrect.
	char buf[MAX_PATH];
	const char* dll_name = name;
	const char* ext = strrchr(name, '.');
	if(!ext)
	{
		snprintf(buf, ARRAY_SIZE(buf), "%s.dll", name);
		dll_name = buf;
	}

	// read file version.
	char dll_ver[128] = "unknown";	// enclosed in () below
	(void)get_ver(dll_name, dll_ver, sizeof(dll_ver));
		// if this fails, default is already set and we don't want to abort.

	const ssize_t max_chars_to_write = (ssize_t)dll_list_chars - (dll_list_pos-dll_list_buf) - 10;
		// reserves enough room for subsequent comma and "..." strings.

	// not first time: prepend comma to string (room was reserved above).
	if(dll_list_pos != dll_list_buf)
		dll_list_pos += sprintf(dll_list_pos, ", ");

	// extract filename.
	const char* slash = strrchr(dll_name, '\\');
	const char* dll_fn = slash? slash+1 : dll_name;

	int len = snprintf(dll_list_pos, max_chars_to_write, "%s (%s)", dll_fn, dll_ver);
	// success
	if(len > 0)
	{
		dll_list_pos += len;
		return ERR_OK;
	}

	// didn't fit; complain
	sprintf(dll_list_pos, "...");	// (room was reserved above)
	dll_list_pos = 0;	// poison pill, prevent further calls
	WARN_RETURN(ERR_BUF_SIZE);
}
