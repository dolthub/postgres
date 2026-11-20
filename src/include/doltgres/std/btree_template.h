/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The template of BTreeMap<K, V> (the port text's sections 2, 4, and 9): a
 * port of alloc/src/collections/btree at Rust 1.99.1, in its modules'
 * order (map.rs, map/entry.rs, mem.rs, node.rs, search.rs), of the paths the
 * crates reach: get, entry, Entry::or_default, VacantEntry::insert and
 * insert_entry, OccupiedEntry::into_mut, and, for an instance its crate
 * iterates, iter and keys with their iterators' next, and for a set's
 * instance BTreeSet::iter, its Iter's next, and &BTreeSet's into_iter,
 * with the inserts' splits as
 * node.rs makes them, so nodes are allocated where std's are. A node is
 * laid out at rustc 1.99.1's offsets for the instance, which
 * std/btree_layouts.h holds, and reached through
 * them. A crate's B-tree is codegened in the crate, so its slice and
 * pointer operations run their precondition checks (std/ptr.h); Rust's std
 * is built without overflow checks, so its arithmetic is plain C's, and its
 * intrinsics::abort is abort. Its conversions between NodeRef's and
 * Handle's borrow and node types (reborrow, borrow_mut, reborrow_mut,
 * forget_type, forget_node_type, cast_to_leaf_unchecked, dormant, awaken,
 * new_kv, new_edge, and DormantMutRef's) change only Rust's types and are
 * the same C value, and force is the height test it makes. K and V pass by
 * pointer as moved values do; the Global allocator has no C form.
 *
 * STD_BTREE_SCOPE is static or extern, STD_BTREE_DECLARE and
 * STD_BTREE_DEFINE choose the declarations and definitions, STD_BTREE_KS
 * and STD_BTREE_VS spell K and V, STD_BTREE_K and STD_BTREE_V are K and V,
 * STD_BTREE_LAYOUT names the instance's numbers in std/btree_layouts.h,
 * STD_BTREE_CMP(a, b) is K's Ord::cmp of two pointers, STD_BTREE_KEY_DROP(k)
 * and STD_BTREE_VAL_DROP(v) are K's and V's drop glue at a pointer, and
 * STD_BTREE_VAL_DEFAULT(v) writes V's Default::default at a pointer.
 * STD_BTREE_ITERATE, defined for an instance its crate iterates, adds the
 * iteration forms, and STD_BTREE_SET, defined with it for a BTreeSet's map,
 * adds the set's. STD_BTREE_DROP, defined with STD_BTREE_ITERATE for an
 * instance its crate drops, adds BTreeMap's drop through its IntoIter.
 * STD_BTREE_INTO_ITERATE, defined with STD_BTREE_DROP for an instance its
 * crate consumes, adds BTreeMap's into_iter, IntoIter's next, and its drop.
 * STD_BTREE_BULK, defined for an instance its crate builds from sorted
 * keys, adds bulk_build_from_sorted_iter with append.rs' bulk_push and
 * fix.rs' right border, STD_BTREE_KEY_NE(a, b) being K's PartialEq::ne of
 * two pointers; with STD_BTREE_SET it adds BTreeSet::new and its
 * From<[T; N]>, STD_BTREE_SLICE_SORT(v, len) being the stable sort of a
 * slice of K, and STD_BTREE_KEY_NEEDS_DROP, defined when K's drop does
 * something, giving the array's IntoIter its keys' drop.
 * STD_BTREE_TABLES is the parameters, each after a comma, of the trait
 * tables V's drop and default take, as a generic of V used only through a
 * pointer passes its table (section 8), and STD_BTREE_TABLE_ARGS is them
 * as arguments; both are empty when V needs none. STD_BTREE_V_ZERO_SIZED,
 * defined for a zero-sized V, keeps V's C form out of the nodes. Every
 * parameter is undefined at the end.
 */

#include "doltgres/std/alloc.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/btree_layouts.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* STD_BTREE_CAT pastes two tokens after expanding them. */
#define STD_BTREE_CAT_(a, b) a##b
#define STD_BTREE_CAT(a, b) STD_BTREE_CAT_(a, b)

/* STD_BTREE_DEF is STD_BTREE_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_BTREE_DEF_static static
#define STD_BTREE_DEF_extern
#define STD_BTREE_DEF STD_BTREE_CAT(STD_BTREE_DEF_, STD_BTREE_SCOPE)

/* STD_BTREE_KV spells the instance's K and V. */
#define STD_BTREE_KV STD_BTREE_CAT(STD_BTREE_KS, STD_BTREE_VS)

/*
 * STD_BTREE_FN names a method of a type spelled with the instance's K and V
 * between its prefix and suffix, as an impl for them spells it.
 */
#define STD_BTREE_FN(pre, post, name) \
	STD_BTREE_CAT(STD_BTREE_CAT(STD_BTREE_CAT(std_btree_##pre, STD_BTREE_KV), post), _##name)

/* STD_BTREE_TYPE names a type spelled with the instance's K and V after it. */
#define STD_BTREE_TYPE(name) STD_BTREE_CAT(StdBTree##name, STD_BTREE_KV)

/* The instance's types. */
#define STD_BTREE_SPLIT_RESULT STD_BTREE_TYPE(SplitResult)
#define STD_BTREE_SPLIT_RESULT_T STD_BTREE_CAT(STD_BTREE_SPLIT_RESULT, _t)
#define STD_BTREE_VACANT_ENTRY STD_BTREE_TYPE(VacantEntry)
#define STD_BTREE_ENTRY STD_BTREE_TYPE(Entry)

/* The instance's functions, by their Rust items. */
#define STD_BTREE_GET STD_BTREE_FN(BTreeMap, , get)
#define STD_BTREE_MAP_ENTRY STD_BTREE_FN(BTreeMap, , entry)
#define STD_BTREE_OR_DEFAULT STD_BTREE_FN(Entry, , or_default)
#define STD_BTREE_VACANT_INSERT STD_BTREE_FN(VacantEntry, , insert)
#define STD_BTREE_INSERT_ENTRY STD_BTREE_FN(VacantEntry, , insert_entry)
#define STD_BTREE_INSERT_ENTRY_CLOSURE_0 STD_BTREE_FN(VacantEntry, , insert_entry_closure_0)
#define STD_BTREE_INTO_MUT STD_BTREE_FN(OccupiedEntry, , into_mut)
#define STD_BTREE_TAKE_MUT STD_BTREE_CAT(STD_BTREE_CAT(std_btree_take_mut_NodeRefOwned, STD_BTREE_KV), LeafOrInternal)
#define STD_BTREE_TAKE_MUT_CLOSURE_0 STD_BTREE_CAT(STD_BTREE_TAKE_MUT, _closure_0)
#define STD_BTREE_TAKE_MUT_CLOSURE_0_DROP STD_BTREE_CAT(STD_BTREE_TAKE_MUT, _closure_0_drop)
#define STD_BTREE_REPLACE \
	STD_BTREE_CAT(STD_BTREE_CAT(std_btree_replace_NodeRefOwned, STD_BTREE_KV), LeafOrInternalTuple)
#define STD_BTREE_LEAF_INIT STD_BTREE_FN(LeafNode, , init)
#define STD_BTREE_LEAF_NEW STD_BTREE_FN(LeafNode, , new)
#define STD_BTREE_INTERNAL_NEW STD_BTREE_FN(InternalNode, , new)
#define STD_BTREE_NEW_LEAF STD_BTREE_FN(NodeRefOwned, Leaf, new_leaf)
#define STD_BTREE_FROM_NEW_LEAF STD_BTREE_FN(NodeRefOwned, Leaf, from_new_leaf)
#define STD_BTREE_NEW_INTERNAL STD_BTREE_FN(NodeRefOwned, Internal, new_internal)
#define STD_BTREE_FROM_NEW_INTERNAL STD_BTREE_FN(NodeRefOwned, Internal, from_new_internal)
#define STD_BTREE_FROM_INTERNAL STD_BTREE_FN(NodeRef, Internal, from_internal)
#define STD_BTREE_AS_INTERNAL_PTR STD_BTREE_FN(NodeRef, Internal, as_internal_ptr)
#define STD_BTREE_LEN STD_BTREE_FN(NodeRef, , len)
#define STD_BTREE_ASCEND STD_BTREE_FN(NodeRef, , ascend)
#define STD_BTREE_KEYS STD_BTREE_FN(NodeRefImmut, , keys)
#define STD_BTREE_KEY_AREA_MUT STD_BTREE_FN(NodeRefMut, , key_area_mut_Usize)
#define STD_BTREE_KEY_AREA_MUT_RANGE STD_BTREE_FN(NodeRefMut, , key_area_mut_RangeUsize)
#define STD_BTREE_VAL_AREA_MUT STD_BTREE_FN(NodeRefMut, , val_area_mut_Usize)
#define STD_BTREE_VAL_AREA_MUT_RANGE STD_BTREE_FN(NodeRefMut, , val_area_mut_RangeUsize)
#define STD_BTREE_EDGE_AREA_MUT STD_BTREE_FN(NodeRefMut, Internal, edge_area_mut_Usize)
#define STD_BTREE_EDGE_AREA_MUT_RANGE STD_BTREE_FN(NodeRefMut, Internal, edge_area_mut_RangeUsize)
#define STD_BTREE_LEN_MUT STD_BTREE_FN(NodeRefMut, , len_mut)
#define STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS \
	STD_BTREE_FN(NodeRefMut, Internal, correct_childrens_parent_links_RangeUsize)
#define STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS_INCLUSIVE \
	STD_BTREE_FN(NodeRefMut, Internal, correct_childrens_parent_links_RangeInclusiveUsize)
#define STD_BTREE_CORRECT_ALL_CHILDRENS_PARENT_LINKS \
	STD_BTREE_FN(NodeRefMut, Internal, correct_all_childrens_parent_links)
#define STD_BTREE_SET_PARENT_LINK STD_BTREE_FN(NodeRefMut, LeafOrInternal, set_parent_link)
#define STD_BTREE_PUSH_INTERNAL_LEVEL STD_BTREE_FN(NodeRefOwned, LeafOrInternal, push_internal_level)
#define STD_BTREE_PUSH_INTERNAL_LEVEL_CLOSURE_0 \
	STD_BTREE_FN(NodeRefOwned, LeafOrInternal, push_internal_level_closure_0)
#define STD_BTREE_PUSH_WITH_HANDLE STD_BTREE_FN(NodeRefMut, Leaf, push_with_handle)
#define STD_BTREE_INTERNAL_PUSH STD_BTREE_FN(NodeRefMut, Internal, push)
#define STD_BTREE_LEAF_INSERT_FIT STD_BTREE_FN(HandleNodeRefMut, LeafEdge, insert_fit)
#define STD_BTREE_LEAF_INSERT STD_BTREE_FN(HandleNodeRefMut, LeafEdge, insert)
#define STD_BTREE_CORRECT_PARENT_LINK STD_BTREE_FN(HandleNodeRefMut, InternalEdge, correct_parent_link)
#define STD_BTREE_INTERNAL_INSERT_FIT STD_BTREE_FN(HandleNodeRefMut, InternalEdge, insert_fit)
#define STD_BTREE_INTERNAL_INSERT STD_BTREE_FN(HandleNodeRefMut, InternalEdge, insert)
#define STD_BTREE_INSERT_RECURSING STD_BTREE_FN(HandleNodeRefMut, LeafEdge, insert_recursing)
#define STD_BTREE_DESCEND STD_BTREE_FN(HandleNodeRef, InternalEdge, descend)
#define STD_BTREE_INTO_KV STD_BTREE_FN(HandleNodeRefImmut, KV, into_kv)
#define STD_BTREE_INTO_VAL_MUT STD_BTREE_FN(HandleNodeRefMut, KV, into_val_mut)
#define STD_BTREE_SPLIT_LEAF_DATA STD_BTREE_FN(HandleNodeRefMut, KV, split_leaf_data)
#define STD_BTREE_LEAF_SPLIT STD_BTREE_FN(HandleNodeRefMut, LeafKV, split)
#define STD_BTREE_INTERNAL_SPLIT STD_BTREE_FN(HandleNodeRefMut, InternalKV, split)
#define STD_BTREE_SLICE_INSERT_K STD_BTREE_CAT(std_btree_slice_insert_, STD_BTREE_KS)
#define STD_BTREE_SLICE_INSERT_V STD_BTREE_CAT(std_btree_slice_insert_, STD_BTREE_VS)
#define STD_BTREE_SLICE_INSERT_EDGE STD_BTREE_CAT(std_btree_slice_insert_NonNullLeafNode, STD_BTREE_KV)
#define STD_BTREE_SEARCH_TREE STD_BTREE_FN(NodeRef, LeafOrInternal, search_tree)
#define STD_BTREE_SEARCH_NODE STD_BTREE_FN(NodeRef, , search_node)
#define STD_BTREE_FIND_KEY_INDEX STD_BTREE_FN(NodeRef, , find_key_index)
#define STD_BTREE_ITER STD_BTREE_FN(BTreeMap, , iter)
#define STD_BTREE_MAP_KEYS STD_BTREE_FN(BTreeMap, , keys)
#define STD_BTREE_ITER_NEXT STD_BTREE_FN(Iter, , Iterator_next)
#define STD_BTREE_KEYS_NEXT STD_BTREE_FN(Keys, , Iterator_next)
#define STD_BTREE_REPLACE_KV \
	STD_BTREE_CAT(STD_BTREE_CAT(std_btree_replace_HandleNodeRefImmut, STD_BTREE_KV), LeafEdgeTupleRefKRefV)
#define STD_BTREE_REPLACE_KV_FRAME_0 STD_BTREE_CAT(STD_BTREE_REPLACE_KV, _frame_0_t)
#define STD_BTREE_REPLACE_KV_BODY_0 STD_BTREE_CAT(STD_BTREE_REPLACE_KV, _body_0)
#define STD_BTREE_REPLACE_KV_PAD_0 STD_BTREE_CAT(STD_BTREE_REPLACE_KV, _pad_0)
#define STD_BTREE_FULL_RANGE_OF \
	STD_BTREE_CAT(STD_BTREE_CAT(std_btree_full_range_NodeRefImmut, STD_BTREE_KV), LeafOrInternal)
#define STD_BTREE_FULL_RANGE STD_BTREE_FN(NodeRefImmut, LeafOrInternal, full_range)
#define STD_BTREE_LAZY_NEXT_UNCHECKED STD_BTREE_FN(LazyLeafRangeImmut, , next_unchecked)
#define STD_BTREE_INIT_FRONT STD_BTREE_FN(LazyLeafRangeImmut, , init_front)
#define STD_BTREE_LEAF_NEXT_KV STD_BTREE_FN(HandleNodeRef, LeafEdge, next_kv)
#define STD_BTREE_EDGE_NEXT_UNCHECKED STD_BTREE_FN(HandleNodeRefImmut, LeafEdge, next_unchecked)
#define STD_BTREE_EDGE_NEXT_UNCHECKED_CLOSURE_0 STD_BTREE_FN(HandleNodeRefImmut, LeafEdge, next_unchecked_closure_0)
#define STD_BTREE_FIRST_LEAF_EDGE STD_BTREE_FN(NodeRef, LeafOrInternal, first_leaf_edge)
#define STD_BTREE_NEXT_LEAF_EDGE STD_BTREE_FN(HandleNodeRef, LeafOrInternalKV, next_leaf_edge)
#define STD_BTREE_FIRST_EDGE STD_BTREE_FN(NodeRef, , first_edge)
#define STD_BTREE_RIGHT_KV STD_BTREE_FN(HandleNodeRef, Edge, right_kv)
#define STD_BTREE_RIGHT_EDGE STD_BTREE_FN(HandleNodeRef, KV, right_edge)
#ifdef STD_BTREE_BULK
#define STD_BTREE_KV_T STD_BTREE_SPLIT_RESULT_T
#define STD_BTREE_DEDUP_SORTED_ITER STD_BTREE_TYPE(DedupSortedIter)
#define STD_BTREE_LAST_EDGE STD_BTREE_FN(NodeRef, , last_edge)
#define STD_BTREE_LAST_KV STD_BTREE_FN(NodeRef, , last_kv)
#define STD_BTREE_LEAF_PUSH STD_BTREE_FN(NodeRefMut, Leaf, push)
#define STD_BTREE_LEFT_EDGE STD_BTREE_FN(HandleNodeRef, KV, left_edge)
#define STD_BTREE_CONSIDER_FOR_BALANCING STD_BTREE_FN(HandleNodeRefMut, InternalKV, consider_for_balancing)
#define STD_BTREE_KV_MUT STD_BTREE_FN(HandleNodeRefMut, KV, kv_mut)
#define STD_BTREE_REPLACE_KV_MUT STD_BTREE_FN(HandleNodeRefMut, KV, replace_kv)
#define STD_BTREE_SLICE_SHR_K STD_BTREE_CAT(std_btree_slice_shr_, STD_BTREE_KS)
#define STD_BTREE_SLICE_SHR_V STD_BTREE_CAT(std_btree_slice_shr_, STD_BTREE_VS)
#define STD_BTREE_SLICE_SHR_EDGE STD_BTREE_CAT(std_btree_slice_shr_NonNullLeafNode, STD_BTREE_KV)
#define STD_BTREE_BULK_STEAL_LEFT STD_BTREE_FN(BalancingContext, , bulk_steal_left)
#define STD_BTREE_LAST_LEAF_EDGE STD_BTREE_FN(NodeRef, LeafOrInternal, last_leaf_edge)
#define STD_BTREE_FIX_RIGHT_BORDER_OF_PLENTIFUL STD_BTREE_FN(NodeRefOwned, LeafOrInternal, fix_right_border_of_plentiful)
#define STD_BTREE_PEEKABLE_NEXT STD_BTREE_FN(PeekableDedup, , Iterator_next)
#define STD_BTREE_PEEKABLE_PEEK STD_BTREE_FN(PeekableDedup, , peek)
#define STD_BTREE_DEDUP_NEXT STD_BTREE_FN(DedupSortedIter, , Iterator_next)
#define STD_BTREE_DEDUP_DROP STD_BTREE_FN(DedupSortedIter, , drop)
#define STD_BTREE_BULK_PUSH STD_BTREE_FN(NodeRefOwned, LeafOrInternal, bulk_push)
#define STD_BTREE_BULK_PUSH_FRAME_0 STD_BTREE_CAT(STD_BTREE_BULK_PUSH, _frame_0_t)
#define STD_BTREE_BULK_PUSH_BODY_0 STD_BTREE_CAT(STD_BTREE_BULK_PUSH, _body_0)
#define STD_BTREE_BULK_PUSH_PAD_0 STD_BTREE_CAT(STD_BTREE_BULK_PUSH, _pad_0)
#define STD_BTREE_BULK_BUILD STD_BTREE_FN(BTreeMap, , bulk_build_from_sorted_iter)
#define STD_BTREE_BULK_BUILD_FRAME_0 STD_BTREE_CAT(STD_BTREE_BULK_BUILD, _frame_0_t)
#define STD_BTREE_BULK_BUILD_BODY_0 STD_BTREE_CAT(STD_BTREE_BULK_BUILD, _body_0)
#define STD_BTREE_BULK_BUILD_PAD_0 STD_BTREE_CAT(STD_BTREE_BULK_BUILD, _pad_0)
#define STD_BTREE_KV_DROP STD_BTREE_CAT(STD_BTREE_KV_T, _drop)
#define STD_BTREE_KV_DROP_BODY_0 STD_BTREE_CAT(STD_BTREE_KV_DROP, _body_0)
#define STD_BTREE_KV_DROP_PAD_0 STD_BTREE_CAT(STD_BTREE_KV_DROP, _pad_0)
#define STD_BTREE_DEDUP_NEXT_FRAME_0 STD_BTREE_CAT(STD_BTREE_DEDUP_NEXT, _frame_0_t)
#define STD_BTREE_DEDUP_NEXT_BODY_0 STD_BTREE_CAT(STD_BTREE_DEDUP_NEXT, _body_0)
#define STD_BTREE_DEDUP_NEXT_PAD_0 STD_BTREE_CAT(STD_BTREE_DEDUP_NEXT, _pad_0)
#define STD_BTREE_DEDUP_DROP_BODY_0 STD_BTREE_CAT(STD_BTREE_DEDUP_DROP, _body_0)
#define STD_BTREE_DEDUP_DROP_PAD_0 STD_BTREE_CAT(STD_BTREE_DEDUP_DROP, _pad_0)
#ifdef STD_BTREE_SET
#define STD_BTREE_SET_NEW STD_BTREE_CAT(STD_BTREE_CAT(std_btree_BTreeSet, STD_BTREE_KS), _new)
#define STD_BTREE_SET_FROM_SORTED_ITER STD_BTREE_CAT(STD_BTREE_CAT(std_btree_BTreeSet, STD_BTREE_KS), _from_sorted_iter)
#define STD_BTREE_SET_FROM_SORTED_ITER_MAP STD_BTREE_CAT(STD_BTREE_SET_FROM_SORTED_ITER, _map_t)
#define STD_BTREE_SET_FROM_SORTED_ITER_MAP_NEXT STD_BTREE_CAT(STD_BTREE_SET_FROM_SORTED_ITER, _map_Iterator_next)
#define STD_BTREE_SET_FROM_SORTED_ITER_MAP_SIZE_HINT STD_BTREE_CAT(STD_BTREE_SET_FROM_SORTED_ITER, _map_Iterator_size_hint)
#define STD_BTREE_SET_FROM_SORTED_ITER_MAP_DROP STD_BTREE_CAT(STD_BTREE_SET_FROM_SORTED_ITER, _map_drop)
#define STD_BTREE_SET_FROM_SORTED_ITER_MAP_OPS STD_BTREE_CAT(STD_BTREE_SET_FROM_SORTED_ITER, _MAP_ITERATOR_OPS)
#define STD_BTREE_SET_FROM_ARRAY STD_BTREE_CAT(STD_BTREE_CAT(std_btree_BTreeSet, STD_BTREE_KS), _FromArray_from)
#define STD_BTREE_SET_FROM_ARRAY_FRAME_0 STD_BTREE_CAT(STD_BTREE_SET_FROM_ARRAY, _frame_0_t)
#define STD_BTREE_SET_FROM_ARRAY_BODY_0 STD_BTREE_CAT(STD_BTREE_SET_FROM_ARRAY, _body_0)
#define STD_BTREE_SET_FROM_ARRAY_PAD_0 STD_BTREE_CAT(STD_BTREE_SET_FROM_ARRAY, _pad_0)
#define STD_BTREE_KEY_DROP_GLUE STD_BTREE_CAT(STD_BTREE_CAT(std_btree_, STD_BTREE_KS), _drop)
#define STD_BTREE_ARRAY_INTO_ITER_DROP STD_BTREE_CAT(STD_BTREE_CAT(std_array_IntoIter, STD_BTREE_KS), _drop)
#define STD_BTREE_ARRAY_INTO_ITER_OPS STD_BTREE_CAT(STD_BTREE_CAT(STD_ARRAY_INTO_ITER, STD_BTREE_KS), _ITERATOR_OPS)
#endif
#endif
#ifdef STD_BTREE_DROP
#define STD_BTREE_MAP_DROP STD_BTREE_FN(BTreeMap, , drop)
#define STD_BTREE_INTO_ITER STD_BTREE_FN(BTreeMap, , IntoIterator_into_iter)
#define STD_BTREE_INTO_ITER_DROP STD_BTREE_FN(IntoIter, , Drop_drop)
#define STD_BTREE_INTO_ITER_DROP_FRAME_0 STD_BTREE_CAT(STD_BTREE_INTO_ITER_DROP, _frame_0_t)
#define STD_BTREE_INTO_ITER_DROP_BODY_0 STD_BTREE_CAT(STD_BTREE_INTO_ITER_DROP, _body_0)
#define STD_BTREE_INTO_ITER_DROP_PAD_0 STD_BTREE_CAT(STD_BTREE_INTO_ITER_DROP, _pad_0)
#define STD_BTREE_DYING_NEXT STD_BTREE_FN(IntoIter, , dying_next)
#define STD_BTREE_INTO_ITER_NEXT STD_BTREE_FN(IntoIter, , Iterator_next)
#define STD_BTREE_INTO_KEY_VAL STD_BTREE_FN(HandleNodeRefDying, LeafOrInternalKV, into_key_val)
#ifdef STD_BTREE_INTO_ITERATE
#define STD_BTREE_INTO_DEF STD_BTREE_DEF
#else
#define STD_BTREE_INTO_DEF static
#endif
#define STD_BTREE_TAKE_FRONT STD_BTREE_FN(LazyLeafRangeDying, , take_front)
#define STD_BTREE_LAZY_DEALLOCATING_NEXT_UNCHECKED STD_BTREE_FN(LazyLeafRangeDying, , deallocating_next_unchecked)
#define STD_BTREE_LAZY_DEALLOCATING_END STD_BTREE_FN(LazyLeafRangeDying, , deallocating_end)
#define STD_BTREE_EDGE_DEALLOCATING_NEXT STD_BTREE_FN(HandleNodeRefDying, LeafEdge, deallocating_next)
#define STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED \
	STD_BTREE_FN(HandleNodeRefDying, LeafEdge, deallocating_next_unchecked)
#define STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED_CLOSURE_0 \
	STD_BTREE_FN(HandleNodeRefDying, LeafEdge, deallocating_next_unchecked_closure_0)
#define STD_BTREE_EDGE_DEALLOCATING_END STD_BTREE_FN(HandleNodeRefDying, LeafEdge, deallocating_end)
#define STD_BTREE_REPLACE_DYING \
	STD_BTREE_CAT(STD_BTREE_CAT(std_btree_replace_HandleNodeRefDying, STD_BTREE_KV), LeafEdgeHandleKV)
#define STD_BTREE_REPLACE_DYING_FRAME_0 STD_BTREE_CAT(STD_BTREE_REPLACE_DYING, _frame_0_t)
#define STD_BTREE_REPLACE_DYING_BODY_0 STD_BTREE_CAT(STD_BTREE_REPLACE_DYING, _body_0)
#define STD_BTREE_REPLACE_DYING_PAD_0 STD_BTREE_CAT(STD_BTREE_REPLACE_DYING, _pad_0)
#define STD_BTREE_DEALLOCATE_AND_ASCEND STD_BTREE_FN(NodeRefDying, LeafOrInternal, deallocate_and_ascend)
#define STD_BTREE_DROP_KEY_VAL STD_BTREE_FN(HandleNodeRefDying, LeafOrInternalKV, drop_key_val)
#define STD_BTREE_DROP_KEY_VAL_FRAME_0 STD_BTREE_CAT(STD_BTREE_DROP_KEY_VAL, _frame_0_t)
#define STD_BTREE_DROP_KEY_VAL_BODY_0 STD_BTREE_CAT(STD_BTREE_DROP_KEY_VAL, _body_0)
#define STD_BTREE_DROP_KEY_VAL_PAD_0 STD_BTREE_CAT(STD_BTREE_DROP_KEY_VAL, _pad_0)
#endif
#ifdef STD_BTREE_SET

/* STD_BTREE_SET_FN names a method of a set's type spelled with the set's T, its map's K. */
#define STD_BTREE_SET_FN(pre, name) STD_BTREE_CAT(STD_BTREE_CAT(std_btree_##pre, STD_BTREE_KS), _##name)
#define STD_BTREE_SET_ITER STD_BTREE_SET_FN(BTreeSet, iter)
#define STD_BTREE_SET_ITER_NEXT STD_BTREE_SET_FN(SetIter, Iterator_next)
#define STD_BTREE_SET_INTO_ITER STD_BTREE_SET_FN(RefBTreeSet, IntoIterator_into_iter)
#endif

/* STD_BTREE_SPLIT_ROOT is insert_recursing's split_root: a closure of a split's result, and V's tables. */
#define STD_BTREE_SPLIT_ROOT(name) void (*name) (void *, STD_BTREE_SPLIT_RESULT * STD_BTREE_TABLES)

#ifdef STD_BTREE_DECLARE

/*
 * The instance's SplitResult<'a, K, V, NodeType>: the node left of the split,
 * the key and value split off, and the new node right of it.
 */
typedef struct STD_BTREE_SPLIT_RESULT_T
{
	STD_BTREE_K f0;
	STD_BTREE_V f1;
}			STD_BTREE_SPLIT_RESULT_T;

typedef struct STD_BTREE_SPLIT_RESULT
{
	StdBTreeNodeRef left;
	STD_BTREE_SPLIT_RESULT_T kv;
	StdBTreeNodeRef right;
}			STD_BTREE_SPLIT_RESULT;

/* The instance's VacantEntry<'a, K, V>: its key, the leaf edge it goes into if the map has a root, and its map. */
typedef struct STD_BTREE_VACANT_ENTRY
{
	STD_BTREE_K key;
	bool		has_handle;
	StdBTreeHandle handle;
	StdBTreeMap *dormant_map;
}			STD_BTREE_VACANT_ENTRY;

/* The instance's Entry<'a, K, V>: vacant or occupied. */
typedef struct STD_BTREE_ENTRY
{
	StdBTreeEntryTag tag;
	union
	{
		STD_BTREE_VACANT_ENTRY vacant;
		StdBTreeOccupiedEntry occupied;
	}			u;
}			STD_BTREE_ENTRY;

STD_BTREE_SCOPE STD_BTREE_V const *STD_BTREE_GET(const StdBTreeMap *self, STD_BTREE_K const *key);
STD_BTREE_SCOPE STD_BTREE_ENTRY STD_BTREE_MAP_ENTRY(StdBTreeMap *self, STD_BTREE_K * key);
STD_BTREE_SCOPE STD_BTREE_V *STD_BTREE_OR_DEFAULT(STD_BTREE_ENTRY * self STD_BTREE_TABLES);
STD_BTREE_SCOPE STD_BTREE_V *STD_BTREE_VACANT_INSERT(STD_BTREE_VACANT_ENTRY * self, STD_BTREE_V * value
													 STD_BTREE_TABLES);
STD_BTREE_SCOPE StdBTreeOccupiedEntry STD_BTREE_INSERT_ENTRY(STD_BTREE_VACANT_ENTRY * self, STD_BTREE_V * value
															 STD_BTREE_TABLES);
STD_BTREE_SCOPE STD_BTREE_V *STD_BTREE_INTO_MUT(StdBTreeOccupiedEntry *self);
#ifdef STD_BTREE_ITERATE
STD_BTREE_SCOPE StdBTreeIter STD_BTREE_ITER(const StdBTreeMap *self);
STD_BTREE_SCOPE StdBTreeKeys STD_BTREE_MAP_KEYS(const StdBTreeMap *self);
STD_BTREE_SCOPE bool STD_BTREE_ITER_NEXT(StdBTreeIter *self, STD_BTREE_K const **out_0, STD_BTREE_V const **out_1);
STD_BTREE_SCOPE bool STD_BTREE_KEYS_NEXT(StdBTreeKeys *self, STD_BTREE_K const **result);
#ifdef STD_BTREE_SET
STD_BTREE_SCOPE StdBTreeSetIter STD_BTREE_SET_ITER(const StdBTreeMap *self);
STD_BTREE_SCOPE bool STD_BTREE_SET_ITER_NEXT(StdBTreeSetIter *self, STD_BTREE_K const **result);
STD_BTREE_SCOPE StdBTreeSetIter STD_BTREE_SET_INTO_ITER(const StdBTreeMap *self);
#endif
#ifdef STD_BTREE_DROP
STD_BTREE_SCOPE void STD_BTREE_MAP_DROP(void *self);
#ifdef STD_BTREE_INTO_ITERATE
STD_BTREE_SCOPE StdBTreeIntoIter STD_BTREE_INTO_ITER(StdBTreeMap *self);
STD_BTREE_SCOPE bool STD_BTREE_INTO_ITER_NEXT(StdBTreeIntoIter *self, STD_BTREE_K * key, STD_BTREE_V * val);
STD_BTREE_SCOPE void STD_BTREE_INTO_ITER_DROP(StdBTreeIntoIter *self);
#endif
#endif
#if defined(STD_BTREE_BULK) && defined(STD_BTREE_SET)
STD_BTREE_SCOPE StdBTreeMap STD_BTREE_SET_NEW(void);
STD_BTREE_SCOPE StdBTreeMap STD_BTREE_SET_FROM_ARRAY(STD_BTREE_K * arr, size_t n);
#endif
#endif

#endif							/* STD_BTREE_DECLARE */

#ifdef STD_BTREE_DEFINE

/* The instance's node layout, rustc 1.99.1's as std/btree_layouts.h holds it. */
#define STD_BTREE_LAYOUT_OF(field) STD_BTREE_CAT(STD_BTREE_CAT(STD_BTREE_LAYOUT_, STD_BTREE_LAYOUT), _##field)
#define STD_BTREE_LEAF_SIZE STD_BTREE_LAYOUT_OF(LEAF_SIZE)
#define STD_BTREE_LEAF_ALIGN STD_BTREE_LAYOUT_OF(LEAF_ALIGN)
#define STD_BTREE_PARENT_OFF STD_BTREE_LAYOUT_OF(PARENT)
#define STD_BTREE_PARENT_IDX_OFF STD_BTREE_LAYOUT_OF(PARENT_IDX)
#define STD_BTREE_LEN_OFF STD_BTREE_LAYOUT_OF(LEN)
#define STD_BTREE_KEYS_OFF STD_BTREE_LAYOUT_OF(KEYS)
#define STD_BTREE_VALS_OFF STD_BTREE_LAYOUT_OF(VALS)
#define STD_BTREE_INTERNAL_SIZE STD_BTREE_LAYOUT_OF(INTERNAL_SIZE)
#define STD_BTREE_INTERNAL_ALIGN STD_BTREE_LAYOUT_OF(INTERNAL_ALIGN)
#define STD_BTREE_EDGES_OFF STD_BTREE_LAYOUT_OF(EDGES)

/*
 * STD_BTREE_V_SIZE is V's size in a node, 0 for a zero-sized V, whose C
 * form's byte a node never holds, as Rust's array of it takes no room.
 */
#ifdef STD_BTREE_V_ZERO_SIZED
#define STD_BTREE_V_SIZE 0
#else
#define STD_BTREE_V_SIZE sizeof(STD_BTREE_V)
#endif

StaticAssertDecl(STD_BTREE_KEYS_OFF % _Alignof(STD_BTREE_K) == 0 &&
				 STD_BTREE_KEYS_OFF + STD_BTREE_CAPACITY * sizeof(STD_BTREE_K) <= STD_BTREE_LEAF_SIZE &&
				 STD_BTREE_VALS_OFF % _Alignof(STD_BTREE_V) == 0 &&
				 STD_BTREE_VALS_OFF + STD_BTREE_CAPACITY * STD_BTREE_V_SIZE <= STD_BTREE_LEAF_SIZE &&
				 STD_BTREE_PARENT_OFF % _Alignof(StdBTreeInternalNode *) == 0 &&
				 STD_BTREE_PARENT_IDX_OFF % _Alignof(uint16) == 0 && STD_BTREE_LEN_OFF % _Alignof(uint16) == 0 &&
				 STD_BTREE_EDGES_OFF >= STD_BTREE_LEAF_SIZE &&
				 STD_BTREE_EDGES_OFF + 2 * STD_BTREE_B * sizeof(StdBTreeLeafNode *) <= STD_BTREE_INTERNAL_SIZE,
				 "the instance's measured node layout holds its fields");

/* STD_BTREE_PARENT, STD_BTREE_PARENT_IDX, and STD_BTREE_LEN_FIELD are a LeafNode's fields. */
#define STD_BTREE_PARENT(node) (*(StdBTreeInternalNode **) ((char *) (node) + STD_BTREE_PARENT_OFF))
#define STD_BTREE_PARENT_IDX(node) (*(uint16 *) ((char *) (node) + STD_BTREE_PARENT_IDX_OFF))
#define STD_BTREE_LEN_FIELD(node) (*(uint16 *) ((char *) (node) + STD_BTREE_LEN_OFF))

/* STD_BTREE_KEYS_OF and STD_BTREE_VALS_OF are a LeafNode's keys and values arrays. */
#define STD_BTREE_KEYS_OF(node) ((STD_BTREE_K *) ((char *) (node) + STD_BTREE_KEYS_OFF))
#define STD_BTREE_VALS_OF(node) ((STD_BTREE_V *) ((char *) (node) + STD_BTREE_VALS_OFF))

/* STD_BTREE_VAL_AT is the value at an index of an array of values, the array's own place for a zero-sized V. */
#define STD_BTREE_VAL_AT(vals, index) ((STD_BTREE_V *) ((char *) (vals) + (index) * STD_BTREE_V_SIZE))

/* STD_BTREE_VAL_MOVE moves a value to a place, which writes nothing of a zero-sized V. */
#define STD_BTREE_VAL_MOVE(place, value) memcpy((place), (value), STD_BTREE_V_SIZE)

/* STD_BTREE_EDGES_OF is an InternalNode's edges array. */
#define STD_BTREE_EDGES_OF(node) ((StdBTreeLeafNode **) ((char *) (node) + STD_BTREE_EDGES_OFF))

/* STD_BTREE_EDGES_LEN is an InternalNode's edges array's length, 2 * B. */
#define STD_BTREE_EDGES_LEN (2 * STD_BTREE_B)

static void STD_BTREE_INSERT_ENTRY_CLOSURE_0(void *captures, STD_BTREE_SPLIT_RESULT * ins STD_BTREE_TABLES);
static void STD_BTREE_TAKE_MUT(StdBTreeNodeRef *v, StdBTreeNodeRef (*change) (void *, StdBTreeNodeRef *),
							   void *change_captures, void (*change_drop) (void *));
static void STD_BTREE_TAKE_MUT_CLOSURE_0(void *captures, StdBTreeNodeRef *value, StdBTreeNodeRef *out_0);
static void STD_BTREE_TAKE_MUT_CLOSURE_0_DROP(void *captures);
static void STD_BTREE_REPLACE(StdBTreeNodeRef *v, void (*change) (void *, StdBTreeNodeRef *, StdBTreeNodeRef *),
							  void *change_captures, void (*change_drop) (void *));
static void STD_BTREE_LEAF_INIT(StdBTreeLeafNode *this);
static StdBTreeLeafNode *STD_BTREE_LEAF_NEW(void);
static StdBTreeInternalNode *STD_BTREE_INTERNAL_NEW(void);
static StdBTreeNodeRef STD_BTREE_NEW_LEAF(void);
static StdBTreeNodeRef STD_BTREE_FROM_NEW_LEAF(StdBTreeLeafNode *leaf);
static StdBTreeNodeRef STD_BTREE_NEW_INTERNAL(StdBTreeNodeRef *child);
static StdBTreeNodeRef STD_BTREE_FROM_NEW_INTERNAL(StdBTreeInternalNode *internal, size_t height);
static StdBTreeNodeRef STD_BTREE_FROM_INTERNAL(StdBTreeInternalNode *node, size_t height);
static StdBTreeInternalNode *STD_BTREE_AS_INTERNAL_PTR(const StdBTreeNodeRef *this);
static size_t STD_BTREE_LEN(const StdBTreeNodeRef *self);
static bool STD_BTREE_ASCEND(StdBTreeNodeRef *self, StdBTreeHandle *result, StdBTreeNodeRef *error);
static STD_BTREE_K const *STD_BTREE_KEYS(const StdBTreeNodeRef *self, size_t *result_len);
static STD_BTREE_K * STD_BTREE_KEY_AREA_MUT(StdBTreeNodeRef *self, size_t index);
static STD_BTREE_K * STD_BTREE_KEY_AREA_MUT_RANGE(StdBTreeNodeRef *self, size_t index_start, size_t index_end);
static STD_BTREE_V * STD_BTREE_VAL_AREA_MUT(StdBTreeNodeRef *self, size_t index);
static STD_BTREE_V * STD_BTREE_VAL_AREA_MUT_RANGE(StdBTreeNodeRef *self, size_t index_start, size_t index_end);
static StdBTreeLeafNode **STD_BTREE_EDGE_AREA_MUT(StdBTreeNodeRef *self, size_t index);
static StdBTreeLeafNode **STD_BTREE_EDGE_AREA_MUT_RANGE(StdBTreeNodeRef *self, size_t index_start,
														size_t index_end);
static uint16 *STD_BTREE_LEN_MUT(StdBTreeNodeRef *self);
static void STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS(StdBTreeNodeRef *self, size_t range_start, size_t range_end);
static void STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS_INCLUSIVE(StdBTreeNodeRef *self, size_t range_start,
															   size_t range_end);
static void STD_BTREE_CORRECT_ALL_CHILDRENS_PARENT_LINKS(StdBTreeNodeRef *self);
static void STD_BTREE_SET_PARENT_LINK(StdBTreeNodeRef *self, StdBTreeInternalNode *parent, size_t parent_idx);
static StdBTreeNodeRef STD_BTREE_PUSH_INTERNAL_LEVEL(StdBTreeNodeRef *self);
static StdBTreeNodeRef STD_BTREE_PUSH_INTERNAL_LEVEL_CLOSURE_0(void *captures, StdBTreeNodeRef *old_root);
static StdBTreeHandle STD_BTREE_PUSH_WITH_HANDLE(StdBTreeNodeRef *self, STD_BTREE_K * key, STD_BTREE_V * val
												 STD_BTREE_TABLES);
static void STD_BTREE_INTERNAL_PUSH(StdBTreeNodeRef *self, STD_BTREE_K * key, STD_BTREE_V * val,
									StdBTreeNodeRef *edge STD_BTREE_TABLES);
static StdBTreeHandle STD_BTREE_LEAF_INSERT_FIT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val);
static void STD_BTREE_LEAF_INSERT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val STD_BTREE_TABLES,
								  bool *has_out_0, STD_BTREE_SPLIT_RESULT * out_0, StdBTreeHandle *out_1);
static void STD_BTREE_CORRECT_PARENT_LINK(StdBTreeHandle *self);
static void STD_BTREE_INTERNAL_INSERT_FIT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val,
										  StdBTreeNodeRef *edge);
static bool STD_BTREE_INTERNAL_INSERT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val,
									  StdBTreeNodeRef *edge STD_BTREE_TABLES, STD_BTREE_SPLIT_RESULT * result);
static StdBTreeHandle STD_BTREE_INSERT_RECURSING(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * value,
												 STD_BTREE_SPLIT_ROOT(split_root), void *split_root_captures,
												 void (*split_root_drop) (void *) STD_BTREE_TABLES);
static StdBTreeNodeRef STD_BTREE_DESCEND(StdBTreeHandle *self);
static void STD_BTREE_INTO_KV(StdBTreeHandle *self, STD_BTREE_K const **out_0, STD_BTREE_V const **out_1);
static STD_BTREE_V * STD_BTREE_INTO_VAL_MUT(StdBTreeHandle *self);
static void STD_BTREE_SPLIT_LEAF_DATA(StdBTreeHandle *self, StdBTreeLeafNode *new_node STD_BTREE_TABLES,
									  STD_BTREE_K * out_0, STD_BTREE_V * out_1);
static STD_BTREE_SPLIT_RESULT STD_BTREE_LEAF_SPLIT(StdBTreeHandle *self STD_BTREE_TABLES);
static STD_BTREE_SPLIT_RESULT STD_BTREE_INTERNAL_SPLIT(StdBTreeHandle *self STD_BTREE_TABLES);
static void STD_BTREE_SLICE_INSERT_K(STD_BTREE_K * slice, size_t slice_len, size_t idx, STD_BTREE_K * val);
static void STD_BTREE_SLICE_INSERT_V(STD_BTREE_V * slice, size_t slice_len, size_t idx, STD_BTREE_V * val);
static void STD_BTREE_SLICE_INSERT_EDGE(StdBTreeLeafNode **slice, size_t slice_len, size_t idx,
										StdBTreeLeafNode **val);
static StdBTreeSearchResult STD_BTREE_SEARCH_TREE(StdBTreeNodeRef *self, STD_BTREE_K const *key);
static StdBTreeSearchResult STD_BTREE_SEARCH_NODE(StdBTreeNodeRef *self, STD_BTREE_K const *key);
static StdBTreeIndexResult STD_BTREE_FIND_KEY_INDEX(const StdBTreeNodeRef *self, STD_BTREE_K const *key,
													size_t start_index);
#ifdef STD_BTREE_ITERATE
static STD_BTREE_K const *STD_BTREE_REPLACE_KV(StdBTreeHandle *v,
											   STD_BTREE_K const *(*change) (StdBTreeHandle *, StdBTreeHandle *,
																			 STD_BTREE_V const **),
											   STD_BTREE_V const **ret_1);
static StdBTreeLazyLeafRange STD_BTREE_FULL_RANGE_OF(StdBTreeNodeRef root1, StdBTreeNodeRef root2);
static StdBTreeLazyLeafRange STD_BTREE_FULL_RANGE(StdBTreeNodeRef self);
static STD_BTREE_K const *STD_BTREE_LAZY_NEXT_UNCHECKED(StdBTreeLazyLeafRange *self, STD_BTREE_V const **out_1);
static StdBTreeHandle *STD_BTREE_INIT_FRONT(StdBTreeLazyLeafRange *self);
static bool STD_BTREE_LEAF_NEXT_KV(StdBTreeHandle self, StdBTreeHandle *result, StdBTreeNodeRef *error);
static STD_BTREE_K const *STD_BTREE_EDGE_NEXT_UNCHECKED(StdBTreeHandle *self, STD_BTREE_V const **out_1);
static STD_BTREE_K const *STD_BTREE_EDGE_NEXT_UNCHECKED_CLOSURE_0(StdBTreeHandle *leaf_edge, StdBTreeHandle *out_0,
																  STD_BTREE_V const **out_1_1);
static StdBTreeHandle STD_BTREE_FIRST_LEAF_EDGE(StdBTreeNodeRef self);
static StdBTreeHandle STD_BTREE_NEXT_LEAF_EDGE(StdBTreeHandle self);
static StdBTreeHandle STD_BTREE_FIRST_EDGE(StdBTreeNodeRef self);
static bool STD_BTREE_RIGHT_KV(StdBTreeHandle self, StdBTreeHandle *result, StdBTreeHandle *error);
static StdBTreeHandle STD_BTREE_RIGHT_EDGE(StdBTreeHandle self);
#endif

/* map.rs */

/* The instance's BTreeMap::get: the key's value, or NULL. */
STD_BTREE_DEF STD_BTREE_V const *
STD_BTREE_GET(const StdBTreeMap *self, STD_BTREE_K const *key)
{
	StdBTreeNodeRef root_node;
	StdBTreeSearchResult tmp_1;
	STD_BTREE_K const *k;
	STD_BTREE_V const *v;

	if (self->root.node == NULL)
		return NULL;
	root_node = self->root;
	tmp_1 = STD_BTREE_SEARCH_TREE(&root_node, key);
	switch (tmp_1.tag)
	{
		case STD_BTREE_SEARCH_RESULT_FOUND:
			STD_BTREE_INTO_KV(&tmp_1.u.found, &k, &v);
			return v;
		case STD_BTREE_SEARCH_RESULT_GO_DOWN:
			return NULL;
	}
	pg_unreachable();
}

/*
 * The instance's BTreeMap::entry: occupied by the key's handle, or vacant
 * with the key and where it goes. A found key is dropped, as is the key when
 * its Ord panics.
 */
STD_BTREE_DEF STD_BTREE_ENTRY
STD_BTREE_MAP_ENTRY(StdBTreeMap *self, STD_BTREE_K * key)
{
	STD_BTREE_ENTRY result;
	StdBTreeNodeRef tmp_1;
	StdBTreeSearchResult tmp_2;
	StdUnwind	unwind;

	if (self->root.node == NULL)
	{
		result.tag = STD_BTREE_ENTRY_VACANT;
		result.u.vacant.key = *key;
		result.u.vacant.has_handle = false;
		result.u.vacant.dormant_map = self;
		return result;
	}
	tmp_1 = self->root;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		tmp_2 = STD_BTREE_SEARCH_TREE(&tmp_1, key);
		std_unwind_pop(&unwind);
	}
	else
	{
		STD_BTREE_KEY_DROP(key);
		std_unwind_resume(&unwind);
	}
	switch (tmp_2.tag)
	{
		case STD_BTREE_SEARCH_RESULT_FOUND:
			result.tag = STD_BTREE_ENTRY_OCCUPIED;
			result.u.occupied.handle = tmp_2.u.found;
			result.u.occupied.dormant_map = self;
			STD_BTREE_KEY_DROP(key);
			return result;
		case STD_BTREE_SEARCH_RESULT_GO_DOWN:
			result.tag = STD_BTREE_ENTRY_VACANT;
			result.u.vacant.key = *key;
			result.u.vacant.has_handle = true;
			result.u.vacant.handle = tmp_2.u.go_down;
			result.u.vacant.dormant_map = self;
			return result;
	}
	pg_unreachable();
}

#ifdef STD_BTREE_ITERATE

/*
 * The instance's BTreeMap::iter: an iterator over the map's entries in order,
 * over the full range of its root if it has one.
 */
STD_BTREE_DEF StdBTreeIter
STD_BTREE_ITER(const StdBTreeMap *self)
{
	StdBTreeIter result = {0};

	if (self->root.node != NULL)
	{
		result.range = STD_BTREE_FULL_RANGE(self->root);
		result.length = self->length;
	}
	return result;
}

/* The instance's BTreeMap::keys: an iterator over the map's keys in order, over its iter. */
STD_BTREE_DEF StdBTreeKeys
STD_BTREE_MAP_KEYS(const StdBTreeMap *self)
{
	StdBTreeKeys result = {STD_BTREE_ITER(self)};

	return result;
}

/* The instance's Iter's Iterator::next: the next entry in order, or none once the iterator has given them all. */
STD_BTREE_DEF bool
STD_BTREE_ITER_NEXT(StdBTreeIter *self, STD_BTREE_K const **out_0, STD_BTREE_V const **out_1)
{
	if (self->length == 0)
		return false;
	self->length -= 1;
	*out_0 = STD_BTREE_LAZY_NEXT_UNCHECKED(&self->range, out_1);
	return true;
}

/* The instance's Keys' Iterator::next: the next key in order, its inner Iter's next's key. */
STD_BTREE_DEF bool
STD_BTREE_KEYS_NEXT(StdBTreeKeys *self, STD_BTREE_K const **result)
{
	STD_BTREE_V const *v;

	return STD_BTREE_ITER_NEXT(&self->inner, result, &v);
}
#endif

/* map/entry.rs */

/*
 * The instance's Entry::or_default: an occupied entry's value, or V's
 * default inserted at a vacant one, whose key is dropped if the default
 * panics.
 */
STD_BTREE_DEF STD_BTREE_V *
STD_BTREE_OR_DEFAULT(STD_BTREE_ENTRY * self STD_BTREE_TABLES)
{
	STD_BTREE_V tmp_1;
	StdUnwind	unwind;

	switch (self->tag)
	{
		case STD_BTREE_ENTRY_OCCUPIED:
			return STD_BTREE_INTO_MUT(&self->u.occupied);
		case STD_BTREE_ENTRY_VACANT:
			STD_UNWIND_PUSH(&unwind);
			if (sigsetjmp(unwind.jump, 0) == 0)
			{
				STD_BTREE_VAL_DEFAULT(&tmp_1);
				std_unwind_pop(&unwind);
			}
			else
			{
				STD_BTREE_KEY_DROP(&self->u.vacant.key);
				std_unwind_resume(&unwind);
			}
			return STD_BTREE_VACANT_INSERT(&self->u.vacant, &tmp_1 STD_BTREE_TABLE_ARGS);
	}
	pg_unreachable();
}

/* The instance's VacantEntry::insert: the entry's key and a value inserted, and the value's place. */
STD_BTREE_DEF STD_BTREE_V *
STD_BTREE_VACANT_INSERT(STD_BTREE_VACANT_ENTRY * self, STD_BTREE_V * value STD_BTREE_TABLES)
{
	StdBTreeOccupiedEntry tmp_1 = STD_BTREE_INSERT_ENTRY(self, value STD_BTREE_TABLE_ARGS);

	return STD_BTREE_INTO_MUT(&tmp_1);
}

/*
 * The instance's VacantEntry::insert_entry: the entry's key and a value
 * inserted, into a new root leaf if the map has none, and the occupied
 * entry. The value and the key are dropped if the new leaf's allocation
 * fails.
 */
STD_BTREE_DEF StdBTreeOccupiedEntry
STD_BTREE_INSERT_ENTRY(STD_BTREE_VACANT_ENTRY * self, STD_BTREE_V * value STD_BTREE_TABLES)
{
	StdBTreeHandle handle;
	StdBTreeOccupiedEntry result;

	if (!self->has_handle)
	{
		StdBTreeMap *map = self->dormant_map;
		StdBTreeNodeRef tmp_1;
		StdBTreeNodeRef *root;
		StdUnwind	unwind;

		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			tmp_1 = STD_BTREE_NEW_LEAF();
			std_unwind_pop(&unwind);
		}
		else
		{
			STD_BTREE_VAL_DROP(value);
			STD_BTREE_KEY_DROP(&self->key);
			std_unwind_resume(&unwind);
		}
		map->root = tmp_1;
		if (map->root.node == NULL)
			std_hint_unreachable_unchecked(STD_BTREE_OPTION_FILE, 1725, 32);
		root = &map->root;
		handle = STD_BTREE_PUSH_WITH_HANDLE(root, &self->key, value STD_BTREE_TABLE_ARGS);
	}
	else
	{
		StdBTreeHandle tmp_2 = self->handle;

		handle = STD_BTREE_INSERT_RECURSING(&tmp_2, &self->key, value, STD_BTREE_INSERT_ENTRY_CLOSURE_0,
											&self->dormant_map, NULL STD_BTREE_TABLE_ARGS);
	}
	self->dormant_map->length += 1;
	result.handle = handle;
	result.dormant_map = self->dormant_map;
	return result;
}

/*
 * The instance's VacantEntry::insert_entry::{closure#0}, insert_recursing's
 * split_root: the root pushed down a level under a new internal root that
 * takes the split's key, value, and right node. Its captures are the
 * entry's dormant_map, by reference; the split's key and value are dropped
 * if the map has no root.
 */
static void
STD_BTREE_INSERT_ENTRY_CLOSURE_0(void *captures, STD_BTREE_SPLIT_RESULT * ins STD_BTREE_TABLES)
{
	StdBTreeMap *map = *(StdBTreeMap **) captures;
	StdBTreeNodeRef *root;
	StdBTreeNodeRef tmp_1;
	StdUnwind	unwind;

	if (map->root.node == NULL)
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
			std_option_unwrap_failed(STD_BTREE_ENTRY_FILE, 465, 46);
		STD_BTREE_KEY_DROP(&ins->kv.f0);
		STD_BTREE_VAL_DROP(&ins->kv.f1);
		std_unwind_resume(&unwind);
	}
	root = &map->root;
	tmp_1 = STD_BTREE_PUSH_INTERNAL_LEVEL(root);
	STD_BTREE_INTERNAL_PUSH(&tmp_1, &ins->kv.f0, &ins->kv.f1, &ins->right STD_BTREE_TABLE_ARGS);
}

/* The instance's OccupiedEntry::into_mut: the entry's value. */
STD_BTREE_DEF STD_BTREE_V *
STD_BTREE_INTO_MUT(StdBTreeOccupiedEntry *self)
{
	return STD_BTREE_INTO_VAL_MUT(&self->handle);
}

/* mem.rs */

/* take_mut at Root<K, V>: v replaced by change of its value, aborting if change panics. */
static void
STD_BTREE_TAKE_MUT(StdBTreeNodeRef *v, StdBTreeNodeRef (*change) (void *, StdBTreeNodeRef *),
				   void *change_captures, void (*change_drop) (void *))
{
	struct
	{
		StdBTreeNodeRef (*change) (void *, StdBTreeNodeRef *);
		void	   *change_captures;
		void		(*change_drop) (void *);
	}			captures;

	captures.change = change;
	captures.change_captures = change_captures;
	captures.change_drop = change_drop;
	STD_BTREE_REPLACE(v, STD_BTREE_TAKE_MUT_CLOSURE_0, &captures, STD_BTREE_TAKE_MUT_CLOSURE_0_DROP);
}

/* take_mut::{closure#0} at Root<K, V>: change of the value, and the unit. Its captures are change, moved. */
static void
STD_BTREE_TAKE_MUT_CLOSURE_0(void *captures, StdBTreeNodeRef *value, StdBTreeNodeRef *out_0)
{
	struct
	{
		StdBTreeNodeRef (*change) (void *, StdBTreeNodeRef *);
		void	   *change_captures;
		void		(*change_drop) (void *);
	}		   *c = captures;

	*out_0 = c->change(c->change_captures, value);
}

/* take_mut::{closure#0}'s drop at Root<K, V>: its captured change's. */
static void
STD_BTREE_TAKE_MUT_CLOSURE_0_DROP(void *captures)
{
	struct
	{
		StdBTreeNodeRef (*change) (void *, StdBTreeNodeRef *);
		void	   *change_captures;
		void		(*change_drop) (void *);
	}		   *c = captures;

	if (c->change_drop != NULL)
		c->change_drop(c->change_captures);
}

/*
 * replace at Root<K, V> and (): v replaced by change of its value, whose
 * PanicGuard aborts if change panics.
 */
static void
STD_BTREE_REPLACE(StdBTreeNodeRef *v, void (*change) (void *, StdBTreeNodeRef *, StdBTreeNodeRef *),
				  void *change_captures, void (*change_drop) (void *))
{
	StdBTreeNodeRef value;
	StdBTreeNodeRef new_value;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		value = *v;
		change(change_captures, &value, &new_value);
		std_unwind_pop(&unwind);
	}
	else
		std_intrinsics_abort();
	*v = new_value;
}

#ifdef STD_BTREE_ITERATE

/* The state of replace at a leaf edge and (&K, &V) across its pad: its arguments and its results. */
typedef struct STD_BTREE_REPLACE_KV_FRAME_0
{
	StdBTreeHandle *v;
	STD_BTREE_K const *(*change) (StdBTreeHandle *, StdBTreeHandle *, STD_BTREE_V const **);
	STD_BTREE_V const **ret_1;
	StdBTreeHandle new_value;
	STD_BTREE_K const *ret_0;
}			STD_BTREE_REPLACE_KV_FRAME_0;

/* The body of replace at a leaf edge and (&K, &V): the change of the edge's value. */
static void
STD_BTREE_REPLACE_KV_BODY_0(void *frame_)
{
	STD_BTREE_REPLACE_KV_FRAME_0 *frame = frame_;
	StdBTreeHandle value = *frame->v;

	frame->ret_0 = frame->change(&value, &frame->new_value, frame->ret_1);
}

/* The pad of replace at a leaf edge and (&K, &V): its PanicGuard's drop, which aborts. */
static void
STD_BTREE_REPLACE_KV_PAD_0(void *frame_)
{
	std_intrinsics_abort();
}

/*
 * replace at Handle<NodeRef<Immut, K, V, Leaf>, Edge> and (&K, &V): v
 * replaced by change of its value, whose PanicGuard aborts if change
 * panics, and change's key returned and its value written to ret_1.
 */
static STD_BTREE_K const *
STD_BTREE_REPLACE_KV(StdBTreeHandle *v,
					 STD_BTREE_K const *(*change) (StdBTreeHandle *, StdBTreeHandle *, STD_BTREE_V const **),
					 STD_BTREE_V const **ret_1)
{
	STD_BTREE_REPLACE_KV_FRAME_0 frame = {.v = v, .change = change, .ret_1 = ret_1};

	std_landing(&frame, STD_BTREE_REPLACE_KV_BODY_0, STD_BTREE_REPLACE_KV_PAD_0);
	*v = frame.new_value;
	return frame.ret_0;
}

/* navigate.rs */

/* The instance's full_range at Immut: a lazy range over two roots, neither descended yet. */
static StdBTreeLazyLeafRange
STD_BTREE_FULL_RANGE_OF(StdBTreeNodeRef root1, StdBTreeNodeRef root2)
{
	StdBTreeLazyLeafRange result;

	result.has_front = true;
	result.front.tag = STD_BTREE_LAZY_LEAF_HANDLE_ROOT;
	result.front.u.root = root1;
	result.has_back = true;
	result.back.tag = STD_BTREE_LAZY_LEAF_HANDLE_ROOT;
	result.back.u.root = root2;
	return result;
}

/* The instance's NodeRef<Immut, K, V, LeafOrInternal>::full_range: the lazy range over all of the tree. */
static StdBTreeLazyLeafRange
STD_BTREE_FULL_RANGE(StdBTreeNodeRef self)
{
	return STD_BTREE_FULL_RANGE_OF(self, self);
}

/* The instance's LazyLeafRange<Immut, K, V>::next_unchecked: the front's next key and value, the front first descended. */
static STD_BTREE_K const *
STD_BTREE_LAZY_NEXT_UNCHECKED(StdBTreeLazyLeafRange *self, STD_BTREE_V const **out_1)
{
	StdBTreeHandle *front = STD_BTREE_INIT_FRONT(self);

	if (front == NULL)
		std_option_unwrap_failed(STD_BTREE_NAVIGATE_FILE, 161, 36);
	return STD_BTREE_EDGE_NEXT_UNCHECKED(front, out_1);
}

/*
 * The instance's LazyLeafRange<Immut, K, V>::init_front: the front's leaf
 * edge, a root front descended to its first leaf edge, or none.
 */
static StdBTreeHandle *
STD_BTREE_INIT_FRONT(StdBTreeLazyLeafRange *self)
{
	if (self->has_front && self->front.tag == STD_BTREE_LAZY_LEAF_HANDLE_ROOT)
	{
		StdBTreeHandle edge = STD_BTREE_FIRST_LEAF_EDGE(self->front.u.root);

		self->front.tag = STD_BTREE_LAZY_LEAF_HANDLE_EDGE;
		self->front.u.edge = edge;
	}
	if (!self->has_front)
		return NULL;
	if (self->front.tag == STD_BTREE_LAZY_LEAF_HANDLE_ROOT)
		std_hint_unreachable_unchecked(STD_BTREE_NAVIGATE_FILE, 231, 55);
	return &self->front.u.edge;
}

/*
 * The instance's Handle<NodeRef<BorrowType, K, V, Leaf>, Edge>::next_kv: the
 * key and value right of the edge, climbing past the nodes it ends, or the
 * root when the tree has none.
 */
static bool
STD_BTREE_LEAF_NEXT_KV(StdBTreeHandle self, StdBTreeHandle *result, StdBTreeNodeRef *error)
{
	StdBTreeHandle edge = self;

	for (;;)
	{
		StdBTreeHandle last_edge;
		StdBTreeNodeRef node;

		if (STD_BTREE_RIGHT_KV(edge, result, &last_edge))
			return true;
		node = last_edge.node;
		if (!STD_BTREE_ASCEND(&node, &edge, error))
			return false;
	}
}

/*
 * The instance's Handle<NodeRef<Immut, K, V, Leaf>, Edge>::next_unchecked:
 * the next key and value, the edge moved past them.
 */
static STD_BTREE_K const *
STD_BTREE_EDGE_NEXT_UNCHECKED(StdBTreeHandle *self, STD_BTREE_V const **out_1)
{
	return STD_BTREE_REPLACE_KV(self, STD_BTREE_EDGE_NEXT_UNCHECKED_CLOSURE_0, out_1);
}

/* next_unchecked's closure: the edge's next key and value, and the leaf edge after them. */
static STD_BTREE_K const *
STD_BTREE_EDGE_NEXT_UNCHECKED_CLOSURE_0(StdBTreeHandle *leaf_edge, StdBTreeHandle *out_0, STD_BTREE_V const **out_1_1)
{
	StdBTreeHandle kv;
	StdBTreeNodeRef root;
	STD_BTREE_K const *k;

	if (!STD_BTREE_LEAF_NEXT_KV(*leaf_edge, &kv, &root))
		std_option_unwrap_failed(STD_BTREE_NAVIGATE_FILE, 534, 47);
	*out_0 = STD_BTREE_NEXT_LEAF_EDGE(kv);
	STD_BTREE_INTO_KV(&kv, &k, out_1_1);
	return k;
}

/* The instance's NodeRef<BorrowType, K, V, LeafOrInternal>::first_leaf_edge: the leftmost leaf's first edge. */
static StdBTreeHandle
STD_BTREE_FIRST_LEAF_EDGE(StdBTreeNodeRef self)
{
	StdBTreeNodeRef node = self;

	for (;;)
	{
		StdBTreeHandle edge;

		if (node.height == 0)
			return STD_BTREE_FIRST_EDGE(node);
		edge = STD_BTREE_FIRST_EDGE(node);
		node = STD_BTREE_DESCEND(&edge);
	}
}

/*
 * The instance's Handle<NodeRef<BorrowType, K, V, LeafOrInternal>,
 * KV>::next_leaf_edge: the leaf edge right after the key and value, the
 * first leaf edge of the subtree right of an internal one.
 */
static StdBTreeHandle
STD_BTREE_NEXT_LEAF_EDGE(StdBTreeHandle self)
{
	StdBTreeHandle next_internal_edge;

	if (self.node.height == 0)
		return STD_BTREE_RIGHT_EDGE(self);
	next_internal_edge = STD_BTREE_RIGHT_EDGE(self);
	return STD_BTREE_FIRST_LEAF_EDGE(STD_BTREE_DESCEND(&next_internal_edge));
}
#endif

/* node.rs */

/* The instance's LeafNode::init: a node with no parent and no keys, its other fields left uninitialized. */
static void
STD_BTREE_LEAF_INIT(StdBTreeLeafNode *this)
{
	STD_BTREE_PARENT(this) = NULL;
	STD_BTREE_LEN_FIELD(this) = 0;
}

/* The instance's LeafNode::new: a new boxed leaf. */
static StdBTreeLeafNode *
STD_BTREE_LEAF_NEW(void)
{
	StdBTreeLeafNode *leaf = std_box_new_uninit_Checked(STD_BTREE_LEAF_SIZE, STD_BTREE_LEAF_ALIGN);

	STD_BTREE_LEAF_INIT(leaf);
	return leaf;
}

/* The instance's InternalNode::new: a new boxed internal node, its edges uninitialized. */
static StdBTreeInternalNode *
STD_BTREE_INTERNAL_NEW(void)
{
	StdBTreeInternalNode *node = std_box_new_uninit_Checked(STD_BTREE_INTERNAL_SIZE, STD_BTREE_INTERNAL_ALIGN);

	STD_BTREE_LEAF_INIT((StdBTreeLeafNode *) node);
	return node;
}

/* The instance's NodeRef<Owned, K, V, Leaf>::new_leaf: a new leaf at height 0. */
static StdBTreeNodeRef
STD_BTREE_NEW_LEAF(void)
{
	return STD_BTREE_FROM_NEW_LEAF(STD_BTREE_LEAF_NEW());
}

/* The instance's NodeRef<Owned, K, V, Leaf>::from_new_leaf: a boxed leaf at height 0. */
static StdBTreeNodeRef
STD_BTREE_FROM_NEW_LEAF(StdBTreeLeafNode *leaf)
{
	StdBTreeNodeRef result;

	std_NonNull_new_unchecked_check(leaf, STD_BTREE_BOXED_FILE, 1733, 19);
	result.height = 0;
	result.node = leaf;
	return result;
}

/*
 * The instance's NodeRef<Owned, K, V, Internal>::new_internal: a new
 * internal node over a child, a level above it. The new node is freed if
 * the height overflows.
 */
static StdBTreeNodeRef
STD_BTREE_NEW_INTERNAL(StdBTreeNodeRef *child)
{
	StdBTreeInternalNode *new_node = STD_BTREE_INTERNAL_NEW();
	size_t		tmp_1;
	StdUnwind	unwind;

	STD_BTREE_EDGES_OF(new_node)[0] = child->node;
	tmp_1 = child->height + 1;
	if (tmp_1 == 0)
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
			std_option_unwrap_failed(STD_BTREE_NODE_FILE, 240, 77);
		std_free(new_node, STD_BTREE_INTERNAL_SIZE, STD_BTREE_INTERNAL_ALIGN);
		std_unwind_resume(&unwind);
	}
	return STD_BTREE_FROM_NEW_INTERNAL(new_node, tmp_1);
}

/* The instance's NodeRef<Owned, K, V, Internal>::from_new_internal: a boxed internal node at a height, its children pointed at it. */
static StdBTreeNodeRef
STD_BTREE_FROM_NEW_INTERNAL(StdBTreeInternalNode *internal, size_t height)
{
	StdBTreeNodeRef this;

	std_NonNull_new_unchecked_check(internal, STD_BTREE_BOXED_FILE, 1733, 19);
	this.height = height;
	this.node = (StdBTreeLeafNode *) internal;
	STD_BTREE_CORRECT_ALL_CHILDRENS_PARENT_LINKS(&this);
	return this;
}

/* The instance's NodeRef<BorrowType, K, V, Internal>::from_internal: a parent link unpacked at a height. */
static StdBTreeNodeRef
STD_BTREE_FROM_INTERNAL(StdBTreeInternalNode *node, size_t height)
{
	StdBTreeNodeRef result;

	result.height = height;
	result.node = (StdBTreeLeafNode *) node;
	return result;
}

/* The instance's NodeRef<BorrowType, K, V, Internal>::as_internal_ptr: the internal node. */
static StdBTreeInternalNode *
STD_BTREE_AS_INTERNAL_PTR(const StdBTreeNodeRef *this)
{
	return (StdBTreeInternalNode *) this->node;
}

/* The instance's NodeRef::len: the node's number of keys and values. */
static size_t
STD_BTREE_LEN(const StdBTreeNodeRef *self)
{
	return (size_t) STD_BTREE_LEN_FIELD(self->node);
}

/*
 * The instance's NodeRef::ascend: the parent's edge to the node, or the
 * node back if it has no parent.
 */
static bool
STD_BTREE_ASCEND(StdBTreeNodeRef *self, StdBTreeHandle *result, StdBTreeNodeRef *error)
{
	StdBTreeLeafNode *leaf_ptr = self->node;

	if (STD_BTREE_PARENT(leaf_ptr) != NULL)
	{
		result->node = STD_BTREE_FROM_INTERNAL(STD_BTREE_PARENT(leaf_ptr), self->height + 1);
		result->idx = (size_t) STD_BTREE_PARENT_IDX(leaf_ptr);
		return true;
	}
	*error = *self;
	return false;
}

/* The instance's NodeRef<Immut, K, V, Type>::keys: the node's keys. */
static STD_BTREE_K const *
STD_BTREE_KEYS(const StdBTreeNodeRef *self, size_t *result_len)
{
	StdBTreeLeafNode *leaf = self->node;
	size_t		tmp_1 = (size_t) STD_BTREE_LEN_FIELD(leaf);

	std_slice_get_unchecked_range_check(0, tmp_1, STD_BTREE_CAPACITY, STD_SLICE_INDEX_FILE, 492, 32);
	*result_len = tmp_1;
	return STD_BTREE_KEYS_OF(leaf);
}

/* The instance's NodeRef<Mut, K, V, Type>::key_area_mut at an index: a key's place. */
static STD_BTREE_K *
STD_BTREE_KEY_AREA_MUT(StdBTreeNodeRef *self, size_t index)
{
	std_slice_get_unchecked_mut_check(index, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 494, 57);
	return STD_BTREE_KEYS_OF(self->node) + index;
}

/*
 * The instance's NodeRef<Mut, K, V, Type>::key_area_mut at ..index_end: the
 * keys' places before an index, written in its caller, where shipped MIR
 * inlines it.
 */
#define STD_BTREE_KEY_AREA_MUT_TO(self, index_end) \
	(std_slice_get_unchecked_mut_range_check(0, (index_end), STD_BTREE_CAPACITY, STD_SLICE_INDEX_FILE, 498, 32), \
	 STD_BTREE_KEYS_OF((self)->node))

/* The instance's NodeRef<Mut, K, V, Type>::key_area_mut at a range: the keys' places in it. */
static STD_BTREE_K *
STD_BTREE_KEY_AREA_MUT_RANGE(StdBTreeNodeRef *self, size_t index_start, size_t index_end)
{
	std_slice_get_unchecked_mut_range_check(index_start, index_end, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 494, 57);
	return STD_BTREE_KEYS_OF(self->node) + index_start;
}

/* The instance's NodeRef<Mut, K, V, Type>::val_area_mut at an index: a value's place. */
static STD_BTREE_V *
STD_BTREE_VAL_AREA_MUT(StdBTreeNodeRef *self, size_t index)
{
	std_slice_get_unchecked_mut_check(index, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 508, 57);
	return STD_BTREE_VAL_AT(STD_BTREE_VALS_OF(self->node), index);
}

/*
 * The instance's NodeRef<Mut, K, V, Type>::val_area_mut at ..index_end: the
 * values' places before an index, written in its caller, where shipped MIR
 * inlines it.
 */
#define STD_BTREE_VAL_AREA_MUT_TO(self, index_end) \
	(std_slice_get_unchecked_mut_range_check(0, (index_end), STD_BTREE_CAPACITY, STD_SLICE_INDEX_FILE, 498, 32), \
	 STD_BTREE_VALS_OF((self)->node))

/* The instance's NodeRef<Mut, K, V, Type>::val_area_mut at a range: the values' places in it. */
static STD_BTREE_V *
STD_BTREE_VAL_AREA_MUT_RANGE(StdBTreeNodeRef *self, size_t index_start, size_t index_end)
{
	std_slice_get_unchecked_mut_range_check(index_start, index_end, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 508, 57);
	return STD_BTREE_VAL_AT(STD_BTREE_VALS_OF(self->node), index_start);
}

/* The instance's NodeRef<Mut, K, V, Internal>::edge_area_mut at an index: an edge's place. */
static StdBTreeLeafNode **
STD_BTREE_EDGE_AREA_MUT(StdBTreeNodeRef *self, size_t index)
{
	std_slice_get_unchecked_mut_check(index, STD_BTREE_EDGES_LEN, STD_BTREE_NODE_FILE, 524, 62);
	return STD_BTREE_EDGES_OF(self->node) + index;
}

/*
 * The instance's NodeRef<Mut, K, V, Internal>::edge_area_mut at ..index_end:
 * the edges' places before an index, written in its caller, where shipped MIR
 * inlines it.
 */
#define STD_BTREE_EDGE_AREA_MUT_TO(self, index_end) \
	(std_slice_get_unchecked_mut_range_check(0, (index_end), STD_BTREE_EDGES_LEN, STD_SLICE_INDEX_FILE, 498, 32), \
	 STD_BTREE_EDGES_OF((self)->node))

/* The instance's NodeRef<Mut, K, V, Internal>::edge_area_mut at a range: the edges' places in it. */
static StdBTreeLeafNode **
STD_BTREE_EDGE_AREA_MUT_RANGE(StdBTreeNodeRef *self, size_t index_start, size_t index_end)
{
	std_slice_get_unchecked_mut_range_check(index_start, index_end, STD_BTREE_EDGES_LEN, STD_BTREE_NODE_FILE, 524, 62);
	return STD_BTREE_EDGES_OF(self->node) + index_start;
}

/* The instance's NodeRef<Mut, K, V, Type>::len_mut: the node's length's place. */
static uint16 *
STD_BTREE_LEN_MUT(StdBTreeNodeRef *self)
{
	return &STD_BTREE_LEN_FIELD(self->node);
}

/* The instance's NodeRef<Mut, K, V, Internal>::correct_childrens_parent_links over a range: those children pointed at the node. */
static void
STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS(StdBTreeNodeRef *self, size_t range_start, size_t range_end)
{
	for (size_t i = range_start; i < range_end; i++)
	{
		StdBTreeHandle tmp_1;

		tmp_1.node = *self;
		tmp_1.idx = i;
		STD_BTREE_CORRECT_PARENT_LINK(&tmp_1);
	}
}

/* The instance's NodeRef<Mut, K, V, Internal>::correct_childrens_parent_links over an inclusive range. */
static void
STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS_INCLUSIVE(StdBTreeNodeRef *self, size_t range_start, size_t range_end)
{
	for (size_t i = range_start; i <= range_end; i++)
	{
		StdBTreeHandle tmp_1;

		tmp_1.node = *self;
		tmp_1.idx = i;
		STD_BTREE_CORRECT_PARENT_LINK(&tmp_1);
	}
}

/* The instance's NodeRef<Mut, K, V, Internal>::correct_all_childrens_parent_links: every child pointed at the node. */
static void
STD_BTREE_CORRECT_ALL_CHILDRENS_PARENT_LINKS(StdBTreeNodeRef *self)
{
	size_t		len = STD_BTREE_LEN(self);

	STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS_INCLUSIVE(self, 0, len);
}

/* The instance's NodeRef<Mut, K, V, LeafOrInternal>::set_parent_link: the node's parent and its edge there. */
static void
STD_BTREE_SET_PARENT_LINK(StdBTreeNodeRef *self, StdBTreeInternalNode *parent, size_t parent_idx)
{
	StdBTreeLeafNode *leaf = self->node;

	STD_BTREE_PARENT(leaf) = parent;
	STD_BTREE_PARENT_IDX(leaf) = (uint16) parent_idx;
}

/*
 * The instance's NodeRef<Owned, K, V, LeafOrInternal>::push_internal_level:
 * a new internal root over the old one, a level higher.
 */
static StdBTreeNodeRef
STD_BTREE_PUSH_INTERNAL_LEVEL(StdBTreeNodeRef *self)
{
	StdBTreeNodeRef result;

	STD_BTREE_TAKE_MUT(self, STD_BTREE_PUSH_INTERNAL_LEVEL_CLOSURE_0, NULL, NULL);
	result.height = self->height;
	result.node = self->node;
	return result;
}

/* The instance's NodeRef<Owned, K, V, LeafOrInternal>::push_internal_level::{closure#0}: the old root under a new one. */
static StdBTreeNodeRef
STD_BTREE_PUSH_INTERNAL_LEVEL_CLOSURE_0(void *captures, StdBTreeNodeRef *old_root)
{
	return STD_BTREE_NEW_INTERNAL(old_root);
}

/*
 * The instance's NodeRef<Mut, K, V, Leaf>::push_with_handle: a key and value
 * added at the leaf's end, and their handle. They are dropped if the leaf is
 * full.
 */
static StdBTreeHandle
STD_BTREE_PUSH_WITH_HANDLE(StdBTreeNodeRef *self, STD_BTREE_K * key, STD_BTREE_V * val STD_BTREE_TABLES)
{
	uint16	   *len = STD_BTREE_LEN_MUT(self);
	size_t		idx = (size_t) *len;
	StdBTreeHandle result;
	StdUnwind	unwind;

	if (!(idx < STD_BTREE_CAPACITY))
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
			std_panic(STD_BTREE_NODE_FILE, 670, 9, "assertion failed: idx < CAPACITY",
					  sizeof("assertion failed: idx < CAPACITY") - 1);
		STD_BTREE_VAL_DROP(val);
		STD_BTREE_KEY_DROP(key);
		std_unwind_resume(&unwind);
	}
	*len += 1;
	*STD_BTREE_KEY_AREA_MUT(self, idx) = *key;
	STD_BTREE_VAL_MOVE(STD_BTREE_VAL_AREA_MUT(self, idx), val);
	result.node = *self;
	result.idx = idx;
	return result;
}

/*
 * The instance's NodeRef<Mut, K, V, Internal>::push: a key, value, and right
 * edge added at the node's end. The key and value are dropped if the edge's
 * height is wrong or the node is full.
 */
static void
STD_BTREE_INTERNAL_PUSH(StdBTreeNodeRef *self, STD_BTREE_K * key, STD_BTREE_V * val,
						StdBTreeNodeRef *edge STD_BTREE_TABLES)
{
	uint16	   *len;
	size_t		idx;
	StdBTreeHandle tmp_1;
	StdUnwind	unwind;

	if (!(edge->height == self->height - 1))
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
			std_panic(STD_BTREE_NODE_FILE, 694, 9, "assertion failed: edge.height == self.height - 1",
					  sizeof("assertion failed: edge.height == self.height - 1") - 1);
		STD_BTREE_VAL_DROP(val);
		STD_BTREE_KEY_DROP(key);
		std_unwind_resume(&unwind);
	}
	len = STD_BTREE_LEN_MUT(self);
	idx = (size_t) *len;
	if (!(idx < STD_BTREE_CAPACITY))
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
			std_panic(STD_BTREE_NODE_FILE, 698, 9, "assertion failed: idx < CAPACITY",
					  sizeof("assertion failed: idx < CAPACITY") - 1);
		STD_BTREE_VAL_DROP(val);
		STD_BTREE_KEY_DROP(key);
		std_unwind_resume(&unwind);
	}
	*len += 1;
	*STD_BTREE_KEY_AREA_MUT(self, idx) = *key;
	STD_BTREE_VAL_MOVE(STD_BTREE_VAL_AREA_MUT(self, idx), val);
	*STD_BTREE_EDGE_AREA_MUT(self, idx + 1) = edge->node;
	tmp_1.node = *self;
	tmp_1.idx = idx + 1;
	STD_BTREE_CORRECT_PARENT_LINK(&tmp_1);
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Leaf>, Edge>::insert_fit: a key
 * and value inserted at the edge of a leaf with room, and their handle.
 */
static StdBTreeHandle
STD_BTREE_LEAF_INSERT_FIT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val)
{
	size_t		new_len = STD_BTREE_LEN(&self->node) + 1;
	StdBTreeHandle result;

	STD_BTREE_SLICE_INSERT_K(STD_BTREE_KEY_AREA_MUT_TO(&self->node, new_len), new_len, self->idx, key);
	STD_BTREE_SLICE_INSERT_V(STD_BTREE_VAL_AREA_MUT_TO(&self->node, new_len), new_len, self->idx, val);
	*STD_BTREE_LEN_MUT(&self->node) = (uint16) new_len;
	result.node = self->node;
	result.idx = self->idx;
	return result;
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Leaf>, Edge>::insert: a key and
 * value inserted at the edge, splitting a full leaf at splitpoint, the
 * split if there was one, and the inserted pair's handle. The key and value
 * are dropped if the split panics.
 */
static void
STD_BTREE_LEAF_INSERT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val STD_BTREE_TABLES,
					  bool *has_out_0, STD_BTREE_SPLIT_RESULT * out_0, StdBTreeHandle *out_1)
{
	size_t		middle_kv_idx;
	StdBTreeLeftOrRight insertion;
	StdBTreeHandle middle;
	STD_BTREE_SPLIT_RESULT result;
	StdBTreeHandle insertion_edge;
	StdUnwind	unwind;

	if (STD_BTREE_LEN(&self->node) < STD_BTREE_CAPACITY)
	{
		*has_out_0 = false;
		*out_1 = STD_BTREE_LEAF_INSERT_FIT(self, key, val);
		return;
	}
	std_btree_splitpoint(self->idx, &middle_kv_idx, &insertion);
	middle.node = self->node;
	middle.idx = middle_kv_idx;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		result = STD_BTREE_LEAF_SPLIT(&middle STD_BTREE_TABLE_ARGS);
		std_unwind_pop(&unwind);
	}
	else
	{
		STD_BTREE_VAL_DROP(val);
		STD_BTREE_KEY_DROP(key);
		std_unwind_resume(&unwind);
	}
	switch (insertion.tag)
	{
		case STD_BTREE_LEFT_OR_RIGHT_LEFT:
			insertion_edge.node = result.left;
			insertion_edge.idx = insertion.u.left;
			break;
		case STD_BTREE_LEFT_OR_RIGHT_RIGHT:
			insertion_edge.node = result.right;
			insertion_edge.idx = insertion.u.right;
			break;
	}
	*out_1 = STD_BTREE_LEAF_INSERT_FIT(&insertion_edge, key, val);
	*has_out_0 = true;
	*out_0 = result;
}

/* The instance's Handle<NodeRef<Mut, K, V, Internal>, Edge>::correct_parent_link: the edge's child pointed at the node. */
static void
STD_BTREE_CORRECT_PARENT_LINK(StdBTreeHandle *self)
{
	StdBTreeInternalNode *ptr = STD_BTREE_AS_INTERNAL_PTR(&self->node);
	size_t		idx;
	StdBTreeNodeRef child;

	std_NonNull_new_unchecked_check(ptr, STD_BTREE_NODE_FILE, 991, 28);
	idx = self->idx;
	child = STD_BTREE_DESCEND(self);
	STD_BTREE_SET_PARENT_LINK(&child, ptr, idx);
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Internal>, Edge>::insert_fit: a
 * key, value, and right edge inserted at the edge of an internal node with
 * room.
 */
static void
STD_BTREE_INTERNAL_INSERT_FIT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val, StdBTreeNodeRef *edge)
{
	size_t		new_len = STD_BTREE_LEN(&self->node) + 1;

	STD_BTREE_SLICE_INSERT_K(STD_BTREE_KEY_AREA_MUT_TO(&self->node, new_len), new_len, self->idx, key);
	STD_BTREE_SLICE_INSERT_V(STD_BTREE_VAL_AREA_MUT_TO(&self->node, new_len), new_len, self->idx, val);
	STD_BTREE_SLICE_INSERT_EDGE(STD_BTREE_EDGE_AREA_MUT_TO(&self->node, new_len + 1), new_len + 1, self->idx + 1,
								&edge->node);
	*STD_BTREE_LEN_MUT(&self->node) = (uint16) new_len;
	STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS(&self->node, self->idx + 1, new_len + 1);
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Internal>, Edge>::insert: a key,
 * value, and right edge inserted at the edge, splitting a full node at
 * splitpoint, and whether it split, with the split. The key and value are
 * dropped if the edge's height is wrong or the split panics.
 */
static bool
STD_BTREE_INTERNAL_INSERT(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * val,
						  StdBTreeNodeRef *edge STD_BTREE_TABLES, STD_BTREE_SPLIT_RESULT * result)
{
	size_t		middle_kv_idx;
	StdBTreeLeftOrRight insertion;
	StdBTreeHandle middle;
	STD_BTREE_SPLIT_RESULT tmp_1;
	StdBTreeHandle insertion_edge;
	StdUnwind	unwind;

	if (!(edge->height == self->node.height - 1))
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
			std_panic(STD_BTREE_NODE_FILE, 1027, 9, "assertion failed: edge.height == self.node.height - 1",
					  sizeof("assertion failed: edge.height == self.node.height - 1") - 1);
		STD_BTREE_VAL_DROP(val);
		STD_BTREE_KEY_DROP(key);
		std_unwind_resume(&unwind);
	}
	if (STD_BTREE_LEN(&self->node) < STD_BTREE_CAPACITY)
	{
		STD_BTREE_INTERNAL_INSERT_FIT(self, key, val, edge);
		return false;
	}
	std_btree_splitpoint(self->idx, &middle_kv_idx, &insertion);
	middle.node = self->node;
	middle.idx = middle_kv_idx;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		tmp_1 = STD_BTREE_INTERNAL_SPLIT(&middle STD_BTREE_TABLE_ARGS);
		std_unwind_pop(&unwind);
	}
	else
	{
		STD_BTREE_VAL_DROP(val);
		STD_BTREE_KEY_DROP(key);
		std_unwind_resume(&unwind);
	}
	switch (insertion.tag)
	{
		case STD_BTREE_LEFT_OR_RIGHT_LEFT:
			insertion_edge.node = tmp_1.left;
			insertion_edge.idx = insertion.u.left;
			break;
		case STD_BTREE_LEFT_OR_RIGHT_RIGHT:
			insertion_edge.node = tmp_1.right;
			insertion_edge.idx = insertion.u.right;
			break;
	}
	STD_BTREE_INTERNAL_INSERT_FIT(&insertion_edge, key, val, edge);
	*result = tmp_1;
	return true;
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Leaf>, Edge>::insert_recursing: a
 * key and value inserted at the leaf edge, each split carried up into its
 * parent, and a split of the root handed to split_root, and the inserted
 * pair's handle. split_root is dropped where it is not called, and if an
 * insert panics.
 */
static StdBTreeHandle
STD_BTREE_INSERT_RECURSING(StdBTreeHandle *self, STD_BTREE_K * key, STD_BTREE_V * value,
						   STD_BTREE_SPLIT_ROOT(split_root), void *split_root_captures,
						   void (*split_root_drop) (void *) STD_BTREE_TABLES)
{
	bool		has_split;
	STD_BTREE_SPLIT_RESULT split;
	StdBTreeHandle handle;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		STD_BTREE_LEAF_INSERT(self, key, value STD_BTREE_TABLE_ARGS, &has_split, &split, &handle);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (split_root_drop != NULL)
			split_root_drop(split_root_captures);
		std_unwind_resume(&unwind);
	}
	if (!has_split)
	{
		if (split_root_drop != NULL)
			split_root_drop(split_root_captures);
		return handle;
	}
	for (;;)
	{
		StdBTreeHandle parent;
		StdBTreeNodeRef root;

		if (STD_BTREE_ASCEND(&split.left, &parent, &root))
		{
			STD_BTREE_SPLIT_RESULT tmp_1;

			STD_UNWIND_PUSH(&unwind);
			if (sigsetjmp(unwind.jump, 0) == 0)
			{
				has_split = STD_BTREE_INTERNAL_INSERT(&parent, &split.kv.f0, &split.kv.f1, &split.right
													  STD_BTREE_TABLE_ARGS, &tmp_1);
				std_unwind_pop(&unwind);
			}
			else
			{
				if (split_root_drop != NULL)
					split_root_drop(split_root_captures);
				std_unwind_resume(&unwind);
			}
			if (!has_split)
			{
				if (split_root_drop != NULL)
					split_root_drop(split_root_captures);
				return handle;
			}
			split = tmp_1;
		}
		else
		{
			STD_BTREE_SPLIT_RESULT tmp_2;

			tmp_2.left = root;
			tmp_2.kv = split.kv;
			tmp_2.right = split.right;
			split_root(split_root_captures, &tmp_2 STD_BTREE_TABLE_ARGS);
			return handle;
		}
	}
}

/* The instance's Handle<NodeRef<BorrowType, K, V, Internal>, Edge>::descend: the edge's child, a level lower. */
static StdBTreeNodeRef
STD_BTREE_DESCEND(StdBTreeHandle *self)
{
	StdBTreeInternalNode *parent_ptr = STD_BTREE_AS_INTERNAL_PTR(&self->node);
	StdBTreeNodeRef result;

	std_slice_get_unchecked_check(self->idx, STD_BTREE_EDGES_LEN, STD_BTREE_NODE_FILE, 1115, 49);
	result.node = STD_BTREE_EDGES_OF(parent_ptr)[self->idx];
	result.height = self->node.height - 1;
	return result;
}

/* The instance's Handle<NodeRef<Immut, K, V, NodeType>, KV>::into_kv: the handle's key and value. */
static void
STD_BTREE_INTO_KV(StdBTreeHandle *self, STD_BTREE_K const **out_0, STD_BTREE_V const **out_1)
{
	StdBTreeLeafNode *leaf = self->node.node;

	std_slice_get_unchecked_check(self->idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1124, 36);
	*out_0 = STD_BTREE_KEYS_OF(leaf) + self->idx;
	std_slice_get_unchecked_check(self->idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1125, 36);
	*out_1 = STD_BTREE_VAL_AT(STD_BTREE_VALS_OF(leaf), self->idx);
}

/* The instance's Handle<NodeRef<Mut, K, V, NodeType>, KV>::into_val_mut: the handle's value. */
static STD_BTREE_V *
STD_BTREE_INTO_VAL_MUT(StdBTreeHandle *self)
{
	StdBTreeLeafNode *leaf = self->node.node;

	std_slice_get_unchecked_mut_check(self->idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1138, 28);
	return STD_BTREE_VAL_AT(STD_BTREE_VALS_OF(leaf), self->idx);
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, NodeType>, KV>::split_leaf_data:
 * the keys and values right of the handle moved into a new node, and the
 * handle's key and value, which are dropped if a move panics.
 */
static void
STD_BTREE_SPLIT_LEAF_DATA(StdBTreeHandle *self, StdBTreeLeafNode *new_node STD_BTREE_TABLES,
						  STD_BTREE_K * out_0, STD_BTREE_V * out_1)
{
	size_t		old_len = STD_BTREE_LEN(&self->node);
	size_t		new_len = old_len - self->idx - 1;
	STD_BTREE_K k;
	STD_BTREE_V v;
	StdUnwind	unwind;

	STD_BTREE_LEN_FIELD(new_node) = (uint16) new_len;
	k = *STD_BTREE_KEY_AREA_MUT(&self->node, self->idx);
	STD_BTREE_VAL_MOVE(&v, STD_BTREE_VAL_AREA_MUT(&self->node, self->idx));
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		STD_BTREE_K *tmp_1 = STD_BTREE_KEY_AREA_MUT_RANGE(&self->node, self->idx + 1, old_len);
		STD_BTREE_V *tmp_2;

		std_range_to_check(new_len, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1232, 35);
		std_btree_move_to_slice(tmp_1, old_len - (self->idx + 1), STD_BTREE_KEYS_OF(new_node), new_len,
								sizeof(STD_BTREE_K), _Alignof(STD_BTREE_K));
		tmp_2 = STD_BTREE_VAL_AREA_MUT_RANGE(&self->node, self->idx + 1, old_len);
		std_range_to_check(new_len, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1236, 35);
		std_btree_move_to_slice(tmp_2, old_len - (self->idx + 1), STD_BTREE_VALS_OF(new_node), new_len,
								STD_BTREE_V_SIZE, _Alignof(STD_BTREE_V));
		std_unwind_pop(&unwind);
	}
	else
	{
		STD_BTREE_VAL_DROP(&v);
		STD_BTREE_KEY_DROP(&k);
		std_unwind_resume(&unwind);
	}
	*STD_BTREE_LEN_MUT(&self->node) = (uint16) self->idx;
	*out_0 = k;
	*out_1 = v;
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Leaf>, KV>::split: the leaf
 * split at the handle into itself, its key and value, and a new leaf, which
 * is freed if the split's moves panic.
 */
static STD_BTREE_SPLIT_RESULT
STD_BTREE_LEAF_SPLIT(StdBTreeHandle *self STD_BTREE_TABLES)
{
	StdBTreeLeafNode *new_node = STD_BTREE_LEAF_NEW();
	STD_BTREE_SPLIT_RESULT result;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		STD_BTREE_SPLIT_LEAF_DATA(self, new_node STD_BTREE_TABLE_ARGS, &result.kv.f0, &result.kv.f1);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_free(new_node, STD_BTREE_LEAF_SIZE, STD_BTREE_LEAF_ALIGN);
		std_unwind_resume(&unwind);
	}
	result.right = STD_BTREE_FROM_NEW_LEAF(new_node);
	result.left = self->node;
	return result;
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Internal>, KV>::split: the
 * internal node split at the handle into itself, its key and value, and a
 * new internal node taking the edges right of it. The new node is freed if
 * a move panics, after the key and value split off.
 */
static STD_BTREE_SPLIT_RESULT
STD_BTREE_INTERNAL_SPLIT(StdBTreeHandle *self STD_BTREE_TABLES)
{
	size_t		old_len = STD_BTREE_LEN(&self->node);
	StdBTreeInternalNode *new_node = STD_BTREE_INTERNAL_NEW();
	STD_BTREE_SPLIT_RESULT_T kv;
	size_t		new_len;
	size_t		height;
	STD_BTREE_SPLIT_RESULT result;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		STD_BTREE_SPLIT_LEAF_DATA(self, (StdBTreeLeafNode *) new_node STD_BTREE_TABLE_ARGS, &kv.f0, &kv.f1);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_free(new_node, STD_BTREE_INTERNAL_SIZE, STD_BTREE_INTERNAL_ALIGN);
		std_unwind_resume(&unwind);
	}
	new_len = (size_t) STD_BTREE_LEN_FIELD(new_node);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdBTreeLeafNode **tmp_1 = STD_BTREE_EDGE_AREA_MUT_RANGE(&self->node, self->idx + 1, old_len + 1);

		std_range_to_check(new_len + 1, STD_BTREE_EDGES_LEN, STD_BTREE_NODE_FILE, 1299, 36);
		std_btree_move_to_slice(tmp_1, old_len - self->idx, STD_BTREE_EDGES_OF(new_node), new_len + 1,
								sizeof(StdBTreeLeafNode *), _Alignof(StdBTreeLeafNode *));
		std_unwind_pop(&unwind);
	}
	else
	{
		STD_BTREE_KEY_DROP(&kv.f0);
		STD_BTREE_VAL_DROP(&kv.f1);
		std_free(new_node, STD_BTREE_INTERNAL_SIZE, STD_BTREE_INTERNAL_ALIGN);
		std_unwind_resume(&unwind);
	}
	std_NonZero_new_unchecked_check(self->node.height, STD_BTREE_NODE_FILE, 1303, 26);
	height = self->node.height;
	result.right = STD_BTREE_FROM_NEW_INTERNAL(new_node, height);
	result.left = self->node;
	result.kv = kv;
	return result;
}

/* slice_insert at K: a key inserted at an index of a slice of keys, those after it moved up. */
static void
STD_BTREE_SLICE_INSERT_K(STD_BTREE_K * slice, size_t slice_len, size_t idx, STD_BTREE_K * val)
{
	size_t		len = slice_len;
	STD_BTREE_K *slice_ptr = slice;

	if (len > idx + 1)
		std_ptr_copy(slice_ptr + idx, slice_ptr + idx + 1, len - idx - 1, sizeof(STD_BTREE_K),
					 _Alignof(STD_BTREE_K));
	slice_ptr[idx] = *val;
}

/* slice_insert at V: a value inserted at an index of a slice of values, those after it moved up. */
static void
STD_BTREE_SLICE_INSERT_V(STD_BTREE_V * slice, size_t slice_len, size_t idx, STD_BTREE_V * val)
{
	size_t		len = slice_len;
	STD_BTREE_V *slice_ptr = slice;

	if (len > idx + 1)
		std_ptr_copy(STD_BTREE_VAL_AT(slice_ptr, idx), STD_BTREE_VAL_AT(slice_ptr, idx + 1), len - idx - 1,
					 STD_BTREE_V_SIZE, _Alignof(STD_BTREE_V));
	STD_BTREE_VAL_MOVE(STD_BTREE_VAL_AT(slice_ptr, idx), val);
}

/* slice_insert at BoxedNode<K, V>: an edge inserted at an index of a slice of edges, those after it moved up. */
static void
STD_BTREE_SLICE_INSERT_EDGE(StdBTreeLeafNode **slice, size_t slice_len, size_t idx, StdBTreeLeafNode **val)
{
	size_t		len = slice_len;
	StdBTreeLeafNode **slice_ptr = slice;

	if (len > idx + 1)
		std_ptr_copy(slice_ptr + idx, slice_ptr + idx + 1, len - idx - 1, sizeof(StdBTreeLeafNode *),
					 _Alignof(StdBTreeLeafNode *));
	slice_ptr[idx] = *val;
}

#ifdef STD_BTREE_ITERATE

/* The instance's NodeRef::first_edge: the node's edge left of its first key. */
static StdBTreeHandle
STD_BTREE_FIRST_EDGE(StdBTreeNodeRef self)
{
	StdBTreeHandle result = {self, 0};

	return result;
}

/* The instance's Handle<NodeRef<BorrowType, K, V, NodeType>, Edge>::right_kv: the key and value right of the edge, or the edge back. */
static bool
STD_BTREE_RIGHT_KV(StdBTreeHandle self, StdBTreeHandle *result, StdBTreeHandle *error)
{
	if (self.idx < STD_BTREE_LEN(&self.node))
	{
		*result = self;
		return true;
	}
	*error = self;
	return false;
}

/* The instance's Handle<NodeRef<BorrowType, K, V, NodeType>, KV>::right_edge: the edge right of the key and value. */
static StdBTreeHandle
STD_BTREE_RIGHT_EDGE(StdBTreeHandle self)
{
	StdBTreeHandle result = {self.node, self.idx + 1};

	return result;
}
#endif

/* search.rs */

/*
 * The instance's NodeRef<BorrowType, K, V, LeafOrInternal>::search_tree: the
 * key's handle if the tree holds it, and otherwise the leaf edge it belongs
 * at.
 */
static StdBTreeSearchResult
STD_BTREE_SEARCH_TREE(StdBTreeNodeRef *self, STD_BTREE_K const *key)
{
	for (;;)
	{
		StdBTreeSearchResult tmp_1 = STD_BTREE_SEARCH_NODE(self, key);
		StdBTreeHandle handle;

		switch (tmp_1.tag)
		{
			case STD_BTREE_SEARCH_RESULT_FOUND:
				return tmp_1;
			case STD_BTREE_SEARCH_RESULT_GO_DOWN:
				handle = tmp_1.u.go_down;
				if (handle.node.height == 0)
					return tmp_1;
				*self = STD_BTREE_DESCEND(&handle);
				break;
		}
	}
}

/* The instance's NodeRef::search_node: the key's handle if the node holds it, and otherwise the edge it belongs below. */
static StdBTreeSearchResult
STD_BTREE_SEARCH_NODE(StdBTreeNodeRef *self, STD_BTREE_K const *key)
{
	StdBTreeIndexResult tmp_1 = STD_BTREE_FIND_KEY_INDEX(self, key, 0);
	StdBTreeSearchResult result;

	switch (tmp_1.tag)
	{
		case STD_BTREE_INDEX_RESULT_KV:
			result.tag = STD_BTREE_SEARCH_RESULT_FOUND;
			result.u.found.node = *self;
			result.u.found.idx = tmp_1.u.kv;
			return result;
		case STD_BTREE_INDEX_RESULT_EDGE:
			result.tag = STD_BTREE_SEARCH_RESULT_GO_DOWN;
			result.u.go_down.node = *self;
			result.u.go_down.idx = tmp_1.u.edge;
			return result;
	}
	pg_unreachable();
}

/*
 * The instance's NodeRef::find_key_index: the index of the key in the node
 * from a start index on, or of the edge it belongs below, by K's Ord.
 */
static StdBTreeIndexResult
STD_BTREE_FIND_KEY_INDEX(const StdBTreeNodeRef *self, STD_BTREE_K const *key, size_t start_index)
{
	StdBTreeNodeRef node = *self;
	size_t		keys_len;
	STD_BTREE_K const *keys = STD_BTREE_KEYS(&node, &keys_len);
	StdBTreeIndexResult result;

	std_slice_get_unchecked_range_check(start_index, keys_len, keys_len, STD_SLICE_INDEX_FILE, 533, 44);
	for (size_t count = 0; count < keys_len - start_index;)
	{
		size_t		offset = count;
		STD_BTREE_K const *k = keys + start_index + offset;

		count = std_Enumerate_count_step(count);

		switch (STD_BTREE_CMP(key, k))
		{
			case STD_ORDERING_GREATER:
				break;
			case STD_ORDERING_EQUAL:
				result.tag = STD_BTREE_INDEX_RESULT_KV;
				result.u.kv = start_index + offset;
				return result;
			case STD_ORDERING_LESS:
				result.tag = STD_BTREE_INDEX_RESULT_EDGE;
				result.u.edge = start_index + offset;
				return result;
		}
	}
	result.tag = STD_BTREE_INDEX_RESULT_EDGE;
	result.u.edge = keys_len;
	return result;
}

#if defined(STD_BTREE_ITERATE) && defined(STD_BTREE_SET)

/* set.rs */

/* The instance's BTreeSet::iter: an iterator over the set's values in order, its map's keys. */
STD_BTREE_DEF StdBTreeSetIter
STD_BTREE_SET_ITER(const StdBTreeMap *self)
{
	StdBTreeSetIter result = {STD_BTREE_MAP_KEYS(self)};

	return result;
}

/* The instance's set Iter's Iterator::next: the next value in order, its keys' next. */
STD_BTREE_DEF bool
STD_BTREE_SET_ITER_NEXT(StdBTreeSetIter *self, STD_BTREE_K const **result)
{
	return STD_BTREE_KEYS_NEXT(&self->iter, result);
}

/* The instance's &BTreeSet's IntoIterator::into_iter: the set's iter. */
STD_BTREE_DEF StdBTreeSetIter
STD_BTREE_SET_INTO_ITER(const StdBTreeMap *self)
{
	return STD_BTREE_SET_ITER(self);
}
#endif

#ifdef STD_BTREE_DROP
static bool STD_BTREE_DYING_NEXT(StdBTreeIntoIter *self, StdBTreeHandle *result);
static bool STD_BTREE_TAKE_FRONT(StdBTreeLazyLeafRange *self, StdBTreeHandle *result);
static StdBTreeHandle STD_BTREE_LAZY_DEALLOCATING_NEXT_UNCHECKED(StdBTreeLazyLeafRange *self);
static void STD_BTREE_LAZY_DEALLOCATING_END(StdBTreeLazyLeafRange *self);
static bool STD_BTREE_EDGE_DEALLOCATING_NEXT(StdBTreeHandle self, StdBTreeHandle *out_0, StdBTreeHandle *out_1);
static StdBTreeHandle STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED_CLOSURE_0(StdBTreeHandle *leaf_edge,
																		   StdBTreeHandle *out_0);
static StdBTreeHandle STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED(StdBTreeHandle *self);
static void STD_BTREE_EDGE_DEALLOCATING_END(StdBTreeHandle self);
static bool STD_BTREE_DEALLOCATE_AND_ASCEND(StdBTreeNodeRef self, StdBTreeHandle *result);
static void STD_BTREE_DROP_KEY_VAL(StdBTreeHandle self);
#endif

#ifdef STD_BTREE_DROP

/* map.rs */

/* The state of IntoIter's drop across its pad: the iterator, and whether its DropGuard is live. */
typedef struct STD_BTREE_INTO_ITER_DROP_FRAME_0
{
	StdBTreeIntoIter *self;
	bool		guard_live;
}			STD_BTREE_INTO_ITER_DROP_FRAME_0;

/* The body of IntoIter's drop: each key and value left dropped, under the DropGuard. */
static void
STD_BTREE_INTO_ITER_DROP_BODY_0(void *frame_)
{
	STD_BTREE_INTO_ITER_DROP_FRAME_0 *frame = frame_;
	StdBTreeHandle kv;

	while (STD_BTREE_DYING_NEXT(frame->self, &kv))
	{
		frame->guard_live = true;
		STD_BTREE_DROP_KEY_VAL(kv);
		frame->guard_live = false;
	}
}

/* The pad of IntoIter's drop: its DropGuard's drop, the rest of the keys and values dropped. */
static void
STD_BTREE_INTO_ITER_DROP_PAD_0(void *frame_)
{
	STD_BTREE_INTO_ITER_DROP_FRAME_0 *frame = frame_;
	StdBTreeHandle kv;

	if (frame->guard_live)
	{
		while (STD_BTREE_DYING_NEXT(frame->self, &kv))
			STD_BTREE_DROP_KEY_VAL(kv);
	}
}

/*
 * The instance's IntoIter<K, V>'s drop: each key and value left dropped and
 * every node freed, the rest still dropped should a drop panic.
 */
STD_BTREE_INTO_DEF void
STD_BTREE_INTO_ITER_DROP(StdBTreeIntoIter *self)
{
	STD_BTREE_INTO_ITER_DROP_FRAME_0 frame = {.self = self};

	std_landing(&frame, STD_BTREE_INTO_ITER_DROP_BODY_0, STD_BTREE_INTO_ITER_DROP_PAD_0);
}

/*
 * The instance's IntoIter<K, V>::dying_next: the next key and value's
 * handle, the nodes it passes freed, or false once none is left, every node
 * left then freed.
 */
static bool
STD_BTREE_DYING_NEXT(StdBTreeIntoIter *self, StdBTreeHandle *result)
{
	if (self->length == 0)
	{
		STD_BTREE_LAZY_DEALLOCATING_END(&self->range);
		return false;
	}
	self->length -= 1;
	*result = STD_BTREE_LAZY_DEALLOCATING_NEXT_UNCHECKED(&self->range);
	return true;
}

/* The instance's BTreeMap<K, V>'s into_iter: an iterator that takes the tree, over all of it. */
STD_BTREE_INTO_DEF StdBTreeIntoIter
STD_BTREE_INTO_ITER(StdBTreeMap *self)
{
	StdBTreeIntoIter result;

	if (self->root.node != NULL)
	{
		StdBTreeNodeRef root = self->root;

		self->root.node = NULL;
		result.range = STD_BTREE_FULL_RANGE(root);
		result.length = self->length;
	}
	else
	{
		result.range.has_front = false;
		result.range.has_back = false;
		result.length = 0;
	}
	return result;
}

/* The instance's BTreeMap<K, V>'s drop: its into_iter's drop. */
STD_BTREE_DEF void
STD_BTREE_MAP_DROP(void *self_)
{
	StdBTreeMap *self = self_;
	StdBTreeIntoIter iter = STD_BTREE_INTO_ITER(self);

	STD_BTREE_INTO_ITER_DROP(&iter);
}

#ifdef STD_BTREE_INTO_ITERATE

/*
 * The instance's Handle<NodeRef<Dying, K, V, LeafOrInternal>,
 * KV>::into_key_val: the key and the value moved out of their places.
 */
static void
STD_BTREE_INTO_KEY_VAL(StdBTreeHandle self, STD_BTREE_K * key, STD_BTREE_V * val)
{
	std_slice_get_unchecked_mut_check(self.idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1184, 33);
	*key = STD_BTREE_KEYS_OF(self.node.node)[self.idx];
	std_slice_get_unchecked_mut_check(self.idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1185, 33);
	STD_BTREE_VAL_MOVE(val, STD_BTREE_VAL_AT(STD_BTREE_VALS_OF(self.node.node), self.idx));
}

/* The instance's IntoIter<K, V>'s Iterator::next: the next key and value moved out, or false once none is left. */
STD_BTREE_DEF bool
STD_BTREE_INTO_ITER_NEXT(StdBTreeIntoIter *self, STD_BTREE_K * key, STD_BTREE_V * val)
{
	StdBTreeHandle kv;

	if (!STD_BTREE_DYING_NEXT(self, &kv))
		return false;
	STD_BTREE_INTO_KEY_VAL(kv, key, val);
	return true;
}

#endif							/* STD_BTREE_INTO_ITERATE */

/* navigate.rs */

/*
 * The instance's LazyLeafRange<Dying, K, V>::take_front: the front's leaf
 * edge, a root front descended to its first leaf edge, the front left none,
 * or false for none.
 */
static bool
STD_BTREE_TAKE_FRONT(StdBTreeLazyLeafRange *self, StdBTreeHandle *result)
{
	StdBTreeLazyLeafHandle front;

	if (!self->has_front)
		return false;
	front = self->front;
	self->has_front = false;
	if (front.tag == STD_BTREE_LAZY_LEAF_HANDLE_ROOT)
		*result = STD_BTREE_FIRST_LEAF_EDGE(front.u.root);
	else
		*result = front.u.edge;
	return true;
}

/* The instance's LazyLeafRange<Dying, K, V>::deallocating_next_unchecked: the front's next key and value, freeing the nodes it passes. */
static StdBTreeHandle
STD_BTREE_LAZY_DEALLOCATING_NEXT_UNCHECKED(StdBTreeLazyLeafRange *self)
{
	StdBTreeHandle *front = STD_BTREE_INIT_FRONT(self);

	if (front == NULL)
		std_option_unwrap_failed(STD_BTREE_NAVIGATE_FILE, 198, 39);
	return STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED(front);
}

/* The instance's LazyLeafRange<Dying, K, V>::deallocating_end: every node from the front up freed. */
static void
STD_BTREE_LAZY_DEALLOCATING_END(StdBTreeLazyLeafRange *self)
{
	StdBTreeHandle front;

	if (STD_BTREE_TAKE_FRONT(self, &front))
		STD_BTREE_EDGE_DEALLOCATING_END(front);
}

/*
 * The instance's Handle<NodeRef<Dying, K, V, Leaf>, Edge>::deallocating_next:
 * the next key and value's handle and the leaf edge after it, freeing each
 * node it climbs out of, or false past the last, every node then freed.
 */
static bool
STD_BTREE_EDGE_DEALLOCATING_NEXT(StdBTreeHandle self, StdBTreeHandle *out_0, StdBTreeHandle *out_1)
{
	StdBTreeHandle edge = self;

	for (;;)
	{
		StdBTreeHandle kv;
		StdBTreeHandle last_edge;

		if (STD_BTREE_RIGHT_KV(edge, &kv, &last_edge))
		{
			*out_0 = STD_BTREE_NEXT_LEAF_EDGE(kv);
			*out_1 = kv;
			return true;
		}
		if (!STD_BTREE_DEALLOCATE_AND_ASCEND(last_edge.node, &edge))
			return false;
	}
}

/* deallocating_next_unchecked's closure: the edge's next key and value's handle, and the leaf edge after it. */
static StdBTreeHandle
STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED_CLOSURE_0(StdBTreeHandle *leaf_edge, StdBTreeHandle *out_0)
{
	StdBTreeHandle kv;

	if (!STD_BTREE_EDGE_DEALLOCATING_NEXT(*leaf_edge, out_0, &kv))
		std_option_unwrap_failed(STD_BTREE_NAVIGATE_FILE, 600, 48);
	return kv;
}

/* The state of replace at a dying leaf edge and a KV's handle across its pad: its arguments and its results. */
typedef struct STD_BTREE_REPLACE_DYING_FRAME_0
{
	StdBTreeHandle *v;
	StdBTreeHandle new_value;
	StdBTreeHandle ret;
}			STD_BTREE_REPLACE_DYING_FRAME_0;

/* The body of replace at a dying leaf edge: deallocating_next_unchecked's closure of the edge's value. */
static void
STD_BTREE_REPLACE_DYING_BODY_0(void *frame_)
{
	STD_BTREE_REPLACE_DYING_FRAME_0 *frame = frame_;
	StdBTreeHandle value = *frame->v;

	frame->ret = STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED_CLOSURE_0(&value, &frame->new_value);
}

/* The pad of replace at a dying leaf edge: its PanicGuard's drop, which aborts. */
static void
STD_BTREE_REPLACE_DYING_PAD_0(void *frame_)
{
	std_intrinsics_abort();
}

/*
 * The instance's Handle<NodeRef<Dying, K, V, Leaf>,
 * Edge>::deallocating_next_unchecked: mem::replace of the edge by its
 * closure, whose PanicGuard aborts if it panics, the next key and value's
 * handle returned.
 */
static StdBTreeHandle
STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED(StdBTreeHandle *self)
{
	STD_BTREE_REPLACE_DYING_FRAME_0 frame = {.v = self};

	std_landing(&frame, STD_BTREE_REPLACE_DYING_BODY_0, STD_BTREE_REPLACE_DYING_PAD_0);
	*self = frame.new_value;
	return frame.ret;
}

/* The instance's Handle<NodeRef<Dying, K, V, Leaf>, Edge>::deallocating_end: the edge's node and every one above it freed. */
static void
STD_BTREE_EDGE_DEALLOCATING_END(StdBTreeHandle self)
{
	StdBTreeHandle edge = self;
	StdBTreeHandle parent_edge;

	while (STD_BTREE_DEALLOCATE_AND_ASCEND(edge.node, &parent_edge))
		edge = parent_edge;
}

/* node.rs */

/*
 * The instance's NodeRef<Dying, K, V, LeafOrInternal>::deallocate_and_ascend:
 * the node freed, as a leaf or an internal node by its height, and its edge
 * in its parent, or false at the root.
 */
static bool
STD_BTREE_DEALLOCATE_AND_ASCEND(StdBTreeNodeRef self, StdBTreeHandle *result)
{
	size_t		height = self.height;
	StdBTreeLeafNode *node = self.node;
	StdBTreeNodeRef root;
	bool		ret = STD_BTREE_ASCEND(&self, result, &root);

	if (height > 0)
		std_free(node, STD_BTREE_INTERNAL_SIZE, STD_BTREE_INTERNAL_ALIGN);
	else
		std_free(node, STD_BTREE_LEAF_SIZE, STD_BTREE_LEAF_ALIGN);
	return ret;
}

/* The state of drop_key_val across its pad: the value its Dropper guard drops. */
typedef struct STD_BTREE_DROP_KEY_VAL_FRAME_0
{
	STD_BTREE_K *key;
	STD_BTREE_V *val;
}			STD_BTREE_DROP_KEY_VAL_FRAME_0;

/* The body of drop_key_val: the key's drop. */
static void
STD_BTREE_DROP_KEY_VAL_BODY_0(void *frame_)
{
	STD_BTREE_DROP_KEY_VAL_FRAME_0 *frame = frame_;

	STD_BTREE_KEY_DROP(frame->key);
}

/* The pad of drop_key_val, and its end: the Dropper guard's drop of the value. */
static void
STD_BTREE_DROP_KEY_VAL_PAD_0(void *frame_)
{
	STD_BTREE_DROP_KEY_VAL_FRAME_0 *frame = frame_;

	STD_BTREE_VAL_DROP(frame->val);
}

/*
 * The instance's Handle<NodeRef<Dying, K, V, LeafOrInternal>,
 * KV>::drop_key_val: the key dropped, then the value, which its guard still
 * drops should the key's drop panic.
 */
static void
STD_BTREE_DROP_KEY_VAL(StdBTreeHandle self)
{
	STD_BTREE_DROP_KEY_VAL_FRAME_0 frame;

	std_slice_get_unchecked_mut_check(self.idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1209, 33);
	frame.key = &STD_BTREE_KEYS_OF(self.node.node)[self.idx];
	std_slice_get_unchecked_mut_check(self.idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1210, 33);
	frame.val = STD_BTREE_VAL_AT(STD_BTREE_VALS_OF(self.node.node), self.idx);
	std_landing(&frame, STD_BTREE_DROP_KEY_VAL_BODY_0, STD_BTREE_DROP_KEY_VAL_PAD_0);
	STD_BTREE_DROP_KEY_VAL_PAD_0(&frame);
}

#endif							/* STD_BTREE_DROP */

#ifdef STD_BTREE_BULK

/*
 * The instance's DedupSortedIter<K, V, I>: its Peekable over the iterator
 * of sorted keys and values, which holds an item it peeked, or none.
 */
typedef struct STD_BTREE_DEDUP_SORTED_ITER
{
	StdIterator iter;
	bool		has_peeked;
	bool		peeked_some;
	STD_BTREE_KV_T peeked;
}			STD_BTREE_DEDUP_SORTED_ITER;

/* node.rs */

/* The instance's NodeRef::last_edge: the edge after the node's last key. */
static StdBTreeHandle
STD_BTREE_LAST_EDGE(StdBTreeNodeRef self)
{
	StdBTreeHandle result = {self, STD_BTREE_LEN(&self)};

	return result;
}

/* The instance's NodeRef::last_kv: the node's last key and value, which a node that has none fails to have. */
static StdBTreeHandle
STD_BTREE_LAST_KV(StdBTreeNodeRef self)
{
	size_t		len = STD_BTREE_LEN(&self);
	StdBTreeHandle result;

	if (!(len > 0))
		std_panic(STD_BTREE_NODE_FILE, 367, 9, "assertion failed: len > 0", sizeof("assertion failed: len > 0") - 1);
	result.node = self;
	result.idx = len - 1;
	return result;
}

/* The instance's NodeRef<Mut, K, V, Leaf>::push: a key and value added at the leaf's end. */
static void
STD_BTREE_LEAF_PUSH(StdBTreeNodeRef *self, STD_BTREE_K * key, STD_BTREE_V * val STD_BTREE_TABLES)
{
	StdBTreeHandle kv = STD_BTREE_PUSH_WITH_HANDLE(self, key, val STD_BTREE_TABLE_ARGS);

	(void) STD_BTREE_INTO_VAL_MUT(&kv);
}

/* The instance's Handle<Node, KV>::left_edge: the edge left of the key and value. */
static StdBTreeHandle
STD_BTREE_LEFT_EDGE(StdBTreeHandle self)
{
	StdBTreeHandle result = {self.node, self.idx};

	return result;
}

/*
 * The instance's Handle<NodeRef<Mut, K, V, Internal>, KV>::consider_for_balancing:
 * the key and value with the children left and right of it.
 */
static StdBTreeBalancingContext
STD_BTREE_CONSIDER_FOR_BALANCING(StdBTreeHandle self)
{
	StdBTreeBalancingContext result;
	StdBTreeHandle left = STD_BTREE_LEFT_EDGE(self);
	StdBTreeHandle right = STD_BTREE_RIGHT_EDGE(self);

	result.parent = self;
	result.left_child = STD_BTREE_DESCEND(&left);
	result.right_child = STD_BTREE_DESCEND(&right);
	return result;
}

/* The instance's Handle<NodeRef<Mut, K, V, NodeType>, KV>::kv_mut: the places of the key and the value. */
static void
STD_BTREE_KV_MUT(StdBTreeHandle *self, STD_BTREE_K * *key, STD_BTREE_V * *val)
{
	std_slice_get_unchecked_mut_check(self->idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1163, 33);
	*key = &STD_BTREE_KEYS_OF(self->node.node)[self->idx];
	std_slice_get_unchecked_mut_check(self->idx, STD_BTREE_CAPACITY, STD_BTREE_NODE_FILE, 1164, 33);
	*val = STD_BTREE_VAL_AT(STD_BTREE_VALS_OF(self->node.node), self->idx);
}

/* The instance's Handle<NodeRef<Mut, K, V, NodeType>, KV>::replace_kv: the key and value put in, the old ones returned. */
static void
STD_BTREE_REPLACE_KV_MUT(StdBTreeHandle *self, STD_BTREE_K * k, STD_BTREE_V * v, STD_BTREE_K * out_0,
						 STD_BTREE_V * out_1)
{
	STD_BTREE_K *key;
	STD_BTREE_V *val;

	STD_BTREE_KV_MUT(self, &key, &val);
	*out_0 = *key;
	*key = *k;
	STD_BTREE_VAL_MOVE(out_1, val);
	STD_BTREE_VAL_MOVE(val, v);
}

/* slice_shr at K: the keys moved distance places right, the last distance of them overwritten. */
static void
STD_BTREE_SLICE_SHR_K(STD_BTREE_K * slice, size_t len, size_t distance)
{
	std_ptr_copy(slice, slice + distance, len - distance, sizeof(STD_BTREE_K), _Alignof(STD_BTREE_K));
}

/* slice_shr at V: the values moved distance places right, the last distance of them overwritten. */
static void
STD_BTREE_SLICE_SHR_V(STD_BTREE_V * slice, size_t len, size_t distance)
{
	std_ptr_copy(slice, STD_BTREE_VAL_AT(slice, distance), len - distance, STD_BTREE_V_SIZE, _Alignof(STD_BTREE_V));
}

/* slice_shr at an edge: the edges moved distance places right, the last distance of them overwritten. */
static void
STD_BTREE_SLICE_SHR_EDGE(StdBTreeLeafNode **slice, size_t len, size_t distance)
{
	std_ptr_copy(slice, slice + distance, len - distance, sizeof(StdBTreeLeafNode *), _Alignof(StdBTreeLeafNode *));
}

/*
 * The instance's BalancingContext::bulk_steal_left: count keys and values,
 * and edges below them, moved from the left child through the parent to the
 * right child's front.
 */
static void
STD_BTREE_BULK_STEAL_LEFT(StdBTreeBalancingContext *self, size_t count)
{
	StdBTreeNodeRef *left_node = &self->left_child;
	StdBTreeNodeRef *right_node = &self->right_child;
	size_t		old_left_len;
	size_t		old_right_len;
	size_t		new_left_len;
	size_t		new_right_len;
	STD_BTREE_K k;
	STD_BTREE_V v;
	STD_BTREE_K parent_k;
	STD_BTREE_V parent_v;

	if (!(count > 0))
		std_panic(STD_BTREE_NODE_FILE, 1529, 9, "assertion failed: count > 0",
				  sizeof("assertion failed: count > 0") - 1);
	old_left_len = STD_BTREE_LEN(left_node);
	old_right_len = STD_BTREE_LEN(right_node);
	if (!(old_right_len + count <= STD_BTREE_CAPACITY))
		std_panic(STD_BTREE_NODE_FILE, 1537, 13, "assertion failed: old_right_len + count <= CAPACITY",
				  sizeof("assertion failed: old_right_len + count <= CAPACITY") - 1);
	if (!(old_left_len >= count))
		std_panic(STD_BTREE_NODE_FILE, 1538, 13, "assertion failed: old_left_len >= count",
				  sizeof("assertion failed: old_left_len >= count") - 1);
	new_left_len = old_left_len - count;
	new_right_len = old_right_len + count;
	*STD_BTREE_LEN_MUT(left_node) = (uint16) new_left_len;
	*STD_BTREE_LEN_MUT(right_node) = (uint16) new_right_len;
	STD_BTREE_SLICE_SHR_K(STD_BTREE_KEY_AREA_MUT_TO(right_node, new_right_len), new_right_len, count);
	STD_BTREE_SLICE_SHR_V(STD_BTREE_VAL_AREA_MUT_TO(right_node, new_right_len), new_right_len, count);
	{
		STD_BTREE_K *src = STD_BTREE_KEY_AREA_MUT_RANGE(left_node, new_left_len + 1, old_left_len);

		std_btree_move_to_slice(src, old_left_len - (new_left_len + 1), STD_BTREE_KEY_AREA_MUT_TO(right_node, count - 1),
								count - 1, sizeof(STD_BTREE_K), _Alignof(STD_BTREE_K));
	}
	{
		STD_BTREE_V *src = STD_BTREE_VAL_AREA_MUT_RANGE(left_node, new_left_len + 1, old_left_len);

		std_btree_move_to_slice(src, old_left_len - (new_left_len + 1), STD_BTREE_VAL_AREA_MUT_TO(right_node, count - 1),
								count - 1, STD_BTREE_V_SIZE, _Alignof(STD_BTREE_V));
	}
	k = *STD_BTREE_KEY_AREA_MUT(left_node, new_left_len);
	STD_BTREE_VAL_MOVE(&v, STD_BTREE_VAL_AREA_MUT(left_node, new_left_len));
	STD_BTREE_REPLACE_KV_MUT(&self->parent, &k, &v, &parent_k, &parent_v);
	*STD_BTREE_KEY_AREA_MUT(right_node, count - 1) = parent_k;
	STD_BTREE_VAL_MOVE(STD_BTREE_VAL_AREA_MUT(right_node, count - 1), &parent_v);
	if (left_node->height > 0 && right_node->height > 0)
	{
		StdBTreeLeafNode **src;

		STD_BTREE_SLICE_SHR_EDGE(STD_BTREE_EDGE_AREA_MUT_TO(right_node, new_right_len + 1), new_right_len + 1, count);
		src = STD_BTREE_EDGE_AREA_MUT_RANGE(left_node, new_left_len + 1, old_left_len + 1);
		std_btree_move_to_slice(src, old_left_len + 1 - (new_left_len + 1), STD_BTREE_EDGE_AREA_MUT_TO(right_node, count),
								count, sizeof(StdBTreeLeafNode *), _Alignof(StdBTreeLeafNode *));
		STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS(right_node, 0, new_right_len + 1);
	}
	else if (!(left_node->height == 0 && right_node->height == 0))
		std_panic(STD_BTREE_NODE_FILE, 1585, 22, "internal error: entered unreachable code",
				  sizeof("internal error: entered unreachable code") - 1);
}

/* navigate.rs */

/* The instance's NodeRef<BorrowType, K, V, LeafOrInternal>::last_leaf_edge: the rightmost leaf's last edge. */
static StdBTreeHandle
STD_BTREE_LAST_LEAF_EDGE(StdBTreeNodeRef self)
{
	StdBTreeNodeRef node = self;

	for (;;)
	{
		StdBTreeHandle edge;

		if (node.height == 0)
			return STD_BTREE_LAST_EDGE(node);
		edge = STD_BTREE_LAST_EDGE(node);
		node = STD_BTREE_DESCEND(&edge);
	}
}

/* fix.rs */

/*
 * The instance's NodeRef<Owned, K, V, LeafOrInternal>::fix_right_border_of_plentiful:
 * each rightmost child below MIN_LEN filled by stealing from its left
 * sibling, from the root down.
 */
static void
STD_BTREE_FIX_RIGHT_BORDER_OF_PLENTIFUL(StdBTreeNodeRef *self)
{
	StdBTreeNodeRef cur_node = *self;

	while (cur_node.height > 0)
	{
		StdBTreeBalancingContext last_kv = STD_BTREE_CONSIDER_FOR_BALANCING(STD_BTREE_LAST_KV(cur_node));
		size_t		right_child_len = STD_BTREE_LEN(&last_kv.right_child);

		if (right_child_len < STD_BTREE_MIN_LEN)
			STD_BTREE_BULK_STEAL_LEFT(&last_kv, STD_BTREE_MIN_LEN - right_child_len);
		cur_node = last_kv.right_child;
	}
}

/* dedup_sorted_iter.rs */

/* The instance's Peekable<I>::next: the item peeked, else the inner iterator's next. */
static bool
STD_BTREE_PEEKABLE_NEXT(STD_BTREE_DEDUP_SORTED_ITER * self, STD_BTREE_KV_T * result)
{
	if (self->has_peeked)
	{
		self->has_peeked = false;
		if (!self->peeked_some)
			return false;
		*result = self->peeked;
		return true;
	}
	return self->iter.ops->next(self->iter.self, result);
}

/* The instance's Peekable<I>::peek: the next item, which the inner iterator gives once and the peek keeps. */
static STD_BTREE_KV_T *
STD_BTREE_PEEKABLE_PEEK(STD_BTREE_DEDUP_SORTED_ITER * self)
{
	if (!self->has_peeked)
	{
		self->peeked_some = self->iter.ops->next(self->iter.self, &self->peeked);
		self->has_peeked = true;
	}
	return self->peeked_some ? &self->peeked : NULL;
}

/* The body of (K, V)'s drop glue: the key's drop. */
static void
STD_BTREE_KV_DROP_BODY_0(void *frame_)
{
	STD_BTREE_KV_T *kv = frame_;

	STD_BTREE_KEY_DROP(&kv->f0);
}

/* The pad of (K, V)'s drop glue: the value's drop, should the key's panic. */
static void
STD_BTREE_KV_DROP_PAD_0(void *frame_)
{
	STD_BTREE_KV_T *kv = frame_;

	STD_BTREE_VAL_DROP(&kv->f1);
}

/* The instance's (K, V)'s drop glue: the key dropped, then the value, which is still dropped should the key's drop panic. */
static void
STD_BTREE_KV_DROP(STD_BTREE_KV_T * kv)
{
	std_landing(kv, STD_BTREE_KV_DROP_BODY_0, STD_BTREE_KV_DROP_PAD_0);
	STD_BTREE_VAL_DROP(&kv->f1);
}

/* The state of DedupSortedIter's next across its pad: the iterator, its result, and the item it holds. */
typedef struct STD_BTREE_DEDUP_NEXT_FRAME_0
{
	STD_BTREE_DEDUP_SORTED_ITER *self;
	STD_BTREE_KV_T *result;
	bool		ret;
	STD_BTREE_KV_T next;
	bool		next_live;
}			STD_BTREE_DEDUP_NEXT_FRAME_0;

/* The body of DedupSortedIter's next: each item with the key of the one after it dropped, until one without. */
static void
STD_BTREE_DEDUP_NEXT_BODY_0(void *frame_)
{
	STD_BTREE_DEDUP_NEXT_FRAME_0 *frame = frame_;

	for (;;)
	{
		STD_BTREE_KV_T *peeked;

		if (!STD_BTREE_PEEKABLE_NEXT(frame->self, &frame->next))
			return;
		frame->next_live = true;
		peeked = STD_BTREE_PEEKABLE_PEEK(frame->self);
		if (peeked == NULL || STD_BTREE_KEY_NE(&frame->next.f0, &peeked->f0))
		{
			frame->next_live = false;
			*frame->result = frame->next;
			frame->ret = true;
			return;
		}
		frame->next_live = false;
		STD_BTREE_KV_DROP(&frame->next);
	}
}

/* The pad of DedupSortedIter's next: the item it holds, should the peek or the keys' ne panic. */
static void
STD_BTREE_DEDUP_NEXT_PAD_0(void *frame_)
{
	STD_BTREE_DEDUP_NEXT_FRAME_0 *frame = frame_;

	if (frame->next_live)
		STD_BTREE_KV_DROP(&frame->next);
}

/*
 * The instance's DedupSortedIter<K, V, I>::next: the next key and value
 * whose key the one after it lacks, each earlier one with the same key
 * dropped, the item it holds dropped should the peek or the keys' ne panic.
 */
static bool
STD_BTREE_DEDUP_NEXT(STD_BTREE_DEDUP_SORTED_ITER * self, STD_BTREE_KV_T * result)
{
	STD_BTREE_DEDUP_NEXT_FRAME_0 frame = {.self = self, .result = result};

	std_landing(&frame, STD_BTREE_DEDUP_NEXT_BODY_0, STD_BTREE_DEDUP_NEXT_PAD_0);
	return frame.ret;
}

/* The body of DedupSortedIter's drop: its Peekable's first field, the inner iterator. */
static void
STD_BTREE_DEDUP_DROP_BODY_0(void *frame_)
{
	STD_BTREE_DEDUP_SORTED_ITER *self = frame_;

	std_iterator_drop(&self->iter);
}

/* The pad of DedupSortedIter's drop, and its end: its Peekable's second field, the item peeked. */
static void
STD_BTREE_DEDUP_DROP_PAD_0(void *frame_)
{
	STD_BTREE_DEDUP_SORTED_ITER *self = frame_;

	if (self->has_peeked && self->peeked_some)
		STD_BTREE_KV_DROP(&self->peeked);
}

/*
 * The instance's DedupSortedIter<K, V, I>'s drop: its Peekable's fields in
 * their order, the inner iterator and then the item peeked, which is still
 * dropped should the iterator's drop panic.
 */
static void
STD_BTREE_DEDUP_DROP(STD_BTREE_DEDUP_SORTED_ITER * self)
{
	std_landing(self, STD_BTREE_DEDUP_DROP_BODY_0, STD_BTREE_DEDUP_DROP_PAD_0);
	STD_BTREE_DEDUP_DROP_PAD_0(self);
}

/* append.rs */

/* The state of bulk_push across its pad: its arguments, the iterator it takes, and the key and value it holds. */
typedef struct STD_BTREE_BULK_PUSH_FRAME_0
{
	StdBTreeNodeRef *self;
	STD_BTREE_DEDUP_SORTED_ITER iter;
	bool		iter_live;
	size_t	   *length;
	STD_BTREE_KV_T item;
	bool		item_live;
}			STD_BTREE_BULK_PUSH_FRAME_0;

/* The body of bulk_push. */
static void
STD_BTREE_BULK_PUSH_BODY_0(void *frame_)
{
	STD_BTREE_BULK_PUSH_FRAME_0 *frame = frame_;
	StdBTreeNodeRef cur_node = STD_BTREE_LAST_LEAF_EDGE(*frame->self).node;

	while (STD_BTREE_DEDUP_NEXT(&frame->iter, &frame->item))
	{
		frame->item_live = true;
		if (STD_BTREE_LEN(&cur_node) < STD_BTREE_CAPACITY)
		{
			frame->item_live = false;
			STD_BTREE_LEAF_PUSH(&cur_node, &frame->item.f0, &frame->item.f1 STD_BTREE_TABLE_ARGS);
		}
		else
		{
			StdBTreeNodeRef open_node;
			StdBTreeNodeRef test_node = cur_node;
			StdBTreeNodeRef right_tree;
			size_t		tree_height;

			for (;;)
			{
				StdBTreeHandle parent;
				StdBTreeNodeRef root;

				if (STD_BTREE_ASCEND(&test_node, &parent, &root))
				{
					if (STD_BTREE_LEN(&parent.node) < STD_BTREE_CAPACITY)
					{
						open_node = parent.node;
						break;
					}
					test_node = parent.node;
				}
				else
				{
					open_node = STD_BTREE_PUSH_INTERNAL_LEVEL(frame->self);
					break;
				}
			}
			tree_height = open_node.height - 1;
			right_tree = STD_BTREE_NEW_LEAF();
			for (size_t i = 0, i_end = tree_height; i < i_end; i = std_forward_unchecked_usize_Unchecked(i, 1))
				(void) STD_BTREE_PUSH_INTERNAL_LEVEL(&right_tree);
			frame->item_live = false;
			STD_BTREE_INTERNAL_PUSH(&open_node, &frame->item.f0, &frame->item.f1, &right_tree STD_BTREE_TABLE_ARGS);
			cur_node = STD_BTREE_LAST_LEAF_EDGE(open_node).node;
		}
		*frame->length += 1;
	}
	frame->iter_live = false;
	STD_BTREE_DEDUP_DROP(&frame->iter);
	STD_BTREE_FIX_RIGHT_BORDER_OF_PLENTIFUL(frame->self);
}

/* The pad of bulk_push: the key and value it holds, then the iterator it takes. */
static void
STD_BTREE_BULK_PUSH_PAD_0(void *frame_)
{
	STD_BTREE_BULK_PUSH_FRAME_0 *frame = frame_;

	if (frame->item_live)
		STD_BTREE_KV_DROP(&frame->item);
	if (frame->iter_live)
		STD_BTREE_DEDUP_DROP(&frame->iter);
}

/*
 * The instance's NodeRef<Owned, K, V, LeafOrInternal>::bulk_push: the
 * iterator's sorted keys and values appended at the tree's right border,
 * each new key counted, then the border filled out.
 */
static void
STD_BTREE_BULK_PUSH(StdBTreeNodeRef *self, STD_BTREE_DEDUP_SORTED_ITER * iter, size_t *length)
{
	STD_BTREE_BULK_PUSH_FRAME_0 frame = {.self = self, .iter = *iter, .iter_live = true, .length = length};

	std_landing(&frame, STD_BTREE_BULK_PUSH_BODY_0, STD_BTREE_BULK_PUSH_PAD_0);
}

/* map.rs */

/* The state of bulk_build_from_sorted_iter's root across its pad: the iterator it takes, and the root. */
typedef struct STD_BTREE_BULK_BUILD_FRAME_0
{
	StdIterator iter;
	StdBTreeNodeRef root;
}			STD_BTREE_BULK_BUILD_FRAME_0;

/* The body of bulk_build_from_sorted_iter's root: Root::new. */
static void
STD_BTREE_BULK_BUILD_BODY_0(void *frame_)
{
	STD_BTREE_BULK_BUILD_FRAME_0 *frame = frame_;

	frame->root = STD_BTREE_NEW_LEAF();
}

/* The pad of bulk_build_from_sorted_iter's root: the iterator it takes, should the root's allocation panic. */
static void
STD_BTREE_BULK_BUILD_PAD_0(void *frame_)
{
	STD_BTREE_BULK_BUILD_FRAME_0 *frame = frame_;

	std_iterator_drop(&frame->iter);
}

/*
 * The instance's BTreeMap<K, V>::bulk_build_from_sorted_iter: a tree of the
 * iterator's sorted keys and values, each key once, the iterator dropped
 * should the root's allocation panic.
 */
static StdBTreeMap
STD_BTREE_BULK_BUILD(StdIterator iter)
{
	StdBTreeMap result;
	STD_BTREE_BULK_BUILD_FRAME_0 frame = {.iter = iter};
	STD_BTREE_DEDUP_SORTED_ITER dedup = {0};

	std_landing(&frame, STD_BTREE_BULK_BUILD_BODY_0, STD_BTREE_BULK_BUILD_PAD_0);
	result.root = frame.root;
	result.length = 0;
	dedup.iter = frame.iter;
	STD_BTREE_BULK_PUSH(&result.root, &dedup, &result.length);
	return result;
}

#ifdef STD_BTREE_SET

/* set.rs */

/* The instance's BTreeSet<T>::new: a set of nothing, its map with no root. */
STD_BTREE_DEF StdBTreeMap
STD_BTREE_SET_NEW(void)
{
	StdBTreeMap result = STD_BTREE_MAP_NEW;

	return result;
}

/* The state of from_sorted_iter's map: the iterator of keys it maps. */
typedef struct STD_BTREE_SET_FROM_SORTED_ITER_MAP
{
	StdIterator iter;
}			STD_BTREE_SET_FROM_SORTED_ITER_MAP;

/* The next of from_sorted_iter's map: the next key, with its zero-sized value, its closure inlined. */
static bool
STD_BTREE_SET_FROM_SORTED_ITER_MAP_NEXT(void *self_, void *result_)
{
	STD_BTREE_SET_FROM_SORTED_ITER_MAP *self = self_;
	STD_BTREE_KV_T *result = result_;

	if (!self->iter.ops->next(self->iter.self, &result->f0))
		return false;
	STD_BTREE_VAL_DEFAULT(&result->f1);
	return true;
}

/* The size_hint of from_sorted_iter's map: its iterator's. */
static StdSizeHint
STD_BTREE_SET_FROM_SORTED_ITER_MAP_SIZE_HINT(const void *self_)
{
	const		STD_BTREE_SET_FROM_SORTED_ITER_MAP *self = self_;

	return self->iter.ops->size_hint(self->iter.self);
}

/* The drop of from_sorted_iter's map: its iterator's. */
static void
STD_BTREE_SET_FROM_SORTED_ITER_MAP_DROP(void *self_)
{
	STD_BTREE_SET_FROM_SORTED_ITER_MAP *self = self_;

	std_iterator_drop(&self->iter);
}

/* The Iterator table of from_sorted_iter's map. */
static const StdIteratorOps STD_BTREE_SET_FROM_SORTED_ITER_MAP_OPS = {STD_BTREE_SET_FROM_SORTED_ITER_MAP_NEXT,
STD_BTREE_SET_FROM_SORTED_ITER_MAP_SIZE_HINT, STD_BTREE_SET_FROM_SORTED_ITER_MAP_DROP};

/* The instance's BTreeSet<T>::from_sorted_iter: a set of the iterator's sorted keys, each once, taking the iterator. */
static StdBTreeMap
STD_BTREE_SET_FROM_SORTED_ITER(StdIterator iter)
{
	STD_BTREE_SET_FROM_SORTED_ITER_MAP map = {iter};
	StdIterator mapped = {&STD_BTREE_SET_FROM_SORTED_ITER_MAP_OPS, &map};

	return STD_BTREE_BULK_BUILD(mapped);
}

#ifdef STD_BTREE_KEY_NEEDS_DROP

/* The instance's K's drop glue, on a pointer to the key. */
static void
STD_BTREE_KEY_DROP_GLUE(void *k)
{
	STD_BTREE_KEY_DROP((STD_BTREE_K *) k);
}

/* The instance's array::IntoIter<K, N>'s drop: the keys still alive, each dropped past one whose drop panics. */
static void
STD_BTREE_ARRAY_INTO_ITER_DROP(void *self_)
{
	StdArrayIntoIter *self = self_;

	std_slice_get_unchecked_mut_range_check(self->start, self->end, self->len, STD_BTREE_ARRAY_ITER_INNER_FILE, 19, 23);
	std_ptr_drop_in_place_slice(STD_PTR_AT(self->data, self->start, sizeof(STD_BTREE_K)), self->end - self->start,
								sizeof(STD_BTREE_K), STD_BTREE_KEY_DROP_GLUE);
}

/* The instance's array::IntoIter<K, N>'s table of Iterator: std's next and size_hint, and its drop at K. */
static const StdIteratorOps STD_BTREE_ARRAY_INTO_ITER_OPS = {std_ArrayIntoIter_Iterator_next,
std_ArrayIntoIter_Iterator_size_hint, STD_BTREE_ARRAY_INTO_ITER_DROP};

#endif

/* The state of From<[T; N]>'s sort across its pad: the array it takes. */
typedef struct STD_BTREE_SET_FROM_ARRAY_FRAME_0
{
	STD_BTREE_K *arr;
	size_t		n;
}			STD_BTREE_SET_FROM_ARRAY_FRAME_0;

/* The body of From<[T; N]>'s sort: the array sorted stably. */
static void
STD_BTREE_SET_FROM_ARRAY_BODY_0(void *frame_)
{
	STD_BTREE_SET_FROM_ARRAY_FRAME_0 *frame = frame_;

	STD_BTREE_SLICE_SORT(frame->arr, frame->n);
}

/* The pad of From<[T; N]>'s sort: the array's drop, each key dropped past one whose drop panics. */
static void
STD_BTREE_SET_FROM_ARRAY_PAD_0(void *frame_)
{
#ifdef STD_BTREE_KEY_NEEDS_DROP
	STD_BTREE_SET_FROM_ARRAY_FRAME_0 *frame = frame_;

	std_ptr_drop_in_place_slice(frame->arr, frame->n, sizeof(STD_BTREE_K), STD_BTREE_KEY_DROP_GLUE);
#endif
}

/*
 * The instance's BTreeSet<T>'s From<[T; N]>: the array's elements sorted
 * stably, then a set of them by from_sorted_iter, each key once, its first
 * kept; taking the array, which is dropped should the sort panic.
 */
STD_BTREE_DEF StdBTreeMap
STD_BTREE_SET_FROM_ARRAY(STD_BTREE_K * arr, size_t n)
{
	StdArrayIntoIter into_iter;
#ifdef STD_BTREE_KEY_NEEDS_DROP
	StdIterator iter = {&STD_BTREE_ARRAY_INTO_ITER_OPS, &into_iter};
#else
	StdIterator iter = {&STD_ARRAY_INTO_ITER_ITERATOR_OPS, &into_iter};
#endif
	STD_BTREE_SET_FROM_ARRAY_FRAME_0 frame = {.arr = arr, .n = n};

	if (n == 0)
		return STD_BTREE_SET_NEW();
	std_landing(&frame, STD_BTREE_SET_FROM_ARRAY_BODY_0, STD_BTREE_SET_FROM_ARRAY_PAD_0);
	into_iter.data = arr;
	into_iter.len = n;
	into_iter.size = sizeof(STD_BTREE_K);
	into_iter.start = 0;
	into_iter.end = n;
	return STD_BTREE_SET_FROM_SORTED_ITER(iter);
}

#endif							/* STD_BTREE_SET */

#endif							/* STD_BTREE_BULK */

#undef STD_BTREE_LAYOUT_OF
#undef STD_BTREE_LEAF_SIZE
#undef STD_BTREE_LEAF_ALIGN
#undef STD_BTREE_PARENT_OFF
#undef STD_BTREE_PARENT_IDX_OFF
#undef STD_BTREE_LEN_OFF
#undef STD_BTREE_KEYS_OFF
#undef STD_BTREE_VALS_OFF
#undef STD_BTREE_INTERNAL_SIZE
#undef STD_BTREE_INTERNAL_ALIGN
#undef STD_BTREE_EDGES_OFF
#undef STD_BTREE_PARENT
#undef STD_BTREE_PARENT_IDX
#undef STD_BTREE_LEN_FIELD
#undef STD_BTREE_KEYS_OF
#undef STD_BTREE_VALS_OF
#undef STD_BTREE_V_SIZE
#undef STD_BTREE_VAL_AT
#undef STD_BTREE_VAL_MOVE
#undef STD_BTREE_EDGES_OF
#undef STD_BTREE_EDGES_LEN

#endif							/* STD_BTREE_DEFINE */

#undef STD_BTREE_CAT_
#undef STD_BTREE_CAT
#undef STD_BTREE_DEF_static
#undef STD_BTREE_DEF_extern
#undef STD_BTREE_DEF
#undef STD_BTREE_KV
#undef STD_BTREE_FN
#undef STD_BTREE_TYPE
#undef STD_BTREE_SPLIT_RESULT
#undef STD_BTREE_SPLIT_RESULT_T
#undef STD_BTREE_VACANT_ENTRY
#undef STD_BTREE_ENTRY
#undef STD_BTREE_GET
#undef STD_BTREE_MAP_ENTRY
#undef STD_BTREE_OR_DEFAULT
#undef STD_BTREE_VACANT_INSERT
#undef STD_BTREE_INSERT_ENTRY
#undef STD_BTREE_INSERT_ENTRY_CLOSURE_0
#undef STD_BTREE_INTO_MUT
#undef STD_BTREE_TAKE_MUT
#undef STD_BTREE_TAKE_MUT_CLOSURE_0
#undef STD_BTREE_TAKE_MUT_CLOSURE_0_DROP
#undef STD_BTREE_REPLACE
#undef STD_BTREE_LEAF_INIT
#undef STD_BTREE_LEAF_NEW
#undef STD_BTREE_INTERNAL_NEW
#undef STD_BTREE_NEW_LEAF
#undef STD_BTREE_FROM_NEW_LEAF
#undef STD_BTREE_NEW_INTERNAL
#undef STD_BTREE_FROM_NEW_INTERNAL
#undef STD_BTREE_FROM_INTERNAL
#undef STD_BTREE_AS_INTERNAL_PTR
#undef STD_BTREE_LEN
#undef STD_BTREE_ASCEND
#undef STD_BTREE_KEYS
#undef STD_BTREE_KEY_AREA_MUT
#undef STD_BTREE_KEY_AREA_MUT_TO
#undef STD_BTREE_KEY_AREA_MUT_RANGE
#undef STD_BTREE_VAL_AREA_MUT
#undef STD_BTREE_VAL_AREA_MUT_TO
#undef STD_BTREE_VAL_AREA_MUT_RANGE
#undef STD_BTREE_EDGE_AREA_MUT
#undef STD_BTREE_EDGE_AREA_MUT_TO
#undef STD_BTREE_EDGE_AREA_MUT_RANGE
#undef STD_BTREE_LEN_MUT
#undef STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS
#undef STD_BTREE_CORRECT_CHILDRENS_PARENT_LINKS_INCLUSIVE
#undef STD_BTREE_CORRECT_ALL_CHILDRENS_PARENT_LINKS
#undef STD_BTREE_SET_PARENT_LINK
#undef STD_BTREE_PUSH_INTERNAL_LEVEL
#undef STD_BTREE_PUSH_INTERNAL_LEVEL_CLOSURE_0
#undef STD_BTREE_PUSH_WITH_HANDLE
#undef STD_BTREE_INTERNAL_PUSH
#undef STD_BTREE_LEAF_INSERT_FIT
#undef STD_BTREE_LEAF_INSERT
#undef STD_BTREE_CORRECT_PARENT_LINK
#undef STD_BTREE_INTERNAL_INSERT_FIT
#undef STD_BTREE_INTERNAL_INSERT
#undef STD_BTREE_INSERT_RECURSING
#undef STD_BTREE_DESCEND
#undef STD_BTREE_INTO_KV
#undef STD_BTREE_INTO_VAL_MUT
#undef STD_BTREE_SPLIT_LEAF_DATA
#undef STD_BTREE_LEAF_SPLIT
#undef STD_BTREE_INTERNAL_SPLIT
#undef STD_BTREE_SLICE_INSERT_K
#undef STD_BTREE_SLICE_INSERT_V
#undef STD_BTREE_SLICE_INSERT_EDGE
#undef STD_BTREE_SEARCH_TREE
#undef STD_BTREE_SEARCH_NODE
#undef STD_BTREE_FIND_KEY_INDEX
#undef STD_BTREE_ITER
#undef STD_BTREE_MAP_KEYS
#undef STD_BTREE_ITER_NEXT
#undef STD_BTREE_KEYS_NEXT
#undef STD_BTREE_REPLACE_KV
#undef STD_BTREE_REPLACE_KV_FRAME_0
#undef STD_BTREE_REPLACE_KV_BODY_0
#undef STD_BTREE_REPLACE_KV_PAD_0
#undef STD_BTREE_FULL_RANGE_OF
#undef STD_BTREE_FULL_RANGE
#undef STD_BTREE_LAZY_NEXT_UNCHECKED
#undef STD_BTREE_INIT_FRONT
#undef STD_BTREE_LEAF_NEXT_KV
#undef STD_BTREE_EDGE_NEXT_UNCHECKED
#undef STD_BTREE_EDGE_NEXT_UNCHECKED_CLOSURE_0
#undef STD_BTREE_FIRST_LEAF_EDGE
#undef STD_BTREE_NEXT_LEAF_EDGE
#undef STD_BTREE_FIRST_EDGE
#undef STD_BTREE_RIGHT_KV
#undef STD_BTREE_RIGHT_EDGE
#undef STD_BTREE_SET_FN
#undef STD_BTREE_SET_ITER
#undef STD_BTREE_SET_ITER_NEXT
#undef STD_BTREE_SET_INTO_ITER
#undef STD_BTREE_KV_T
#undef STD_BTREE_DEDUP_SORTED_ITER
#undef STD_BTREE_LAST_EDGE
#undef STD_BTREE_LAST_KV
#undef STD_BTREE_LEAF_PUSH
#undef STD_BTREE_LEFT_EDGE
#undef STD_BTREE_CONSIDER_FOR_BALANCING
#undef STD_BTREE_KV_MUT
#undef STD_BTREE_REPLACE_KV_MUT
#undef STD_BTREE_SLICE_SHR_K
#undef STD_BTREE_SLICE_SHR_V
#undef STD_BTREE_SLICE_SHR_EDGE
#undef STD_BTREE_BULK_STEAL_LEFT
#undef STD_BTREE_LAST_LEAF_EDGE
#undef STD_BTREE_FIX_RIGHT_BORDER_OF_PLENTIFUL
#undef STD_BTREE_PEEKABLE_NEXT
#undef STD_BTREE_PEEKABLE_PEEK
#undef STD_BTREE_DEDUP_NEXT
#undef STD_BTREE_DEDUP_DROP
#undef STD_BTREE_BULK_PUSH
#undef STD_BTREE_BULK_PUSH_FRAME_0
#undef STD_BTREE_BULK_PUSH_BODY_0
#undef STD_BTREE_BULK_PUSH_PAD_0
#undef STD_BTREE_BULK_BUILD
#undef STD_BTREE_SET_NEW
#undef STD_BTREE_SET_FROM_SORTED_ITER
#undef STD_BTREE_SET_FROM_SORTED_ITER_MAP
#undef STD_BTREE_SET_FROM_SORTED_ITER_MAP_NEXT
#undef STD_BTREE_SET_FROM_SORTED_ITER_MAP_SIZE_HINT
#undef STD_BTREE_SET_FROM_SORTED_ITER_MAP_DROP
#undef STD_BTREE_SET_FROM_SORTED_ITER_MAP_OPS
#undef STD_BTREE_SET_FROM_ARRAY
#undef STD_BTREE_SET_FROM_ARRAY_FRAME_0
#undef STD_BTREE_SET_FROM_ARRAY_BODY_0
#undef STD_BTREE_SET_FROM_ARRAY_PAD_0
#undef STD_BTREE_KEY_DROP_GLUE
#undef STD_BTREE_ARRAY_INTO_ITER_DROP
#undef STD_BTREE_ARRAY_INTO_ITER_OPS
#undef STD_BTREE_KV_DROP
#undef STD_BTREE_BULK_BUILD_FRAME_0
#undef STD_BTREE_BULK_BUILD_BODY_0
#undef STD_BTREE_BULK_BUILD_PAD_0
#undef STD_BTREE_KV_DROP_BODY_0
#undef STD_BTREE_KV_DROP_PAD_0
#undef STD_BTREE_DEDUP_NEXT_FRAME_0
#undef STD_BTREE_DEDUP_NEXT_BODY_0
#undef STD_BTREE_DEDUP_NEXT_PAD_0
#undef STD_BTREE_DEDUP_DROP_BODY_0
#undef STD_BTREE_DEDUP_DROP_PAD_0
#undef STD_BTREE_BULK
#undef STD_BTREE_KEY_NE
#undef STD_BTREE_SLICE_SORT
#undef STD_BTREE_KEY_NEEDS_DROP
#undef STD_BTREE_MAP_DROP
#undef STD_BTREE_INTO_ITER
#undef STD_BTREE_INTO_ITER_DROP
#undef STD_BTREE_INTO_ITER_DROP_FRAME_0
#undef STD_BTREE_INTO_ITER_DROP_BODY_0
#undef STD_BTREE_INTO_ITER_DROP_PAD_0
#undef STD_BTREE_DYING_NEXT
#undef STD_BTREE_INTO_ITER_NEXT
#undef STD_BTREE_INTO_KEY_VAL
#undef STD_BTREE_INTO_DEF
#undef STD_BTREE_INTO_ITERATE
#undef STD_BTREE_TAKE_FRONT
#undef STD_BTREE_LAZY_DEALLOCATING_NEXT_UNCHECKED
#undef STD_BTREE_LAZY_DEALLOCATING_END
#undef STD_BTREE_EDGE_DEALLOCATING_NEXT
#undef STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED
#undef STD_BTREE_EDGE_DEALLOCATING_NEXT_UNCHECKED_CLOSURE_0
#undef STD_BTREE_EDGE_DEALLOCATING_END
#undef STD_BTREE_REPLACE_DYING
#undef STD_BTREE_REPLACE_DYING_FRAME_0
#undef STD_BTREE_REPLACE_DYING_BODY_0
#undef STD_BTREE_REPLACE_DYING_PAD_0
#undef STD_BTREE_DEALLOCATE_AND_ASCEND
#undef STD_BTREE_DROP_KEY_VAL
#undef STD_BTREE_DROP_KEY_VAL_FRAME_0
#undef STD_BTREE_DROP_KEY_VAL_BODY_0
#undef STD_BTREE_DROP_KEY_VAL_PAD_0
#undef STD_BTREE_DROP
#undef STD_BTREE_SPLIT_ROOT

#undef STD_BTREE_SCOPE
#undef STD_BTREE_DECLARE
#undef STD_BTREE_DEFINE
#undef STD_BTREE_KS
#undef STD_BTREE_K
#undef STD_BTREE_VS
#undef STD_BTREE_V
#undef STD_BTREE_V_ZERO_SIZED
#undef STD_BTREE_ITERATE
#undef STD_BTREE_SET
#undef STD_BTREE_LAYOUT
#undef STD_BTREE_CMP
#undef STD_BTREE_KEY_DROP
#undef STD_BTREE_VAL_DROP
#undef STD_BTREE_VAL_DEFAULT
#undef STD_BTREE_TABLES
#undef STD_BTREE_TABLE_ARGS
