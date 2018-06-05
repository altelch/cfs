/*
  Copyright (C) 2010 Proxmox Server Solutions GmbH

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  Author: Dietmar Maurer <dietmar@proxmox.com>

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <glib.h>

#include "cfs-utils.h"
#include "memdb.h"
#include "status.h"

#define CFS_LOCK_TIMEOUT (60*2)

memdb_tree_entry_t *
memdb_tree_entry_new(
	const char *name)
{
	g_return_val_if_fail(name != NULL, NULL);

	memdb_tree_entry_t *te = g_malloc0(sizeof(memdb_tree_entry_t) + strlen(name) + 1);
	g_return_val_if_fail(te != NULL, NULL);

	strcpy(te->name, name);

	return te;
}

memdb_tree_entry_t *
memdb_tree_entry_copy(
	memdb_tree_entry_t *te, 
	gboolean with_data)
{
	g_return_val_if_fail(te != NULL, NULL);

	memdb_tree_entry_t *cpy = memdb_tree_entry_new(te->name);

	cpy->parent = te->parent;
	cpy->inode = te->inode;
	cpy->version = te->version;
	cpy->writer = te->writer;
	cpy->mtime = te->mtime;
	cpy->type = te->type;
	cpy->size = te->size;

	if (with_data && te->size && te->type == DT_REG) {
		cpy->data.value = g_memdup(te->data.value, te->size);
	} else { 
		cpy->data.value = NULL;
	}

	return cpy;
}

void 
memdb_tree_entry_free(
	memdb_tree_entry_t *te)
{
	g_return_if_fail(te != NULL);

	if (te->type == DT_REG) {
		if (te->data.value)
			g_free(te->data.value);
	}

	if (te->type == DT_DIR) {
		if (te->data.entries)
			g_hash_table_destroy(te->data.entries);
	}

	g_free(te);
}

void 
memdb_lock_info_free(memdb_lock_info_t *li)
{
	g_return_if_fail(li != NULL);

	if (li->path)
		g_free(li->path);

	g_free(li);
}

static gint
memdb_tree_compare(
	gconstpointer v1,
	gconstpointer v2)
{
	guint64 a = ((const memdb_tree_entry_t *)v1)->inode;
	guint64 b = ((const memdb_tree_entry_t *)v2)->inode;

	if (a == b)
		return 0;

	if (a > b)
		return 1;
	
	return -1;
}

static void 
split_path(
	const char *path, 
	char **dirname, 
	char **basename)
{
	char *dup = g_strdup (path);
	int len = strlen (dup) - 1;
	while (len >= 0 && dup[len] == '/') dup[len--] = 0;

	char *dn = g_path_get_dirname (dup);
	char *bn = g_path_get_basename (dup);

	g_free (dup);

	*dirname = dn;
	*basename = bn;
}

static memdb_tree_entry_t *
memdb_lookup_dir_entry(
	memdb_t *memdb, 
	const char *name, 
	memdb_tree_entry_t *parent)
{

	g_return_val_if_fail(memdb != NULL, NULL);
	g_return_val_if_fail(name != NULL, NULL);
	g_return_val_if_fail(parent != NULL, NULL);
	g_return_val_if_fail(parent->type == DT_DIR, NULL);
	
	GHashTable *ht = parent->data.entries;

	g_return_val_if_fail(ht != NULL, NULL);

	return g_hash_table_lookup(ht, name);
}

static memdb_tree_entry_t *
memdb_lookup_path(
	memdb_t *memdb, 
	const char *path,
	memdb_tree_entry_t **parent)
{
	g_return_val_if_fail(memdb != NULL, NULL);
	g_return_val_if_fail(path != NULL, NULL);
	g_return_val_if_fail(parent != NULL, NULL);

	memdb_tree_entry_t *cdir = memdb->root; 
	*parent = NULL;

	if (path[0] == 0 || ((path[0] == '.' || path[0] == '/')  && path[1] == 0))
		return cdir;

	gchar **set = g_strsplit_set(path, "/", 0);

	int i = 0;
	char *name;
       
	while ((name = set[i++])) {

		if (name[0] == 0) continue;

		*parent = cdir;
		if ((cdir = memdb_lookup_dir_entry(memdb, name, cdir)) == NULL)
			break;
	}

	g_strfreev(set);

	return cdir;
}

gboolean
memdb_lock_expired(
	memdb_t *memdb,
	const char *path,
	const guchar csum[32])
{
	g_return_val_if_fail(memdb != NULL, FALSE);
	g_return_val_if_fail(memdb->locks != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);
	g_return_val_if_fail(csum != NULL, FALSE);

	memdb_lock_info_t *li;
	uint32_t ctime = time(NULL);

	if ((li = g_hash_table_lookup(memdb->locks, path))) {
		if (memcmp(csum, li->csum, 32) != 0) {
			li->ltime = ctime;
			memcpy(li->csum, csum, 32);
			g_critical("wrong lock csum - reset timeout");
			return FALSE;
		}
		if ((ctime > li->ltime) && ((ctime - li->ltime) > CFS_LOCK_TIMEOUT))
			return TRUE;
	} else {
		li = g_new0(memdb_lock_info_t, 1);
		li->path = g_strdup(path);
		li->ltime = ctime;
		memcpy(li->csum, csum, 32);
		g_hash_table_replace(memdb->locks, li->path, li);
	}

	return FALSE;
}

void
memdb_update_locks(memdb_t *memdb)
{
	g_return_if_fail(memdb != NULL);
	g_return_if_fail(memdb->locks != NULL);

	memdb_tree_entry_t *te, *parent;

	if (!(te = memdb_lookup_path(memdb, "priv/lock", &parent)))
		return;
		
	if (te->type != DT_DIR)
		return;


	GHashTable *old =  memdb->locks;
	memdb->locks = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					     (GDestroyNotify)memdb_lock_info_free);
	GHashTableIter iter;
	GHashTable *ht = te->data.entries;

	gpointer key, value;

	g_hash_table_iter_init (&iter, ht);
	while (g_hash_table_iter_next (&iter, &key, &value)) {

		memdb_tree_entry_t *lock_te = (memdb_tree_entry_t *)value;
		if (lock_te->type != DT_DIR)
			continue;

		memdb_lock_info_t *li;
		li = g_new0(memdb_lock_info_t, 1);
		li->path = g_strdup_printf("priv/lock/%s", lock_te->name);

		guchar csum[32];
		if (memdb_tree_entry_csum(lock_te, csum)) {
			memcpy(li->csum, csum, 32);
			memdb_lock_info_t *oldli;
			if ((oldli = g_hash_table_lookup(memdb->locks, lock_te->name)) &&
			    (memcmp(csum, oldli->csum, 32) == 0)) {
				li->ltime = oldli->ltime;
			} else {
				li->ltime = time(NULL);
			}
			g_hash_table_insert(memdb->locks, li->path, li);
		} else {
			memdb_lock_info_free(li);
		}
	}

	if (old)
		g_hash_table_destroy(old);
	
}

memdb_t *
memdb_open(const char *dbfilename)
{
	memdb_t *memdb = g_new0(memdb_t, 1);
	
	g_mutex_init(&memdb->mutex);

	memdb->dbfilename = g_strdup(dbfilename);

	memdb->root = memdb_tree_entry_new("");
	memdb->root->data.entries = g_hash_table_new(g_str_hash, g_str_equal);
	memdb->root->type = DT_DIR;

	memdb->index = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
					     (GDestroyNotify)memdb_tree_entry_free);

	g_hash_table_replace(memdb->index, &memdb->root->inode, memdb->root);

	memdb->locks = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					     (GDestroyNotify)memdb_lock_info_free);

	if (!(memdb->bdb = bdb_backend_open(dbfilename, memdb->root, memdb->index))) {
		memdb_close(memdb);
		return NULL;
	}

	record_memdb_reload();

	memdb_update_locks(memdb);

	cfs_debug("memdb open '%s' successful (version = %016zX)", 
		  dbfilename, memdb->root->version);

	return memdb;
}

void 
memdb_close(memdb_t *memdb)
{
	g_return_if_fail(memdb != NULL);

	g_mutex_lock (&memdb->mutex);

	if (memdb->bdb)
		bdb_backend_close(memdb->bdb);

	if (memdb->index)
		g_hash_table_destroy(memdb->index);

	if (memdb->locks)
		g_hash_table_destroy(memdb->locks);

	if (memdb->dbfilename)
		g_free(memdb->dbfilename);

	memdb->index = NULL;
	memdb->bdb = NULL;
	memdb->dbfilename = NULL;

	g_mutex_unlock (&memdb->mutex);

	g_mutex_clear (&memdb->mutex);

	g_free(memdb);
}

int memdb_mkdir(
	memdb_t *memdb, 
	const char *path,
	guint32 writer,
	guint32 mtime)
{
	g_return_val_if_fail(memdb != NULL, -EINVAL);
	g_return_val_if_fail(path != NULL, -EINVAL);

	int ret = -EACCES;

	char *dirname = NULL;
	char *base = NULL;

	g_mutex_lock (&memdb->mutex);

	if (memdb->errors) {
		ret = -EIO;
		goto ret;
	}

	split_path(path, &dirname, &base);

	memdb_tree_entry_t *parent, *unused;

	if (!(parent = memdb_lookup_path(memdb, dirname, &unused))) {
		ret = -ENOENT;
		goto ret;
	}
		
	if (parent->type != DT_DIR) {
		ret = -ENOTDIR;
		goto ret;
	}

	/* do not allow '.' and '..' */
	if ((base[0] == 0) ||
	    (base[0] == '.' && base[1] == 0) ||
	    (base[0] == '.' && base[1] == '.' && base[2] == 0)) {
		ret = -EACCES;		
		goto ret;
	}

	memdb_tree_entry_t *te;
	if ((te = memdb_lookup_dir_entry(memdb, base, parent))) {
		ret = -EEXIST;
		goto ret;
	}

	memdb->root->version++;
	memdb->root->mtime = mtime;
	memdb->root->writer = writer;

	te = memdb_tree_entry_new(base);
	te->parent = parent->inode;
	te->data.entries = g_hash_table_new(g_str_hash, g_str_equal);
	te->inode = te->version = memdb->root->version;
	te->writer = writer;
	te->type = DT_DIR;
	te->mtime = mtime;

	g_hash_table_replace(parent->data.entries, te->name, te);
	g_hash_table_replace(memdb->index, &te->inode, te);

	cfs_debug("memdb_mkdir %s %s %016zX", dirname, base, memdb->root->version);

	if (bdb_backend_write(memdb->bdb, te->inode, te->parent, te->version, 
			      te->writer, te->mtime, 0, DT_DIR, te->name, NULL, 0)) {
		memdb->errors = 1;
		ret = -EIO;
		goto ret;
	}

	if (strcmp(dirname, "priv/lock") == 0) {
		g_hash_table_remove(memdb->locks, path);
		guchar csum[32];
		if (memdb_tree_entry_csum(te, csum)) {
			memdb_lock_expired(memdb, path, csum); // insert a new entry
		}
	}

	ret = 0;

 ret:
	g_mutex_unlock (&memdb->mutex);

	if (dirname) g_free (dirname);
	if (base) g_free (base);

	return ret;
}

int 
memdb_read(
	memdb_t *memdb, 
	const char *path, 
	gpointer *data_ret)
{
	g_return_val_if_fail(memdb != NULL, -EINVAL);
	g_return_val_if_fail(path != NULL, -EINVAL);
	g_return_val_if_fail(data_ret != NULL, -EINVAL);

	memdb_tree_entry_t *te, *parent; 

	g_mutex_lock (&memdb->mutex);

	if ((te = memdb_lookup_path(memdb, path, &parent))) {
		if (te->type == DT_REG) {
			*data_ret = g_memdup(te->data.value, te->size);
			guint32 size = te->size;
			g_mutex_unlock (&memdb->mutex);
			return size;
		}
	}

	g_mutex_unlock (&memdb->mutex);

	return -ENOENT;
}

static int 
memdb_pwrite(
	memdb_t *memdb, 
	const char *path, 
	guint32 writer, 
	guint32 mtime, 
	gconstpointer data, 
	size_t count, 
	off_t offset, 
	gboolean truncate, 
	gboolean create)
{
	g_return_val_if_fail(memdb != NULL, -EINVAL);
	g_return_val_if_fail(path != NULL, -EINVAL);
	g_return_val_if_fail(count == 0 || data != NULL, -EINVAL);

	int ret = -EACCES;

	char *dirname = NULL;
	char *base = NULL;
	char *nodename = NULL;

	g_mutex_lock (&memdb->mutex);

	if (memdb->errors) {
		ret = -EIO;
		goto ret;
	}

	if ((offset + count) > MEMDB_MAX_FILE_SIZE) {
		ret = -EFBIG;
		goto ret;
	}

	split_path(path, &dirname, &base);

	memdb_tree_entry_t *parent, *unused;
	if (!(parent = memdb_lookup_path(memdb, dirname, &unused))) {
		ret = -ENOENT;
		goto ret;
	}
	if (parent->type != DT_DIR) {
		ret = -ENOTDIR;
		goto ret;
	}

	/* do not allow '.' and '..' */
	if ((base[0] == 0) ||
	    (base[0] == '.' && base[1] == 0) ||
	    (base[0] == '.' && base[1] == '.' && base[2] == 0)) {
		ret = -EACCES;		
		goto ret;
	}

	gpointer olddata = NULL;

	memdb_tree_entry_t *te, *old;
	if ((old = te = memdb_lookup_dir_entry(memdb, base, parent))) {
		if (te->type != DT_REG) { 
			ret = -ENOTDIR;
			goto ret;
		}

		if (create) {
			ret = -EEXIST;
			goto ret;
		}

		memdb->root->version++;
		memdb->root->mtime = mtime;
		memdb->root->writer = writer;

		olddata = te->data.value;
	} else {

		if (!create) {
			ret = -ENOENT;
			goto ret;
		}

		memdb->root->version++;
		memdb->root->mtime = mtime;
		memdb->root->writer = writer;

		te = memdb_tree_entry_new(base);
		te->parent = parent->inode;
		te->type = DT_REG;
		te->inode = memdb->root->version;
	}

	te->version = memdb->root->version;
	te->writer = writer;
	te->mtime = mtime;

	size_t newsize = offset + count;

	gpointer newdata = NULL;

	if (olddata) {

		if (newsize > te->size) {
			newdata = g_malloc0(newsize);
			memcpy(newdata, olddata, te->size);

		} else {

			if (!truncate) {
				newsize = te->size;
			}
			newdata = g_malloc0(newsize);
			memcpy(newdata, olddata, newsize);
		}

		if (count && data)
			memcpy(newdata + offset, data, count);

	} else {

		if (count && data) {
			newdata = g_malloc0(newsize);
			memcpy(newdata + offset, data, count);
		}
	}

	te->size = newsize;
	te->data.value = newdata;

	g_free(olddata);

	if (!old) {
		g_hash_table_replace(parent->data.entries, te->name, te);
		g_hash_table_replace(memdb->index, &te->inode, te);
	}

	record_memdb_change(path);

	cfs_debug("memdb_pwrite %s %s %016zX %016zX", dirname, te->name, te->inode, te->version);

	if (bdb_backend_write(memdb->bdb, te->inode, te->parent, te->version, 
			      te->writer, te->mtime, te->size, te->type, te->name, 
			      te->data.value, 0)) {
		memdb->errors = 1;
		ret = -EIO;
		goto ret;
	}

	ret = count;

 ret:
	g_mutex_unlock (&memdb->mutex);

	if (nodename) g_free (nodename);
	if (dirname) g_free (dirname);
	if (base) g_free (base);

	return ret;
}

int 
memdb_mtime(
	memdb_t *memdb, 
	const char *path, 
	guint32 writer, 
	guint32 mtime)
{
	g_return_val_if_fail(memdb != NULL, -EINVAL);
	g_return_val_if_fail(path != NULL, -EINVAL);

	int ret = -EACCES;

	char *dirname = NULL;
	char *base = NULL;

	g_mutex_lock (&memdb->mutex);

	if (memdb->errors) {
		ret = -EIO;
		goto ret;
	}

	split_path(path, &dirname, &base);

	memdb_tree_entry_t *parent, *unused;
	if (!(parent = memdb_lookup_path(memdb, dirname, &unused))) {
		ret = -ENOENT;
		goto ret;
	}
	if (parent->type != DT_DIR) {
		ret = -ENOTDIR;
		goto ret;
	}

	/* do not allow '.' and '..' */
	if ((base[0] == 0) ||
	    (base[0] == '.' && base[1] == 0) ||
	    (base[0] == '.' && base[1] == '.' && base[2] == 0)) {
		ret = -EACCES;		
		goto ret;
	}

	memdb_tree_entry_t *te;
	if (!(te = memdb_lookup_dir_entry(memdb, base, parent))) {
		ret = -ENOENT;
		goto ret;
	}

        int is_lock = (strcmp(dirname, "priv/lock") == 0) && (te->type == DT_DIR);

	/* NOTE: we use utime(0,0) to trigger 'unlock', so we do not
	 * allow to change mtime for locks (only if mtime is newer).
	 * See README for details about locks.
	 */
        if (is_lock) {
            if (mtime < te->mtime) {
		cfs_debug("dir is locked");
		ret = -EACCES;		
		goto ret;
            } else {
                /* only allow lock updates if the writer is the same */
                if (te->writer != writer) { 
                    ret = -EACCES;		
                    goto ret;
                }
            }
	}
	
	memdb->root->version++;
	memdb->root->mtime = mtime;
	memdb->root->writer = writer;

	te->version = memdb->root->version;
	te->writer = writer;
	te->mtime = mtime;

	record_memdb_change(path);

	cfs_debug("memdb_mtime %s %s %016zX %016zX", dirname, te->name, te->inode, te->version);

	if (bdb_backend_write(memdb->bdb, te->inode, te->parent, te->version, 
			      te->writer, te->mtime, te->size, te->type, te->name, 
			      te->data.value, 0)) {
		memdb->errors = 1;
		ret = -EIO;
		goto ret;
	}

        if (is_lock) {
            cfs_debug("update cfs lock");
            g_hash_table_remove(memdb->locks, path);
            guchar csum[32];
            if (memdb_tree_entry_csum(te, csum)) {
                memdb_lock_expired(memdb, path, csum); // insert a new entry
            }
        }

	ret = 0;

 ret:
	g_mutex_unlock (&memdb->mutex);

	if (dirname) g_free (dirname);
	if (base) g_free (base);

	return ret;
}

int 
memdb_create(
	memdb_t *memdb, 
	const char *path,
	guint32 writer,
	guint32 mtime)
{
	return memdb_pwrite(memdb, path, writer, mtime, NULL, 0, 0, FALSE, TRUE);
}

int 
memdb_write(
	memdb_t *memdb, 
	const char *path, 
	guint32 writer, 
	guint32 mtime,
	gconstpointer data, 
	size_t count, 
	off_t offset, 
	gboolean truncate)
{
	return memdb_pwrite(memdb, path, writer, mtime, data, count, offset, truncate, FALSE);
}

memdb_tree_entry_t *
memdb_getattr(
	memdb_t *memdb, 
	const char *path)
{
	memdb_tree_entry_t *te, *parent; 

	g_mutex_lock (&memdb->mutex);

	if ((te = memdb_lookup_path(memdb, path, &parent))) {

		memdb_tree_entry_t *cpy = memdb_tree_entry_copy(te, 0);

		g_mutex_unlock (&memdb->mutex);

		return cpy;
	}

	g_mutex_unlock (&memdb->mutex);

	return NULL;
}

GList *
memdb_readdir(
	memdb_t *memdb, 
	const char *path)
{
	g_return_val_if_fail(memdb != NULL, NULL);
	g_return_val_if_fail(path != NULL, NULL);

	memdb_tree_entry_t *te, *parent;

	GList *list = NULL;

	g_mutex_lock (&memdb->mutex);

	if (!(te = memdb_lookup_path(memdb, path, &parent)))
		goto ret;
		
	if (te->type != DT_DIR)
		goto ret;

	GHashTable *ht = te->data.entries;

	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init (&iter, ht);

	while (g_hash_table_iter_next (&iter, &key, &value)) {

		te = (memdb_tree_entry_t *)value;

		memdb_tree_entry_t *cpy = memdb_tree_entry_copy(te, 0);

		list = g_list_append(list, cpy);
	}

 ret:
	g_mutex_unlock (&memdb->mutex);
	
	return list;
}

void 
memdb_dirlist_free(GList *dirlist)
{
	GList *l = dirlist;

	while (l) {
		if (l->data) 
			g_free (l->data);

		l = g_list_next(l);
	}

	if (dirlist) 
		g_list_free(dirlist);
}

static int 
unlink_tree_entry(
	memdb_t *memdb,
	memdb_tree_entry_t *parent,
	memdb_tree_entry_t *te)
{
	g_return_val_if_fail(parent != NULL, -EACCES);
	g_return_val_if_fail(parent->inode == te->parent, -EACCES);

	if (te->type == DT_DIR)
		if (g_hash_table_size(te->data.entries))
			return -ENOTEMPTY;

	if (!g_hash_table_steal(parent->data.entries, te->name)) {
		cfs_critical("internal error - can't delete entry");
		memdb->errors = 1;
		return -EIO;
	}

	if (!g_hash_table_steal(memdb->index, &te->inode)) {
		cfs_critical("internal error - can't delete entry");
		memdb->errors = 1;
		return -EIO;
	}

	return 0;
}

int 
memdb_rename(
	memdb_t *memdb, 
	const char *from, 
	const char *to, 
	guint32 writer,
	guint32 mtime)
{
	int ret = -EACCES;

	char *nodename = NULL;
	char *dirname = NULL;
	char *base = NULL;

	char *from_node = NULL;

	g_mutex_lock (&memdb->mutex);

	if (memdb->errors) {
		ret = -EIO;
		goto ret;
	}

	memdb_tree_entry_t *from_te, *from_parent;
	memdb_tree_entry_t *to_te, *to_parent;
	memdb_tree_entry_t *target_te, *target_parent;

	guint64 delete_inode = 0;

	if (!(from_te = memdb_lookup_path(memdb, from, &from_parent))) {
		ret = -ENOENT;
		goto ret;
	}	

	if (!from_parent) { /* can't rename root */
		ret = -EACCES;
		goto ret;
	}

	/* we do not allow rename for locks */
	if (from_te->type == DT_DIR && path_is_lockdir(from)) {
		ret = -EACCES;
		goto ret;
	}

	if ((to_te = memdb_lookup_path(memdb, to, &to_parent))) {

		if ((ret = unlink_tree_entry(memdb, to_parent, to_te)) != 0)
			goto ret;

		base = strdup(to_te->name);

		delete_inode = to_te->inode;

		target_te = to_parent;

		memdb_tree_entry_free(to_te);

	} else {

		split_path(to, &dirname, &base);

		if (!(target_te = memdb_lookup_path(memdb, dirname, &target_parent))) {
			ret = -ENOENT;
			goto ret;
		}
		
		if (target_te->type != DT_DIR) {
			ret = -ENOTDIR;
			goto ret;
		}
	}

	record_memdb_change(from);
	record_memdb_change(to);

	/* NOTE: unlink_tree_entry() make sure that we can only 
	   rename emtpy directories */

	if ((ret = unlink_tree_entry(memdb, from_parent, from_te)) != 0)
		goto ret;

	memdb->root->version++;
	memdb->root->mtime = mtime;
	memdb->root->writer = writer;

	memdb_tree_entry_t *new = memdb_tree_entry_new(base);
	new->parent = target_te->inode;
	new->inode = from_te->inode;
	new->version = memdb->root->version;
	new->writer = writer;
	new->mtime = mtime;
	new->size = from_te->size;
	new->type =  from_te->type;
	new->data = from_te->data;

	g_free(from_te);

	g_hash_table_replace(target_te->data.entries, new->name, new);
	g_hash_table_replace(memdb->index, &new->inode, new);

	if (bdb_backend_write(memdb->bdb, new->inode, new->parent, 
			      new->version, new->writer, new->mtime,
			      new->size, new->type, new->name, 
			      new->data.value, delete_inode)) {
		memdb->errors = 1;
		ret = -EIO;
		goto ret;
	}

	if (new->type == DT_REG) {
	        // Was vm handling	
	} else if (new->type == DT_DIR) {
		/* directories are alwayse empty (see unlink_tree_entry) */
	}

	ret = 0;

 ret:
	g_mutex_unlock (&memdb->mutex);

	if (from_node) g_free(from_node);
	if (nodename) g_free (nodename);
	if (dirname) g_free (dirname);
	if (base) g_free (base);

	return ret;
}

int 
memdb_delete(
	memdb_t *memdb, 
	const char *path, 
	guint32 writer, 
	guint32 mtime)
{
	memdb_tree_entry_t *te, *parent;

	g_mutex_lock (&memdb->mutex);

	int ret = -EACCES;

	if (memdb->errors) {
		ret = -EIO;
		goto ret;
	}

	if (!(te = memdb_lookup_path(memdb, path, &parent))) {
		ret = -ENOENT;
		goto ret;
	}	
		
	if (!parent) { /* cant remove root */
		ret = -EACCES;
		goto ret;
	}

	if (te->type == DT_DIR) {
		if (g_hash_table_size(te->data.entries)) {
			ret = -ENOTEMPTY;
			goto ret;
		}

		g_hash_table_remove(memdb->locks, path);
	}

	record_memdb_change(path);

	if ((ret = unlink_tree_entry(memdb, parent, te)) != 0)
		goto ret;

	memdb->root->version++;
	memdb->root->mtime = mtime;
	memdb->root->writer = writer;

	if (bdb_backend_write(memdb->bdb, 0, 0, memdb->root->version, writer, mtime, 0, 
			      DT_REG, NULL, NULL, te->inode)) {
		memdb->errors = 1;
		memdb_tree_entry_free(te);
		ret = -EIO;
		goto ret;
	}

	memdb_tree_entry_free(te);

	ret = 0;

 ret:
	g_mutex_unlock (&memdb->mutex);

	return ret;
}

int 
memdb_statfs(
	memdb_t *memdb, 
	struct statvfs *stbuf)
{
	g_return_val_if_fail(memdb != NULL, -EINVAL);
	g_return_val_if_fail(stbuf != NULL, -EINVAL);

	g_mutex_lock (&memdb->mutex);

	GHashTableIter iter;
	gpointer key, value;

	size_t size = 0;
	size_t files = 0;

	g_hash_table_iter_init (&iter, memdb->index);

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		memdb_tree_entry_t *te = (memdb_tree_entry_t *)value;
		files++;
		size += te->size;
	}

	g_mutex_unlock (&memdb->mutex);

	stbuf->f_bsize = MEMDB_BLOCKSIZE;
	stbuf->f_blocks = MEMDB_BLOCKS;
	stbuf->f_bfree = stbuf->f_bavail = stbuf->f_blocks - 
		((size + stbuf->f_bsize - 1)/stbuf->f_bsize);
	stbuf->f_files = MEMDB_MAX_INODES;
	stbuf->f_ffree = stbuf->f_files - files;

	stbuf->f_namemax = 256;

	return 0;
}

void 
tree_entry_debug(memdb_tree_entry_t *te)
{
	g_return_if_fail(te != NULL);

	// same as  tree_entry_print(), but use cfs_debug() instead of g_print()

	cfs_debug("%016zX %c %016zX %016zX %08X %08X %08X %s\n", 
		te->inode, te->type == DT_DIR ? 'D' : 'R', te->parent, te->version,
		te->writer, te->mtime, te->size, te->name); 
}

void 
tree_entry_print(memdb_tree_entry_t *te)
{
	g_return_if_fail(te != NULL);

	g_print("%016zX %c %016zX %016zX %08X %08X %08X %s\n", 
		te->inode, te->type == DT_DIR ? 'D' : 'R', te->parent, te->version,
		te->writer, te->mtime, te->size, te->name); 
}

void 
memdb_dump(memdb_t *memdb)
{
	g_return_if_fail(memdb != NULL);

	g_mutex_lock (&memdb->mutex);
	
	GList *list = g_hash_table_get_values(memdb->index);
      
	list = g_list_sort(list, memdb_tree_compare);

	g_print("%16s %c %16s %16s %8s %8s %8s %s\n",
		"INODE", 'T', "PARENT", "VERSION", "WRITER", "MTIME", "SIZE", "NAME");

	GList *l = list;
	while (l) {
		memdb_tree_entry_t *te = (memdb_tree_entry_t *)l->data;

		tree_entry_print(te);

		l = g_list_next(l);
	}

	g_list_free(list);

	g_mutex_unlock (&memdb->mutex);
}

void 
memdb_dump_index (memdb_index_t *idx)
{
	g_return_if_fail(idx != NULL);

	g_print ("INDEX DUMP %016zX\n", idx->version);

	int i;
	for (i = 0; i < idx->size; i++) {
		g_print ("%016zX %016zX%016zX%016zX%016zX\n", idx->entries[i].inode,
			 *((guint64 *)idx->entries[i].digest),
			 *((guint64 *)(idx->entries[i].digest + 8)),
			 *((guint64 *)(idx->entries[i].digest + 16)),
			 *((guint64 *)(idx->entries[i].digest + 24)));
	}
}

memdb_index_t *
memdb_index_copy(memdb_index_t *idx)
{
	g_return_val_if_fail(idx != NULL, NULL);

	int bytes = sizeof(memdb_index_t) + idx->size*sizeof(memdb_index_extry_t);
	if (idx->bytes != bytes) {
		cfs_critical("memdb index contains wrong number of bytes");
		return NULL;
	}

 	memdb_index_t *copy = (memdb_index_t *)g_memdup(idx, bytes);

	return copy;
}

gboolean
memdb_tree_entry_csum(
	memdb_tree_entry_t *te,
	guchar csum[32])
{
	g_return_val_if_fail(te != NULL, FALSE);
	g_return_val_if_fail(csum != NULL, FALSE);

	GChecksum *sha256 = g_checksum_new(G_CHECKSUM_SHA256); 

	g_checksum_update(sha256, (unsigned char*)&te->inode, sizeof(te->inode));
	g_checksum_update(sha256, (unsigned char*)&te->version, sizeof(te->version));
	g_checksum_update(sha256, (unsigned char*)&te->writer, sizeof(te->writer));
	g_checksum_update(sha256, (unsigned char*)&te->mtime, sizeof(te->mtime));
	g_checksum_update(sha256, (unsigned char*)&te->size, sizeof(te->size));
	g_checksum_update(sha256, (unsigned char*)&te->type, sizeof(te->type));
	g_checksum_update(sha256, (unsigned char*)&te->parent, sizeof(te->parent));
	g_checksum_update(sha256, (unsigned char*)te->name, strlen(te->name));

	if (te->type == DT_REG && te->size) 
		g_checksum_update(sha256, (unsigned char*)te->data.value, te->size);

	size_t csum_len = 32;
	g_checksum_get_digest(sha256, csum, &csum_len);
	g_checksum_free(sha256);

	return TRUE;
} 

gboolean 
memdb_compute_checksum(
	GHashTable *index, 
	memdb_tree_entry_t *root, 
	guchar *csum, 
	size_t csum_len)
{
	g_return_val_if_fail(index != NULL, FALSE);
	g_return_val_if_fail(root != NULL, FALSE);

	GChecksum *sha256 = g_checksum_new(G_CHECKSUM_SHA256); 

	GList *list = g_hash_table_get_values(index);
      
	list = g_list_sort(list, memdb_tree_compare);
	
	GList *l = list;
	while (l) {
		memdb_tree_entry_t *te = (memdb_tree_entry_t *)l->data;

		g_checksum_update(sha256, (unsigned char*)&te->inode, sizeof(te->inode));
		g_checksum_update(sha256, (unsigned char*)&te->version, sizeof(te->version));
		g_checksum_update(sha256, (unsigned char*)&te->writer, sizeof(te->writer));
		g_checksum_update(sha256, (unsigned char*)&te->mtime, sizeof(te->mtime));
		g_checksum_update(sha256, (unsigned char*)&te->size, sizeof(te->size));
		g_checksum_update(sha256, (unsigned char*)&te->type, sizeof(te->type));
		g_checksum_update(sha256, (unsigned char*)&te->parent, sizeof(te->parent));
		g_checksum_update(sha256, (unsigned char*)te->name, strlen(te->name));

		if (te->type == DT_REG && te->size) 
			g_checksum_update(sha256, (unsigned char*)te->data.value, te->size);
		
		l = g_list_next(l);
	}

	g_list_free(list);

	g_checksum_get_digest(sha256, csum, &csum_len);

	cfs_debug("checksum: %s", g_checksum_get_string(sha256));

	g_checksum_free(sha256);

	return TRUE;
}

memdb_index_t *
memdb_encode_index(
	GHashTable *index, 
	memdb_tree_entry_t *root)
{
	g_return_val_if_fail(index != NULL, NULL);
	g_return_val_if_fail(root != NULL, NULL);

	memdb_index_t *idx = NULL;

	int count = g_hash_table_size(index);
	if (!count) {
		cfs_critical("memdb index has no entires");
		return NULL;
	}

	int bytes = sizeof(memdb_index_t) + count*sizeof(memdb_index_extry_t);
	idx = g_malloc0(bytes);

	idx->size = count;
	idx->bytes = bytes;
	idx->version = root->version;
	idx->mtime = root->mtime;
	idx->writer = root->writer;

	GChecksum *sha256 = g_checksum_new(G_CHECKSUM_SHA256); 

	GList *list = g_hash_table_get_values(index);
      
	list = g_list_sort(list, memdb_tree_compare);
	
	int ind = 0;
	GList *l = list;
	while (l) {
		memdb_tree_entry_t *te = (memdb_tree_entry_t *)l->data;

		if (te->inode > idx->last_inode)
			idx->last_inode = te->inode;

		idx->entries[ind].inode = te->inode;

		g_checksum_reset (sha256);

		g_checksum_update(sha256, (unsigned char*)&te->version, sizeof(te->version));
		g_checksum_update(sha256, (unsigned char*)&te->writer, sizeof(te->writer));
		g_checksum_update(sha256, (unsigned char*)&te->mtime, sizeof(te->mtime));
		g_checksum_update(sha256, (unsigned char*)&te->size, sizeof(te->size));
		g_checksum_update(sha256, (unsigned char*)&te->type, sizeof(te->type));
		g_checksum_update(sha256, (unsigned char*)&te->parent, sizeof(te->parent));
		g_checksum_update(sha256, (unsigned char*)te->name, strlen(te->name));

		if (te->type == DT_REG && te->size) 
			g_checksum_update(sha256, (unsigned char*)te->data.value, te->size);

		gsize len = 32;
		g_checksum_get_digest(sha256, (guint8 *)idx->entries[ind].digest, &len);
		
		ind++;

		l = g_list_next(l);
	}

	g_list_free(list);

	g_checksum_free(sha256);

	return idx;
}
