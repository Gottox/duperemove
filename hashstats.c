/*
 * hashstats.c
 *
 * Copyright (C) 2014 SUSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>

#include "rbtree.h"
#include "list.h"
#include "csum.h"
#include "filerec.h"
#include "hash-tree.h"
#include "util.h"
#include "serialize.h"
#include "btrfs-util.h"

#include "bswap.h"

int verbose = 0, debug = 0;
unsigned int blocksize;
static int version_only = 0;
static int print_all_hashes = 0;
static int print_blocks = 0;
static int num_to_print = 10;
static int show_subvol_info = 0;
static int print_file_list = 0;
static char *serialize_fname = NULL;
static struct rb_root by_size = RB_ROOT;

static int cmp(struct dupe_blocks_list *tmp, struct dupe_blocks_list *dups)
{
	if (tmp->dl_num_elem < dups->dl_num_elem)
		return -1;
	else if (tmp->dl_num_elem > dups->dl_num_elem)
		return 1;
	return memcmp(dups->dl_hash, tmp->dl_hash, digest_len);
}

static void insert_by_size(struct dupe_blocks_list *dups)
{
	struct rb_node **p = &by_size.rb_node;
	struct rb_node *parent = NULL;
	struct dupe_blocks_list *tmp;
	int ret;

	while (*p) {
		parent = *p;

		tmp = rb_entry(parent, struct dupe_blocks_list, dl_by_size);

		ret = cmp(tmp, dups);
		if (ret < 0)
			p = &(*p)->rb_left;
		else if (ret > 0)
			p = &(*p)->rb_right;
		else
			break;
	}

	rb_link_node(&dups->dl_by_size, parent, p);
	rb_insert_color(&dups->dl_by_size, &by_size);
}

static void sort_by_size(struct hash_tree *tree)
{
	struct rb_root *root = &tree->root;
	struct rb_node *node = rb_first(root);
	struct dupe_blocks_list *dups;

	while (1) {
		if (node == NULL)
			break;

		dups = rb_entry(node, struct dupe_blocks_list, dl_node);

		insert_by_size(dups);

		node = rb_next(node);
	}
}

static void printf_file_block_flags(struct file_block *block)
{
	if (!block->b_flags)
		return;

	printf("( ");
	if (block->b_flags & FILE_BLOCK_SKIP_COMPARE)
		printf("skip_compare ");
	if (block->b_flags & FILE_BLOCK_DEDUPED)
		printf("deduped ");
	if (block->b_flags & FILE_BLOCK_HOLE)
		printf("hole ");
	printf(")");
}

static void print_by_size(void)
{
	struct rb_node *node = rb_first(&by_size);
	struct dupe_blocks_list *dups;
	struct file_block *block;

	if (print_all_hashes)
		printf("Print all hashes\n");
	else
		printf("Print top %d hashes\n", num_to_print);

	printf("Hash, # Blocks, # Files\n");

	while (1) {
		if (node == NULL)
			break;

		dups = rb_entry(node, struct dupe_blocks_list, dl_by_size);

		debug_print_digest(stdout, dups->dl_hash);
		printf(", %u, %u\n", dups->dl_num_elem, dups->dl_num_files);
		if (print_blocks) {
			list_for_each_entry(block, &dups->dl_list,
					    b_list) {
				struct filerec *f = block->b_file;
				printf("  %s\tloff: %llu lblock: %llu "
				       "flags: 0x%x ", f->filename,
				       (unsigned long long)block->b_loff,
				       (unsigned long long)block->b_loff / blocksize,
				       block->b_flags);
				printf_file_block_flags(block);
				printf("\n");
			}
		}

		if (!print_all_hashes && --num_to_print == 0)
			break;

		node = rb_next(node);
	}
}

static void print_filerecs(void)
{
	struct filerec *file;

	printf("Showing %llu files.\nInode\tBlocks Stored\tSubvold ID\tFilename\n",
		num_filerecs);

	list_for_each_entry(file, &filerec_list, rec_list) {
		printf("%"PRIu64"\t%"PRIu64"\t%"PRIu64"\t%s\n", file->inum,
		       file->num_blocks, file->subvolid, file->filename);
	}
}

static void print_subvol_info(void)
{
	struct rb_node *n = rb_first(&subvols_by_id);
	struct btrfs_subvol *subvol;

	if (num_subvols)
		printf("Showing %u subvolumes\nSubvol ID\tSubvol Gen\tUUID\tPath\n",
		       num_subvols);

	while (n) {
		subvol = rb_entry(n, struct btrfs_subvol, subvol_node);

		printf("%"PRIu64"\t%"PRIu64"\t%.*s\t%s\n", subvol->subvol_id,
		       subvol->subvol_gen, UUID_SIZE, subvol->subvol_uuid,
			subvol->subvol_path);

		n = rb_next(n);
	}
}

static void print_file_info(struct hash_tree *tree,
			    struct hash_file_header *h)
{
	printf("Raw header info for \"%s\":\n", serialize_fname);
	printf("  version: %"PRIu64".%"PRIu64"\tblock_size: %u\n",
	       le64_to_cpu(h->major), le64_to_cpu(h->minor),
	       le32_to_cpu(h->block_size));
	printf("  num_files: %"PRIu64"\tnum_hashes: %"PRIu64"\n",
	       le64_to_cpu(h->num_files), le64_to_cpu(h->num_hashes));
	printf("  num_subvol_info: %u\tsubvol_info_off: %"PRIu64"\n",
	       le32_to_cpu(h->num_subvol_info),
	       le64_to_cpu(h->subvol_info_off));
	printf("Loaded hashes from %"PRIu64" blocks into %"PRIu64" nodes\n",
	       tree->num_blocks, tree->num_hashes);
	printf("Loaded %llu file records\n", num_filerecs);
}

static void usage(const char *prog)
{
	printf("hashstats %s\n", VERSTRING);
	if (version_only)
		return;

	printf("Print information about duperemove hashes.\n\n");
	printf("Usage: %s [-n NUM] [-a] [-b] [-l] hashfile\n", prog);
	printf("Where \"hashfile\" is a file generated by running duperemove\n");
	printf("with the '--write-hashes' option. By default a list of hashes\n");
	printf("with the most shared blocks are printed.\n");
	printf("\n\t<switches>\n");
	printf("\t-n NUM\t\tPrint top N hashes, sorted by bucket size.\n");
	printf("\t      \t\tDefault is 10.\n");
	printf("\t-a\t\tPrint all hashes (overrides '-n', above)\n");
	printf("\t-b\t\tPrint info on each block within our hash buckets\n");
	printf("\t-l\t\tPrint a list of all files\n");
	printf("\t-s\t\tPrint the subvolume info\n");
	printf("\t--help\t\tPrints this help text.\n");
}

enum {
	HELP_OPTION = CHAR_MAX + 1,
	VERSION_OPTION,
	HASH_OPTION,
};

static int parse_options(int argc, char **argv)
{
	int c;
	static struct option long_ops[] = {
		{ "help", 0, 0, HELP_OPTION },
		{ "version", 0, 0, VERSION_OPTION },
		{ 0, 0, 0, 0}
	};

	if (argc < 2)
		return 1;

	while ((c = getopt_long(argc, argv, "labn:s?", long_ops, NULL))
	       != -1) {
		switch (c) {
		case 'l':
			print_file_list = 1;
			break;
		case 'a':
			print_all_hashes = 1;
			break;
		case 'b':
			print_blocks = 1;
			break;
		case 'n':
			num_to_print = atoi(optarg);
			break;
		case 's':
			show_subvol_info = 1;
			break;
		case VERSION_OPTION:
			version_only = 1;
			return 1;
		case HELP_OPTION:
		case '?':
		default:
			version_only = 0;
			return 1;
		}
	}

	if ((argc - optind) != 1)
		return 1;

	serialize_fname = argv[optind];

	return 0;
}

int main(int argc, char **argv)
{
	int ret;
	struct hash_tree tree;
	struct hash_file_header h;

	init_filerec();
	init_hash_tree(&tree);

	if (parse_options(argc, argv)) {
		usage(argv[0]);
		return EINVAL;
	}

	if (init_csum_module(DEFAULT_HASH_STR))
		return ENOMEM;

	ret = read_hash_tree(serialize_fname, &tree, &blocksize, &h, 0, NULL, 0);
	if (ret) {
		print_hash_tree_errcode(stderr, serialize_fname, ret);
		return ret;
	}

	print_file_info(&tree, &h);

	if (show_subvol_info)
		print_subvol_info();

	if (num_to_print || print_all_hashes) {
		sort_by_size(&tree);
		print_by_size();
	}

	if (print_file_list)
		print_filerecs();

	return ret;
}
