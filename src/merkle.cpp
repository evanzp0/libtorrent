/*

Copyright (c) 2015, 2017, 2019-2021, Arvid Norberg
Copyright (c) 2015, Mike Tzou
Copyright (c) 2017, Steven Siloti
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/aux_/merkle.hpp"
#include "libtorrent/aux_/vector.hpp"
#include "libtorrent/bitfield.hpp"

namespace libtorrent {

	int merkle_layer_start(int const layer)
	{
		TORRENT_ASSERT(layer >= 0);
		TORRENT_ASSERT(layer < int(sizeof(int) * 8));
		return (1 << layer) - 1;
	}

	int merkle_to_flat_index(int const layer, int const offset)
	{
		TORRENT_ASSERT(layer >= 0);
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(layer < int(sizeof(int) * 8));
		return merkle_layer_start(layer) + offset;
	}

	/**
	 * 获取Merkle树中指定节点的父节点
	 * 
	 * @param tree_node 当前节点的索引。索引从0开始，0代表根节点。
	 * 
	 * @return int 返回父节点的索引。
	 * 
	 * @example
	 * - 假设 tree_node 是 3，父节点索引：(3 - 1) / 2 = 1。
	 * - 假设 tree_node 是 6，父节点索引：(6 - 1) / 2 = 2。
	 * 
	 *         0
	 *       /   \
	 *      1      2
	 *     / \    / \
	 *    3   4  5   6
	 * 
	 */
	int merkle_get_parent(int const tree_node)
	{
		// node 0 doesn't have a parent
		TORRENT_ASSERT(tree_node > 0);
		return (tree_node - 1) / 2;
	}

	int merkle_get_sibling(int const tree_node)
	{
		// node 0 doesn't have a sibling
		TORRENT_ASSERT(tree_node > 0);
		// even numbers have their sibling to the left
		// odd numbers have their sibling to the right
		return tree_node + ((tree_node&1)?1:-1);
	}

	int merkle_get_first_child(int const tree_node)
	{
		return tree_node * 2 + 1;
	}

	int merkle_get_first_child(int const tree_node, int depth)
	{
		return ((tree_node + 1) << depth) - 1;
	}

	int merkle_num_nodes(int const leafs)
	{
		TORRENT_ASSERT(leafs > 0);
		TORRENT_ASSERT(leafs <= (std::numeric_limits<int>::max() / 2) + 1);
		TORRENT_ASSERT((leafs & (leafs - 1)) == 0);
		// This is a way to calculate: (leafs << 1) - 1 without requiring an extra
		// bit in the far left. The first 1 we subtract is worth 2 after we
		// multiply by 2, so by just adding back one, we have effectively
		// subtracted one from the result of multiplying by 2
		return ((leafs - 1) << 1) + 1;
	}

	int merkle_first_leaf(int num_leafs)
	{
		// num_leafs must be a power of 2
		TORRENT_ASSERT(((num_leafs - 1) & num_leafs) == 0);
		TORRENT_ASSERT(num_leafs > 0);
		return num_leafs - 1;
	}

	/**
	 * 用于计算 Merkle 树所需的叶子节点数量。
	 * 
	 * @param blocks 实际数据块数量（可以是 block 或者 piece 数）。
	 * 
	 * @return 返回所需叶子节点的数量。
	 * 
	 * @note
	 * - 其核心逻辑是将输入块数(blocks)向上取整到最近的 2 的幂次方。
	 */
	int merkle_num_leafs(int const blocks)
	{
		TORRENT_ASSERT(blocks > 0);
		// 防止整数溢出, blocks <= INT_MAX / 2 可以确保最后一次左移后 ret 不会溢出。
		TORRENT_ASSERT(blocks <= std::numeric_limits<int>::max() / 2);
		// round up to nearest 2 exponent
		// 最小的2的幂次方（2^0=1）
		int ret = 1;

		// 左移等价于乘以 2，直到 ret 刚好 >= blocks。
		while (blocks > ret) 
			ret <<= 1; 

		return ret;
	}

	/**
	 * 计算 Merkle 树的层数。
	 * 
	 * @param leaves 叶子节点数（必须为2的幂次方，如1, 2, 4, 8...）
	 * 
	 * @return 返回 Merkle 树的层数(log2(leaves))。
	 */
	int merkle_num_layers(int leaves)
	{
		// leaves must be a power of 2
		// leaves（叶子节点数）必须是 2 的幂次方。
		// 2的幂次方的二进制表示只有最高位是1（如 8=1000），因此 leaves & (leaves - 1) 必须为0
		TORRENT_ASSERT((leaves & (leaves - 1)) == 0);

		// 计算层数
		int layers = 0;
		// 右移等价于除以2，通过循环右移统计需要多少次除以 2 才能让 leaves 变为 1。
		while (leaves > 1)
		{
			++layers;
			leaves >>= 1;
		}
		return layers;
	}

	void merkle_fill_tree(span<sha256_hash> tree, int const num_leafs)
	{
		merkle_fill_tree(tree, num_leafs, merkle_num_nodes(num_leafs) - num_leafs);
	}

	/**
	 * 用于填充 Merkle 树的中间哈希节点。
	 * 
	 * @param tree 存储Merkle树节点的数组（叶子节点已预先填充）
	 * @param num_leafs 最底层的叶子节点数（必须 ≥ 1且为2的幂次方）
	 * @param level_start 当前层的起始的节点的索引，而不是层的索引。
	 * 
	 * @note
	 * 其核心功能是通过叶子节点的哈希值逐层计算父节点哈希，最终生成根哈希。
	 * 
	 * @example
	 * 1. 初始状态
	 * ```
	 * Level 0 (Leaves): [0, 1, 2, 3, 4, 5, 6, 7]
	 * Level 1: []
	 * Level 2: []
	 * Level 3 (Root): []
	 * ```
	 * 
	 * 2. 第一次迭代
	 * level_size = 8, level_start = 0
	 * 
	 * ```
	 * Parent(0,1) -> 8
	 * Parent(2,3) -> 9
	 * Parent(4,5) -> 10
	 * Parent(6,7) -> 11
	 * 
	 * Level 0: [0, 1, 2, 3, 4, 5, 6, 7]
	 * Level 1: [8, 9, 10, 11]
	 * Level 2: []
	 * Level 3: []
	 * ```
	 * 
	 * 3. 第二次迭代
	 * level_size = 4, level_start = 8
	 * 
	 * ```
	 * Parent(8,9) -> 12
	 * Parent(10,11) -> 13
	 * 
	 * Level 0: [0, 1, 2, 3, 4, 5, 6, 7]
	 * Level 1: [8, 9, 10, 11]
	 * Level 2: [12, 13]
	 * Level 3: []
	 * ```
	 * 
	 * 4. 第三次迭代
	 * level_size = 2, level_start = 12
	 * 
	 * ```
	 * Parent(12,13) -> 14
	 * 
	 * Level 0: [0, 1, 2, 3, 4, 5, 6, 7]
	 * Level 1: [8, 9, 10, 11]
	 * Level 2: [12, 13]
	 * Level 3: [14]
	 * ```
	 */
	void merkle_fill_tree(span<sha256_hash> tree, int const num_leafs, int level_start)
	{
		TORRENT_ASSERT(level_start >= 0);
		TORRENT_ASSERT(num_leafs >= 1);

		// 当前层的叶子节点数
		int level_size = num_leafs;

		// 循环直到根节点，
		// 每次循环处理当前层的所有节点，并生成上一层的父节点。
		while (level_size > 1)
		{
			//  返回节点 i 的父节点索引（通常为 i/2）
			int parent = merkle_get_parent(level_start);
			
			// 每一次 for 循环，要用该层的所有节点，计算它们的父节点的 hash（即上一层节点）。
			for (int i = level_start; i < level_start + level_size; i += 2, ++parent)
			// 循环一次使用 2 个 节点 （i, i + 1）来计算父节点的 tree 哈希值。
			{
				hasher256 h;
				h.update(tree[i]);
				h.update(tree[i + 1]);
				// 计算父节点的 hash 值并存入 tree 数组中。
				tree[parent] = h.final();
			}

			// 跳到父层起始节点位置
			level_start = merkle_get_parent(level_start);
			
			// 父层的节点数（表明每层的节点数是上一层的一半，这是典型的二叉树结构）。
			level_size /= 2;
		}
		TORRENT_ASSERT(level_size == 1);
	}

	void merkle_fill_partial_tree(span<sha256_hash> tree)
	{
		int const num_nodes = aux::numeric_cast<int>(tree.size());
		// the tree size must be one less than a power of two
		TORRENT_ASSERT(((num_nodes+1) & num_nodes) == 0);

		// we do two passes over the tree, first to compute all the missing
		// "interior" hashes. Then to clear all the ones that don't have a
		// parent (i.e. "orphan" hashes). We clear them since we can't validate
		// them against the root, which mean they may be incorrect.
		int const num_leafs = (num_nodes + 1) / 2;
		int level_size = num_leafs;
		int level_start = merkle_first_leaf(num_leafs);
		while (level_size > 1)
		{
			level_start = merkle_get_parent(level_start);
			level_size /= 2;

			for (int i = level_start; i < level_start + level_size; ++i)
			{
				int const child = merkle_get_first_child(i);
				bool const zeros_left = tree[child].is_all_zeros();
				bool const zeros_right = tree[child + 1].is_all_zeros();
				if (zeros_left || zeros_right) continue;
				hasher256 h;
				h.update(tree[child]);
				h.update(tree[child + 1]);
				tree[i] = h.final();
			}
		}
		TORRENT_ASSERT(level_size == 1);

		int parent = 0;
		for (int i = 1; i < int(tree.size()); i += 2, parent += 1)
		{
			if (tree[parent].is_all_zeros())
			{
				// if the parent is all zeros, the validation chain up to the
				// root is broken, and this cannot be validated
				tree[i].clear();
				tree[i + 1].clear();
			}
			else if (tree[i + 1].is_all_zeros())
			{
				// if the sibling is all zeros, this hash cannot be validated
				tree[i].clear();
			}
			else if (tree[i].is_all_zeros())
			{
				// if this hash is all zeros, the sibling hash cannot be validated
				tree[i + 1].clear();
			}
		}
	}

	void merkle_clear_tree(span<sha256_hash> tree, int const num_leafs, int level_start)
	{
		TORRENT_ASSERT(num_leafs >= 1);
		TORRENT_ASSERT(level_start >= 0);
		TORRENT_ASSERT(level_start < tree.size());
		TORRENT_ASSERT(level_start + num_leafs <= tree.size());
		// the range of nodes must be within a single level
		TORRENT_ASSERT(merkle_get_layer(level_start) == merkle_get_layer(level_start + num_leafs - 1));

		int level_size = num_leafs;
		for (;;)
		{
			for (int i = level_start; i < level_start + level_size; ++i)
				tree[i].clear();
			if (level_size == 1) break;
			level_start = merkle_get_parent(level_start);
			level_size /= 2;
		}
		TORRENT_ASSERT(level_size == 1);
	}

	// compute the merkle tree root, given the leaves and the has to use for
	// padding
	sha256_hash merkle_root(span<sha256_hash const> const leaves, sha256_hash const& pad)
	{
		int const num_leafs = merkle_num_leafs(int(leaves.size()));
		aux::vector<sha256_hash> merkle_tree;
		return merkle_root_scratch(leaves, num_leafs, pad, merkle_tree);
	}

	// compute the merkle tree root, given the leaves and the has to use for
	// padding
	sha256_hash merkle_root_scratch(span<sha256_hash const> leaves
		, int num_leafs, sha256_hash pad
		, std::vector<sha256_hash>& scratch_space)
	{
		TORRENT_ASSERT(((num_leafs - 1) & num_leafs) == 0);

		scratch_space.resize(std::size_t(leaves.size() + 1) / 2);
		TORRENT_ASSERT(num_leafs > 0);

		if (num_leafs == 1) return leaves[0];

		while (num_leafs > 1)
		{
			int i = 0;
			for (; i < int(leaves.size()) / 2; ++i)
			{
				scratch_space[std::size_t(i)] = hasher256()
					.update(leaves[i * 2])
					.update(leaves[i * 2 + 1])
					.final();
			}
			if (leaves.size() & 1)
			{
				// if we have an odd number of leaves, compute the boundary hash
				// here, that spans both a payload-hash and a pad hash
				scratch_space[std::size_t(i)] = hasher256()
					.update(leaves[i * 2])
					.update(pad)
					.final();
				++i;
			}
			// we don't have to copy any pad hashes into memory, they are implied
			// just keep track of the current layer's pad hash
			pad = hasher256().update(pad).update(pad).final();

			// step one level up
			leaves = span<sha256_hash const>(scratch_space.data(), i);
			num_leafs /= 2;
		}

		return scratch_space[0];
	}

	// returns the layer the given offset into the tree falls into.
	// Layer 0 is the root of the tree, layer 1 is the two hashes below the
	// root, and so on.
	int merkle_get_layer(int idx)
	{
		TORRENT_ASSERT(idx >= 0);
		int layer = 1;
		while (idx > (1 << layer) - 2) layer++;
		return layer - 1;
	}

	// returns the start of the layer, offset `idx` falls into.
	int merkle_get_layer_offset(int idx)
	{
		return idx - ((1 << merkle_get_layer(idx)) - 1);
	}

	// generates the pad hash for the tree level with "pieces" nodes, given the
	// full tree has "blocks" number of blocks.
	sha256_hash merkle_pad(int blocks, int pieces)
	{
		TORRENT_ASSERT(blocks >= pieces);
		sha256_hash ret{};
		while (pieces < blocks)
		{
			hasher256 h;
			h.update(ret);
			h.update(ret);
			ret = h.final();
			pieces *= 2;
		}
		return ret;
	}

	bool merkle_validate_and_insert_proofs(span<sha256_hash> target_tree
		, int const target_node_idx, sha256_hash const& node, span<sha256_hash const> uncle_hashes)
	{
		if (target_tree[target_node_idx] == node)
			return true;

		if (!target_tree[target_node_idx].is_all_zeros())
			return false;

		if (uncle_hashes.empty())
			return false;

		int cursor = target_node_idx;
		target_tree[cursor] = node;
		for (auto const& proof : uncle_hashes)
		{
			int const proof_idx = merkle_get_sibling(cursor);
			TORRENT_ASSERT(target_tree[proof_idx].is_all_zeros());
			target_tree[proof_idx] = proof;
			int const left = std::min(proof_idx, cursor);
			auto const hash = hasher256().update(target_tree[left]).update(target_tree[left + 1]).final();
			cursor = merkle_get_parent(cursor);
			if (target_tree[cursor] == hash) return true;
			if (!target_tree[cursor].is_all_zeros())
				break;
			target_tree[cursor] = hash;
		}

		// we get here if we never reached a known hash in the tree, i.e. the
		// uncle_hashes failed to prove the specified node hash.
		// we now need to clear up all the hashes we've inserted into the tree
		int clear_cursor = target_node_idx;
		while (clear_cursor > cursor)
		{
			int const proof_idx = merkle_get_sibling(clear_cursor);
			target_tree[clear_cursor].clear();
			target_tree[proof_idx].clear();
			clear_cursor = merkle_get_parent(clear_cursor);
		}
		return false;
	}

	bool merkle_validate_node(sha256_hash const& left, sha256_hash const& right
		, sha256_hash const& parent)
	{
		hasher256 h;
		h.update(left);
		h.update(right);
		return (h.final() == parent);
	}

	void merkle_validate_copy(span<sha256_hash const> const src
		, span<sha256_hash> const dst, sha256_hash const& root
		, bitfield& verified_leafs)
	{
		TORRENT_ASSERT(src.size() == dst.size());
		int const num_leafs = int((dst.size() + 1) / 2);
		if (src.empty()) return;
		if (src[0] != root) return;
		dst[0] = src[0];
		int const leaf_layer_start = int(src.size() - num_leafs);
		for (int i = 0; i < leaf_layer_start; ++i)
		{
			if (dst[i].is_all_zeros()) continue;
			int const left_child = merkle_get_first_child(i);
			int const right_child = left_child + 1;
			if (merkle_validate_node(src[left_child], src[right_child], dst[i]))
			{
				dst[left_child] = src[left_child];
				dst[right_child] = src[right_child];
				int const block_idx = left_child - leaf_layer_start;
				if (left_child >= leaf_layer_start
					&& block_idx < verified_leafs.size())
				{
					verified_leafs.set_bit(block_idx);
					// the right child may be the first block of padding hash,
					// in which case it's not part of the verified bitfield
					if (block_idx + 1 < verified_leafs.size())
						verified_leafs.set_bit(block_idx + 1);
				}
			}
		}
	}

	bool merkle_validate_single_layer(span<sha256_hash const> tree)
	{
		if (tree.size() == 1) return true;
		int const num_leafs = int((tree.size() + 1) / 2);
		int const end = int(tree.size());
		TORRENT_ASSERT((num_leafs & (num_leafs - 1)) == 0);

		int idx = merkle_first_leaf(num_leafs);
		TORRENT_ASSERT(idx >= 1);

		while (idx < end)
		{
			if (!merkle_validate_node(tree[idx], tree[idx + 1], tree[merkle_get_parent(idx)]))
				return false;
			idx += 2;
		}
		return true;
	}

	std::tuple<int, int, int> merkle_find_known_subtree(span<sha256_hash const> const tree
		, int const block_index, int const num_valid_leafs)
	{
		// find the largest block of leafs from a single subtree we know the hashes of
		int leafs_start = block_index;
		int leafs_size = 1;
		int const first_leaf = int(tree.size() / 2);
		int root_index = merkle_get_sibling(first_leaf + block_index);
		for (int i = block_index;; i >>= 1)
		{
			int const first_check_index = leafs_start + ((i & 1) ? -leafs_size : leafs_size);
			for (int j = 0; j < std::min(leafs_size, num_valid_leafs - first_check_index); ++j)
			{
				if (tree[first_leaf + first_check_index + j].is_all_zeros())
					return std::make_tuple(leafs_start, leafs_size, root_index);
			}
			if (i & 1) leafs_start -= leafs_size;
			leafs_size *= 2;
			root_index = merkle_get_parent(root_index);
			// if an inner node is known then its parent must be known too
			// so if the root is known the next sibling subtree should already
			// be computed if all of its leafs have valid hashes
			if (!tree[root_index].is_all_zeros()) break;
			TORRENT_ASSERT(root_index != 0);
			TORRENT_ASSERT(leafs_start >= 0);
			TORRENT_ASSERT(leafs_size <= merkle_num_leafs(num_valid_leafs));
		}

		TORRENT_ASSERT(leafs_start >= 0);
		TORRENT_ASSERT(leafs_start < merkle_num_leafs(num_valid_leafs));
		TORRENT_ASSERT(leafs_start + leafs_size > block_index);

		return std::make_tuple(leafs_start, leafs_size, root_index);
	}
}

