/*

Copyright (c) 2013, Project OSRM, Dennis Luxen, others
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef STATICRTREE_H
#define STATICRTREE_H

#include "DeallocatingVector.h"
#include "HilbertValue.h"
#include "PhantomNodes.h"
#include "QueryNode.h"
#include "SharedMemoryFactory.h"
#include "SharedMemoryVectorWrapper.h"

#include "../ThirdParty/variant/variant.hpp"
#include "../Util/floating_point.hpp"
#include "../Util/MercatorUtil.h"
#include "../Util/OSRMException.h"
#include "../Util/simple_logger.hpp"
#include "../Util/TimingUtil.h"
#include "../typedefs.h"

#include <osrm/Coordinate.h>

#include <boost/assert.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/thread.hpp>

#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <vector>

// Implements a static, i.e. packed, R-tree
template <class EdgeDataT,
          class CoordinateListT = std::vector<FixedPointCoordinate>,
          bool UseSharedMemory = false,
          uint32_t BRANCHING_FACTOR=64,
          uint32_t LEAF_NODE_SIZE=1024>
class StaticRTree
{
  public:
    struct RectangleInt2D
    {
        RectangleInt2D() : min_lon(INT_MAX), max_lon(INT_MIN), min_lat(INT_MAX), max_lat(INT_MIN) {}

        int32_t min_lon, max_lon;
        int32_t min_lat, max_lat;

        inline void InitializeMBRectangle(const std::array<EdgeDataT, LEAF_NODE_SIZE> &objects,
                                          const uint32_t element_count,
                                          const std::vector<NodeInfo> &coordinate_list)
        {
            for (uint32_t i = 0; i < element_count; ++i)
            {
                min_lon = std::min(min_lon,
                                   std::min(coordinate_list.at(objects[i].u).lon,
                                            coordinate_list.at(objects[i].v).lon));
                max_lon = std::max(max_lon,
                                   std::max(coordinate_list.at(objects[i].u).lon,
                                            coordinate_list.at(objects[i].v).lon));

                min_lat = std::min(min_lat,
                                   std::min(coordinate_list.at(objects[i].u).lat,
                                            coordinate_list.at(objects[i].v).lat));
                max_lat = std::max(max_lat,
                                   std::max(coordinate_list.at(objects[i].u).lat,
                                            coordinate_list.at(objects[i].v).lat));
            }
            BOOST_ASSERT(min_lat != std::numeric_limits<int>::min());
            BOOST_ASSERT(min_lon != std::numeric_limits<int>::min());
            BOOST_ASSERT(max_lat != std::numeric_limits<int>::min());
            BOOST_ASSERT(max_lon != std::numeric_limits<int>::min());
        }

        inline void MergeBoundingBoxes(const RectangleInt2D &other)
        {
            min_lon = std::min(min_lon, other.min_lon);
            max_lon = std::max(max_lon, other.max_lon);
            min_lat = std::min(min_lat, other.min_lat);
            max_lat = std::max(max_lat, other.max_lat);
            BOOST_ASSERT(min_lat != std::numeric_limits<int>::min());
            BOOST_ASSERT(min_lon != std::numeric_limits<int>::min());
            BOOST_ASSERT(max_lat != std::numeric_limits<int>::min());
            BOOST_ASSERT(max_lon != std::numeric_limits<int>::min());
        }

        inline FixedPointCoordinate Centroid() const
        {
            FixedPointCoordinate centroid;
            // The coordinates of the midpoints are given by:
            // x = (x1 + x2) /2 and y = (y1 + y2) /2.
            centroid.lon = (min_lon + max_lon) / 2;
            centroid.lat = (min_lat + max_lat) / 2;
            return centroid;
        }

        inline bool Intersects(const RectangleInt2D &other) const
        {
            FixedPointCoordinate upper_left(other.max_lat, other.min_lon);
            FixedPointCoordinate upper_right(other.max_lat, other.max_lon);
            FixedPointCoordinate lower_right(other.min_lat, other.max_lon);
            FixedPointCoordinate lower_left(other.min_lat, other.min_lon);

            return (Contains(upper_left) || Contains(upper_right) || Contains(lower_right) ||
                    Contains(lower_left));
        }

        inline float GetMinDist(const FixedPointCoordinate &location) const
        {
            const bool is_contained = Contains(location);
            if (is_contained)
            {
                return 0.;
            }

            enum Direction
            {
                INVALID    = 0,
                NORTH      = 1,
                SOUTH      = 2,
                EAST       = 4,
                NORTH_EAST = 5,
                SOUTH_EAST = 6,
                WEST       = 8,
                NORTH_WEST = 9,
                SOUTH_WEST = 10
            };

            Direction d = INVALID;
            if (location.lat > max_lat)
                d = (Direction) (d | NORTH);
            else if (location.lat < min_lat)
                d = (Direction) (d | SOUTH);
            if (location.lon > max_lon)
                d = (Direction) (d | EAST);
            else if (location.lon < min_lon)
                d = (Direction) (d | WEST);

            BOOST_ASSERT(d != INVALID);

            float min_dist = std::numeric_limits<float>::max();
            switch (d)
            {
                case NORTH:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(max_lat, location.lon));
                    break;
                case SOUTH:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(min_lat, location.lon));
                    break;
                case WEST:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(location.lat, min_lon));
                    break;
                case EAST:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(location.lat, max_lon));
                    break;
                case NORTH_EAST:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(max_lat, max_lon));
                    break;
                case NORTH_WEST:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(max_lat, min_lon));
                    break;
                case SOUTH_EAST:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(min_lat, max_lon));
                    break;
                case SOUTH_WEST:
                    min_dist = FixedPointCoordinate::ApproximateEuclideanDistance(location, FixedPointCoordinate(min_lat, min_lon));
                    break;
                default:
                    break;
            }

            BOOST_ASSERT(min_dist != std::numeric_limits<float>::max());

            return min_dist;
        }

        inline float GetMinMaxDist(const FixedPointCoordinate &location) const
        {
            float min_max_dist = std::numeric_limits<float>::max();
            // Get minmax distance to each of the four sides
            const FixedPointCoordinate upper_left(max_lat, min_lon);
            const FixedPointCoordinate upper_right(max_lat, max_lon);
            const FixedPointCoordinate lower_right(min_lat, max_lon);
            const FixedPointCoordinate lower_left(min_lat, min_lon);

            min_max_dist = std::min(
                min_max_dist,
                std::max(
                    FixedPointCoordinate::ApproximateEuclideanDistance(location, upper_left),
                    FixedPointCoordinate::ApproximateEuclideanDistance(location, upper_right)));

            min_max_dist = std::min(
                min_max_dist,
                std::max(
                    FixedPointCoordinate::ApproximateEuclideanDistance(location, upper_right),
                    FixedPointCoordinate::ApproximateEuclideanDistance(location, lower_right)));

            min_max_dist = std::min(
                min_max_dist,
                std::max(FixedPointCoordinate::ApproximateEuclideanDistance(location, lower_right),
                         FixedPointCoordinate::ApproximateEuclideanDistance(location, lower_left)));

            min_max_dist = std::min(
                min_max_dist,
                std::max(FixedPointCoordinate::ApproximateEuclideanDistance(location, lower_left),
                         FixedPointCoordinate::ApproximateEuclideanDistance(location, upper_left)));
            return min_max_dist;
        }

        inline bool Contains(const FixedPointCoordinate &location) const
        {
            const bool lats_contained = (location.lat >= min_lat) && (location.lat <= max_lat);
            const bool lons_contained = (location.lon >= min_lon) && (location.lon <= max_lon);
            return lats_contained && lons_contained;
        }

        inline friend std::ostream &operator<<(std::ostream &out, const RectangleInt2D &rect)
        {
            out << rect.min_lat / COORDINATE_PRECISION << "," << rect.min_lon / COORDINATE_PRECISION
                << " " << rect.max_lat / COORDINATE_PRECISION << ","
                << rect.max_lon / COORDINATE_PRECISION;
            return out;
        }
    };

    using RectangleT = RectangleInt2D;

    struct TreeNode
    {
        TreeNode() : child_count(0), child_is_on_disk(false) {}
        RectangleT minimum_bounding_rectangle;
        uint32_t child_count : 31;
        bool child_is_on_disk : 1;
        uint32_t children[BRANCHING_FACTOR];
    };

  private:
    struct WrappedInputElement
    {
        explicit WrappedInputElement(const uint64_t _hilbert_value, const uint32_t _array_index)
            : m_hilbert_value(_hilbert_value), m_array_index(_array_index)
        {
        }

        WrappedInputElement() : m_hilbert_value(0), m_array_index(UINT_MAX) {}

        uint64_t m_hilbert_value;
        uint32_t m_array_index;

        inline bool operator<(const WrappedInputElement &other) const
        {
            return m_hilbert_value < other.m_hilbert_value;
        }
    };

    struct LeafNode
    {
        LeafNode() : object_count(0), objects() {}
        uint32_t object_count;
        std::array<EdgeDataT, LEAF_NODE_SIZE> objects;
    };

    struct QueryCandidate
    {
        explicit QueryCandidate(const float dist, const uint32_t n_id)
            : min_dist(dist), node_id(n_id)
        {
        }
        QueryCandidate() : min_dist(std::numeric_limits<float>::max()), node_id(UINT_MAX) {}
        float min_dist;
        uint32_t node_id;
        inline bool operator<(const QueryCandidate &other) const
        {
            // Attn: this is reversed order. std::pq is a max pq!
            return other.min_dist < min_dist;
        }
    };

    using IncrementalQueryNodeType = mapbox::util::variant<TreeNode, EdgeDataT>;
    struct IncrementalQueryCandidate
    {
        explicit IncrementalQueryCandidate(const float dist, const IncrementalQueryNodeType &node)
            : min_dist(dist), node(node)
        {
        }

        IncrementalQueryCandidate() : min_dist(std::numeric_limits<float>::max()) {}

        inline bool operator<(const IncrementalQueryCandidate &other) const
        {
            // Attn: this is reversed order. std::pq is a max pq!
            return other.min_dist < min_dist;
        }

        float min_dist;
        IncrementalQueryNodeType node;
    };

    typename ShM<TreeNode, UseSharedMemory>::vector m_search_tree;
    uint64_t m_element_count;
    const std::string m_leaf_node_filename;
    std::shared_ptr<CoordinateListT> m_coordinate_list;
    boost::filesystem::ifstream leaves_stream;

  public:
    StaticRTree() = delete;
    StaticRTree(const StaticRTree &) = delete;

    // Construct a packed Hilbert-R-Tree with Kamel-Faloutsos algorithm [1]
    explicit StaticRTree(std::vector<EdgeDataT> &input_data_vector,
                         const std::string tree_node_filename,
                         const std::string leaf_node_filename,
                         const std::vector<NodeInfo> &coordinate_list)
        : m_element_count(input_data_vector.size()), m_leaf_node_filename(leaf_node_filename)
    {
        SimpleLogger().Write() << "constructing r-tree of " << m_element_count
                               << " edge elements build on-top of " << coordinate_list.size()
                               << " coordinates";

        TIMER_START(construction);
        std::vector<WrappedInputElement> input_wrapper_vector(m_element_count);

        HilbertCode get_hilbert_number;

        // generate auxiliary vector of hilbert-values
        tbb::parallel_for(
            tbb::blocked_range<uint64_t>(0, m_element_count),
            [&input_data_vector, &input_wrapper_vector, &get_hilbert_number, &coordinate_list](
                const tbb::blocked_range<uint64_t> &range)
            {
                for (uint64_t element_counter = range.begin(); element_counter != range.end();
                     ++element_counter)
                {
                    WrappedInputElement &current_wrapper = input_wrapper_vector[element_counter];
                    current_wrapper.m_array_index = element_counter;

                    EdgeDataT const &current_element = input_data_vector[element_counter];

                    // Get Hilbert-Value for centroid in mercartor projection
                    FixedPointCoordinate current_centroid = EdgeDataT::Centroid(
                        FixedPointCoordinate(coordinate_list.at(current_element.u).lat,
                                             coordinate_list.at(current_element.u).lon),
                        FixedPointCoordinate(coordinate_list.at(current_element.v).lat,
                                             coordinate_list.at(current_element.v).lon));
                    current_centroid.lat =
                        COORDINATE_PRECISION * lat2y(current_centroid.lat / COORDINATE_PRECISION);

                    current_wrapper.m_hilbert_value = get_hilbert_number(current_centroid);
                }
            });

        // open leaf file
        boost::filesystem::ofstream leaf_node_file(leaf_node_filename, std::ios::binary);
        leaf_node_file.write((char *)&m_element_count, sizeof(uint64_t));

        // sort the hilbert-value representatives
        tbb::parallel_sort(input_wrapper_vector.begin(), input_wrapper_vector.end());
        std::vector<TreeNode> tree_nodes_in_level;

        // pack M elements into leaf node and write to leaf file
        uint64_t processed_objects_count = 0;
        while (processed_objects_count < m_element_count)
        {

            LeafNode current_leaf;
            TreeNode current_node;
            // SimpleLogger().Write() << "reading " << tree_size << " tree nodes in " <<
            // (sizeof(TreeNode)*tree_size) << " bytes";
            for (uint32_t current_element_index = 0; LEAF_NODE_SIZE > current_element_index;
                 ++current_element_index)
            {
                if (m_element_count > (processed_objects_count + current_element_index))
                {
                    uint32_t index_of_next_object =
                        input_wrapper_vector[processed_objects_count + current_element_index]
                            .m_array_index;
                    current_leaf.objects[current_element_index] =
                        input_data_vector[index_of_next_object];
                    ++current_leaf.object_count;
                }
            }

            // generate tree node that resemble the objects in leaf and store it for next level
            current_node.minimum_bounding_rectangle.InitializeMBRectangle(
                current_leaf.objects, current_leaf.object_count, coordinate_list);
            current_node.child_is_on_disk = true;
            current_node.children[0] = tree_nodes_in_level.size();
            tree_nodes_in_level.emplace_back(current_node);

            // write leaf_node to leaf node file
            leaf_node_file.write((char *)&current_leaf, sizeof(current_leaf));
            processed_objects_count += current_leaf.object_count;
        }

        // close leaf file
        leaf_node_file.close();

        uint32_t processing_level = 0;
        while (1 < tree_nodes_in_level.size())
        {
            std::vector<TreeNode> tree_nodes_in_next_level;
            uint32_t processed_tree_nodes_in_level = 0;
            while (processed_tree_nodes_in_level < tree_nodes_in_level.size())
            {
                TreeNode parent_node;
                // pack BRANCHING_FACTOR elements into tree_nodes each
                for (uint32_t current_child_node_index = 0;
                     BRANCHING_FACTOR > current_child_node_index;
                     ++current_child_node_index)
                {
                    if (processed_tree_nodes_in_level < tree_nodes_in_level.size())
                    {
                        TreeNode &current_child_node =
                            tree_nodes_in_level[processed_tree_nodes_in_level];
                        // add tree node to parent entry
                        parent_node.children[current_child_node_index] = m_search_tree.size();
                        m_search_tree.emplace_back(current_child_node);
                        // merge MBRs
                        parent_node.minimum_bounding_rectangle.MergeBoundingBoxes(
                            current_child_node.minimum_bounding_rectangle);
                        // increase counters
                        ++parent_node.child_count;
                        ++processed_tree_nodes_in_level;
                    }
                }
                tree_nodes_in_next_level.emplace_back(parent_node);
            }
            tree_nodes_in_level.swap(tree_nodes_in_next_level);
            ++processing_level;
        }
        BOOST_ASSERT_MSG(1 == tree_nodes_in_level.size(), "tree broken, more than one root node");
        // last remaining entry is the root node, store it
        m_search_tree.emplace_back(tree_nodes_in_level[0]);

        // reverse and renumber tree to have root at index 0
        std::reverse(m_search_tree.begin(), m_search_tree.end());

        uint32_t search_tree_size = m_search_tree.size();
        tbb::parallel_for(tbb::blocked_range<uint32_t>(0, search_tree_size),
                          [this, &search_tree_size](const tbb::blocked_range<uint32_t> &range)
                          {
            for (uint32_t i = range.begin(); i != range.end(); ++i)
            {
                TreeNode &current_tree_node = this->m_search_tree[i];
                for (uint32_t j = 0; j < current_tree_node.child_count; ++j)
                {
                    const uint32_t old_id = current_tree_node.children[j];
                    const uint32_t new_id = search_tree_size - old_id - 1;
                    current_tree_node.children[j] = new_id;
                }
            }
        });

        // open tree file
        boost::filesystem::ofstream tree_node_file(tree_node_filename, std::ios::binary);

        uint32_t size_of_tree = m_search_tree.size();
        BOOST_ASSERT_MSG(0 < size_of_tree, "tree empty");
        tree_node_file.write((char *)&size_of_tree, sizeof(uint32_t));
        tree_node_file.write((char *)&m_search_tree[0], sizeof(TreeNode) * size_of_tree);
        // close tree node file.
        tree_node_file.close();

        TIMER_STOP(construction);
        SimpleLogger().Write() << "finished r-tree construction in " << TIMER_SEC(construction)
                               << " seconds";
    }

    // Read-only operation for queries
    explicit StaticRTree(const boost::filesystem::path &node_file,
                         const boost::filesystem::path &leaf_file,
                         const std::shared_ptr<CoordinateListT> coordinate_list)
        : m_leaf_node_filename(leaf_file.string())
    {
        // open tree node file and load into RAM.
        m_coordinate_list = coordinate_list;

        if (!boost::filesystem::exists(node_file))
        {
            throw OSRMException("ram index file does not exist");
        }
        if (0 == boost::filesystem::file_size(node_file))
        {
            throw OSRMException("ram index file is empty");
        }
        boost::filesystem::ifstream tree_node_file(node_file, std::ios::binary);

        uint32_t tree_size = 0;
        tree_node_file.read((char *)&tree_size, sizeof(uint32_t));

        m_search_tree.resize(tree_size);
        if (tree_size > 0)
        {
            tree_node_file.read((char *)&m_search_tree[0], sizeof(TreeNode) * tree_size);
        }
        tree_node_file.close();
        // open leaf node file and store thread specific pointer
        if (!boost::filesystem::exists(leaf_file))
        {
            throw OSRMException("mem index file does not exist");
        }
        if (0 == boost::filesystem::file_size(leaf_file))
        {
            throw OSRMException("mem index file is empty");
        }

        leaves_stream.open(leaf_file, std::ios::binary);
        leaves_stream.read((char *)&m_element_count, sizeof(uint64_t));

        // SimpleLogger().Write() << tree_size << " nodes in search tree";
        // SimpleLogger().Write() << m_element_count << " elements in leafs";
    }

    explicit StaticRTree(TreeNode *tree_node_ptr,
                         const uint64_t number_of_nodes,
                         const boost::filesystem::path &leaf_file,
                         std::shared_ptr<CoordinateListT> coordinate_list)
        : m_search_tree(tree_node_ptr, number_of_nodes), m_leaf_node_filename(leaf_file.string()),
          m_coordinate_list(coordinate_list)
    {
        // open leaf node file and store thread specific pointer
        if (!boost::filesystem::exists(leaf_file))
        {
            throw OSRMException("mem index file does not exist");
        }
        if (0 == boost::filesystem::file_size(leaf_file))
        {
            throw OSRMException("mem index file is empty");
        }

        leaves_stream.open(leaf_file, std::ios::binary);
        leaves_stream.read((char *)&m_element_count, sizeof(uint64_t));

        // SimpleLogger().Write() << tree_size << " nodes in search tree";
        // SimpleLogger().Write() << m_element_count << " elements in leafs";
    }
    // Read-only operation for queries

    bool LocateClosestEndPointForCoordinate(const FixedPointCoordinate &input_coordinate,
                                            FixedPointCoordinate &result_coordinate,
                                            const unsigned zoom_level)
    {
        bool ignore_tiny_components = (zoom_level <= 14);

        float min_dist = std::numeric_limits<float>::max();
        float min_max_dist = std::numeric_limits<float>::max();

        // initialize queue with root element
        std::priority_queue<QueryCandidate> traversal_queue;
        traversal_queue.emplace(0.f, 0);

        while (!traversal_queue.empty())
        {
            const QueryCandidate current_query_node = traversal_queue.top();
            traversal_queue.pop();

            const bool prune_downward = (current_query_node.min_dist >= min_max_dist);
            const bool prune_upward = (current_query_node.min_dist >= min_dist);
            if (!prune_downward && !prune_upward)
            { // downward pruning
                TreeNode &current_tree_node = m_search_tree[current_query_node.node_id];
                if (current_tree_node.child_is_on_disk)
                {
                    LeafNode current_leaf_node;
                    LoadLeafFromDisk(current_tree_node.children[0], current_leaf_node);
                    for (uint32_t i = 0; i < current_leaf_node.object_count; ++i)
                    {
                        EdgeDataT const &current_edge = current_leaf_node.objects[i];
                        if (ignore_tiny_components && current_edge.is_in_tiny_cc)
                        {
                            continue;
                        }

                        float current_minimum_distance =
                            FixedPointCoordinate::ApproximateEuclideanDistance(
                                input_coordinate.lat,
                                input_coordinate.lon,
                                m_coordinate_list->at(current_edge.u).lat,
                                m_coordinate_list->at(current_edge.u).lon);
                        if (current_minimum_distance < min_dist)
                        {
                            // found a new minimum
                            min_dist = current_minimum_distance;
                            result_coordinate = m_coordinate_list->at(current_edge.u);
                        }

                        current_minimum_distance =
                            FixedPointCoordinate::ApproximateEuclideanDistance(
                                input_coordinate.lat,
                                input_coordinate.lon,
                                m_coordinate_list->at(current_edge.v).lat,
                                m_coordinate_list->at(current_edge.v).lon);

                        if (current_minimum_distance < min_dist)
                        {
                            // found a new minimum
                            min_dist = current_minimum_distance;
                            result_coordinate = m_coordinate_list->at(current_edge.v);
                        }
                    }
                }
                else
                {
                    min_max_dist = ExploreTreeNode(current_tree_node,
                                                   input_coordinate,
                                                   min_dist,
                                                   min_max_dist,
                                                   traversal_queue);
                }
            }
        }
        return result_coordinate.isValid();
    }

    // implementation of the Hjaltason/Samet query [3], a BFS traversal of the tree
    bool
    IncrementalFindPhantomNodeForCoordinate(const FixedPointCoordinate &input_coordinate,
                                            std::vector<PhantomNode> &result_phantom_node_vector,
                                            const unsigned zoom_level,
                                            const unsigned number_of_results,
                                            const unsigned max_checked_segments = 4*LEAF_NODE_SIZE)
    {
        // TIMER_START(samet);
        // SimpleLogger().Write(logDEBUG) << "searching for " << number_of_results << " results";
        std::vector<float> min_found_distances(number_of_results, std::numeric_limits<float>::max());

        unsigned dequeues = 0;
        unsigned inspected_mbrs = 0;
        unsigned loaded_leafs = 0;
        unsigned inspected_segments = 0;
        unsigned pruned_elements = 0;
        unsigned ignored_segments = 0;
        unsigned ignored_mbrs = 0;

        unsigned number_of_results_found_in_big_cc = 0;
        unsigned number_of_results_found_in_tiny_cc = 0;

        // initialize queue with root element
        std::priority_queue<IncrementalQueryCandidate> traversal_queue;
        traversal_queue.emplace(0.f, m_search_tree[0]);

        while (!traversal_queue.empty())
        {
            const IncrementalQueryCandidate current_query_node = traversal_queue.top();
            traversal_queue.pop();

            ++dequeues;

            const float current_min_dist = min_found_distances[number_of_results-1];

            if (current_query_node.min_dist > current_min_dist)
            {
                ++pruned_elements;
                continue;
            }

            if (current_query_node.node.template is<TreeNode>())
            {
                const TreeNode & current_tree_node = current_query_node.node.template get<TreeNode>();
                if (current_tree_node.child_is_on_disk)
                {
                    ++loaded_leafs;
                    // SimpleLogger().Write(logDEBUG) << "loading leaf: " << current_tree_node.children[0] << " w/ mbr [" <<
                    //     current_tree_node.minimum_bounding_rectangle.min_lat/COORDINATE_PRECISION << "," <<
                    //     current_tree_node.minimum_bounding_rectangle.min_lon/COORDINATE_PRECISION << "," <<
                    //     current_tree_node.minimum_bounding_rectangle.max_lat/COORDINATE_PRECISION << "-" <<
                    //     current_tree_node.minimum_bounding_rectangle.max_lon/COORDINATE_PRECISION << "]";

                    LeafNode current_leaf_node;
                    LoadLeafFromDisk(current_tree_node.children[0], current_leaf_node);
                    // Add all objects from leaf into queue
                    for (uint32_t i = 0; i < current_leaf_node.object_count; ++i)
                    {
                        const auto &current_edge = current_leaf_node.objects[i];
                        const float current_perpendicular_distance =
                            FixedPointCoordinate::ComputePerpendicularDistance(
                                m_coordinate_list->at(current_edge.u),
                                m_coordinate_list->at(current_edge.v),
                                input_coordinate);
                        // distance must be non-negative
                        BOOST_ASSERT(0. <= current_perpendicular_distance);

                        if (current_perpendicular_distance < current_min_dist)
                        {
                            traversal_queue.emplace(current_perpendicular_distance, current_edge);
                        }
                        else
                        {
                            ++ignored_segments;
                        }
                    }
                    // SimpleLogger().Write(logDEBUG) << "added " << current_leaf_node.object_count << " roads into queue of " << traversal_queue.size();
                }
                else
                {
                    ++inspected_mbrs;
                    // explore inner node
                    // SimpleLogger().Write(logDEBUG) << "explore inner node w/ mbr [" <<
                    //     current_tree_node.minimum_bounding_rectangle.min_lat/COORDINATE_PRECISION << "," <<
                    //     current_tree_node.minimum_bounding_rectangle.min_lon/COORDINATE_PRECISION << "," <<
                    //     current_tree_node.minimum_bounding_rectangle.max_lat/COORDINATE_PRECISION << "-" <<
                    //     current_tree_node.minimum_bounding_rectangle.max_lon/COORDINATE_PRECISION << "," << "]";

                    // for each child mbr
                    for (uint32_t i = 0; i < current_tree_node.child_count; ++i)
                    {
                        const int32_t child_id = current_tree_node.children[i];
                        const TreeNode &child_tree_node = m_search_tree[child_id];
                        const RectangleT &child_rectangle = child_tree_node.minimum_bounding_rectangle;
                        const float lower_bound_to_element = child_rectangle.GetMinDist(input_coordinate);

                        // TODO - enough elements found, i.e. nearest distance > maximum distance?
                        //        ie. some measure of 'confidence of accuracy'

                        // check if it needs to be explored by mindist
                        if (lower_bound_to_element < current_min_dist)
                        {
                            traversal_queue.emplace(lower_bound_to_element, child_tree_node);
                        }
                        else
                        {
                            ++ignored_mbrs;
                        }
                    }
                    // SimpleLogger().Write(logDEBUG) << "added " << current_tree_node.child_count << " mbrs into queue of " << traversal_queue.size();
                }
            }
            else
            {
                ++inspected_segments;
                // inspecting an actual road segment
                const EdgeDataT & current_segment = current_query_node.node.template get<EdgeDataT>();

                // don't collect too many results from small components
                if (number_of_results_found_in_big_cc == number_of_results && !current_segment.is_in_tiny_cc)
                {
                    continue;
                }

                // don't collect too many results from big components
                if (number_of_results_found_in_tiny_cc == number_of_results && current_segment.is_in_tiny_cc)
                {
                    continue;
                }

                // check if it is smaller than what we had before
                float current_ratio = 0.;
                FixedPointCoordinate foot_point_coordinate_on_segment;
                const float current_perpendicular_distance =
                    FixedPointCoordinate::ComputePerpendicularDistance(
                        m_coordinate_list->at(current_segment.u),
                        m_coordinate_list->at(current_segment.v),
                        input_coordinate,
                        foot_point_coordinate_on_segment,
                        current_ratio);

                BOOST_ASSERT(0. <= current_perpendicular_distance);

                if ((current_perpendicular_distance < current_min_dist) &&
                    !osrm::epsilon_compare(current_perpendicular_distance, current_min_dist))
                {
                    // store phantom node in result vector
                    result_phantom_node_vector.emplace_back(
                        current_segment.forward_edge_based_node_id,
                         current_segment.reverse_edge_based_node_id,
                         current_segment.name_id,
                         current_segment.forward_weight,
                         current_segment.reverse_weight,
                         current_segment.forward_offset,
                         current_segment.reverse_offset,
                         current_segment.packed_geometry_id,
                         foot_point_coordinate_on_segment,
                         current_segment.fwd_segment_position,
                         current_segment.forward_travel_mode,
                         current_segment.backward_travel_mode);

                    // Hack to fix rounding errors and wandering via nodes.
                    FixUpRoundingIssue(input_coordinate, result_phantom_node_vector.back());

                    // set forward and reverse weights on the phantom node
                    SetForwardAndReverseWeightsOnPhantomNode(current_segment,
                                                             result_phantom_node_vector.back());

                    // do we have results only in a small scc
                    if (current_segment.is_in_tiny_cc)
                    {
                        ++number_of_results_found_in_tiny_cc;
                    }
                    else
                    {
                        // found an element in a large component
                        min_found_distances[number_of_results_found_in_big_cc] = current_perpendicular_distance;
                        ++number_of_results_found_in_big_cc;
                        // SimpleLogger().Write(logDEBUG) << std::setprecision(8) << foot_point_coordinate_on_segment << " at " << current_perpendicular_distance;
                    }
                }
            }

            // TODO add indicator to prune if maxdist > threshold
            if (number_of_results == number_of_results_found_in_big_cc || inspected_segments >= max_checked_segments)
            {
                // SimpleLogger().Write(logDEBUG) << "flushing queue of " << traversal_queue.size() << " elements";
                // work-around for traversal_queue.clear();
                traversal_queue = std::priority_queue<IncrementalQueryCandidate>{};
            }
        }

        // for (const PhantomNode& result_node : result_phantom_node_vector)
        // {
        //     SimpleLogger().Write(logDEBUG) << std::setprecision(8) << "found location " << result_node.forward_node_id << " at " << result_node.location;
        // }
        // SimpleLogger().Write(logDEBUG) << "dequeues: " << dequeues;
        // SimpleLogger().Write(logDEBUG) << "inspected_mbrs: " << inspected_mbrs;
        // SimpleLogger().Write(logDEBUG) << "loaded_leafs: " << loaded_leafs;
        // SimpleLogger().Write(logDEBUG) << "inspected_segments: " << inspected_segments;
        // SimpleLogger().Write(logDEBUG) << "pruned_elements: " << pruned_elements;
        // SimpleLogger().Write(logDEBUG) << "ignored_segments: " << ignored_segments;
        // SimpleLogger().Write(logDEBUG) << "ignored_mbrs: " << ignored_mbrs;

        // SimpleLogger().Write(logDEBUG) << "number_of_results_found_in_big_cc: " << number_of_results_found_in_big_cc;
        // SimpleLogger().Write(logDEBUG) << "number_of_results_found_in_tiny_cc: " << number_of_results_found_in_tiny_cc;
        // TIMER_STOP(samet);
        // SimpleLogger().Write() << "query took " << TIMER_MSEC(samet) << "ms";

        // if we found an element in either category, then we are good
        return !result_phantom_node_vector.empty();
    }

    // implementation of the Hjaltason/Samet query [3], a BFS traversal of the tree
    bool
    IncrementalFindPhantomNodeForCoordinateWithDistance(const FixedPointCoordinate &input_coordinate,
                                                        std::vector<std::pair<PhantomNode, double>> &result_phantom_node_vector,
                                                        const unsigned zoom_level,
                                                        const unsigned number_of_results,
                                                        const unsigned max_checked_segments = 4*LEAF_NODE_SIZE)
    {
        std::vector<float> min_found_distances(number_of_results, std::numeric_limits<float>::max());

        unsigned number_of_results_found_in_big_cc = 0;
        unsigned number_of_results_found_in_tiny_cc = 0;

        unsigned inspected_segments = 0;

        // initialize queue with root element
        std::priority_queue<IncrementalQueryCandidate> traversal_queue;
        traversal_queue.emplace(0.f, m_search_tree[0]);

        while (!traversal_queue.empty())
        {
            const IncrementalQueryCandidate current_query_node = traversal_queue.top();
            traversal_queue.pop();

            const float current_min_dist = min_found_distances[number_of_results-1];

            if (current_query_node.min_dist > current_min_dist)
            {
                continue;
            }

            if (current_query_node.RepresentsTreeNode())
            {
                const TreeNode & current_tree_node = current_query_node.node.template get<TreeNode>();
                if (current_tree_node.child_is_on_disk)
                {
                    LeafNode current_leaf_node;
                    LoadLeafFromDisk(current_tree_node.children[0], current_leaf_node);
                    // Add all objects from leaf into queue
                    for (uint32_t i = 0; i < current_leaf_node.object_count; ++i)
                    {
                        const auto &current_edge = current_leaf_node.objects[i];
                        const float current_perpendicular_distance =
                            FixedPointCoordinate::ComputePerpendicularDistance(
                                m_coordinate_list->at(current_edge.u),
                                m_coordinate_list->at(current_edge.v),
                                input_coordinate);
                        // distance must be non-negative
                        BOOST_ASSERT(0. <= current_perpendicular_distance);

                        if (current_perpendicular_distance < current_min_dist)
                        {
                            traversal_queue.emplace(current_perpendicular_distance, current_edge);
                        }
                    }
                }
                else
                {
                    // for each child mbr
                    for (uint32_t i = 0; i < current_tree_node.child_count; ++i)
                    {
                        const int32_t child_id = current_tree_node.children[i];
                        const TreeNode &child_tree_node = m_search_tree[child_id];
                        const RectangleT &child_rectangle = child_tree_node.minimum_bounding_rectangle;
                        const float lower_bound_to_element = child_rectangle.GetMinDist(input_coordinate);

                        // TODO - enough elements found, i.e. nearest distance > maximum distance?
                        //        ie. some measure of 'confidence of accuracy'

                        // check if it needs to be explored by mindist
                        if (lower_bound_to_element < current_min_dist)
                        {
                            traversal_queue.emplace(lower_bound_to_element, child_tree_node);
                        }
                    }
                    // SimpleLogger().Write(logDEBUG) << "added " << current_tree_node.child_count << " mbrs into queue of " << traversal_queue.size();
                }
            }
            else
            {
                ++inspected_segments;
                // inspecting an actual road segment
                const EdgeDataT & current_segment = current_query_node.node.template get<EdgeDataT>();

                // don't collect too many results from small components
                if (number_of_results_found_in_big_cc == number_of_results && !current_segment.is_in_tiny_cc)
                {
                    continue;
                }

                // don't collect too many results from big components
                if (number_of_results_found_in_tiny_cc == number_of_results && current_segment.is_in_tiny_cc)
                {
                    continue;
                }

                // check if it is smaller than what we had before
                float current_ratio = 0.;
                FixedPointCoordinate foot_point_coordinate_on_segment;
                const float current_perpendicular_distance =
                    FixedPointCoordinate::ComputePerpendicularDistance(
                        m_coordinate_list->at(current_segment.u),
                        m_coordinate_list->at(current_segment.v),
                        input_coordinate,
                        foot_point_coordinate_on_segment,
                        current_ratio);

                BOOST_ASSERT(0. <= current_perpendicular_distance);

                if ((current_perpendicular_distance < current_min_dist) &&
                    !osrm::epsilon_compare(current_perpendicular_distance, current_min_dist))
                {
                    // store phantom node in result vector
                    result_phantom_node_vector.emplace_back(
                        current_segment.forward_edge_based_node_id,
                        current_segment.reverse_edge_based_node_id,
                        current_segment.name_id,
                        current_segment.forward_weight,
                        current_segment.reverse_weight,
                        current_segment.forward_offset,
                        current_segment.reverse_offset,
                        current_segment.packed_geometry_id,
                        foot_point_coordinate_on_segment,
                        current_segment.fwd_segment_position,
                        current_perpendicular_distance);

                    // Hack to fix rounding errors and wandering via nodes.
                    FixUpRoundingIssue(input_coordinate, result_phantom_node_vector.back());

                    // set forward and reverse weights on the phantom node
                    SetForwardAndReverseWeightsOnPhantomNode(current_segment,
                                                             result_phantom_node_vector.back());

                    // do we have results only in a small scc
                    if (current_segment.is_in_tiny_cc)
                    {
                        ++number_of_results_found_in_tiny_cc;
                    }
                    else
                    {
                        // found an element in a large component
                        min_found_distances[number_of_results_found_in_big_cc] = current_perpendicular_distance;
                        ++number_of_results_found_in_big_cc;
                        // SimpleLogger().Write(logDEBUG) << std::setprecision(8) << foot_point_coordinate_on_segment << " at " << current_perpendicular_distance;
                    }
                }
            }

            // TODO add indicator to prune if maxdist > threshold
            if (number_of_results == number_of_results_found_in_big_cc || inspected_segments >= max_checked_segments)
            {
                // SimpleLogger().Write(logDEBUG) << "flushing queue of " << traversal_queue.size() << " elements";
                // work-around for traversal_queue.clear();
                traversal_queue = std::priority_queue<IncrementalQueryCandidate>{};
            }
        }

        return !result_phantom_node_vector.empty();
    }



    bool FindPhantomNodeForCoordinate(const FixedPointCoordinate &input_coordinate,
                                      PhantomNode &result_phantom_node,
                                      const unsigned zoom_level)
    {
        const bool ignore_tiny_components = (zoom_level <= 14);
        EdgeDataT nearest_edge;

        float min_dist = std::numeric_limits<float>::max();
        float min_max_dist = std::numeric_limits<float>::max();

        std::priority_queue<QueryCandidate> traversal_queue;
        traversal_queue.emplace(0.f, 0);

        while (!traversal_queue.empty())
        {
            const QueryCandidate current_query_node = traversal_queue.top();
            traversal_queue.pop();

            const bool prune_downward = (current_query_node.min_dist > min_max_dist);
            const bool prune_upward = (current_query_node.min_dist > min_dist);
            if (!prune_downward && !prune_upward)
            { // downward pruning
                const TreeNode &current_tree_node = m_search_tree[current_query_node.node_id];
                if (current_tree_node.child_is_on_disk)
                {
                    LeafNode current_leaf_node;
                    LoadLeafFromDisk(current_tree_node.children[0], current_leaf_node);
                    for (uint32_t i = 0; i < current_leaf_node.object_count; ++i)
                    {
                        const EdgeDataT &current_edge = current_leaf_node.objects[i];
                        if (ignore_tiny_components && current_edge.is_in_tiny_cc)
                        {
                            continue;
                        }

                        float current_ratio = 0.;
                        FixedPointCoordinate nearest;
                        const float current_perpendicular_distance =
                            FixedPointCoordinate::ComputePerpendicularDistance(
                                m_coordinate_list->at(current_edge.u),
                                m_coordinate_list->at(current_edge.v),
                                input_coordinate,
                                nearest,
                                current_ratio);

                        BOOST_ASSERT(0. <= current_perpendicular_distance);

                        if ((current_perpendicular_distance < min_dist) &&
                            !osrm::epsilon_compare(current_perpendicular_distance, min_dist))
                        { // found a new minimum
                            min_dist = current_perpendicular_distance;
                            result_phantom_node = {current_edge.forward_edge_based_node_id,
                                                   current_edge.reverse_edge_based_node_id,
                                                   current_edge.name_id,
                                                   current_edge.forward_weight,
                                                   current_edge.reverse_weight,
                                                   current_edge.forward_offset,
                                                   current_edge.reverse_offset,
                                                   current_edge.packed_geometry_id,
                                                   nearest,
                                                   current_edge.fwd_segment_position,
                                                   current_edge.forward_travel_mode,
                                                   current_edge.backward_travel_mode};
                            nearest_edge = current_edge;
                        }
                    }
                }
                else
                {
                    min_max_dist = ExploreTreeNode(current_tree_node,
                                                   input_coordinate,
                                                   min_dist,
                                                   min_max_dist,
                                                   traversal_queue);
                }
            }
        }

        if (result_phantom_node.location.isValid())
        {
            // Hack to fix rounding errors and wandering via nodes.
            FixUpRoundingIssue(input_coordinate, result_phantom_node);

            // set forward and reverse weights on the phantom node
            SetForwardAndReverseWeightsOnPhantomNode(nearest_edge, result_phantom_node);
        }
        return result_phantom_node.location.isValid();
    }

  private:

    inline void SetForwardAndReverseWeightsOnPhantomNode(const EdgeDataT & nearest_edge,
                                                         PhantomNode &result_phantom_node) const
    {
        const float distance_1 = FixedPointCoordinate::ApproximateEuclideanDistance(
            m_coordinate_list->at(nearest_edge.u), result_phantom_node.location);
        const float distance_2 = FixedPointCoordinate::ApproximateEuclideanDistance(
            m_coordinate_list->at(nearest_edge.u), m_coordinate_list->at(nearest_edge.v));
        const float ratio = std::min(1.f, distance_1 / distance_2);

        if (SPECIAL_NODEID != result_phantom_node.forward_node_id)
        {
            result_phantom_node.forward_weight *= ratio;
        }
        if (SPECIAL_NODEID != result_phantom_node.reverse_node_id)
        {
            result_phantom_node.reverse_weight *= (1.f - ratio);
        }
    }

    // fixup locations if too close to inputs
    inline void FixUpRoundingIssue(const FixedPointCoordinate &input_coordinate,
                                  PhantomNode &result_phantom_node) const
    {
            if (1 == std::abs(input_coordinate.lon - result_phantom_node.location.lon))
            {
                result_phantom_node.location.lon = input_coordinate.lon;
            }
            if (1 == std::abs(input_coordinate.lat - result_phantom_node.location.lat))
            {
                result_phantom_node.location.lat = input_coordinate.lat;
            }
    }

    template <class QueueT>
    inline float ExploreTreeNode(const TreeNode &parent,
                                 const FixedPointCoordinate &input_coordinate,
                                 const float min_dist,
                                 const float min_max_dist,
                                 QueueT &traversal_queue)
    {
        float new_min_max_dist = min_max_dist;
        // traverse children, prune if global mindist is smaller than local one
        for (uint32_t i = 0; i < parent.child_count; ++i)
        {
            const int32_t child_id = parent.children[i];
            const TreeNode &child_tree_node = m_search_tree[child_id];
            const RectangleT &child_rectangle = child_tree_node.minimum_bounding_rectangle;
            const float lower_bound_to_element = child_rectangle.GetMinDist(input_coordinate);
            const float upper_bound_to_element = child_rectangle.GetMinMaxDist(input_coordinate);
            new_min_max_dist = std::min(new_min_max_dist, upper_bound_to_element);
            if (lower_bound_to_element > new_min_max_dist)
            {
                continue;
            }
            if (lower_bound_to_element > min_dist)
            {
                continue;
            }
            traversal_queue.emplace(lower_bound_to_element, child_id);
        }
        return new_min_max_dist;
    }

    inline void LoadLeafFromDisk(const uint32_t leaf_id, LeafNode &result_node)
    {
        if (!leaves_stream.is_open())
        {
            leaves_stream.open(m_leaf_node_filename, std::ios::in | std::ios::binary);
        }
        if (!leaves_stream.good())
        {
            leaves_stream.clear(std::ios::goodbit);
            SimpleLogger().Write(logDEBUG) << "Resetting stale filestream";
        }
        const uint64_t seek_pos = sizeof(uint64_t) + leaf_id * sizeof(LeafNode);
        leaves_stream.seekg(seek_pos);
        BOOST_ASSERT_MSG(leaves_stream.good(),
                         "Seeking to position in leaf file failed.");
        leaves_stream.read((char *)&result_node, sizeof(LeafNode));
        BOOST_ASSERT_MSG(leaves_stream.good(), "Reading from leaf file failed.");
    }

    inline bool EdgesAreEquivalent(const FixedPointCoordinate &a,
                                   const FixedPointCoordinate &b,
                                   const FixedPointCoordinate &c,
                                   const FixedPointCoordinate &d) const
    {
        return (a == b && c == d) || (a == c && b == d) || (a == d && b == c);
    }
};

//[1] "On Packing R-Trees"; I. Kamel, C. Faloutsos; 1993; DOI: 10.1145/170088.170403
//[2] "Nearest Neighbor Queries", N. Roussopulos et al; 1995; DOI: 10.1145/223784.223794
//[3] "Distance Browsing in Spatial Databases"; G. Hjaltason, H. Samet; 1999; ACM Trans. DB Sys
// Vol.24 No.2, pp.265-318
#endif // STATICRTREE_H
