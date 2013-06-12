/*
 * Copyright (C) 2011 Andrea Mazzoleni
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "portable.h"

#include "util.h"
#include "elem.h"
#include "state.h"

struct snapraid_scan {
	/**
	 * Counters of changes.
	 */
	unsigned count_equal;
	unsigned count_moved;
	unsigned count_change;
	unsigned count_remove;
	unsigned count_insert;

	tommy_list file_insert_list; /**< Files to insert. */
	tommy_list link_insert_list; /**< Links to insert. */
	tommy_list dir_insert_list; /**< Dirs to insert. */

	/* nodes for data structures */
	tommy_node node;
};

/**
 * Removes the specified link from the data set.
 */
static void scan_link_remove(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_link* link)
{
	/* state changed */
	state->need_write = 1;

	/* remove the file from the link containers */
	tommy_hashdyn_remove_existing(&disk->linkset, &link->nodeset);
	tommy_list_remove_existing(&disk->linklist, &link->nodelist);

	/* deallocate */
	link_free(link);
}

/**
 * Inserts the specified link in the data set.
 */
static void scan_link_insert(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_link* link)
{
	/* state changed */
	state->need_write = 1;

	/* insert the link in the link containers */
	tommy_hashdyn_insert(&disk->linkset, &link->nodeset, link, link_name_hash(link->sub));
	tommy_list_insert_tail(&disk->linklist, &link->nodelist, link);
}

/**
 * Processes a symbolic link.
 */
static void scan_link(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* sub, const char* linkto, unsigned link_flag)
{
	struct snapraid_link* link;

	/* check if the link already exists */
	link = tommy_hashdyn_search(&disk->linkset, link_name_compare, sub, link_name_hash(sub));
	if (link) {
		/* check if multiple files have the same name */
		if (link_flag_has(link, FILE_IS_PRESENT)) {
			fprintf(stderr, "Internal inconsistency for link '%s%s'\n", disk->dir, sub);
			exit(EXIT_FAILURE);
		}

		/* mark as present */
		link_flag_set(link, FILE_IS_PRESENT);

		/* check if the link is not changed and it's of the same kind */
		if (strcmp(link->linkto, linkto) == 0 && link_flag == link_flag_get(link, FILE_IS_LINK_MASK)) {
			/* it's equal */
			++scan->count_equal;

			if (state->gui) {
				fprintf(stdlog, "scan:equal:%s:%s\n", disk->name, link->sub);
				fflush(stdlog);
			}

			/* nothing more to do */
			return;
		} else {
			/* it's an update */
			if (state->gui) {
				fprintf(stdlog, "scan:update:%s:%s\n", disk->name, link->sub);
				fflush(stdlog);
			}
			if (output) {
				printf("Update '%s%s'\n", disk->dir, link->sub);
			}

			++scan->count_change;

			/* update it */
			free(link->linkto);
			link->linkto = strdup_nofail(linkto);
			link_flag_let(link, link_flag, FILE_IS_LINK_MASK);

			/* nothing more to do */
			return;
		}
	} else {
		/* create the new link */
		++scan->count_insert;

		if (state->gui) {
			fprintf(stdlog, "scan:add:%s:%s\n", disk->name, sub);
			fflush(stdlog);
		}
		if (output) {
			printf("Add '%s%s'\n", disk->dir, sub);
		}

		/* and continue to insert it */
	}

	/* insert it */
	link = link_alloc(sub, linkto, link_flag);

	/* mark it as present */
	link_flag_set(link, FILE_IS_PRESENT);

	/* insert it in the delayed insert list */
	tommy_list_insert_tail(&scan->link_insert_list, &link->nodelist, link);
}

/**
 * Removes the specified file from the data set.
 */
static void scan_file_remove(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_file* file)
{
	block_off_t i;

	/* state changed */
	state->need_write = 1;

	/* free all the blocks of the file */
	for(i=0;i<file->blockmax;++i) {
		struct snapraid_block* block = &file->blockvec[i];
		block_off_t block_pos = block->parity_pos;
		unsigned block_state;
		struct snapraid_deleted* deleted;

		/* adjust the first free position */
		/* note that doing all the deletions before alllocations, */
		/* first_free_block is always 0 and the "if" is never triggered */
		/* but we keep this code anyway for completeness. */
		if (disk->first_free_block > block_pos)
			disk->first_free_block = block_pos;

		/* in case we scan after an aborted sync, */
		/* we could get also intermediate states like inv/chg/new */
		block_state = block_state_get(block);
		switch (block_state) {
		case BLOCK_STATE_BLK :
			/* we keep the hash making it an "old" hash, because the parity is still containing data for it */
			break;
		case BLOCK_STATE_CHG :
		case BLOCK_STATE_NEW :
			/* in these cases we don't know if the old state is still the one */
			/* stored inside the parity, because after an aborted sync, the parity */
			/* may be or may be not have been updated with the new data */
			/* Them we reset the hash to a bogus value */
			/* Note that this condition is possible only if: */
			/* - new files added/modified */
			/* - aborted sync, without saving the content file */
			/* - files deleted after the aborted sync */
			memset(block->hash, 0, HASH_SIZE);
			break;
		default:
			fprintf(stderr, "Internal state inconsistency in scanning for block %u state %u\n", block->parity_pos, block_state);
			exit(EXIT_FAILURE);
		}

		/* allocated a new deleted block from the block we are going to delete */
		deleted = deleted_dup(block);

		/* insert it in the list of deleted blocks */
		tommy_list_insert_tail(&disk->deletedlist, &deleted->node, deleted);

		/* set the deleted block in the block array */
		tommy_array_set(&disk->blockarr, block_pos, &deleted->block);
	}

	/* remove the file from the file containers */
	tommy_hashdyn_remove_existing(&disk->inodeset, &file->nodeset);
	tommy_hashdyn_remove_existing(&disk->pathset, &file->pathset);
	tommy_list_remove_existing(&disk->filelist, &file->nodelist);

	/* deallocate */
	file_free(file);
}

/**
 * Inserts the specified file in the data set.
 */
static void scan_file_insert(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_file* file)
{
	block_off_t i;
	block_off_t block_max;
	block_off_t block_pos;

	/* state changed */
	state->need_write = 1;

	/* allocate the blocks of the file */
	block_pos = disk->first_free_block;
	block_max = tommy_array_size(&disk->blockarr);
	for(i=0;i<file->blockmax;++i) {
		struct snapraid_block* block;

		/* find a free block */
		while (block_pos < block_max && block_has_file(tommy_array_get(&disk->blockarr, block_pos)))
			++block_pos;

		/* if not found, allocate a new one */
		if (block_pos == block_max) {
			++block_max;
			tommy_array_grow(&disk->blockarr, block_max);
		}

		/* set the position */
		file->blockvec[i].parity_pos = block_pos;

		/* block to overwrite */
		block = tommy_array_get(&disk->blockarr, block_pos);

		/* if the block is an empty one */
		if (block == BLOCK_EMPTY) {
			/* we just overwrite it with a NEW one */
			block_state_set(&file->blockvec[i], BLOCK_STATE_NEW);
		} else {
			/* otherwise it's a DELETED one, that we convert in CHG keeping the hash */
			block_state_set(&file->blockvec[i], BLOCK_STATE_CHG);
			memcpy(file->blockvec[i].hash, block->hash, HASH_SIZE);
		}

		/* store in the disk map, after invalidating all the other blocks */
		tommy_array_set(&disk->blockarr, block_pos, &file->blockvec[i]);
	}
	if (file->blockmax) {
		/* set the new free position, but only if allocated something */
		disk->first_free_block = block_pos + 1;
	}

	/* note that the file is already added in the file hashtables */
	tommy_list_insert_tail(&disk->filelist, &file->nodelist, file);
}

/**
 * Processes a file.
 */
static void scan_file(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* sub, const struct stat* st)
{
	struct snapraid_file* file;

	if (state->find_by_name) {
		/* check if the file path already exists */
		file = tommy_hashdyn_search(&disk->pathset, file_path_compare, sub, file_path_hash(sub));
	} else {
		/* check if the file inode already exists */
		uint64_t inode = st->st_ino;
		file = tommy_hashdyn_search(&disk->inodeset, file_inode_compare, &inode, file_inode_hash(inode));
	}

	if (file) {
		/* check if multiple files have the same inode */
		if (file_flag_has(file, FILE_IS_PRESENT)) {
			if (st->st_nlink > 1) {
				/* it's a hardlink */
				scan_link(scan, state, output, disk, sub, file->sub, FILE_IS_HARDLINK);
				return;
			} else {
				fprintf(stderr, "Internal inode '%"PRIu64"' inconsistency for file '%s%s'\n", (uint64_t)st->st_ino, disk->dir, sub);
				exit(EXIT_FAILURE);
			}
		}

		/* check if the file is not changed */
		if (file->size == st->st_size
			&& file->mtime_sec == st->st_mtime
			/* always accept the value if it's FILE_MTIME_NSEC_INVALID */
			/* it happens when upgrading from an old version of SnapRAID */
			&& (file->mtime_nsec == STAT_NSEC(st) || file->mtime_nsec == FILE_MTIME_NSEC_INVALID)
		) {
			/* mark as present */
			file_flag_set(file, FILE_IS_PRESENT);

			/* update the nano seconds mtime if required */
			if (file->mtime_nsec == FILE_MTIME_NSEC_INVALID
				&& STAT_NSEC(st) != FILE_MTIME_NSEC_INVALID
			) {
				file->mtime_nsec = STAT_NSEC(st);

				/* we have to save the new mtime */
				state->need_write = 1;
			}

			if (strcmp(file->sub, sub) != 0) {
				/* if the path is different, it means a moved file with the same inode */
				++scan->count_moved;

				if (file->inode != st->st_ino) {
					fprintf(stderr, "Internal inode inconsistency for file '%s%s'\n", disk->dir, sub);
					exit(EXIT_FAILURE);
				}

				if (state->gui) {
					fprintf(stdlog, "scan:move:%s:%s:%s\n", disk->name, file->sub, sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Move '%s%s' '%s%s'\n", disk->dir, file->sub, disk->dir, sub);
				}

				/* remove from the set */
				tommy_hashdyn_remove_existing(&disk->pathset, &file->pathset);

				/* save the new name */
				file_rename(file, sub);

				/* reinsert in the set */
				tommy_hashdyn_insert(&disk->pathset, &file->pathset, file, file_path_hash(file->sub));

				/* we have to save the new name */
				state->need_write = 1;
			} else if (file->inode != st->st_ino) {
				/* if the inode is different, it means a rewritten file with the same path */
				++scan->count_moved;

				if (state->gui) {
					fprintf(stdlog, "scan:move:%s:%s:%s\n", disk->name, file->sub, sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Move '%s%s' '%s%s'\n", disk->dir, file->sub, disk->dir, sub);
				}

				/* remove from the set */
				tommy_hashdyn_remove_existing(&disk->inodeset, &file->nodeset);

				/* save the new inode */
				file->inode = st->st_ino;

				/* reinsert in the set */
				tommy_hashdyn_insert(&disk->inodeset, &file->nodeset, file, file_inode_hash(file->inode));

				/* we have to save the new name */
				state->need_write = 1;
			} else {
				/* otherwise it's equal */
				++scan->count_equal;

				if (state->gui) {
					fprintf(stdlog, "scan:equal:%s:%s\n", disk->name, file->sub);
					fflush(stdlog);
				}
			}

			/* nothing more to do */
			return;
		} else {
			/* here if the file is changed */
		
			/* do a safety check to ensure that the common ext4 case of zeroing */
			/* the size of a file after a crash doesn't propagate to the backup */
			if (file->size != 0 && st->st_size == 0) {
				/* do the check ONLY if the name is the same */
				/* otherwise it could be a deleted and recreated file */
				if (strcmp(file->sub, sub) == 0) {
					if (!state->force_zero) {
						fprintf(stderr, "The file '%s%s' has unexpected zero size! If this an expected state\n", disk->dir, sub);
						fprintf(stderr, "you can '%s' anyway usinge 'snapraid --force-zero %s'\n", state->command, state->command);
						fprintf(stderr, "Instead, it's possible that after a kernel crash this file was lost,\n");
						fprintf(stderr, "and you can use 'snapraid --filter %s fix' to recover it.\n", sub);
						exit(EXIT_FAILURE);
					}
				}
			}

			if (strcmp(file->sub, sub) == 0) {
				/* if the name is the same, it's an update */
				if (state->gui) {
					fprintf(stdlog, "scan:update:%s:%s\n", disk->name, file->sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Update '%s%s'\n", disk->dir, file->sub);
				}

				++scan->count_change;
			} else {
				/* if the name is different, it's an inode reuse */
				if (state->gui) {
					fprintf(stdlog, "scan:remove:%s:%s\n", disk->name, file->sub);
					fprintf(stdlog, "scan:add:%s:%s\n", disk->name, sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Remove '%s%s'\n", disk->dir, file->sub);
					printf("Add '%s%s'\n", disk->dir, sub);
				}

				++scan->count_remove;
				++scan->count_insert;
			}

			/* remove it */
			scan_file_remove(state, disk, file);

			/* and continue to reinsert it */
		}
	} else {
		/* create the new file */
		++scan->count_insert;

		if (state->gui) {
			fprintf(stdlog, "scan:add:%s:%s\n", disk->name, sub);
			fflush(stdlog);
		}
		if (output) {
			printf("Add '%s%s'\n", disk->dir, sub);
		}

		/* and continue to insert it */
	}

	/* insert it */
	file = file_alloc(state->block_size, sub, st->st_size, st->st_mtime, STAT_NSEC(st), st->st_ino);

	/* mark it as present */
	file_flag_set(file, FILE_IS_PRESENT);

	/* insert the file in the file hashtables, to allow to find duplicate hardlinks */
	tommy_hashdyn_insert(&disk->inodeset, &file->nodeset, file, file_inode_hash(file->inode));
	tommy_hashdyn_insert(&disk->pathset, &file->pathset, file, file_path_hash(file->sub));

	/* insert the file in the delayed block allocation */
	tommy_list_insert_tail(&scan->file_insert_list, &file->nodelist, file);
}

/**
 * Removes the specified dir from the data set.
 */
static void scan_emptydir_remove(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_dir* dir)
{
	/* state changed */
	state->need_write = 1;

	/* remove the file from the dir containers */
	tommy_hashdyn_remove_existing(&disk->dirset, &dir->nodeset);
	tommy_list_remove_existing(&disk->dirlist, &dir->nodelist);

	/* deallocate */
	dir_free(dir);
}

/**
 * Inserts the specified dir in the data set.
 */
static void scan_emptydir_insert(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_dir* dir)
{
	/* state changed */
	state->need_write = 1;

	/* insert the dir in the dir containers */
	tommy_hashdyn_insert(&disk->dirset, &dir->nodeset, dir, dir_name_hash(dir->sub));
	tommy_list_insert_tail(&disk->dirlist, &dir->nodelist, dir);
}

/**
 * Processes a dir.
 */
static void scan_emptydir(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* sub)
{
	struct snapraid_dir* dir;

	/* check if the dir already exists */
	dir = tommy_hashdyn_search(&disk->dirset, dir_name_compare, sub, dir_name_hash(sub));
	if (dir) {
		/* check if multiple files have the same name */
		if (dir_flag_has(dir, FILE_IS_PRESENT)) {
			fprintf(stderr, "Internal inconsistency for dir '%s%s'\n", disk->dir, sub);
			exit(EXIT_FAILURE);
		}

		/* mark as present */
		dir_flag_set(dir, FILE_IS_PRESENT);

		/* it's equal */
		++scan->count_equal;

		if (state->gui) {
			fprintf(stdlog, "scan:equal:%s:%s\n", disk->name, dir->sub);
			fflush(stdlog);
		}

		/* nothing more to do */
		return;
	} else {
		/* create the new dir */
		++scan->count_insert;

		if (state->gui) {
			fprintf(stdlog, "scan:add:%s:%s\n", disk->name, sub);
			fflush(stdlog);
		}
		if (output) {
			printf("Add '%s%s'\n", disk->dir, sub);
		}

		/* and continue to insert it */
	}

	/* insert it */
	dir = dir_alloc(sub);

	/* mark it as present */
	dir_flag_set(dir, FILE_IS_PRESENT);

	/* insert it in the delayed insert list */
	tommy_list_insert_tail(&scan->dir_insert_list, &dir->nodelist, dir);
}

/**
 * Processes a directory.
 * Return != 0 if at least one file or link is processed.
 */
static int scan_dir(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* dir, const char* sub)
{
	int processed = 0;
	DIR* d;

	d = opendir(dir);
	if (!d) {
		fprintf(stderr, "Error opening directory '%s'. %s.\n", dir, strerror(errno));
		fprintf(stderr, "You can exclude it in the config file with:\n\texclude /%s\n", sub);
		exit(EXIT_FAILURE);
	}
   
	while (1) { 
		char path_next[PATH_MAX];
		char sub_next[PATH_MAX];
		struct stat st;
		const char* name;
		struct dirent* dd;

		/* clear errno to detect erroneous conditions */
		errno = 0;
		dd = readdir(d);
		if (dd == 0 && errno != 0) {
			fprintf(stderr, "Error reading directory '%s'. %s.\n", dir, strerror(errno));
			fprintf(stderr, "You can exclude it in the config file with:\n\texclude /%s\n", sub);
			exit(EXIT_FAILURE);
		}
		if (dd == 0) {
			break; /* finished */
		}

		/* skip "." and ".." files */
		name = dd->d_name;
		if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
			continue;

		pathprint(path_next, sizeof(path_next), "%s%s", dir, name);
		pathprint(sub_next, sizeof(sub_next), "%s%s", sub, name);

		/* check for not supported file names, limitation derived from the content file format */
		if (name[0] == 0 || strchr(name, '\n') != 0 || name[strlen(name)-1] == '\r') {
			fprintf(stderr, "Unsupported name '%s' in file '%s'.\n", name, path_next);
			exit(EXIT_FAILURE);
		}

		/* exclude hidden files even beforer calling lstat() */
		if (filter_hidden(state->filter_hidden, dd) != 0) {
			if (state->verbose) {
				printf("Excluding hidden '%s'\n", path_next);
			}
			continue;
		}

		/* exclude content files even before calling lstat() */
		if (filter_content(&state->contentlist, path_next) != 0) {
			if (state->verbose) {
				printf("Excluding content '%s'\n", path_next);
			}
			continue;
		}

#if HAVE_DIRENT_LSTAT
		/* convert dirent to lstat result */
		dirent_lstat(dd, &st);
#else
		/* get lstat info about the file */
		if (lstat(path_next, &st) != 0) {
			fprintf(stderr, "Error in stat file/directory '%s'. %s.\n", path_next, strerror(errno));
			exit(EXIT_FAILURE);
		}
#endif

		if (S_ISREG(st.st_mode)) {
			if (filter_path(&state->filterlist, disk->name, sub_next) == 0) {
#if HAVE_LSTAT_EX
				/* get inode info about the file, Windows needs an additional step */
				/* also for hardlink, the real size of the file is read here */
				if (lstat_ex(path_next, &st) != 0) {
					fprintf(stderr, "Error in stat_inode file '%s'. %s.\n", path_next, strerror(errno));
					exit(EXIT_FAILURE);
				}
#endif

				scan_file(scan, state, output, disk, sub_next, &st);
				processed = 1;
			} else {
				if (state->verbose) {
					printf("Excluding file '%s'\n", path_next);
				}
			}
		} else if (S_ISLNK(st.st_mode)) {
			if (filter_path(&state->filterlist, disk->name, sub_next) == 0) {
				char subnew[PATH_MAX];
				int ret;

				ret = readlink(path_next, subnew, sizeof(subnew));
				if (ret >= PATH_MAX) {
					fprintf(stderr, "Error in readlink file '%s'. Symlink too long.\n", path_next);
					exit(EXIT_FAILURE);
				}
				if (ret < 0) {
					fprintf(stderr, "Error in readlink file '%s'. %s.\n", path_next, strerror(errno));
					exit(EXIT_FAILURE);
				}

				/* readlink doesn't put the final 0 */
				subnew[ret] = 0;

				/* process as a symbolic link */
				scan_link(scan, state, output, disk, sub_next, subnew, FILE_IS_SYMLINK);
				processed = 1;
			} else {
				if (state->verbose) {
					printf("Excluding link '%s'\n", path_next);
				}
			}
		} else if (S_ISDIR(st.st_mode)) {
			if (filter_dir(&state->filterlist, disk->name, sub_next) == 0) {
				char sub_dir[PATH_MAX];

				/* recurse */
				pathslash(path_next, sizeof(path_next));
				pathcpy(sub_dir, sizeof(sub_dir), sub_next);
				pathslash(sub_dir, sizeof(sub_dir));
				if (scan_dir(scan, state, output, disk, path_next, sub_dir) == 0) {
					/* scan the directory as empty dir */
					scan_emptydir(scan, state, output, disk, sub_next);
				}
				/* or we processed something internally, or we have added the empty dir */
				processed = 1;
			} else {
				if (state->verbose) {
					printf("Excluding directory '%s'\n", path_next);
				}
			}
		} else {
			if (filter_path(&state->filterlist, disk->name, sub_next) == 0) {
				fprintf(stderr, "warning: Ignoring special '%s' file '%s'\n", stat_desc(&st), path_next);
			} else {
				if (state->verbose) {
					printf("Excluding special '%s' file '%s'\n", stat_desc(&st), path_next);
				}
			}
		}
	}

	if (closedir(d) != 0) {
		fprintf(stderr, "Error closing directory '%s'. %s.\n", dir, strerror(errno));
		exit(EXIT_FAILURE);
	}

	return processed;
}

void state_scan(struct snapraid_state* state, int output)
{
	tommy_node* i;
	tommy_node* j;
	tommy_list scanlist;

	tommy_list_init(&scanlist);

	for(i=state->disklist;i!=0;i=i->next) {
		struct snapraid_disk* disk = i->data;
		struct snapraid_scan* scan;
		tommy_node* node;

		scan = malloc_nofail(sizeof(struct snapraid_scan));
		scan->count_equal = 0;
		scan->count_moved = 0;
		scan->count_change = 0;
		scan->count_remove = 0;
		scan->count_insert = 0;
		tommy_list_init(&scan->file_insert_list);
		tommy_list_init(&scan->link_insert_list);
		tommy_list_init(&scan->dir_insert_list);

		tommy_list_insert_tail(&scanlist, &scan->node, scan);

		printf("Scanning disk %s...\n", disk->name);

		scan_dir(scan, state, output, disk, disk->dir, "");

		/* check for removed files */
		node = disk->filelist;
		while (node) {
			struct snapraid_file* file = node->data;

			/* next node */
			node = node->next;

			/* remove if not present */
			if (!file_flag_has(file, FILE_IS_PRESENT)) {
				++scan->count_remove;

				if (state->gui) {
					fprintf(stdlog, "scan:remove:%s:%s\n", disk->name, file->sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Remove '%s%s'\n", disk->dir, file->sub);
				}

				scan_file_remove(state, disk, file);
			}
		}

		/* check for removed links */
		node = disk->linklist;
		while (node) {
			struct snapraid_link* link = node->data;

			/* next node */
			node = node->next;

			/* remove if not present */
			if (!link_flag_has(link, FILE_IS_PRESENT)) {
				++scan->count_remove;

				if (state->gui) {
					fprintf(stdlog, "scan:remove:%s:%s\n", disk->name, link->sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Remove '%s%s'\n", disk->dir, link->sub);
				}

				scan_link_remove(state, disk, link);
			}
		}

		/* check for removed dirs */
		node = disk->dirlist;
		while (node) {
			struct snapraid_dir* dir = node->data;

			/* next node */
			node = node->next;

			/* remove if not present */
			if (!dir_flag_has(dir, FILE_IS_PRESENT)) {
				++scan->count_remove;

				if (state->gui) {
					fprintf(stdlog, "scan:remove:%s:%s\n", disk->name, dir->sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Remove '%s%s'\n", disk->dir, dir->sub);
				}

				scan_emptydir_remove(state, disk, dir);
			}
		}

		/* insert all the new files, we insert them only after the deletion */
		/* to reuse the just freed space */
		node = scan->file_insert_list;
		while (node) {
			struct snapraid_file* file = node->data;

			/* next node */
			node = node->next;

			/* insert it */
			scan_file_insert(state, disk, file);
		}

		/* insert all the new links */
		node = scan->link_insert_list;
		while (node) {
			struct snapraid_link* link = node->data;

			/* next node */
			node = node->next;

			/* insert it */
			scan_link_insert(state, disk, link);
		}

		/* insert all the new dirs */
		node = scan->dir_insert_list;
		while (node) {
			struct snapraid_dir* dir = node->data;

			/* next node */
			node = node->next;

			/* insert it */
			scan_emptydir_insert(state, disk, dir);
		}
	}

	/* checks for disks where all the previously existing files where removed */
	if (!state->force_empty) {
		int has_empty = 0;
		for(i=state->disklist,j=scanlist;i!=0;i=i->next,j=j->next) {
			struct snapraid_disk* disk = i->data;
			struct snapraid_scan* scan = j->data;

			if (scan->count_equal == 0 && scan->count_moved == 0 && scan->count_remove != 0) {
				if (!has_empty) {
					has_empty = 1;
					fprintf(stderr, "All the files previously present in disk '%s' at dir '%s'", disk->name, disk->dir);
				} else {
					fprintf(stderr, ", disk '%s' at dir '%s'", disk->name, disk->dir);
				}
			}
		}
		if (has_empty) {
			fprintf(stderr, " are now missing or rewritten!\n");
			fprintf(stderr, "This happens when deleting all the files from a disk,\n");
			fprintf(stderr, "or when all the files are recreated after a 'fix' command,\n");
			fprintf(stderr, "or manually copied. If this is really what you are doing, \n");
			fprintf(stderr, "you can '%s' anyway, using 'snapraid --force-empty %s'.\n", state->command, state->command);
			fprintf(stderr, "Instead, it's possible that you have some disks not mounted.\n");
			exit(EXIT_FAILURE);
		}
	}

	if (state->verbose || output) {
		struct snapraid_scan total;

		total.count_equal = 0;
		total.count_moved = 0;
		total.count_change = 0;
		total.count_remove = 0;
		total.count_insert = 0;

		for(i=scanlist;i!=0;i=i->next) {
			struct snapraid_scan* scan = i->data;
			total.count_equal += scan->count_equal;
			total.count_moved += scan->count_moved;
			total.count_change += scan->count_change;
			total.count_remove += scan->count_remove;
			total.count_insert += scan->count_insert;
		}

		if (state->verbose) {
			printf("\tequal %d\n", total.count_equal);
			printf("\tmoved %d\n", total.count_moved);
			printf("\tchanged %d\n", total.count_change);
			printf("\tremoved %d\n", total.count_remove);
			printf("\tadded %d\n", total.count_insert);
		}

		if (output) {
			if (!total.count_moved && !total.count_change && !total.count_remove && !total.count_insert) {
				printf("No difference.\n");
			}
		}
	}

	tommy_list_foreach(&scanlist, (tommy_foreach_func*)free);

	printf("Using %u MiB of memory.\n", (unsigned)(malloc_counter() / 1024 / 1024));
}

