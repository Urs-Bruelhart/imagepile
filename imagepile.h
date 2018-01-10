/* Image drop data pile headers
 * See imagepile.c for copyright information */

#ifndef IMAGEPILE_H
#define IMAGEPILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include "jody_hash.h"

#define VER "0.1"
#define VERDATE "2015-04-13"
/* #define DEBUG 1 */

#ifdef DEBUG
#define DLOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DLOG(...)
#endif

/*
 * Size of IPIL file header in bytes
 * 0-3:  'IPIL' signature
 * 4-7:  Truncate first block size (bytes)
 * 8-11: Last block total size (bytes)
 */
#define HDR_SIZE 12

/* Universal disk block size for the entire program
 * DO NOT CHANGE UNLESS YOU KNOW WHAT YOU ARE DOING! */
#define B_SIZE 4096

/* How many hash table entries to allocate at once */
#define HASH_ALLOC_SIZE 64

/* Hash type and macro definitions */
typedef uint16_t hash_head_t;

/* Extract hash_head_t from a jodyhash_t */
#define HASH_HEAD(x) (hash_head_t)(x >> ((sizeof(jodyhash_t) - sizeof(hash_head_t)) * 8))

/* Master block database */
struct files_t {
	char dbfile[PATH_MAX];
	FILE * restrict db;
	char indexfile[PATH_MAX];
	FILE * restrict hashindex;
	char infile[PATH_MAX];
	FILE * restrict in;
	char outfile[PATH_MAX];
	FILE * restrict out;
};

/* Hash leaf table entries */
struct hash_node {
	jodyhash_t hash;	/* Final hash */
	off_t offset;	/* Offset (in B_SIZE blocks) into master DB */
};

/* Hash leaves */
struct hash_leaf {
	struct hash_leaf *next;
	unsigned int entries;	
	struct hash_node node[HASH_ALLOC_SIZE];
};

#ifdef __cplusplus
}
#endif

#endif	/* IMAGEPILE_H */
