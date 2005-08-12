// vi: set ts=4 sw=4 :
// vim: set tw=75 :

// mreg.cpp - functions for registered items (classes MRegCmd, MRegCmdList)

/*
 * Copyright (c) 2001-2005 Will Day <willday@hpgx.net>
 *
 *    This file is part of Metamod.
 *
 *    Metamod is free software; you can redistribute it and/or modify it
 *    under the terms of the GNU General Public License as published by the
 *    Free Software Foundation; either version 2 of the License, or (at
 *    your option) any later version.
 *
 *    Metamod is distributed in the hope that it will be useful, but
 *    WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Metamod; if not, write to the Free Software Foundation,
 *    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    In addition, as a special exception, the author gives permission to
 *    link the code of this program with the Half-Life Game Engine ("HL
 *    Engine") and Modified Game Libraries ("MODs") developed by Valve,
 *    L.L.C ("Valve").  You must obey the GNU General Public License in all
 *    respects for all of the code used other than the HL Engine and MODs
 *    from Valve.  If you modify this file, you may extend this exception
 *    to your version of the file, but you are not obligated to do so.  If
 *    you do not wish to do so, delete this exception statement from your
 *    version.
 *
 */

#ifdef linux
// enable extra routines in system header files, like strsignal
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#endif /* linux */

#include <string.h>			// strsignal, strdup, etc
#include <errno.h>			// strerror, etc

#include <extdll.h>			// always

#include "mreg.h"			// me
#include "metamod.h"		// Plugins, etc
#include "mlist.h"			// class MPluginList
#include "mplugin.h"		// class MPlugin
#include "types_meta.h"		// mBOOL
#include "log_meta.h"		// META_LOG, etc
#include "osdep.h"			// os_safe_call, etc


///// class MRegCmd:

// Init values.  It would probably be more "proper" to use containers and
// constructors, rather than arrays and init-functions.
void DLLINTERNAL MRegCmd::init(int idx)
{
	index = idx;
	name = NULL;
	pfnCmd = NULL;
	plugid = 0;
	status = RG_INVALID;
}

// Try to call the function.  Relies on OS-specific routine to attempt
// calling the function without generating a segfault from an unloaded
// plugin DLL.
// meta_errno values:
//  - ME_BADREQ		function disabled/invalid
//  - ME_ARGUMENT	function pointer is null
mBOOL DLLINTERNAL MRegCmd::call(void) {
	mBOOL ret;

	// can we expect to call this function?
	if(unlikely(status != RG_VALID))
		RETURN_ERRNO(mFALSE, ME_BADREQ);
	if(unlikely(!pfnCmd))
		RETURN_ERRNO(mFALSE, ME_ARGUMENT);

	// try to call this function
	ret=os_safe_call(pfnCmd);
	if(unlikely(!ret)) {
		META_DEBUG(4, ("Plugin reg_cmd '%s' called after unloaded; removed from list", name));
		status=RG_INVALID;
		pfnCmd=NULL;
		// NOTE: we can't free the malloc'd space for the name, as that
		// would just re-introduce the segfault problem..
	}
	// meta_errno (if failed) is set already in os_safe_call()
	return(ret);
}


///// class MRegCmdList:

// Constructor
MRegCmdList::MRegCmdList(void)
	: mlist(0), size(REG_CMD_GROWSIZE), endlist(0)
{
	int i;
	mlist = (MRegCmd *) calloc(1, size * sizeof(MRegCmd));
	// initialize array
	for(i=0; likely(i < size); i++)
		mlist[i].init(i+1);		// 1-based index
	endlist=0;
}

// Try to find a registered function with the given name.
// meta_errno values:
//  - ME_NOTFOUND	couldn't find a matching function
MRegCmd * DLLINTERNAL MRegCmdList::find(const char *findname) {
	int i;
	for(i=0; likely(i < endlist); i++) {
		if(unlikely(!strcasecmp(mlist[i].name, findname)))
			return(&mlist[i]);
	}
	RETURN_ERRNO(NULL, ME_NOTFOUND);
}

// Add the given name to the list and return the instance.  This only
// writes the "name" to the new cmd; other fields are writtin by caller
// (meta_AddServerCommand).
// meta_errno values:
//  - ME_NOMEM			couldn't realloc or malloc for various parts
MRegCmd * DLLINTERNAL MRegCmdList::add(const char *addname) {
	MRegCmd *icmd;

	if(unlikely(endlist==size)) {
		// grow array
		MRegCmd *temp;
		int i, newsize;
		newsize=size+REG_CMD_GROWSIZE;
		META_DEBUG(6, ("Growing reg cmd list from %d to %d", size, newsize));
		temp = (MRegCmd *) realloc(mlist, newsize*sizeof(MRegCmd));
		if(unlikely(!temp)) {
			META_WARNING("Couldn't grow registered command list to %d for '%s': %s", newsize, addname, strerror(errno));
			RETURN_ERRNO(NULL, ME_NOMEM);
		}
		mlist=temp;
		size=newsize;
		// initialize new (unused) entries
		for(i=endlist; likely(i<size); i++) {
			memset(&mlist[i], 0, sizeof(mlist[i]));
			mlist[i].init(i+1);		// 1-based
		}
	}
	icmd = &mlist[endlist];

	// Malloc space separately for the command name, because:
	//  - Can't point to memory loc in plugin (another segv waiting to 
	//    happen).
	//  - Can't point to memory in mlist which might get moved later by 
	//    realloc (again, segv).
	icmd->name=strdup(addname);
	if(unlikely(!icmd->name)) {
		META_WARNING("Couldn't strdup for adding reg cmd name '%s': %s", 
				addname, strerror(errno));
		RETURN_ERRNO(NULL, ME_NOMEM);
	}
	endlist++;
	
	return(icmd);
}

// Disable any functions belonging to the given plugin (by index id).
void DLLINTERNAL MRegCmdList::disable(int plugin_id) {
	int i;
	for(i=0; likely(i < size); i++) {
		if(unlikely(mlist[i].plugid == plugin_id))
			mlist[i].status = RG_INVALID;
	}
}

// List all the registered commands.
void DLLINTERNAL MRegCmdList::show(void) {
	int i, n=0, a=0;
	MRegCmd *icmd;
	MPlugin *iplug;
	char bplug[18+1];	// +1 for term null

	META_CONS("Registered plugin commands:");
	META_CONS("  %*s  %-*s  %-s", 
			WIDTH_MAX_REG, "",
			sizeof(bplug)-1, "plugin", "command");
	
	for(i=0; likely(i < endlist); i++) {
		icmd = &mlist[i];
		
		if(likely(icmd->status==RG_VALID)) {
			iplug=Plugins->find(icmd->plugid);
		
			if(likely(iplug))
				STRNCPY(bplug, iplug->desc, sizeof(bplug));
			else
				STRNCPY(bplug, "(unknown)", sizeof(bplug));
		}
		else
			STRNCPY(bplug, "(unloaded)", sizeof(bplug));
		
		META_CONS(" [%*d] %-*s  %-s", 
				WIDTH_MAX_REG, icmd->index, 
				sizeof(bplug)-1, bplug,
				icmd->name);
		
		if(likely(icmd->status==RG_VALID))
			a++;
		
		n++;
	}
	
	META_CONS("%d commands, %d available (%d allocated)", n, a, size);
}

// List all the registered commands for the given plugin id.
void DLLINTERNAL MRegCmdList::show(int plugin_id) {
	int i, n=0;
	MRegCmd *icmd;
	
	/*
	// If OS doesn't support DLFNAME, then we can't know what the plugin's
	// registered cvars are.
	DLFNAME(NULL);
	if(unlikely(meta_errno==ME_OSNOTSUP)) {
		META_CONS("Registered commands: unknown (can't get info under this OS)");
		return;
	}
	*/
	
	META_CONS("Registered commands:");
	for(i=0; likely(i < endlist); i++) {
		icmd = &mlist[i];
		if(likely(icmd->plugid != plugin_id))
			continue;
		META_CONS("   %s", icmd->name);
		n++;
	}
	META_CONS("%d commands", n);
}


///// class MRegCvar:

// Init values.  It would probably be more "proper" to use containers and
// constructors, rather than arrays and init-functions.
void DLLINTERNAL MRegCvar::init(int idx)
{
	index = idx;
	data = NULL;
	plugid = 0;
	status = RG_INVALID;
}

// Set the cvar, copying values from given cvar.
// meta_errno values:
//  - ME_ARGUMENT	given cvar doesn't match this cvar
mBOOL DLLINTERNAL MRegCvar::set(cvar_t *src) {
	if(unlikely(strcasecmp(src->name, data->name))) {
		META_WARNING("Tried to set cvar with mismatched name; src=%s dst=%s",
				src->name, data->name);
		RETURN_ERRNO(mFALSE, ME_ARGUMENT);
	}
	// Would like to free() existing string, but can't tell where it was
	// allocated...
	data->string = strdup(src->string);
	data->flags = src->flags;
	data->value = src->value;
	data->next = src->next;
	return(mTRUE);
}


///// class MRegCvarList:

// Constructor
MRegCvarList::MRegCvarList(void)
	: vlist(0), size(REG_CVAR_GROWSIZE), endlist(0)
{
	int i;
	vlist = (MRegCvar *) calloc(1, size * sizeof(MRegCvar));
	// initialize array
	for(i=0; likely(i < size); i++)
		vlist[i].init(i+1);		// 1-based
	endlist=0;
}

// Add the given cvar name to the list and return the instance.  This only
// writes the "name" to the new cvar; other fields are written with
// cvar::set().
// meta_errno values:
//  - ME_NOMEM			couldn't alloc or realloc for various parts
MRegCvar * DLLINTERNAL MRegCvarList::add(const char *addname) {
	MRegCvar *icvar;

	if(unlikely(endlist==size)) {
		// grow array
		MRegCvar *temp;
		int i, newsize;
		newsize=size+REG_CVAR_GROWSIZE;
		META_DEBUG(6, ("Growing reg cvar list from %d to %d", size, newsize));
		temp = (MRegCvar *) realloc(vlist, newsize*sizeof(MRegCvar));
		if(unlikely(!temp)) {
			META_WARNING("Couldn't grow registered cvar list to %d for '%s'; %s", newsize, addname, strerror(errno));
			RETURN_ERRNO(NULL, ME_NOMEM);
		}
		vlist=temp;
		size=newsize;
		// initialize new (unused) entries
		for(i=endlist; likely(i<size); i++) {
			memset(&vlist[i], 0, sizeof(vlist[i]));
			vlist[i].init(i+1);		// 1-based
		}
	}

	icvar = &vlist[endlist];

	// Malloc space for the cvar and cvar name, for two reasons:
	//  - Can't point to memory loc in plugin (another segv waiting to 
	//    happen).
	//  - Can't point to memory in vlist which might get moved later by 
	//    realloc (again, segv).
	icvar->data = (cvar_t *) calloc(1, sizeof(cvar_t));
	if(unlikely(!icvar->data)) {
		META_WARNING("Couldn't malloc cvar for adding reg cvar name '%s': %s", 
				addname, strerror(errno));
		RETURN_ERRNO(NULL, ME_NOMEM);
	}
	icvar->data->name=strdup(addname);
	if(unlikely(!icvar->data->name)) {
		META_WARNING("Couldn't strdup for adding reg cvar name '%s': %s", 
				addname, strerror(errno));
		RETURN_ERRNO(NULL, ME_NOMEM);
	}
	endlist++;
	
	return(icvar);
}

// Try to find a registered cvar with the given name.
// meta_errno values:
//  - ME_NOTFOUND	couldn't find a matching cvar
MRegCvar * DLLINTERNAL MRegCvarList::find(const char *findname) {
	int i;
	for(i=0; likely(i < endlist); i++) {
		if(unlikely(!strcasecmp(vlist[i].data->name, findname)))
			return(&vlist[i]);
	}
	RETURN_ERRNO(NULL, ME_NOTFOUND);
}

// Disable any cvars belonging to the given plugin (by index id).
void DLLINTERNAL MRegCvarList::disable(int plugin_id) {
	int i;
	MRegCvar *icvar;
	for(i=0; likely(i < size); i++) {
		icvar=&vlist[i];
		if(unlikely(icvar->plugid == plugin_id)) {
			icvar->status = RG_INVALID;
			icvar->plugid = 0;
			// Decided not to do this, in order to keep pre-existing values
			// after a plugin reload.
			// CVAR_SET_STRING(icvar->data->name, "[metamod: cvar invalid; plugin unloaded]");
		}
	}
}

// List all the registered cvars.
void DLLINTERNAL MRegCvarList::show(void) {
	int i, n=0, a=0;
	MRegCvar *icvar;
	MPlugin *iplug;
	char bplug[13+1], bname[20+1], bval[15+1];	// +1 for term null

	META_CONS("Registered plugin cvars:");
	META_CONS("  %*s  %-*s  %-*s  %*s  %s", 
			WIDTH_MAX_REG, "",
			sizeof(bplug)-1, "plugin",
			sizeof(bname)-1, "cvar",
			sizeof(bval)-1, "float value",
			"string value");
	
	for(i=0; likely(i < endlist); i++) {
		icvar = &vlist[i];
		if(likely(icvar->status==RG_VALID)) {
			iplug=Plugins->find(icvar->plugid);
			if(likely(iplug))
				STRNCPY(bplug, iplug->desc, sizeof(bplug));
			else
				STRNCPY(bplug, "(unknown)", sizeof(bplug));
		}
		else
			STRNCPY(bplug, "(unloaded)", sizeof(bplug));
		STRNCPY(bname, icvar->data->name, sizeof(bname));
		safe_snprintf(bval, sizeof(bval), "%f", icvar->data->value);
		META_CONS(" [%*d] %-*s  %-*s  %*s  %s", 
				WIDTH_MAX_REG, icvar->index, 
				sizeof(bplug)-1, bplug,
				sizeof(bname)-1, bname,
				sizeof(bval)-1, bval,
				icvar->data->string);
		if(likely(icvar->status==RG_VALID))
			a++;
		n++;
	}
	META_CONS("%d cvars, %d available (%d allocated)", n, a, size);
}

// List the registered cvars for the given plugin id.
void DLLINTERNAL MRegCvarList::show(int plugin_id) {
	int i, n=0;
	MRegCvar *icvar;
	char bname[30+1], bval[15+1];	// +1 for term null
	
	/*
	// If OS doesn't support DLFNAME, then we can't know what the plugin's
	// registered cvars are.
	DLFNAME(NULL);
	if(unlikely(meta_errno==ME_OSNOTSUP)) {
		META_CONS("Registered cvars: unknown (can't get info under this OS)");
		return;
	}
	*/
	
	META_CONS("%-*s     %*s  %s", 
			sizeof(bname)-1, "Registered cvars:",
			sizeof(bval)-1, "float value",
			"string value");
	for(i=0; likely(i < endlist); i++) {
		icvar = &vlist[i];
		if(likely(icvar->plugid != plugin_id))
			continue;
		STRNCPY(bname, icvar->data->name, sizeof(bname));
		safe_snprintf(bval, sizeof(bval), "%f", icvar->data->value);
		META_CONS("   %-*s  %*s  %s", 
				sizeof(bname)-1, bname,
				sizeof(bval)-1, bval,
				icvar->data->string);
		n++;
	}
	META_CONS("%d cvars", n); 
}


///// class MRegMsgList:

// Constructor
MRegMsgList::MRegMsgList(void)
	: endlist(0)
{
	int i;
	// initialize array
	memset(mlist, 0, sizeof(mlist));
	for(i=0; likely(i < size); i++) {
		mlist[i].index=i+1;		// 1-based
	}
	endlist=0;
	sort_flist();
}

// Add the given user msg the list and return the instance.
// meta_errno values:
//  - ME_MAXREACHED		reached max number of msgs allowed
MRegMsg * DLLINTERNAL MRegMsgList::add(const char *addname, int addmsgid, int addsize) {
	MRegMsg *imsg;

	if(unlikely(endlist==size)) {
		// all slots used
		META_WARNING("Couldn't add registered msg '%s' to list; reached max msgs (%d)",
				addname, size);
		RETURN_ERRNO(NULL, ME_MAXREACHED);
	}

	imsg = &mlist[endlist];
	endlist++;
	
	// Copy msg data into empty slot.
	imsg->name=addname; //input must not be erased (const string in gamedll or strdup'd string from plugin)
	imsg->msgid=addmsgid;
	imsg->size=addsize;
	imsg->plugid=0;
	imsg->search_count=0;

	return(imsg);
}

// 
void DLLINTERNAL MRegMsgList::reset_counts(void) {
	int i;
	for(i=0; likely(i < size); i++)
		mlist[i].search_count=0;
	sort_flist();
}

// sort function for qsort
static int sort_by_count(const void* a, const void* b) {
	const MRegMsg ** A = (const MRegMsg **)a;
	const MRegMsg ** B = (const MRegMsg **)b;
	
	return((*B)->search_count-(*A)->search_count);
}

//
void DLLINTERNAL MRegMsgList::sort_flist(void) {
	int i;
	for(i=0; likely(i < size); i++) {
		if(unlikely(mlist[i].search_count<0))
			mlist[i].search_count=10000;
		flist[i]=&mlist[i];
	}
	
	qsort(&flist, endlist, sizeof(flist[0]), sort_by_count);
}

// Try to find a registered msg with the given name.
// meta_errno values:
//  - ME_NOTFOUND	couldn't find a matching cvar
MRegMsg * DLLINTERNAL MRegMsgList::fast_find(const char *findname) {
	int i;
	
	for(i=0; likely(i < endlist); i++) {
		if(unlikely(!mm_strcmp(flist[i]->name, findname))) {
			MRegMsg * found = flist[i];
			
			found->search_count++;
			
			if(likely(i>=1) && unlikely((found->search_count - flist[i-1]->search_count)>=100))
				sort_flist();
			
			return(found);
		}
	}
	RETURN_ERRNO(NULL, ME_NOTFOUND);
}

// Try to find a registered msg with the given name.
// meta_errno values:
//  - ME_NOTFOUND	couldn't find a matching cvar
MRegMsg * DLLINTERNAL MRegMsgList::find(const char *findname) {
	int i;
	for(i=0; likely(i < endlist); i++) {
		if(unlikely(!mm_strcmp(mlist[i].name, findname)))
			return(&mlist[i]);
	}
	RETURN_ERRNO(NULL, ME_NOTFOUND);
}

// Try to find a registered msg with the given msgid.
// meta_errno values:
//  - ME_NOTFOUND	couldn't find a matching cvar
MRegMsg * DLLINTERNAL MRegMsgList::find(int findmsgid) {
	int i;
	for(i=0; likely(i < endlist); i++) {
		if(unlikely(mlist[i].msgid == findmsgid))
			return(&mlist[i]);
	}
	RETURN_ERRNO(NULL, ME_NOTFOUND);
}

// List the registered usermsgs for the gamedll.
void DLLINTERNAL MRegMsgList::show(void) {
	int i, n=0;
	MRegMsg *imsg;
	char bname[25+1];	// +1 for term null

	META_CONS("%-*s    %5s  %5s", 
			sizeof(bname)-1, "Game registered user msgs:", "msgid", "size");
	for(i=0; likely(i < endlist); i++) {
		imsg = &mlist[i];
		if(unlikely(imsg->plugid!=0))
			continue;
		STRNCPY(bname, imsg->name, sizeof(bname));
		META_CONS("   %-*s   %3d    %3d", 
				sizeof(bname)-1, bname,
				imsg->msgid,
				imsg->size);
		n++;
	}
	META_CONS("%d game user msgs", n); 
}


///// class MQueryClientCvarList:

// Constructor
MQueryClientCvarList::MQueryClientCvarList(void) {
	int i;
	// initialize array
	memset(mlist, 0, sizeof(mlist));
	for(i=0; likely(i < size); i++) {
		mlist[i].index=i+1;		// 1-based
	}
}

// 
void MQueryClientCvarList::reset(void) {
	int i;
	for(i=0; likely(i < size); i++) {
		if(unlikely(mlist[i].name)) {
			free(mlist[i].name);
		}
	}
	(*this) = MQueryClientCvarList();
}

// Add the given client cvar query the list and return the instance.
// meta_errno values:
//  - ME_ARGUMENT		invalid player or name==NULL
MQueryClientCvar * DLLINTERNAL MQueryClientCvarList::add(const edict_t *player, const char *name) {
	int index;
	
	if(unlikely(fast_FNullEnt(player))) {
		// invalid input
		META_WARNING("Couldn't add client cvar query '%s' to list; given player edict is invalid.", name);
		RETURN_ERRNO(NULL, ME_ARGUMENT);
	}
	if(unlikely(!name)) {
		// invalid input
		META_WARNING("Couldn't add client cvar query '%s' to list; name is invalid.", name);
		RETURN_ERRNO(NULL, ME_ARGUMENT);
	}
	
	// index is 1-based.
	index = ENTINDEX((edict_t*)player);
	if(unlikely(index <= 0) || unlikely(index > gpGlobals->maxClients)) {
		// invalid input
		META_WARNING("Couldn't add client cvar query '%s' to list; given edict is not client.", name);
		RETURN_ERRNO(NULL, ME_ARGUMENT);
	}
	
	if(unlikely(mlist[index-1].name)) {
		META_WARNING("Client cvar query slot not empty; old query name '%s', new query name '%s'; removing old.", mlist[index-1].name, name);
		free(mlist[index-1].name);
		mlist[index-1].name = 0;
	}
	
	// Copy msg data into empty slot.
	mlist[index-1].player=player;
	mlist[index-1].name=strdup(name); //input must not be erased (const string in gamedll or strdup'd string from plugin)

	return(&mlist[index-1]);
}

// Remove the given client cvar query the list.
// meta_errno values:
//  - ME_NOTFOUND		not found
mBOOL DLLINTERNAL MQueryClientCvarList::remove(const edict_t *player) {
	MQueryClientCvar *imsg;
	
	imsg = find(player);
	if(unlikely(!imsg)) {
		RETURN_ERRNO(mFALSE, ME_NOTFOUND);
	}
	
	imsg->player=0;
	if(likely(imsg->name))
		free(imsg->name);
	imsg->name=0;
	
	return(mTRUE);
}

// Try to find a registered msg with the given name.
// meta_errno values:
//  - ME_NOTFOUND	couldn't find a matching cvar
MQueryClientCvar * DLLINTERNAL MQueryClientCvarList::find(const edict_t *player) {
	int index;
	
	// index is 1-based.
	index = ENTINDEX((edict_t*)player);
	if(likely(index > 0) && likely(index <= gpGlobals->maxClients) && likely(mlist[index-1].player==player))
		return(&mlist[index-1]);
	
	RETURN_ERRNO(NULL, ME_NOTFOUND);
}

