#include <stdio.h>
#include <stdlib.h>

/* A real hash table (separate chaining), not a plain array indexed by
 * key - a flat array would test something different (see alloc.c's same
 * note for cons cells vs a plain array). */
typedef struct Node {
	long key;
	long value;
	struct Node *next;
} Node;

#define BUCKETS 4096

static size_t hash_key(long key)
{
	return ((size_t)key * 2654435761u) % BUCKETS;
}

static void set(Node **table, long key, long value)
{
	size_t b = hash_key(key);
	for (Node *n = table[b]; n; n = n->next) {
		if (n->key == key) {
			n->value = value;
			return;
		}
	}
	Node *n = malloc(sizeof(Node));
	n->key = key;
	n->value = value;
	n->next = table[b];
	table[b] = n;
}

static long get(Node **table, long key, long def)
{
	size_t b = hash_key(key);
	for (Node *n = table[b]; n; n = n->next)
		if (n->key == key)
			return n->value;
	return def;
}

static void remove_key(Node **table, long key)
{
	size_t b = hash_key(key);
	Node **link = &table[b];
	while (*link) {
		if ((*link)->key == key) {
			Node *dead = *link;
			*link = dead->next;
			free(dead);
			return;
		}
		link = &(*link)->next;
	}
}

static long count(Node **table)
{
	long c = 0;
	for (size_t b = 0; b < BUCKETS; b++)
		for (Node *n = table[b]; n; n = n->next)
			c++;
	return c;
}

int main(void)
{
	static Node *table[BUCKETS];
	long n = 6000;

	for (long i = 0; i < n; i++)
		set(table, i, i * i);
	for (long i = 1; i < n; i += 2)
		remove_key(table, i);

	long total = 0;
	for (long i = 0; i < n; i++)
		total += get(table, i, 0);

	printf("%ld\n", total + count(table));
	return 0;
}
