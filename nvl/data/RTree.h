#pragma once

#include "nvl/data/List.h"
#include "nvl/data/Map.h"
#include "nvl/data/Once.h"
#include "nvl/data/Range.h"
#include "nvl/data/Ref.h"
#include "nvl/data/Set.h"
#include "nvl/geo/Box.h"
#include "nvl/geo/Pos.h"
#include "nvl/macros/Aliases.h"
#include "nvl/macros/ReturnIf.h"
#include "nvl/math/Bitwise.h"
#include "nvl/traits/HasBox.h"
#include "nvl/traits/HasID.h"

namespace nvl {

namespace rtree_detail {

/**
 * @class Node
 * @brief A node within an RTree.
 */
template <U64 N, typename Value>
struct Node {
    struct Parent {
        Node *node; // The parent node
        Box<N> box; // Bounds in reference to the parent's grid
    };

    struct Entry {
        enum Kind { kNode, kList };
        Kind kind = kList;
        Node *node = nullptr;
        List<Ref<Value>> list;
    };

    Node() = default;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    pure bool operator==(const Node &rhs) const { return id == rhs.id; }
    pure bool operator!=(const Node &rhs) const { return !(*this == rhs); }

    pure Entry *get(const Pos<N> &pos) const { return map.get(pos.clamp_down(grid)); }

    void init_list(const Pos<N> &pos, Value &value) { map[pos].list.emplace_back(value); }

    pure Range<typename Box<N>::pos_iterator> pos_iter(const Box<N> &vol) const {
        return vol.clamp(grid).pos_iter(Pos<N>::fill(grid));
    }

    U64 id = -1;
    Maybe<Parent> parent;
    I64 grid = -1;
    Map<Pos<N>, Entry> map;
};

template <U64 N, typename Value>
struct Work {
    using Node = Node<N, Value>;

    explicit Work(Node *node, const Box<N> &volume) : node(node), vol(volume) {}

    pure const Pos<N> &pos() const {
        ASSERT(pair_range.has_value(), "Attempted to dereference an empty iterator.");
        return *pair_range.begin();
    }

    pure Value &value() const {
        ASSERT(node != nullptr && list_range.has_value(), "Attempted to dereference an empty iterator.");
        return list_range->raw();
    }

    Node *node;
    Box<N> vol;

    // 1) Iterating across values
    Once<typename List<Ref<Value>>::iterator> list_range;

    // 2) Iterating within a node - (node, pos) pairs
    Once<typename Box<N>::pos_iterator> pair_range;
};

} // namespace rtree_detail

/**
 * @class RTree
 * @brief Data structure for storing volumes within an N-dimensional space with O(log(N)) lookup.
 *
 * @tparam N Number of dimensions in the N-dimensional space.
 * @tparam Value Value type being stored.
 * @tparam kMaxEntries Maximum number of entries per node. Defaults to 10.
 * @tparam kGridExpMin Minimum node grid size (2 ^ min_grid_exp). Defaults to 2.
 * @tparam kGridExpMax Initial grid size of the root. (2 ^ root_grid_exp). Defaults to 10.
 */
template <U64 N, typename Value, U64 kMaxEntries = 10, U64 kGridExpMin = 2, U64 kGridExpMax = 10>
    requires traits::HasBox<Value> && traits::HasID<Value>
class RTree {
    friend struct Debug;
    using Work = rtree_detail::Work<N, Value>;
    using Node = rtree_detail::Node<N, Value>;

public:
    class window_iterator;
    using value_iterator = typename Map<U64, Value>::value_iterator;

    explicit RTree() : root_(next_node(None, grid_max, {})) {}

    explicit RTree(const List<Value> &values) : RTree() {
        for (const auto &value : values) {
            insert(value);
        }
    }

    /// Inserts the value into the tree.
    RTree &insert(const Value &value) { return insert_over(value, value.box(), value.id(), true); }

    /// Removes the value from the tree.
    RTree &remove(const Value &value) { return remove_over(value.box(), value.id(), true); }

    /// Registers a value as having moved from the previous volume `prev` to its current volume.
    RTree &move(const Value &value, const Box<N> &prev) { return move(value.box(), value.id(), prev); }

    /// Returns an iterator over all unique stored values in the given volume.
    pure Range<window_iterator> operator[](const Pos<N> &pos) {
        return Range<window_iterator>(*this, Box<N>::unit(pos));
    }
    pure Range<window_iterator> operator[](const Box<N> &box) { return Range<window_iterator>(*this, box); }

    /// Returns a Range for unordered iteration over all values in this tree.
    pure Range<value_iterator> unordered() { return values_.unordered_values(); }

    /// Returns the current bounding box for this tree.
    pure const Box<N> &bbox() const {
        if (bbox_.has_value())
            return bbox_.value();
        return Box<N>::kUnitBox;
    }

    /// Returns the shape of the bounding box for this tree.
    pure Pos<N> shape() const { return bbox().shape(); }

    /// Returns the total number of distinct values stored in this tree.
    pure U64 size() const { return values_.size(); }

    /// Returns true if this tree is empty.
    pure bool empty() const { return values_.empty(); }

    void clear() {
        bbox_ = None;
        node_id_ = 0;
        values_.clear();
        nodes_.clear();
        garbage_.clear();
        root_ = next_node(None, grid_max, {});
    }

    struct Debug {
        static std::ostream &indented(const U64 n) {
            for (U64 i = 0; i < n; i++) {
                std::cout << "  ";
            }
            return std::cout;
        }

        explicit Debug(RTree &tree) : tree(tree) {}

        struct PreorderWork {
            PreorderWork(Node *node, const U64 depth) : node(node), depth(depth) {
                ASSERT(node->parent.has_value(), "No parent defined for node #" << node->id);
                const Box<N> &node_range = node->parent->box;
                indented(depth - 1) << ">>[" << node->id << "] @ " << node->grid << std::endl;
                pos_range = node_range.pos_iter(node->grid).once();
            }
            PreorderWork(Node *node, const Box<N> &bounds) : node(node), depth(0) {
                indented(depth) << "[" << node->id << "] @ " << node->grid << std::endl;
                pos_range = bounds.clamp(node->grid).pos_iter(node->grid).once();
            }
            Node *node;
            U64 depth;
            Once<typename Box<N>::pos_iterator> pos_range;
        };

        /// Dumps a string representation of this tree to stdout.
        void dump() const {
            const auto bounds = tree.bbox_.value_or(Box<N>::unit(Pos<N>::fill(1)));
            std::cout << "[[RTree with bounds " << bounds << "]]" << std::endl;

            List<PreorderWork> worklist;
            worklist.emplace_back(tree.root_, bounds);

            while (!worklist.empty()) {
                PreorderWork &current = worklist.back();
                Node *node = current.node;
                const U64 depth = current.depth;
                bool found_node = false;
                while (!found_node && current.pos_range.has_next()) {
                    const Pos<N> pos = *current.pos_range.begin();
                    ++current.pos_range.begin();
                    if (auto *entry = node->get(pos)) {
                        const Box<N> range(pos, pos + node->grid - 1);
                        indented(depth) << "[" << node->id << "][" << range << "]:" << std::endl;
                        if (entry->kind == Node::Entry::kList) {
                            if (entry->list.empty()) {
                                indented(depth) << ">> EMPTY LIST" << std::endl;
                            }
                            for (const Ref<Value> value : entry->list) {
                                indented(depth) << ">> " << value << std::endl;
                            }
                        } else {
                            worklist.emplace_back(entry->node, depth + 1);
                            found_node = true;
                        }
                    }
                }
                if (!worklist.back().pos_range.has_next()) {
                    worklist.pop_back();
                }
            }
        }

        /// Returns a Map from the lowest level volume buckets to all IDs contained in that bucket.
        pure Map<Box<N>, Set<U64>> collect_ids() const {
            Map<Box<N>, Set<U64>> ids;
            for (const auto &[node, pos] : tree.entries_in(tree.bbox_.value_or(Box<N>::kUnitBox))) {
                if (auto *entry = node->get(pos); entry && entry->kind == Node::Entry::kList) {
                    const Box<N> box(pos, pos + node->grid - 1);
                    for (const auto &value : entry->list) {
                        ids[box].insert(value->id());
                    }
                }
            }
            return ids;
        }

        /// Returns the total number of nodes in this tree.
        pure U64 nodes() const { return tree.nodes_.size(); }

        /// Returns the maximum depth, in nodes, of this tree.
        pure U64 depth() const {
            const U64 root_grid = tree.root_->grid;
            U64 min_grid = root_grid;
            for (auto &[_, node] : tree.nodes_) {
                min_grid = (node.grid < min_grid) ? node.grid : min_grid;
            }
            return ceil_log2(root_grid) - ceil_log2(min_grid) + 1;
        }

        RTree &tree;
    } debug = Debug(*this);

private:
    enum class Traversal {
        kPoints,  // All possible points in existing nodes
        kEntries, // All existing entries
        kValues   // All existing values
    };
    template <Traversal mode, typename Concrete>
    class abstract_iterator {
    public:
        static Concrete begin(RTree &tree, const Box<N> &box) {
            Concrete iter(tree, box);
            if (tree.root_ != nullptr) {
                iter.worklist.emplace_back(tree.root_, box);
                iter.advance();
            }
            return iter;
        }

        static Concrete end(RTree &tree, const Box<N> &box) { return Concrete(tree, box); }

        Concrete &operator++() {
            advance();
            return *static_cast<Concrete *>(this);
        }

        pure bool operator==(const abstract_iterator &rhs) const {
            return &tree == &rhs.tree && worklist.get_back() == rhs.worklist.get_back();
        }
        pure bool operator!=(const abstract_iterator &rhs) const { return !(*this == rhs); }

    protected:
        explicit abstract_iterator(RTree &tree, const Box<N> &box) : tree(tree), box(box) {}

        bool skip_value(Work &current) {
            auto &value = current.value();
            return visited.contains(value.id()) || !value.box().overlaps(box);
        }

        bool visit_next_pair(Work &current) {
            Node *node = current.node;

            if (!current.pair_range.has_next()) {
                worklist.pop_back();
                return false;
            }

            const Pos<N> &pos = current.pos();

            if (auto *entry = node->get(pos)) {
                if (entry->kind == Node::Entry::kList) {
                    if constexpr (mode == Traversal::kValues) {
                        current.list_range = {entry->list.begin(), entry->list.end()};
                        while (current.list_range.has_next() && skip_value(current)) {
                            ++current.list_range;
                        }
                        if (current.list_range.has_next()) {
                            // Start visiting this list if there is at least one valid value
                            visited.insert(current.value().id());
                            return true;
                        }
                        // Skip this list entirely if it had no new unique values
                        return false;
                    }
                    // Visit this list or (node, pos)
                    return true;
                }
                ASSERT(entry->node->parent.has_value(), "Sub-node had no parent entry.");
                if (auto range = entry->node->parent->box.intersect(box)) {
                    // Continue to this child node by updating the worklist
                    worklist.emplace_back(entry->node, *range);
                }
            } else {
                // Visit this (currently unset) (node, pos) pair
                if constexpr (mode == Traversal::kPoints) {
                    return true;
                }
            }
            return false;
        }

        bool advance_list(Work &current) {
            do {
                ++current.list_range;
            } while (current.list_range.has_next() && skip_value(current));

            if (current.list_range.has_next()) {
                visited.insert(current.value().id());
                return true;
            }
            return false;
        }

        bool advance_pair(Work &current) {
            ++current.pair_range;
            return visit_next_pair(current);
        }

        bool advance_node(Work &current) {
            current.pair_range = current.node->pos_iter(current.vol).once();
            return visit_next_pair(current);
        }

        /// Advances this iterator to either the next list item, next (node, pos) pair, or next child node.
        void advance() {
            while (!worklist.empty()) {
                auto &current = worklist.back();
                if (current.list_range.has_next()) {
                    return_if(advance_list(current));
                } else if (current.pair_range.has_next()) {
                    return_if(advance_pair(current));
                } else {
                    return_if(advance_node(current));
                }
            }
        }

        // 3) Iterating across nodes
        List<Work> worklist = {};
        Set<U64> visited;

        RTree &tree;
        Box<N> box;
    };

    class entry_iterator : public abstract_iterator<Traversal::kEntries, entry_iterator> {
    public:
        using Parent = abstract_iterator<Traversal::kEntries, entry_iterator>;
        using value_type = std::pair<Node *, Pos<N>>;
        using pointer = value_type;
        using reference = value_type;

        entry_iterator() = delete;

        value_type operator*() const {
            ASSERT(!this->worklist.empty(), "Attempted to dereference an empty iterator");
            auto &current = this->worklist.back();
            return {current.node, current.pos()};
        }
        value_type operator->() const {
            ASSERT(!this->worklist.empty(), "Attempted to dereference an empty iterator");
            auto &current = this->worklist.back();
            return {current.node, current.pos()};
        }

    private:
        friend class RTree;
        using Parent::Parent;
    };

    class point_iterator : public abstract_iterator<Traversal::kPoints, point_iterator> {
    public:
        using Parent = abstract_iterator<Traversal::kPoints, point_iterator>;
        using value_type = std::pair<Node *, Pos<N>>;
        using pointer = value_type;
        using reference = value_type;

        point_iterator() = delete;

        value_type operator*() const {
            ASSERT(!this->worklist.empty(), "Attempted to dereference an empty iterator");
            auto &current = this->worklist.back();
            return {current.node, current.pos()};
        }
        value_type operator->() const {
            ASSERT(!this->worklist.empty(), "Attempted to dereference an empty iterator");
            auto &current = this->worklist.back();
            return {current.node, current.pos()};
        }

    private:
        friend class RTree;
        using Parent::Parent;
    };

    static constexpr I64 grid_min = 0x1 << kGridExpMin;
    static constexpr I64 grid_max = 0x1 << kGridExpMax;

public:
    class window_iterator : public abstract_iterator<Traversal::kValues, window_iterator> {
    public:
        using Parent = abstract_iterator<Traversal::kValues, window_iterator>;
        using value_type = Value;
        using pointer = Value *;
        using reference = Value &;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;

        window_iterator() = delete;

        pure INLINE bool has_value() const { return !this->worklist.empty(); }

        Value &operator*() const {
            ASSERT(has_value(), "Attempted to dereference empty iterator");
            return this->worklist.back().value();
        }
        Value *operator->() const {
            ASSERT(has_value(), "Attempted to dereference empty iterator");
            return &this->worklist.back().value();
        }

    private:
        friend class RTree;
        using Parent::Parent;
    };

private:
    static bool should_increase_depth(const U64 size, const U64 grid) { return size > kMaxEntries && grid > grid_min; }

    Node *next_node(const Maybe<typename Node::Parent> &parent, const I64 grid, const List<Ref<Value>> &values) {
        const Pos<N> grid_fill = Pos<N>::fill(grid);
        const U64 id = node_id_++;
        Node *node = &nodes_.emplace(std::piecewise_construct, std::forward_as_tuple(id), std::tuple{});
        node->parent = parent;
        node->id = id;
        node->grid = grid;
        for (const Ref<Value> value : values) {
            const Box<N> &value_box = value->box();
            Range<typename Box<N>::box_iterator> points; // empty iterable to start with
            if (parent.has_value()) {
                if (const Maybe<Box<N>> intersection = value_box.intersect(parent->box)) {
                    points = intersection->clamp(grid_fill).box_iter(grid_fill);
                }
            } else {
                points = value_box.clamp(grid_fill).box_iter(grid_fill);
            }
            for (const Box<N> &range : points) {
                if (range.overlaps(value_box)) {
                    typename Node::Entry &entry = node->map.get_or_add(range.min, {});
                    entry.list.push_back(value);
                }
            }
        }
        // Re-balance the newly created node. This may create more nodes!
        balance(node);
        return node;
    }

    void balance_pos(Node *node, const Pos<N> &pos) {
        if (auto *entry = node->get(pos)) {
            if (entry->kind == Node::Entry::kList) {
                if (should_increase_depth(entry->list.size(), node->grid)) {
                    const Box<N> child_box(pos, pos + node->grid - 1);
                    const I64 child_grid = node->grid / 2;
                    const typename Node::Parent parent{.node = node, .box = child_box};
                    entry->node = next_node(parent, child_grid, entry->list);
                    entry->kind = Node::Entry::kNode;
                    entry->list.clear();
                }
            } else if (entry->kind == Node::Entry::kNode) {
                balance(entry->node);
            }
        }
    }

    void balance(Node *node) {
        return_if(node->grid <= grid_min); // Can't further balance
        for (auto &[pos, _] : node->map.unordered_entries()) {
            balance_pos(node, pos);
        }
    }

    void balance(Node *node, const Pos<N> &pos) {
        return_if(node->grid <= grid_min); // Can't further balance
        balance_pos(node, pos);
    }

    void insert(Node *node, const Pos<N> &pos, Ref<Value> value) {
        node->map[pos].list.emplace_back(value);
        balance(node, pos);
    }

    void remove(Node *node, const Pos<N> &pos) {
        if (auto *entry = node->get(pos)) {
            if (entry->kind == Node::Entry::kNode) {
                Node *child = entry->node;
                garbage_.push_back(child->id);
            }
            node->map.remove(pos);
        }
        if (node->map.empty() && node->parent.has_value()) {
            remove(node->parent->node, node->parent->box.min);
        }
    }

    void remove(Node *node, const Pos<N> &pos, const Ref<Value> value) {
        if (auto *entry = node->get(pos)) {
            ASSERT(entry->kind == Node::Entry::kList, "Cannot remove from non-list entry");
            entry->list.remove(value); // TODO: O(N) with number of values here
            if (entry->list.empty()) {
                remove(node, pos);
            }
        }
    }

    Range<point_iterator> points_in(const Box<N> &box) { return Range<point_iterator>(*this, box); }
    Range<entry_iterator> entries_in(const Box<N> &box) { return Range<entry_iterator>(*this, box); }

    RTree &move(const Box<N> &new_box, const U64 id, const Box<N> &prev_box) {
        const Value &value = values_[id];
        for (const auto &removed : prev_box.diff(new_box)) {
            remove_over(removed, id, false);
        }
        for (const auto &added : new_box.diff(prev_box)) {
            insert_over(value, added, id, false);
        }
        return *this;
    }

    RTree &insert_over(const Value &value, const Box<N> &box, const U64 id, const bool is_new) {
        bbox_ = bbox_ ? bounding_box(*bbox_, box) : box;
        Value &value_entry = values_[id];
        if (is_new) {
            value_entry = value;
        }
        const Ref<Value> value_ref = value_entry;
        for (auto [node, pos] : points_in(box)) {
            insert(node, pos, value_ref);
        }
        return *this;
    }

    RTree &remove_over(const Box<N> &box, const U64 id, const bool remove_all) {
        if (Value *value = values_.get(id)) {
            // TODO: Update bounds?
            const Ref<Value> value_ref = *value;
            for (auto [node, pos] : entries_in(box)) {
                remove(node, pos, value_ref);
            }
            if (remove_all) {
                values_.remove(id);
            }
            for (const U64 removed_id : garbage_) {
                nodes_.erase(removed_id);
            }
            garbage_.clear();
        }
        return *this;
    }

    Maybe<Box<N>> bbox_ = None;
    U64 node_id_ = 0;

    // Nodes keep references to the values stored in the above map to avoid storing two copies of each value.
    // These references are guaranteed stable as long as the value itself is not removed from the map.
    // See: https://cplusplus.com/reference/unordered_map/unordered_map/operator[]/
    Map<U64, Value> values_;

    Map<U64, Node> nodes_;
    Node *root_;

    // List of nodes to be removed
    List<U64> garbage_;
};

} // namespace nvl
