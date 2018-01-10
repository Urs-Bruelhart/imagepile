/*
 * Image drop data pile management
 * Copyright (C) 2014-2017 by Jody Bruchon <jody@jodybruchon.com>
 * See LICENSE for licensing information
 *
 * Background: We store a lot of Windows disk images. These images consist
 * largely of heavily duplicated data (the vast majority of file data is not
 * unique between images) resulting in hundreds of GB of wasted disk usage
 * on the image server. This program aims to combine image data at the
 * B_SIZE-byte block level into one large chunk of data and handle individual
 * image data as metadata files consisting of a simple header that handles
 * corner cases such as pre-Vista 4KB sector misalignment, followed by
 * a list of 4KB-sized offsets into the image data file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include "imagepile.h"
#include "jody_hash.h"

/* Detect Windows and modify as needed */
#if defined _WIN32 || defined __CYGWIN__
 #define ON_WINDOWS 1
 #define NO_SYMLINKS 1
 #define NO_PERMS 1
 #define NO_SIGACTION 1
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <io.h>
#endif

/* Statistics variables */
uint64_t stats_total_searches = 0;
uint64_t stats_hash_failures = 0;

/* Global hash prefix starting array */
struct hash_leaf *hash_top[65536];

#ifndef NO_SIGACTION

/* Signal stuff */
int siglock = 0;
int sigterm = 0;

/* Handle signals */
void sig_handler(const int signo)
{
	if (!siglock) {
		fprintf(stderr, "\n\nCaught signal %d, terminating\n", signo);
		exit(EXIT_FAILURE);
	} else sigterm = 1;
}

#endif /* NO_SIGACTION */

/* Find the next instance of a hash in the master hash table.
 * entry is used to resume search in case of a failed match
 * Returns offset to match or -1 if no match found */
static off_t find_hash_match(const jodyhash_t hash, const int reset)
{
	static struct hash_leaf * restrict leaf;
	static struct hash_node * restrict node;
	static unsigned int rel_offset;	/* Offset in current table */
	static int dirty = 1;	/* Always reset on first run */

	DLOG("find_hash_match: hash %016lx, r %d, d %d\n", hash, reset, dirty);

	/* Only use old values if a reset is not needed */
	if (reset || dirty) {
		/* Find the hash leaf using the first 16 bits as a key */
		leaf = hash_top[HASH_HEAD(hash)];
		rel_offset = 0;
		node = leaf->node;
		dirty = 0;
	} else {
		/* Advance to next node */
		if (rel_offset < leaf->entries) {
			rel_offset++;
			node++;
		} else if (leaf->next != NULL) {
			leaf = leaf->next;
			node = leaf->node;
			rel_offset = 0;
		} else goto not_found;
	}

	/* Scan through the hash tree until a match is found */
	while (1) {
		/* Scan one hash block */
		while (rel_offset < leaf->entries) {
			stats_total_searches++;
			if (hash == node->hash) return node->offset;
			/* Move to next hash node */
			rel_offset++;
			node++;
		}
		/* Advance to next leaf (or terminate) */
		if (leaf->next != NULL) {
			leaf = leaf->next;
			node = leaf->node;
			rel_offset = 0;
		} else goto not_found;
	}

not_found:
	dirty = 1;
	return -1;
}

/* Add hash to memory hash table (and optionally to hash index file) */
static int index_hash(const jodyhash_t hash, const off_t offset, const int write,
		const struct files_t * const restrict files)
{
	struct hash_leaf *leaf;
	struct hash_node *node;

	if (write) { DLOG("index_hash: %016lx (write)\n", hash); }
	else { DLOG("index_hash: %016lx\n", hash); }

	leaf = hash_top[HASH_HEAD(hash)];

	/* Scan hash leaves/nodes for next empty slot */
	while (1) {
		/* Skip through any leaves that are full */
		if (leaf->entries == HASH_ALLOC_SIZE) {
			if (leaf->next != NULL) {
				leaf = leaf->next;
				continue;
			} else {
				/* Allocate and initialize new leaves as needed */
				leaf->next = (struct hash_leaf *)malloc(sizeof(struct hash_leaf));
				if (leaf->next == NULL) goto oom;
				leaf = leaf->next;
				leaf->entries = 0;
			}
		}
		/* Insert value into next available node in this leaf */
		node = leaf->node + leaf->entries;
		node->hash = hash;
		node->offset = offset;
		leaf->entries++;
		break;
	}

	/* Write hash to database if requested */
	if (write) {
		size_t i;
		i = fwrite(&hash, sizeof(jodyhash_t), 1, files->hashindex);
		if (!i) {
			fprintf(stderr, "Error: short write to hash index\n");
			exit(EXIT_FAILURE);
		}
	}
	return 0;
oom:
	fprintf(stderr, "Error: out of memory\n");
	exit(EXIT_FAILURE);
}

/* Read a block from the block database
 * This is written as a separate function so that it may be enhanced
 * with inline compression functionality later */
static int read_db_block(void * const restrict blk,
		const off_t offset, const struct files_t * const restrict files)
{
	int i;
	size_t j;

	DLOG("read_db_block, offset %d\n", offset);
	fflush(files->db);
	i = fseeko(files->db, (off_t)(B_SIZE * (off_t)offset), SEEK_SET);
       	if (i < 0) {
		fprintf(stderr, "Error: cannot seek to block %jd in database.\n", (intmax_t)offset);
		exit(EXIT_FAILURE);
	}
	j = fread(blk, 1, B_SIZE, files->db);
	if (j != B_SIZE) {
		fprintf(stderr, "Error: cannot read block %jd in database (%jd read).\n", (intmax_t)offset, (intmax_t)j);
		exit(EXIT_FAILURE);
	}
	return 0;
}

/* Compare an input block against a block in the block database */
static int compare_blocks(const void *blk1, const off_t offset,
		const struct files_t * const restrict files)
{
	int blk2[B_SIZE / sizeof(int)];
	const int * const check1 = (const int *)blk1;
	const int * const check2 = blk2;

	DLOG("compare_blocks, offset %d\n", offset);

	/* Make sure no one passes us a negative offset, grr */
	if (offset < 0) return -1;

	read_db_block(blk2, offset, files);

	/* Compare first machine word before calling memcmp */
	if (*check1 != *check2) return -1;

	/* Compare the entire block */
	if (!memcmp(blk1, blk2, B_SIZE)) return 0;

	return -1;
}

/* Append a block to the block db, returning offset in B_SIZE blocks */
/* This may be enhanced with compression functionality later */
static int add_db_block(const void * const restrict blk,
		const struct files_t * const restrict files)
{
	int offset;
	size_t i;

	/* Seek to end and record the B_SIZE offset for later */
	DLOG("add_db_block\n");
	fseeko(files->db, 0, SEEK_END);
	offset = (int)(ftello(files->db) / B_SIZE);

	DLOG("DB seek to %ld\n", ftello(files->db));

	i = fwrite(blk, 1, B_SIZE, files->db);
	if (i != B_SIZE) goto error_write;
	if (ferror(files->db)) goto error_write;

	return offset;

error_write:
	fprintf(stderr, "Error: write to block DB failed: %ju of %d written\n", (uintmax_t)i, B_SIZE);
	exit(EXIT_FAILURE);
}

/* Add an incoming block to (or find in) the databse; return its offset */
static uint32_t get_block_offset(const void * const restrict blk,
		const struct files_t * const restrict files)
{
	jodyhash_t hash;
	off_t offset = 0;
	int reset = 1;

	DLOG("get_block_offset\n");
	hash = jody_block_hash((const jodyhash_t *)blk, 0, B_SIZE);

	/* Search existing hashes for a match until they are exhausted */
	while (1) {
		offset = find_hash_match(hash, reset);
		reset = 0;
		DLOG("get_block_offset: find_hash_match returned %d\n", offset);
		if (offset < 0) break;
		if (!compare_blocks(blk, offset, files)) return (uint32_t)offset;
		DLOG("Compare blocks FAILED, offset %d\n", offset);
		stats_hash_failures++;
	}

	/* Hash not found in the hash list, so add it to the database */
#ifndef NO_SIGACTION
	siglock = 1;
#endif
	offset = add_db_block(blk, files);
	/* ...and add it to the hash index */
	index_hash(hash, offset, 1, files);
	DLOG("Indexed new hash at offset %d\n", offset);

#ifndef NO_SIGACTION
	/* If a terminating signal was sent, stop immediately */
	siglock = 0;
	if (sigterm) {
		fflush(files->db);
		fflush(files->hashindex);
		fflush(files->out);
		exit(EXIT_FAILURE);
	}
#endif /* NO_SIGACTION */

	return (uint32_t)offset;
}

/* Add an image file to the image pile database */
static int input_image(const struct files_t * const restrict files,
		uint32_t start_offset)
{
	const uint32_t z = B_SIZE;
	char blk[B_SIZE];
	size_t cnt = 1;
	off_t size = 1, temp;

	DLOG("input_image\n");
	/* Output magic number and first/last sector offsets */
	fwrite("IPIL", 4, 1, files->out);
	fwrite(&start_offset, 4, 1, files->out);
	fwrite(&z, 4, 1, files->out);
	/* Set up status indicator */
	if (files->in != stdin) {
		temp = ftello(files->in);
		fseeko(files->in, 0, SEEK_END);
		size = ftello(files->in);
		fseeko(files->in, temp, SEEK_SET);
		size /= 100;	/* Get 1% value */
	} else fprintf(stderr, "Reading from stdin; progress display unavailable\n");

	/* Read entire input file and hash the blocks, padding if necessary */
	while (cnt > 0) {
		uint32_t offset;
		static off_t percent = 0;

		if (start_offset > 0) cnt = fread(blk, 1, (B_SIZE - start_offset), files->in);
		else cnt = fread(blk, 1, B_SIZE, files->in);
		DLOG("fread() got %ju bytes\n", (uintmax_t)cnt);
		if (ferror(files->in)) {
			fprintf(stderr, "Error reading %s\n", files->infile);
			exit(EXIT_FAILURE);
		}
		if (files->in != stdin) {
			temp = ftello(files->in);
			temp /= size;
			if (temp > percent) {
				fprintf(stderr, "\r%u%% complete (%jd hash fails) ",
					(unsigned int)temp, (intmax_t)stats_hash_failures);
				percent = temp;
			}
		}
		
		/* Stop processing if no data was read */
		if (cnt == 0) break;
		/* If read is not B_SIZE long, pad remaining data
		 * Some images have stray data at the end; we pad that data
		 * with zeroes and store it as a B_SIZE block. */
		if (cnt < B_SIZE) {
			DLOG("Stopping: read count %ju < %d\n", (uintmax_t)cnt, B_SIZE);
			memset((blk + cnt), 0, (B_SIZE - cnt));
			/* Write size of final sector(s) and quit */
			if (feof(files->in)) {
				/* Output final offset */
				offset = get_block_offset(blk, files);
				fwrite(&offset, sizeof(offset), 1, files->out);
				/* Write last block size to disk */
				fseeko(files->out, 8, SEEK_SET);
				fwrite(&cnt, 4, 1, files->out);
				break;
			} else if (start_offset == 0) {
				fprintf(stderr, "\nError: short read (%ju/%d) but not start or end of image\n", (uintmax_t)cnt, B_SIZE);
				exit(EXIT_FAILURE);
			}
		}
		/* Handle any start_offset */
		start_offset = 0;
		offset = get_block_offset(blk, files);

		/* Output offset to image file */
		fwrite(&offset, sizeof(offset), 1, files->out);

		if (feof(files->in)) break;
	}

	if (files->in != stdin) fprintf(stderr, "\n");	/* Compensate for status indicator */
	return 0;
}

/* Read out an image file that was previously added to the image pile */
static int output_original(const struct files_t * const restrict files)
{
	size_t i;
	size_t w, written = 0;
	uint32_t start_offset, end_size;
	char blk[B_SIZE];
	char data[B_SIZE];
	uint32_t *p;
	off_t size = 1, temp;

	DLOG("output_original\n");
	/* Verify magic number at start of file */
	i = fread(blk, 1, HDR_SIZE, files->in);
	if (i != HDR_SIZE) goto error_in;
	if (strncmp(blk, "IPIL", 4)) {
		fprintf(stderr, "Error: bad magic number at start of %s\n", files->infile);
		exit(EXIT_FAILURE);
	}

	/* Set up status indicator */
	if (files->in != stdin) {
		temp = ftello(files->in);
		fseeko(files->in, 0, SEEK_END);
		size = ftello(files->in);
		DLOG("total size: %jd\n", (intmax_t)((B_SIZE / 4) * (size - HDR_SIZE)));
		fseeko(files->in, temp, SEEK_SET);
		size /= 100;	/* Get 1% value */
		if (size == 0) size = 1; /* To avoid division by zero later */
	} else fprintf(stderr, "Reading from stdin; progress display unavailable\n");

	/* Get offsets stored in the header */
	start_offset = *((uint32_t *)blk + 1);
	if (start_offset >= B_SIZE) goto error_start_offset;
	end_size = *((uint32_t *)blk + 2);
	if (end_size > B_SIZE) goto error_endsize;

	/* Read image file and write out original data */
	while((i = fread(blk, 4, (B_SIZE / 4), files->in))) {
		off_t offset;
		static off_t percent = 0;

		/* Iterate through block of offsets */
		if (ferror(files->in)) goto error_in;
		if (files->in != stdin) {
			temp = ftello(files->in);
			temp /= size;
			if (temp > percent) {
				fprintf(stderr, "\r%u%% complete", (unsigned int)temp);
				percent = temp;
			}
		}
		p = (uint32_t *)blk;
		/* TODO: Queue, reschedule, and merge reads to minimize seeking */
		while (i > 0) {
			/* Read the data block specified by the offset */
			offset = (off_t)(B_SIZE * (off_t)*p);
			DLOG("seeking to block %jd\n", offset);
			if (fseeko(files->db, offset, SEEK_SET)) goto error_db;
			DLOG("reading a block\n");
			if (!fread(data, B_SIZE, 1, files->db)) goto error_db;
			if (ferror(files->db)) goto error_db;

			/* Handle the last block */
			if ((i == 1) && feof(files->in)) {
				DLOG("writing final block of size %jd\n", (intmax_t)end_size);
				w = fwrite(data, 1, end_size, files->out);
				written += w;
				break;
			}
			/* Write data block, compensating for any offset */
			if (start_offset > 0) {
				w = fwrite(data, 1, (B_SIZE - start_offset), files->out);
				start_offset = 0;
			} else w = fwrite(data, 1, B_SIZE, files->out);
			if (ferror(files->out)) goto error_out;
			written += w;

			p++;
			i--;
		}
	}

	if (files->in != stdin) fprintf(stderr, "\n");	/* Compensate for status indicator */
	return 0;

error_in:
	fprintf(stderr, "Error reading %s\n", files->infile);
	exit(EXIT_FAILURE);
error_db:
	fprintf(stderr, "Error reading %s\n", files->dbfile);
	exit(EXIT_FAILURE);
error_out:
	fprintf(stderr, "Error writing %s\n", files->outfile);
	exit(EXIT_FAILURE);
error_start_offset:
	fprintf(stderr, "Error: input header start_offset %d >= block size %d\n",
			start_offset, B_SIZE);
	exit(EXIT_FAILURE);
error_endsize:
	fprintf(stderr, "Error: input header end_size %d > block size %d\n",
			start_offset, B_SIZE);
	exit(EXIT_FAILURE);
}


int main(int argc, char **argv)
{
	struct files_t file_vars;
	struct files_t * const restrict files = &file_vars;
	char blk[B_SIZE];
	char path[PATH_MAX];
	size_t i;
	char *p;
	int hashcount = 0;
	struct hash_leaf **start;
	struct hash_leaf *leaf;
	uint32_t start_offset = 0;
	size_t offset = 0;
#ifndef NO_SIGACTION
	struct sigaction act;
#endif

	fprintf(stderr, "Imagepile disk image database utility %s (%s)\n", VER, VERDATE);
	/* Handle arguments */
	if (argc < 4) goto usage;
	if ((p = getenv("IMGDIR"))) {
		strncpy(path, p, PATH_MAX);
		strncat(path, "/imagepile.db", PATH_MAX);
		strncpy(files->dbfile, path, PATH_MAX);
		strncpy(path, p, PATH_MAX);
		strncat(path, "/imagepile.hash_index", PATH_MAX);
		strncpy(files->indexfile, path, PATH_MAX);
	} else {
		fprintf(stderr, "Error: IMGDIR environment variable not set\n");
		exit(EXIT_FAILURE);
	}
	strncpy(files->infile, argv[argc - 2], PATH_MAX);
	strncpy(files->outfile, argv[argc - 1], PATH_MAX);

	DLOG("Using: db %s, idx %s,\nin %s, out %s\n",
			files->dbfile, files->indexfile,
			files->infile, files->outfile);

	if (!strncmp(files->infile, files->outfile, PATH_MAX)) {
		fprintf(stderr, "Input and output files must be different. Aborting.\n");
		exit(EXIT_FAILURE);
	}

#ifndef NO_SIGACTION
	/* Set up signal handler */
	memset (&act, 0, sizeof(act));
	act.sa_handler = sig_handler;
	if (sigaction(SIGINT, &act, 0)) goto signal_error;
	if (sigaction(SIGTERM, &act, 0)) goto signal_error;
	if (sigaction(SIGABRT, &act, 0)) goto signal_error;
	if (sigaction(SIGHUP, &act, 0)) goto signal_error;
#endif /* NO_SIGACTION */

	/* Open master block database */
	if (!(files->db = fopen(files->dbfile, "a+b"))) {
		fprintf(stderr, "Error: cannot open DB: %s\n", files->dbfile);
		exit(EXIT_FAILURE);
	}

	/* Open input file */
	if (!strncmp(files->infile, "-", PATH_MAX)) {
		/* fprintf(stderr, "Reading from stdin\n"); */
#ifdef ON_WINDOWS
		_setmode(_fileno(stdin), _O_BINARY);
#endif
		files->in = stdin;
	} else if (!(files->in = fopen(files->infile, "rb"))) {
		fprintf(stderr, "Error: cannot open infile: %s\n", files->infile);
		exit(EXIT_FAILURE);
	}

	/* Open output file for writing */
	if (!strncmp(files->outfile, "-", PATH_MAX)) {
		fprintf(stderr, "Writing to stdout\n");
		files->out = stdout;
	} else if (!(files->out = fopen(files->outfile, "wb"))) {
		fprintf(stderr, "Error: cannot open outfile: %s\n", files->outfile);
		exit(EXIT_FAILURE);
	}

	if (!strncmp(argv[1], "add", PATH_MAX)) {
		/* Add an image file to the database */
		if (argc > 4) {
			char *check;
			errno = 0;
			start_offset = (uint32_t)strtol(argv[argc - 3], &check, 10);
			if (errno || (check == argv[argc - 3])) goto usage;
			if (start_offset >= B_SIZE) goto usage;
		}

		/* Initialize hash leaves */
		leaf = (struct hash_leaf *)malloc(sizeof(struct hash_leaf) * 65536);
		if (!leaf) goto oom;
		start = &hash_top[0];
		for (i=0; i < 65536; i++) {
			*start = leaf;
			leaf->entries = 0;
			leaf->next = NULL;
			start++;
			leaf++;
		}

		/* Open DB hash index and read it in */
		if (!(files->hashindex = fopen(files->indexfile, "a+b"))) {
			fprintf(stderr, "Error: cannot open index: %s\n", files->indexfile);
			exit(EXIT_FAILURE);
		}
		while ((i = fread(blk, sizeof(jodyhash_t), (B_SIZE / sizeof(jodyhash_t)), files->hashindex))) {
			if (ferror(files->hashindex)) {
				fprintf(stderr, "Error: can't read index: %s\n", files->indexfile);
				exit(EXIT_FAILURE);
			}
			/* Add each B_SIZE wide block of hashes to the in-memory hash index */
			offset = 0;
			while (offset < i) {
				index_hash((jodyhash_t)*((jodyhash_t *)blk + offset), hashcount, 0, files);
				offset++;
				hashcount++;
			}
		}
		fprintf(stderr, "Read in %d hashes from hash index\n", hashcount);
	
		input_image(files, start_offset);
		fflush(files->hashindex);
		fclose(files->hashindex);
		/* Output final statistics */
		fprintf(stderr, "Stats: %ju total searches, %ju hash failures\n",
			(intmax_t)stats_total_searches,
			(intmax_t)stats_hash_failures);
	} else if (!strncmp(argv[1], "read", PATH_MAX)) {
		/* Read an image from the databse */
		output_original(files);
	} else goto usage;

	fflush(files->db);
	fflush(files->in);
	fflush(files->out);

	fclose(files->in);
	fclose(files->out);
	fclose(files->db);

	exit(EXIT_SUCCESS);

oom:
	/* If any malloc() fails, end up here. */
	fprintf(stderr, "Error: out of memory\n");
	exit(EXIT_FAILURE);

#ifndef NO_SIGACTION
signal_error:
	fprintf(stderr, "Cannot catch signals, aborting.\n");
	exit(EXIT_FAILURE);
#endif /* NO_SIGACTION */

usage:
	fprintf(stderr, "\nSpecify a verb and file (use - for stdin/stdout). List of verbs:\n\n");
	fprintf(stderr, "   add <offset> input_file image_file  - Add to database, produce image_file\n");
	fprintf(stderr, "         ^-- offset in bytes to shorten the first block (DOS/2K/XP compat)\n\n");
	fprintf(stderr, "   read image_file output_file - Read original data for image_file\n\n");
	fprintf(stderr, "The IMGDIR environment variable determines where the image pile is located\n\n");
	exit(EXIT_FAILURE);
}
