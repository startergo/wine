/*
 * WineCfg configuration management
 *
 * Copyright 2002 Jaco Greeff
 * Copyright 2003 Dimitrie O. Paun
 * Copyright 2003-2004 Mike Hearn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * TODO:
 *  - Use unicode
 *  - Icons in listviews/icons
 *  - Better add app dialog, scan c: for EXE files and add to list in background
 *  - Use [GNOME] HIG style groupboxes rather than win32 style (looks nicer, imho)
 *
 */

#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <windows.h>
#include <winreg.h>
#include <wine/debug.h>
#include <wine/list.h>

WINE_DEFAULT_DEBUG_CHANNEL(winecfg);

#include "winecfg.h"

HKEY config_key = NULL;



/* this is called from the WM_SHOWWINDOW handlers of each tab page.
 *
 * it's a nasty hack, necessary because the property sheet insists on resetting the window title
 * to the title of the tab, which is utterly useless. dropping the property sheet is on the todo list.
 */
void set_window_title(HWND dialog)
{
    char *newtitle;

    /* update the window title  */
    if (current_app)
    {
        char *template = "Wine Configuration for %s";
        newtitle = HeapAlloc(GetProcessHeap(), 0, strlen(template) + strlen(current_app) + 1);
        sprintf(newtitle, template, current_app);
    }
    else
    {
        newtitle = strdupA("Wine Configuration");
    }

    WINE_TRACE("setting title to %s\n", newtitle);
    SendMessage(GetParent(dialog), PSM_SETTITLE, 0, (LPARAM) newtitle);
    HeapFree(GetProcessHeap(), 0, newtitle);
}


/**
 * getkey: Retrieves a configuration value from the registry
 *
 * char *subkey : the name of the config section
 * char *name : the name of the config value
 * char *default : if the key isn't found, return this value instead
 *
 * Returns a buffer holding the value if successful, NULL if
 * not. Caller is responsible for releasing the result.
 *
 */
static char *getkey (char *subkey, char *name, char *def)
{
    LPBYTE buffer = NULL;
    DWORD len;
    HKEY hSubKey = NULL;
    DWORD res;

    WINE_TRACE("subkey=%s, name=%s, def=%s\n", subkey, name, def);

    res = RegOpenKeyEx(config_key, subkey, 0, KEY_READ, &hSubKey);
    if (res != ERROR_SUCCESS)
    {
        if (res == ERROR_FILE_NOT_FOUND)
        {
            WINE_TRACE("Section key not present - using default\n");
            return def ? strdupA(def) : NULL;
        }
        else
        {
            WINE_ERR("RegOpenKey failed on wine config key (res=%ld)\n", res);
        }
        goto end;
    }

    res = RegQueryValueExA(hSubKey, name, NULL, NULL, NULL, &len);
    if (res == ERROR_FILE_NOT_FOUND)
    {
        WINE_TRACE("Value not present - using default\n");
        buffer = def ? strdupA(def) : NULL;
	goto end;
    } else if (res != ERROR_SUCCESS)
    {
        WINE_ERR("Couldn't query value's length (res=%ld)\n", res);
        goto end;
    }

    buffer = HeapAlloc(GetProcessHeap(), 0, len + 1);

    RegQueryValueEx(hSubKey, name, NULL, NULL, buffer, &len);

    WINE_TRACE("buffer=%s\n", buffer);
end:
    if (hSubKey) RegCloseKey(hSubKey);

    return buffer;
}

/**
 * setkey: convenience wrapper to set a key/value pair
 *
 * const char *subKey : the name of the config section
 * const char *valueName : the name of the config value
 * const char *value : the value to set the configuration key to
 *
 * Returns 0 on success, non-zero otherwise
 *
 * If valueName or value is NULL, an empty section will be created
 */
int setkey(const char *subkey, const char *name, const char *value) {
    DWORD res = 1;
    HKEY key = NULL;

    WINE_TRACE("subkey=%s: name=%s, value=%s\n", subkey, name, value);

    assert( subkey != NULL );

    res = RegCreateKey(config_key, subkey, &key);
    if (res != ERROR_SUCCESS) goto end;
    if (name == NULL || value == NULL) goto end;

    res = RegSetValueEx(key, name, 0, REG_SZ, value, strlen(value) + 1);
    if (res != ERROR_SUCCESS) goto end;

    res = 0;
end:
    if (key) RegCloseKey(key);
    if (res != 0) WINE_ERR("Unable to set configuration key %s in section %s to %s, res=%ld\n", name, subkey, value, res);
    return res;
}

/* removes the requested value from the registry, however, does not
 * remove the section if empty. Returns S_OK (0) on success.
 */
static HRESULT remove_value(const char *subkey, const char *name)
{
    HRESULT hr;
    HKEY key;

    WINE_TRACE("subkey=%s, name=%s\n", subkey, name);

    hr = RegOpenKeyEx(config_key, subkey, 0, KEY_READ, &key);
    if (hr != S_OK) return hr;

    hr = RegDeleteValue(key, name);
    if (hr != ERROR_SUCCESS) return hr;

    return S_OK;
}

/* removes the requested subkey from the registry, assuming it exists */
static HRESULT remove_path(char *section) {
    WINE_TRACE("section=%s\n", section);

    return RegDeleteKey(config_key, section);
}


/* ========================================================================= */

/* This code exists for the following reasons:
 *
 * - It makes working with the registry easier
 * - By storing a mini cache of the registry, we can more easily implement
 *   cancel/revert and apply. The 'settings list' is an overlay on top of
 *   the actual registry data that we can write out at will.
 *
 * Rather than model a tree in memory, we simply store each absolute (rooted
 * at the config key) path.
 *
 */

struct setting
{
    struct list entry;
    char *path;   /* path in the registry rooted at the config key  */
    char *name;   /* name of the registry value  */
    char *value;  /* contents of the registry value. if null, this means a deletion  */
};

struct list *settings;

static void free_setting(struct setting *setting)
{
    assert( setting != NULL );

    WINE_TRACE("destroying %p\n", setting);

    assert( setting->path && setting->name );

    HeapFree(GetProcessHeap(), 0, setting->path);
    HeapFree(GetProcessHeap(), 0, setting->name);
    if (setting->value) HeapFree(GetProcessHeap(), 0, setting->value);

    list_remove(&setting->entry);

    HeapFree(GetProcessHeap(), 0, setting);
}

/**
 * Returns the contents of the value at path. If not in the settings
 * list, it will be fetched from the registry - failing that, the
 * default will be used.
 *
 * If already in the list, the contents as given there will be
 * returned. You are expected to HeapFree the result.
 */
char *get(char *path, char *name, char *def)
{
    struct list *cursor;
    struct setting *s;
    char *val;

    WINE_TRACE("path=%s, name=%s, def=%s\n", path, name, def);

    /* check if it's in the list */
    LIST_FOR_EACH( cursor, settings )
    {
        s = LIST_ENTRY(cursor, struct setting, entry);

        if (strcasecmp(path, s->path) != 0) continue;
        if (strcasecmp(name, s->name) != 0) continue;

        WINE_TRACE("found %s:%s in settings list, returning %s\n", path, name, s->value);
        return strdupA(s->value);
    }

    /* no, so get from the registry */
    val = getkey(path, name, def);

    WINE_TRACE("returning %s\n", val);

    return val;
}

/**
 * Used to set a registry key.
 *
 * path is rooted at the config key, ie use "Version" or
 * "AppDefaults\\fooapp.exe\\Version". You can use keypath()
 * to get such a string.
 *
 * name is the value name, it must not be null (you cannot create
 * empty groups, sorry ...)
 *
 * value is what to set the value to, or NULL to delete it.
 *
 * These values will be copied when necessary.
 */
void set(char *path, char *name, char *value)
{
    struct list *cursor;
    struct setting *s;

    assert( path != NULL );
    assert( name != NULL );

    WINE_TRACE("path=%s, name=%s, value=%s\n", path, name, value);

    /* firstly, see if we already set this setting  */
    LIST_FOR_EACH( cursor, settings )
    {
        struct setting *s = LIST_ENTRY(cursor, struct setting, entry);

        if (strcasecmp(s->path, path) != 0) continue;
        if (strcasecmp(s->name, name) != 0) continue;

        /* yes, we have already set it, so just replace the content and return  */
        if (s->value) HeapFree(GetProcessHeap(), 0, s->value);
        s->value = value ? strdupA(value) : NULL;

        return;
    }

    /* otherwise add a new setting for it  */
    s = HeapAlloc(GetProcessHeap(), 0, sizeof(struct setting));
    s->path = strdupA(path);
    s->name = strdupA(name);
    s->value = value ? strdupA(value) : NULL;

    list_add_tail(settings, &s->entry);
}

/**
 * enumerates the value names at the given path, taking into account
 * the changes in the settings list.
 *
 * you are expected to HeapFree each element of the array, which is null
 * terminated, as well as the array itself.
 */
char **enumerate_values(char *path)
{
    HKEY key;
    DWORD res, i = 0;
    char **values = NULL;
    int valueslen = 0;
    struct list *cursor;

    res = RegOpenKeyEx(config_key, path, 0, KEY_READ, &key);
    if (res == ERROR_SUCCESS)
    {
        while (TRUE)
        {
            char name[1024];
            DWORD namesize = sizeof(name);
            BOOL removed = FALSE;

            /* find out the needed size, allocate a buffer, read the value  */
            if ((res = RegEnumValue(key, i, name, &namesize, NULL, NULL, NULL, NULL)) != ERROR_SUCCESS)
                break;

            WINE_TRACE("name=%s\n", name);

            /* check if this value name has been removed in the settings list  */
            LIST_FOR_EACH( cursor, settings )
            {
                struct setting *s = LIST_ENTRY(cursor, struct setting, entry);
                if (strcasecmp(s->path, path) != 0) continue;
                if (strcasecmp(s->name, name) != 0) continue;

                if (!s->value)
                {
                    WINE_TRACE("this key has been removed, so skipping\n");
                    removed = TRUE;
                    break;
                }
            }

            if (removed)            /* this value was deleted by the user, so don't include it */
            {
                HeapFree(GetProcessHeap(), 0, name);
                i++;
                continue;
            }

            /* grow the array if necessary, add buffer to it, iterate  */
            if (values) values = HeapReAlloc(GetProcessHeap(), 0, values, sizeof(char*) * (valueslen + 1));
            else values = HeapAlloc(GetProcessHeap(), 0, sizeof(char*));

            values[valueslen++] = strdupA(name);
            WINE_TRACE("valueslen is now %d\n", valueslen);
            i++;
        }
    }
    else
    {
        WINE_WARN("failed opening registry key %s, res=0x%lx\n", path, res);
    }

    WINE_TRACE("adding settings in list but not registry\n");

    /* now we have to add the values that aren't in the registry but are in the settings list */
    LIST_FOR_EACH( cursor, settings )
    {
        struct setting *setting = LIST_ENTRY(cursor, struct setting, entry);
        BOOL found = FALSE;

        if (strcasecmp(setting->path, path) != 0) continue;

        if (!setting->value) continue;

        for (i = 0; i < valueslen; i++)
        {
            if (strcasecmp(setting->name, values[i]) == 0)
            {
                found = TRUE;
                break;
            }
        }

        if (found) continue;

        WINE_TRACE("%s in list but not registry\n", setting->name);

        /* otherwise it's been set by the user but isn't in the registry */
        if (values) values = HeapReAlloc(GetProcessHeap(), 0, values, sizeof(char*) * (valueslen + 1));
        else values = HeapAlloc(GetProcessHeap(), 0, sizeof(char*));

        values[valueslen++] = strdupA(setting->name);
    }

    WINE_TRACE("adding null terminator\n");
    if (values)
    {
        values = HeapReAlloc(GetProcessHeap(), 0, values, sizeof(char*) * (valueslen + 1));
        values[valueslen] = NULL;
    }

    RegCloseKey(key);

    return values;
}

/**
 * returns true if the given key/value pair exists in the registry or
 * has been written to.
 */
BOOL exists(char *path, char *name)
{
    char *val = get(path, name, NULL);

    if (val)
    {
        HeapFree(GetProcessHeap(), 0, val);
        return TRUE;
    }

    return FALSE;
}

static void process_setting(struct setting *s)
{
    if (s->value)
    {
	WINE_TRACE("Setting %s:%s to '%s'\n", s->path, s->name, s->value);
        setkey(s->path, s->name, s->value);
    }
    else
    {
        /* NULL name means remove that path/section entirely */
	if (s->path && s->name) remove_value(s->path, s->name);
        else if (s->path && !s->name) remove_path(s->path);
    }
}

void apply(void)
{
    if (list_empty(settings)) return; /* we will be called for each page when the user clicks OK */

    WINE_TRACE("()\n");

    while (!list_empty(settings))
    {
        struct setting *s = (struct setting *) list_head(settings);
        process_setting(s);
        free_setting(s);
    }
}

/* ================================== utility functions ============================ */

char *current_app = NULL; /* the app we are currently editing, or NULL if editing global */

/* returns a registry key path suitable for passing to addTransaction  */
char *keypath(char *section)
{
    static char *result = NULL;

    if (result) HeapFree(GetProcessHeap(), 0, result);

    if (current_app)
    {
        result = HeapAlloc(GetProcessHeap(), 0, strlen("AppDefaults\\") + strlen(current_app) + 2 /* \\ */ + strlen(section) + 1 /* terminator */);
        sprintf(result, "AppDefaults\\%s\\%s", current_app, section);
    }
    else
    {
        result = strdupA(section);
    }

    return result;
}

void PRINTERROR(void)
{
        LPSTR msg;

        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
                       0, GetLastError(), MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),
                       (LPSTR)&msg, 0, NULL);

        /* eliminate trailing newline, is this a Wine bug? */
        *(strrchr(msg, '\n')) = '\0';
        
        WINE_TRACE("error: '%s'\n", msg);
}

int initialize(void) {
    DWORD res = RegCreateKey(HKEY_LOCAL_MACHINE, WINE_KEY_ROOT, &config_key);

    if (res != ERROR_SUCCESS) {
	WINE_ERR("RegOpenKey failed on wine config key (%ld)\n", res);
	return 1;
    }

    /* we could probably just have the list as static data  */
    settings = HeapAlloc(GetProcessHeap(), 0, sizeof(struct list));
    list_init(settings);

    return 0;
}
