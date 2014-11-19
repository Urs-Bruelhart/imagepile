/*
 * Image pile headers
 */

#define VER "0.1"
#define VERDATE "2014-11-18"
/* #define DEBUG 1 */

#ifdef DEBUG
#define DLOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DLOG(...)
#endif

/* Size of IPIL file header in bytes
 * 0-3:  'IPIL' signature
 * 4-7:  Truncate first block size (bytes)
 * 8-11: Last block total size (bytes)
 */
#define HDR_SIZE 12

/* Universal disk block size for the entire program
 * DO NOT CHANGE UNLESS YOU KNOW WHAT YOU ARE DOING! */
#define B_SIZE 4096

/* Hash tuning. DO NOT CHANGE because it will no longer
 * work properly with any existing data that does not
 * use the same HASH_SHIFT value */
#define HASH_SHIFT 11

/* How many hash table entries to allocate at once */
#define HASH_ALLOC_SIZE 64

/* Hash type and macro definitions */
typedef uint64_t hash_t;
typedef uint16_t hash_head_t;
/* Extract hash_head_t from a hash_t */
#define HASH_HEAD(x) (hash_head_t)(x >> ((sizeof(hash_t) - sizeof(hash_head_t)) * 8))

/* Master block database */
struct files_t {
	char dbfile[PATH_MAX];
	FILE *db;
	char indexfile[PATH_MAX];
	FILE *hashindex;
	char infile[PATH_MAX];
	FILE *in;
	char outfile[PATH_MAX];
	FILE *out;
};

/* Hash leaf table entries */
struct hash_node {
	hash_t hash;	/* Final hash */
	uint32_t offset;	/* Offset (in B_SIZE blocks) into master DB */
};

/* Hash leaves */
struct hash_leaf {
	struct hash_leaf *next;
	unsigned int entries;	
	struct hash_node node[HASH_ALLOC_SIZE];
};

void print_hash(hash_t);
hash_t block_hash(const hash_t *, const int);
int find_hash_match(const hash_t, int);
int index_hash(const hash_t, const int, const int, struct files_t *);
int read_db_block(void *, const int, struct files_t *);
int compare_blocks(const void *, const int, struct files_t *);
int add_db_block(const void *, hash_t, struct files_t *);
int get_block_offset(const void *, struct files_t *);
int input_image(struct files_t *, uint32_t);
int output_original(struct files_t *);

