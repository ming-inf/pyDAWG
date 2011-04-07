#include "dawg.h"
#include <string.h>

#include <malloc.h>


static int
DAWG_init(DAWG* dawg) {
	dawg->q0	= NULL;
	dawg->count	= 0;
	dawg->state	= EMPTY;
	dawg->longest_word = 0;

	hashtable_init(&dawg->reg, 101);

	dawg->prev_word.chars	= NULL;
	dawg->prev_word.length	= 0;

	return 0;
}


static int
DAWG_free(DAWG* dawg) {
	DAWG_clear(dawg);
	hashtable_destroy(&dawg->reg);
	return 0;
}


static DAWGNode*
DAWG_replace_or_register(DAWG* dawg, DAWGNode* state, String string, const size_t index);



static int
string_cmp(const String s1, const String s2) {
	size_t i;
	size_t n = s1.length < s2.length ? s1.length : s2.length;
	for (i=0; i < n; i++) {
		if ((unsigned char)s1.chars[i] < (unsigned char)s2.chars[i])
			return -1;
		else
		if ((unsigned char)s1.chars[i] > (unsigned char)s2.chars[i])
			return +1;
	}

	if (i < s1.length)
		return 1;
	else if (i < s2.length)
		return -1;
	else
		return 0;
}


int
DAWG_add_word(DAWG* dawg, String word) {
	const int k = string_cmp(dawg->prev_word, word);
	if (k > 0)
		return -2;
	else
		return DAWG_add_word_unchecked(dawg, word);
}


static int ALWAYS_INLINE
resize_hash(HashTable* hashtable) {
	if (hashtable->count > hashtable->count_threshold)
		return hashtable_resize(hashtable, hashtable->size * 2);
	else
		return 0;
}


int
DAWG_add_word_unchecked(DAWG* dawg, String word) {
	if (dawg->state == CLOSED)
		return -3;

	int ret = 1;
	int i = 0;

	if (dawg->q0 == NULL) {
		dawg->q0 = dawgnode_new(0);
		if (UNLIKELY(dawg->q0 == NULL))
			return -1;
	}
	
	DAWGNode* state = dawg->q0;
	ASSERT(state);

	// 1. skip existing prefix
	while (i < word.length and dawgnode_has_child(state, word.chars[i])) {
		state = dawgnode_get_child(state, word.chars[i]);
		ASSERT(state);
		i += 1;
	}

	// 2. minimize
	if (i < dawg->prev_word.length)
		DAWG_replace_or_register(dawg, state, dawg->prev_word, i);

	// 3. add sufix
	while (i < word.length) {
		DAWGNode* new = dawgnode_new(word.chars[i]);
		if (new == NULL)
			return -1;
		
		HashListItem* item = hashtable_del(
			&dawg->reg,
			HASH_GET_HASH(state),
			state
		);

		dawgnode_set_child(state, word.chars[i], new);

		if (item) {
			memfree(item);
			resize_hash(&dawg->reg);
			hashtable_add(
				&dawg->reg,
				HASH_GET_HASH(state),
				state
			);
		}

		state = new;
		i += 1;
	}

	if (state->eow == false) {
		state->eow = true;
		dawg->count += 1;
		ret = 1; // new word
	}
	else
		ret = 0; // existing word

	dawg->state = ACTIVE;

	// update longest_word
	if (word.length > dawg->longest_word)
		dawg->longest_word = word.length;

	// save previous word
	if (dawg->prev_word.chars)
		free(dawg->prev_word.chars);

	dawg->prev_word.length	= word.length;
	dawg->prev_word.chars	= (char*)memalloc(word.length);
	if (UNLIKELY(dawg->prev_word.chars == NULL))
		return -1;
	else
		memcpy(dawg->prev_word.chars, word.chars, word.length);

	word.length	= 0;
	word.chars	= NULL;

	return ret;
}


static int
DAWG_close(DAWG* dawg) {
	ASSERT(dawg);

	if (dawg->q0) {
		DAWG_replace_or_register(dawg, dawg->q0, dawg->prev_word, 0);
		hashtable_destroy(&dawg->reg);
		if (dawg->prev_word.chars) {
			memfree(dawg->prev_word.chars);

			dawg->prev_word.chars	= NULL;
			dawg->prev_word.length	= 0;
		}
		dawg->state = CLOSED;
		return 1;
	}
	else
		return 0;
}


#include "slist.c"

typedef struct StackItem {
	LISTITEM_data

	DAWGNode*	parent;	///< parent node
	DAWGNode*	child;	///< child node
	uint8_t		label;	///< edge label
} StackItem;


static DAWGNode*
DAWG_replace_or_register(DAWG* dawg, DAWGNode* state, String string, const size_t index) {
	List stack;
	list_init(&stack);

	// save in reverse order suffix of word
	size_t i;
	for (i=index; i < string.length; i++) {
		StackItem* item = (StackItem*)list_item_new(sizeof(StackItem));
		if (item) {
			item->parent = state;
			item->label  = string.chars[i];
			item->child  = state = dawgnode_get_child(item->parent, item->label);
			list_push_front(&stack, (ListItem*)item);
		}
		else
			goto error;
	}

	// replace or register
	while (1) {
		StackItem* item = (StackItem*)list_pop_first(&stack);
		if (item == NULL)
			break;

		// 1) try replace
		bool replaced = false;

		HashListItem* reg = hashtable_get_list(&dawg->reg, HASH_GET_HASH(item->child));
		while (reg) {
			DAWGNode* r = reg->key;
			ASSERT(r);

			if (dawgnode_equivalence(item->child, r)) {
				ASSERT(dawgnode_get_child(item->parent, item->label) == item->child);

				HashListItem* prev = hashtable_del(
										&dawg->reg,
										HASH_GET_HASH(item->parent),
										item->parent
									);

				dawgnode_set_child(item->parent, item->label, r);
				dawgnode_free(item->child);

				if (prev) {
					memfree(prev);
					resize_hash(&dawg->reg);
					hashtable_add(
						&dawg->reg,
						HASH_GET_HASH(item->parent),
						item->parent
					);
				}

				replaced = true;
				break;
			}
			else
				reg = reg->next;
		}

		// 2) register new unique state
		if (not replaced) {
			resize_hash(&dawg->reg);
			hashtable_add(
				&dawg->reg,
				HASH_GET_HASH(item->child),
				item->child
			);
		}

		list_item_delete((ListItem*)item);
	} // while

error:
	list_delete(&stack);
	return 0;
}


static bool PURE
dawgnode_equivalence(DAWGNode* p, DAWGNode* q) {
	/*
		Both states p and q are equivalent (subtrees
		rooted at p and q forms same languages):

		1. both are final/non-final
		2. has same number of children
		3. outgoing edges has same labels
		4. outgoing edges come to the same states
	
	*/

	if (p->eow != q->eow)
		return false;

	if (p->n != q->n)
		return false;

	size_t n = p->n;
	size_t i;
	for (i=0; i < n; i++) {
		// nodes are always sorted, so side-by-side compare is possible

		if (p->next[i].letter != q->next[i].letter)
			return false;

		if (p->next[i].child != q->next[i].child)
			return false;
	}

	return true;
}


/* used by hashtable for registry */
static uint32_t PURE
dawgnode_hash(DAWGNode* p) {
	/*
		hash is calulated from following components:
		- eow marker
		- outgoing link count
		- link labels
		- address of link destinations

		compare with dawgnode_equivalence
	*/
	static const uint32_t FNV_offset	= 2166136261u;
	static const uint32_t FNV_prime		= 16777619u;
	
	uint32_t hash = FNV_offset;
#define FNV_step(x) hash = (hash * FNV_prime); hash = hash ^ (x);
	FNV_step(p->n);
	FNV_step(p->eow);

	size_t i;
	for (i=0; i < p->n; i++) {
		FNV_step(p->next[i].letter);

		const uint32_t ptr = (uint32_t)(p->next[i].child);
		FNV_step(ptr & 0xff);
		FNV_step((ptr >> 8) & 0xff);
		FNV_step((ptr >> 16) & 0xff);
		FNV_step((ptr >> 24) & 0xff);
	}
	
#undef FNV_step
	return hash;
}


int
DAWG_clear_aux(DAWGNode* node, const size_t depth, void* extra) {
	if (node->next)
		memfree(node->next);

	memfree(node);
	return 1;
}


static int
DAWG_clear(DAWG* dawg) {
	DAWG_traverse_DFS_once(dawg, DAWG_clear_aux, NULL);

	dawg->q0	= NULL;
	dawg->count	= 0;
	dawg->state	= EMPTY;
	dawg->longest_word = 0;

	if (dawg->reg.size == 0)
		hashtable_init(&dawg->reg, 101);
	else
		hashtable_clear(&dawg->reg);

	if (dawg->prev_word.chars) {
		memfree(dawg->prev_word.chars);
		dawg->prev_word.chars = NULL;
	}

	dawg->prev_word.chars	= NULL;
	dawg->prev_word.length	= 0;

	return 0;
}


int
DAWG_traverse_DFS_aux(DAWGNode* node, const size_t depth, DAWG_traverse_callback callback, void* extra) {
	if (callback(node, depth, extra) == 0)
		return 0;

	size_t i;
	for (i=0; i < node->n; i++) {
		if (DAWG_traverse_DFS_aux(node->next[i].child, depth + 1, callback, extra) == 0)
			return 0;
	}

	return 1;
}


static int
DAWG_traverse_DFS(DAWG* dawg, DAWG_traverse_callback callback, void* extra) {
	ASSERT(dawg);
	ASSERT(callback);

	return DAWG_traverse_DFS_aux(dawg->q0, 0, callback, extra);
}


void
DAWG_traverse_clear_visited(DAWGNode* node) {
	node->visited = 0;

	int i;
	for (i=0; i < node->n; i++)
		DAWG_traverse_clear_visited(node->next[i].child);
}


int
DAWG_traverse_DFS_once_aux(DAWGNode* node, const size_t depth, DAWG_traverse_callback callback, void* extra) {
	if (node->visited != 0)
		return 1;

	node->visited = 1;
	int i;
	for (i=0; i < node->n; i++)
		if (DAWG_traverse_DFS_once_aux(node->next[i].child, depth + 1, callback, extra) == 0)
			return 0;

	return callback(node, depth, extra);
}


static int
DAWG_traverse_DFS_once(DAWG* dawg, DAWG_traverse_callback callback, void* extra) {
	ASSERT(dawg);
	ASSERT(callback);

	if (dawg->q0) {
		DAWG_traverse_clear_visited(dawg->q0);
		return DAWG_traverse_DFS_once_aux(dawg->q0, 0, callback, extra);
	}
	else
		return 1;
}


int
DAWG_get_stats_aux(DAWGNode* node, const size_t depth, void* extra) {
#define stats ((DAWGStatistics*)extra)
	stats->nodes_count	+= 1;
	stats->edges_count	+= node->n;
	stats->graph_size	+= dawgnode_get_size(node);
#undef stats
	return 1;
}


static void 
DAWG_get_stats(DAWG* dawg, DAWGStatistics* stats) {
	ASSERT(dawg);
	ASSERT(stats);

	stats->nodes_count	= 0;
	stats->edges_count	= 0;
	stats->words_count	= dawg->count;
	stats->longest_word	= dawg->longest_word;
	stats->sizeof_node	= sizeof(DAWGNode);
	stats->graph_size	= 0;
	stats->hash_tbl_size	= dawg->reg.size;
	stats->hash_tbl_count	= dawg->reg.count;

	DAWG_traverse_DFS_once(dawg, DAWG_get_stats_aux, stats);
}



static size_t PURE
DAWG_find(DAWG* dawg, const uint8_t* word, const size_t wordlen, DAWGNode** result) {
	ASSERT(dawg);

	DAWGNode* node = dawg->q0;
	size_t i=0;
	for (i=0; i < wordlen; i++) {
		node = dawgnode_get_child(node, word[i]);
		if (node == NULL)
			break;
	}

	if (result)
		*result = node;

	return i;
}


static bool PURE
DAWG_exists(DAWG* dawg, const uint8_t* word, const size_t wordlen) {
	DAWGNode* node;

	if (DAWG_find(dawg, word, wordlen, &node) > 0 and node)
		return node->eow;
	else
		return false;
}


static bool PURE
DAWG_longest_prefix(DAWG* dawg, const uint8_t* word, const size_t wordlen) {
	return DAWG_find(dawg, word, wordlen, NULL);
}


static bool PURE
DAWG_match(DAWG* dawg, const uint8_t* word, const size_t wordlen) {
	return DAWG_longest_prefix(dawg, word, wordlen) > 0;
}
