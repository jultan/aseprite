/* ASE - Allegro Sprite Editor
 * Copyright (C) 2001-2008  David A. Capello
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Some of the original code to handle PIDLs come from the
   MiniExplorer example of the Vaca library:
   http://vaca.sourceforge.net/
 */

#include "config.h"

#include <cassert>
#include <cstdio>
#include <vector>
#include <map>
#include <algorithm>
#include <utility>

#include <allegro.h>

// in Windows we can use PIDLS
#if defined ALLEGRO_WINDOWS
  // uncomment this if you don't want to use PIDLs in windows
  #define USE_PIDLS
#endif

#if defined ALLEGRO_UNIX || defined ALLEGRO_DJGPP || defined ALLEGRO_MINGW32
  #include <sys/stat.h>
#endif
#if defined ALLEGRO_UNIX || defined ALLEGRO_MINGW32
  #include <sys/unistd.h>
#endif

#if defined USE_PIDLS
  #include <winalleg.h>
  #include <shlobj.h>
  #include <shlwapi.h>
#endif

#include "jinete/jfilesel.h"
#include "jinete/jstring.h"

#include "core/file_system.h"

//////////////////////////////////////////////////////////////////////

#ifdef USE_PIDLS
  // ..using Win32 Shell (PIDLs)

  #define IS_FOLDER(fi)					\
    (((fi)->attrib & SFGAO_FOLDER) == SFGAO_FOLDER)

  #define MYPC_CSLID  "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"

#else
  // ..using Allegro (for_each_file)

  #define IS_FOLDER(fi)					\
    (((fi)->attrib & FA_DIREC) == FA_DIREC)

  #if (DEVICE_SEPARATOR != 0) && (DEVICE_SEPARATOR != '\0')
    #define HAVE_DRIVES
  #else
    #define CASE_SENSITIVE
  #endif

  #ifndef FA_ALL
    #define FA_ALL     FA_RDONLY | FA_DIREC | FA_ARCH | FA_HIDDEN | FA_SYSTEM
  #endif
  #define FA_TO_SHOW   FA_RDONLY | FA_DIREC | FA_ARCH | FA_SYSTEM

#endif

//////////////////////////////////////////////////////////////////////

#ifndef MAX_PATH
#  define MAX_PATH 4096
#endif

#define NOTINITIALIZED	"{__not_initialized_path__}"

// a position in the file-system
class FileItem
{
public:
  jstring keyname;
  jstring filename;
  jstring displayname;
  FileItem* parent;
  FileItemList children;
  unsigned int version;
  bool removed;
#ifdef USE_PIDLS
  LPITEMIDLIST pidl;		// relative to parent
  LPITEMIDLIST fullpidl;	// relative to the Desktop folder
				// (like a full path-name, because the
				// desktop is the root on Windows)
  SFGAOF attrib;
#else
  int attrib;
#endif

  FileItem(FileItem* parent);
  ~FileItem();

  void insert_child_sorted(FileItem* child);
  int compare(const FileItem& that) const;

  bool operator<(const FileItem& that) const { return compare(that) < 0; }
  bool operator>(const FileItem& that) const { return compare(that) > 0; }
  bool operator==(const FileItem& that) const { return compare(that) == 0; }
  bool operator!=(const FileItem& that) const { return compare(that) != 0; }
};

typedef std::map<jstring, FileItem*> FileItemMap;
typedef std::map<jstring, BITMAP*> ThumbnailMap;

// the root of the file-system
static FileItem* rootitem = NULL;
static FileItemMap fileitems_map;
static ThumbnailMap thumbnail_map;
static unsigned int current_file_system_version = 0;

#ifdef USE_PIDLS
  static IMalloc* shl_imalloc = NULL;
  static IShellFolder* shl_idesktop = NULL;
#endif

/* a more easy PIDLs interface (without using the SH* & IL* routines of W2K) */
#ifdef USE_PIDLS
  static void update_by_pidl(FileItem* fileitem);
  static LPITEMIDLIST concat_pidl(LPITEMIDLIST pidlHead, LPITEMIDLIST pidlTail);
  static UINT get_pidl_size(LPITEMIDLIST pidl);
  static LPITEMIDLIST get_next_pidl(LPITEMIDLIST pidl);
  static LPITEMIDLIST get_last_pidl(LPITEMIDLIST pidl);
  static LPITEMIDLIST clone_pidl(LPITEMIDLIST pidl);
  static LPITEMIDLIST remove_last_pidl(LPITEMIDLIST pidl);
  static void free_pidl(LPITEMIDLIST pidl);
  static jstring get_key_for_pidl(LPITEMIDLIST pidl);

  static FileItem* get_fileitem_by_fullpidl(LPITEMIDLIST pidl, bool create_if_not);
  static void put_fileitem(FileItem* fileitem);
#else
  static FileItem* get_fileitem_by_path(const jstring& path, bool create_if_not);
  static void for_each_child_callback(const char *filename, int attrib, int param);
  static jstring remove_backslash_if_needed(const jstring& filename);
  static jstring get_key_for_filename(const jstring& filename);
  static void put_fileitem(FileItem* fileitem);
#endif

/**
 * Initializes the file-system module to navigate the file-system.
 */
bool file_system_init()
{
#ifdef USE_PIDLS
  /* get the IMalloc interface */
  SHGetMalloc(&shl_imalloc);

  /* get desktop IShellFolder interface */
  SHGetDesktopFolder(&shl_idesktop);
#endif

  // first version of the file system
  ++current_file_system_version;

  // get the root element of the file system (this will create
  // the 'rootitem' FileItem)
  get_root_fileitem();

  return TRUE;
}
 
/**
 * Shutdowns the file-system module.
 */
void file_system_exit()
{
  for (FileItemMap::iterator
	 it=fileitems_map.begin(); it!=fileitems_map.end(); ++it) {
    delete it->second;
  }
  fileitems_map.clear();

  for (ThumbnailMap::iterator
	 it=thumbnail_map.begin(); it!=thumbnail_map.end(); ++it) {
    destroy_bitmap(it->second);
  }
  thumbnail_map.clear();

#ifdef USE_PIDLS
  // relase desktop IShellFolder interface
  shl_idesktop->Release();

  // release IMalloc interface
  shl_imalloc->Release();
  shl_imalloc = NULL;
#endif
}

/**
 * Marks all FileItems as deprecated to be refresh the next time they
 * are queried through @ref fileitem_get_children.
 *
 * @see fileitem_get_children
 */
void file_system_refresh()
{
  ++current_file_system_version;
}

FileItem* get_root_fileitem()
{
  FileItem* fileitem;

  if (rootitem)
    return rootitem;

  fileitem = new FileItem(NULL);
  rootitem = fileitem;

#ifdef USE_PIDLS
  {
    // get the desktop PIDL
    LPITEMIDLIST pidl = NULL;

    if (SHGetSpecialFolderLocation(NULL, CSIDL_DESKTOP, &pidl) != S_OK) {
      // TODO do something better
      assert(false);
      exit(1);
    }
    fileitem->pidl = pidl;
    fileitem->fullpidl = pidl;
    fileitem->attrib = SFGAO_FOLDER;
    shl_idesktop->GetAttributesOf(1, (LPCITEMIDLIST *)&pidl, &fileitem->attrib);

    update_by_pidl(fileitem);
  }
#else
  {
    const char* root;

#if defined HAVE_DRIVES
    root = "C:\\";
#else
    root = "/";
#endif

    fileitem->filename = root;
    fileitem->displayname = root;
    fileitem->attrib = FA_DIREC;
  }
#endif

  // insert the file-item in the hash-table
  put_fileitem(fileitem);
  return fileitem;
}

/**
 * Returns the FileItem through the specified @a path.
 * 
 * @warning You have to call path.fix_separators() before.
 */
FileItem* get_fileitem_from_path(const jstring& path)
{
  FileItem* fileitem = NULL;

  PRINTF("get_fileitem_from_path(%s)\n", path.c_str());

#ifdef USE_PIDLS
  {
    ULONG cbEaten;
    WCHAR wStr[MAX_PATH];
    LPITEMIDLIST fullpidl = NULL;
    SFGAOF attrib = SFGAO_FOLDER;

    if (path.empty()) {
      fileitem = get_root_fileitem();
      PRINTF("  > %p (root)\n", fileitem);
      return fileitem;
    }

    MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED,
			path.c_str(), path.size()+1, wStr, MAX_PATH);
    if (shl_idesktop->ParseDisplayName(NULL, NULL,
				       wStr, &cbEaten,
				       &fullpidl,
				       &attrib) != S_OK) {
      PRINTF("  > (null)\n");
      return NULL;
    }

    fileitem = get_fileitem_by_fullpidl(fullpidl, TRUE);
    free_pidl(fullpidl);
  }
#else
  {
    jstring buf = remove_backslash_if_needed(path);
    fileitem = get_fileitem_by_path(buf, TRUE);
  }
#endif

  PRINTF("  > fileitem = %p\n", fileitem);

  return fileitem;
}

bool fileitem_is_folder(FileItem* fileitem)
{
  assert(fileitem);

  return IS_FOLDER(fileitem);
}

bool fileitem_is_browsable(FileItem* fileitem)
{
  assert(fileitem);
  assert(fileitem->filename != NOTINITIALIZED);

#ifdef USE_PIDLS
  return IS_FOLDER(fileitem)
    && (fileitem->filename.extension() != "zip")
    && ((!fileitem->filename.empty() && fileitem->filename.front() != ':') ||
	(fileitem->filename == MYPC_CSLID));
#else
  return IS_FOLDER(fileitem);
#endif
}

jstring fileitem_get_keyname(FileItem* fileitem)
{
  assert(fileitem);
  assert(fileitem->keyname != NOTINITIALIZED);

  return fileitem->keyname;
}

jstring fileitem_get_filename(FileItem* fileitem)
{
  assert(fileitem);
  assert(fileitem->filename != NOTINITIALIZED);

  return fileitem->filename;
}

jstring fileitem_get_displayname(FileItem* fileitem)
{
  assert(fileitem);
  assert(fileitem->displayname != NOTINITIALIZED);

  return fileitem->displayname;
}

FileItem* fileitem_get_parent(FileItem* fileitem)
{
  assert(fileitem);

  if (fileitem == rootitem)
    return NULL;
  else {
    assert(fileitem->parent);
    return fileitem->parent;
  }
}

const FileItemList& fileitem_get_children(FileItem* fileitem)
{
  assert(fileitem);

  /* is the file-item a folder? */
  if (IS_FOLDER(fileitem) &&
      // if the children list is empty, or the file-system version
      // change (it's like to say: the current fileitem->children list
      // is outdated)...
      (fileitem->children.empty() ||
       current_file_system_version > fileitem->version)) {
    FileItemList::iterator it;
    FileItem* child;

    // we have to mark current items as deprecated
    for (it=fileitem->children.begin();
	 it!=fileitem->children.end(); ++it) {
      child = *it;
      child->removed = true;
    }

    /* printf("Loading files for %p (%s)\n", fileitem, fileitem->displayname); fflush(stdout); */
#ifdef USE_PIDLS
    {
      IShellFolder* pFolder = NULL;

      if (fileitem == rootitem)
	pFolder = shl_idesktop;
      else
	shl_idesktop->BindToObject(fileitem->fullpidl,
				   NULL,
				   IID_IShellFolder,
				   (LPVOID *)&pFolder);

      if (pFolder != NULL) {
	IEnumIDList *pEnum = NULL;
	ULONG c, fetched;

	/* get the interface to enumerate subitems */
	pFolder->EnumObjects(win_get_window(),
			     SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &pEnum);

	if (pEnum != NULL) {
	  LPITEMIDLIST itempidl[256];
	  SFGAOF attribs[256];

	  /* enumerate the items in the folder */
	  while (pEnum->Next(256, itempidl, &fetched) == S_OK && fetched > 0) {
	    /* request the SFGAO_FOLDER attribute to know what of the
	       item is a folder */
	    for (c=0; c<fetched; ++c)
	      attribs[c] = SFGAO_FOLDER;

	    if (pFolder->GetAttributesOf(fetched,
					 (LPCITEMIDLIST *)itempidl, attribs) != S_OK) {
	      for (c=0; c<fetched; ++c)
		attribs[c] = 0;
	    }

	    /* generate the FileItems */
	    for (c=0; c<fetched; ++c) {
	      LPITEMIDLIST fullpidl = concat_pidl(fileitem->fullpidl,
						  itempidl[c]);

	      child = get_fileitem_by_fullpidl(fullpidl, FALSE);
	      if (!child) {
		child = new FileItem(fileitem);

		child->pidl = itempidl[c];
		child->fullpidl = fullpidl;
		child->attrib = attribs[c];

		update_by_pidl(child);
		put_fileitem(child);
	      }
	      else {
		assert(child->parent == fileitem);
		free_pidl(fullpidl);
		free_pidl(itempidl[c]);
	      }

	      fileitem->insert_child_sorted(child);
	    }
	  }

	  pEnum->Release();
	}

	if (pFolder != shl_idesktop)
	  pFolder->Release();
      }
    }
#else
    {
      char buf[MAX_PATH], path[MAX_PATH], tmp[32];

      ustrcpy(path, fileitem->filename.c_str());
      put_backslash(path);

      replace_filename(buf,
		       path,
		       uconvert_ascii("*.*", tmp),
		       sizeof(buf));

      for_each_file(buf, FA_TO_SHOW,
		    for_each_child_callback,
		    (int)fileitem);	/* TODO warning with 64bits */
    }
#endif

    // check old file-items (maybe removed directories or file-items)
    for (it=fileitem->children.begin();
	 it!=fileitem->children.end(); ) {
      child = *it;
      if (child->removed) {
	it = fileitem->children.erase(it);
	delete child;
      }
      else
	++it;
    }

    // now this file-item is updated
    fileitem->version = current_file_system_version;
  }

  return fileitem->children;
}

bool fileitem_has_extension(FileItem* fileitem, const jstring& csv_extensions)
{
  assert(fileitem);
  assert(fileitem->filename != NOTINITIALIZED);

  return fileitem->filename.has_extension(csv_extensions);
}

BITMAP* fileitem_get_thumbnail(FileItem* fileitem)
{
  assert(fileitem);

  ThumbnailMap::iterator it = thumbnail_map.find(fileitem->filename);
  if (it != thumbnail_map.end())
    return it->second;
  else
    return NULL;
}

void fileitem_set_thumbnail(FileItem* fileitem, BITMAP* thumbnail)
{
  assert(fileitem);

  // destroy the current thumbnail of the file (if exists)
  ThumbnailMap::iterator it = thumbnail_map.find(fileitem->filename);
  if (it != thumbnail_map.end()) {
    destroy_bitmap(it->second);
    thumbnail_map.erase(it);
  }

  // insert the new one in the map
  thumbnail_map.insert(std::make_pair(fileitem->filename, thumbnail));
}

FileItem::FileItem(FileItem* parent)
{
  this->keyname = NOTINITIALIZED;
  this->filename = NOTINITIALIZED;
  this->displayname = NOTINITIALIZED;
  this->parent = parent;
  this->version = current_file_system_version;
  this->removed = false;
#ifdef USE_PIDLS
  this->pidl = NULL;
  this->fullpidl = NULL;
  this->attrib = 0;
#else
  this->attrib = 0;
#endif
}

FileItem::~FileItem()
{
#ifdef USE_PIDLS
  if (this->fullpidl && this->fullpidl != this->pidl) {
    free_pidl(this->fullpidl);
    this->fullpidl = NULL;
  }

  if (this->pidl) {
    free_pidl(this->pidl);
    this->pidl = NULL;
  }
#endif
}

void FileItem::insert_child_sorted(FileItem* child)
{
  // this file-item wasn't removed from the last lookup
  child->removed = false;

  // if the fileitem is already in the list we can go back
  if (std::find(children.begin(), children.end(), child) != children.end())
    return;

  bool inserted = false;

  for (FileItemList::iterator
	 it=children.begin(); it!=children.end(); ++it) {
    if (*(*it) > *child) {
      children.insert(it, child);
      inserted = true;
      break;
    }
  }

  if (!inserted)
    children.push_back(child);
}

/**
 * Compares two FileItems.
 *
 * Based on 'ustricmp' of Allegro. It makes sure that eg "foo.bar"
 * comes before "foo-1.bar", and also that "foo9.bar" comes before
 * "foo10.bar".
 */
int FileItem::compare(const FileItem& that) const
{
  if (IS_FOLDER(this)) {
    if (!IS_FOLDER(&that))
      return -1;
  }
  else if (IS_FOLDER(&that))
    return 1;

#ifndef USE_PIDLS
  {
    int c1, c2;
    int x1, x2;
    char *t1, *t2;
    const char* s1 = this->displayname.c_str(); // TODO fix this
    const char* s2 = that.displayname.c_str();

    for (;;) {
      c1 = utolower(ugetxc(&s1));
      c2 = utolower(ugetxc(&s2));

      if ((c1 >= '0') && (c1 <= '9') && (c2 >= '0') && (c2 <= '9')) {
	x1 = ustrtol(s1 - ucwidth(c1), &t1, 10);
	x2 = ustrtol(s2 - ucwidth(c2), &t2, 10);
	if (x1 != x2)
	  return x1 - x2;
	else if (t1 - s1 != t2 - s2)
	  return (t2 - s2) - (t1 - s1);
	s1 = t1;
	s2 = t2;
      }
      else if (c1 != c2) {
	if (!c1)
	  return -1;
	else if (!c2)
	  return 1;
	else if (c1 == '.')
	  return -1;
	else if (c2 == '.')
	  return 1;
	return c1 - c2;
      }

      if (!c1)
	return 0;
    }
  }
#endif

  return -1;
}

//////////////////////////////////////////////////////////////////////
// PIDLS: Only for Win32
//////////////////////////////////////////////////////////////////////

#ifdef USE_PIDLS

/* updates the names of the file-item through its PIDL */
static void update_by_pidl(FileItem* fileitem)
{
  STRRET strret;
  TCHAR pszName[MAX_PATH];
  IShellFolder *pFolder = NULL;

  if (fileitem == rootitem)
    pFolder = shl_idesktop;
  else {
    assert(fileitem->parent);
    shl_idesktop->BindToObject(fileitem->parent->fullpidl,
			       NULL,
			       IID_IShellFolder,
			       (LPVOID *)&pFolder);
  }

  /****************************************/
  /* get the file name */

  if (pFolder != NULL &&
      pFolder->GetDisplayNameOf(fileitem->pidl,
				SHGDN_NORMAL | SHGDN_FORPARSING,
				&strret) == S_OK) {
    StrRetToBuf(&strret, fileitem->pidl, pszName, MAX_PATH);
    fileitem->filename = pszName;
  }
  else if (shl_idesktop->GetDisplayNameOf(fileitem->fullpidl,
					  SHGDN_NORMAL | SHGDN_FORPARSING,
					  &strret) == S_OK) {
    StrRetToBuf(&strret, fileitem->fullpidl, pszName, MAX_PATH);
    fileitem->filename = pszName;
  }
  else
    fileitem->filename = "ERR";

  /****************************************/
  /* get the name to display */

  if (pFolder &&
      pFolder->GetDisplayNameOf(fileitem->pidl,
				SHGDN_INFOLDER,
				&strret) == S_OK) {
    StrRetToBuf(&strret, fileitem->pidl, pszName, MAX_PATH);
    fileitem->displayname = pszName;
  }
  else if (shl_idesktop->GetDisplayNameOf(fileitem->fullpidl,
					  SHGDN_INFOLDER,
					  &strret) == S_OK) {
    StrRetToBuf(&strret, fileitem->fullpidl, pszName, MAX_PATH);
    fileitem->displayname = pszName;
  }
  else {
    fileitem->displayname = "ERR";
  }

  if (pFolder != NULL && pFolder != shl_idesktop) {
    pFolder->Release();
  }
}

static LPITEMIDLIST concat_pidl(LPITEMIDLIST pidlHead, LPITEMIDLIST pidlTail)
{
  LPITEMIDLIST pidlNew;
  UINT cb1, cb2;

  assert(pidlHead);
  assert(pidlTail);

  cb1 = get_pidl_size(pidlHead) - sizeof(pidlHead->mkid.cb);
  cb2 = get_pidl_size(pidlTail);

  pidlNew = (LPITEMIDLIST)shl_imalloc->Alloc(cb1 + cb2);
  if (pidlNew) {
    CopyMemory(pidlNew, pidlHead, cb1);
    CopyMemory(((LPSTR)pidlNew) + cb1, pidlTail, cb2);
  }

  return pidlNew;
}

static UINT get_pidl_size(LPITEMIDLIST pidl)
{
  UINT cbTotal = 0;

  if (pidl) {
    cbTotal += sizeof(pidl->mkid.cb); /* null terminator */

    while (pidl) {
      cbTotal += pidl->mkid.cb;
      pidl = get_next_pidl(pidl);
    }
  }

  return cbTotal;
}

static LPITEMIDLIST get_next_pidl(LPITEMIDLIST pidl)
{
  if (pidl != NULL && pidl->mkid.cb > 0) {
    pidl = (LPITEMIDLIST)(((LPBYTE)(pidl)) + pidl->mkid.cb);
    if (pidl->mkid.cb > 0)
      return pidl;
  }

  return NULL;
} 

static LPITEMIDLIST get_last_pidl(LPITEMIDLIST pidl)
{
  LPITEMIDLIST pidlLast = pidl;
  LPITEMIDLIST pidlNew = NULL;

  while (pidl) {
    pidlLast = pidl;
    pidl = get_next_pidl(pidl);
  }

  if (pidlLast) {
    ULONG sz = get_pidl_size(pidlLast);
    pidlNew = (LPITEMIDLIST)shl_imalloc->Alloc(sz);
    CopyMemory(pidlNew, pidlLast, sz);
  }

  return pidlNew;
}

static LPITEMIDLIST clone_pidl(LPITEMIDLIST pidl)
{
  ULONG sz = get_pidl_size(pidl);
  LPITEMIDLIST pidlNew = (LPITEMIDLIST)shl_imalloc->Alloc(sz);

  CopyMemory(pidlNew, pidl, sz);

  return pidlNew;
}

static LPITEMIDLIST remove_last_pidl(LPITEMIDLIST pidl)
{
  LPITEMIDLIST pidlFirst = pidl;
  LPITEMIDLIST pidlLast = pidl;

  while (pidl) {
    pidlLast = pidl;
    pidl = get_next_pidl(pidl);
  }

  if (pidlLast)
    pidlLast->mkid.cb = 0;

  return pidlFirst;
}

static void free_pidl(LPITEMIDLIST pidl)
{
  shl_imalloc->Free(pidl);
}

static jstring get_key_for_pidl(LPITEMIDLIST pidl)
{
#if 0
  char *key = jmalloc(get_pidl_size(pidl)+1);
  UINT c, i = 0;

  while (pidl) {
    for (c=0; c<pidl->mkid.cb; ++c) {
      if (pidl->mkid.abID[c])
	key[i++] = pidl->mkid.abID[c];
      else
	key[i++] = 1;
    }
    pidl = get_next_pidl(pidl);
  }
  key[i] = 0;

  return key;
#else
  STRRET strret;
  TCHAR pszName[MAX_PATH];
  char key[4096];
  int len;

  ustrcpy(key, empty_string);

  /* go pidl by pidl from the fullpidl to the root (desktop) */
/*   printf("***\n"); fflush(stdout); */
  pidl = clone_pidl(pidl);
  while (pidl->mkid.cb > 0) {
    if (shl_idesktop->GetDisplayNameOf(pidl,
				       SHGDN_INFOLDER | SHGDN_FORPARSING,
				       &strret) == S_OK) {
      StrRetToBuf(&strret, pidl, pszName, MAX_PATH);

/*       printf("+ %s\n", pszName); fflush(stdout); */

      len = ustrlen(pszName);
      if (len > 0 && ustrncmp(key, pszName, len) != 0) {
	if (*key) {
	  if (pszName[len-1] != '\\') {
	    memmove(key+len+1, key, ustrlen(key)+1);
	    key[len] = '\\';
	  }
	  else
	    memmove(key+len, key, ustrlen(key)+1);
	}
	else
	  key[len] = 0;

	memcpy(key, pszName, len);
      }
    }
    remove_last_pidl(pidl);
  }
  free_pidl(pidl);

  // printf("=%s\n***\n", key); fflush(stdout);
  return key;
#endif
}

static FileItem* get_fileitem_by_fullpidl(LPITEMIDLIST fullpidl, bool create_if_not)
{
  FileItemMap::iterator it = fileitems_map.find(get_key_for_pidl(fullpidl));
  if (it != fileitems_map.end())
    return it->second;

  if (!create_if_not)
    return NULL;

  // new file-item
  FileItem* fileitem = new FileItem(NULL);
  fileitem->fullpidl = clone_pidl(fullpidl);

  fileitem->attrib = SFGAO_FOLDER;
  shl_idesktop->GetAttributesOf(1, (LPCITEMIDLIST *)&fileitem->fullpidl,
				&fileitem->attrib);

  {
    LPITEMIDLIST parent_fullpidl = clone_pidl(fileitem->fullpidl);
    remove_last_pidl(parent_fullpidl);

    fileitem->pidl = get_last_pidl(fileitem->fullpidl);
    fileitem->parent = get_fileitem_by_fullpidl(parent_fullpidl, TRUE);

    free_pidl(parent_fullpidl);
  }

  update_by_pidl(fileitem);
  put_fileitem(fileitem);

  return fileitem;
}

/**
 * Inserts the @a fileitem in the hash map of items.
 */
static void put_fileitem(FileItem* fileitem)
{
  assert(fileitem->filename != NOTINITIALIZED);
  assert(fileitem->keyname == NOTINITIALIZED);

  fileitem->keyname = get_key_for_pidl(fileitem->fullpidl);

  assert(fileitem->keyname != NOTINITIALIZED);

  // insert this file-item in the hash-table
  fileitems_map.insert(std::make_pair(fileitem->keyname, fileitem));
}

#else

//////////////////////////////////////////////////////////////////////
// Allegro for_each_file: Portable
//////////////////////////////////////////////////////////////////////

static FileItem* get_fileitem_by_path(const jstring& path, bool create_if_not)
{
#ifdef ALLEGRO_UNIX
  if (path.empty())
    return rootitem;
#endif

  FileItemMap::iterator it = fileitems_map.find(get_key_for_filename(path));
  if (it != fileitems_map.end())
    return it->second;

  if (!create_if_not)
    return NULL;

  // get the attributes of the file
  int attrib = 0;
  if (!file_exists(path.c_str(), FA_ALL, &attrib)) {
    if (!ji_dir_exists(path.c_str()))
      return NULL;
    attrib = FA_DIREC;
  }

  // new file-item
  FileItem* fileitem = new FileItem(NULL);

  fileitem->filename = path;
  fileitem->displayname = path.filename();
  fileitem->attrib = attrib;

  // get the parent
  {
    jstring parent_path = remove_backslash_if_needed(path.filepath() / "");
    fileitem->parent = get_fileitem_by_path(parent_path, TRUE);
  }

  put_fileitem(fileitem);

  return fileitem;
}

static void for_each_child_callback(const char *filename, int attrib, int param)
{
  FileItem* fileitem = (FileItem*)param;
  FileItem* child;
  const char *filename_without_path = get_filename(filename);

  if (*filename_without_path == '.' &&
      (ustrcmp(filename_without_path, ".") == 0 ||
       ustrcmp(filename_without_path, "..") == 0))
    return;

  child = get_fileitem_by_path(filename, FALSE);
  if (!child) {
    child = new FileItem(fileitem);

    child->filename = filename;
    child->displayname = filename_without_path;
    child->attrib = attrib;

    put_fileitem(child);
  }
  else {
    assert(child->parent == fileitem);
  }

  fileitem->insert_child_sorted(child);
}

static jstring remove_backslash_if_needed(const jstring& filename)
{
  if (!filename.empty() && jstring::is_separator(filename.back())) {
    int len = filename.size();
#ifdef HAVE_DRIVES
    // if the name is C:\ or something like that, the backslash isn't
    // removed
    if (len == 3 && filename[1] == ':')
      return filename;
#else
    // this is just the root '/' slash
    if (len == 1)
      return filename;
#endif
    jstring tmp(filename);
    tmp.remove_separator();
    return tmp;
  }
  return filename;
}

static jstring get_key_for_filename(const jstring& filename)
{
  jstring buf(filename);

#if !defined CASE_SENSITIVE
  buf = buf.lower();
#endif
  buf.fix_separators();

  return buf;
}

static void put_fileitem(FileItem* fileitem)
{
  assert(fileitem->filename != NOTINITIALIZED);
  assert(fileitem->keyname == NOTINITIALIZED);

  fileitem->keyname = get_key_for_filename(fileitem->filename);

  assert(fileitem->keyname != NOTINITIALIZED);

  // insert this file-item in the hash-table
  fileitems_map.insert(std::make_pair(fileitem->keyname, fileitem));
}

#endif
