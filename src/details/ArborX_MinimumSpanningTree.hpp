/****************************************************************************
 * Copyright (c) 2017-2022 by the ArborX authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef ARBORX_MINIMUM_SPANNING_TREE_HPP
#define ARBORX_MINIMUM_SPANNING_TREE_HPP

#include <ArborX_AccessTraits.hpp>
#include <ArborX_DetailsKokkosExtArithmeticTraits.hpp>
#include <ArborX_DetailsKokkosExtMinMaxOperations.hpp>
#include <ArborX_DetailsMutualReachabilityDistance.hpp>
#include <ArborX_DetailsTreeNodeLabeling.hpp>
#include <ArborX_DetailsUtils.hpp>
#include <ArborX_LinearBVH.hpp>

#include <Kokkos_Core.hpp>

#if KOKKOS_VERSION >= 30500

namespace ArborX
{
namespace Details
{

struct WeightedEdge
{
  int source;
  int target;
  float weight;

private:
  // performs lexicographical comparison by comparing first the weights and then
  // the unordered pair of vertices
  friend KOKKOS_FUNCTION constexpr bool operator<(WeightedEdge const &lhs,
                                                  WeightedEdge const &rhs)
  {
    if (lhs.weight != rhs.weight)
    {
      return (lhs.weight < rhs.weight);
    }
    using KokkosExt::min;
    auto const lhs_min = min(lhs.source, lhs.target);
    auto const rhs_min = min(rhs.source, rhs.target);
    if (lhs_min != rhs_min)
    {
      return (lhs_min < rhs_min);
    }
    using KokkosExt::max;
    auto const lhs_max = max(lhs.source, lhs.target);
    auto const rhs_max = max(rhs.source, rhs.target);
    return (lhs_max < rhs_max);
  }
};

template <class BVH, class Labels, class Edges, class Metric, class Radii>
struct FindComponentNearestNeighbors
{
  BVH _bvh;
  Labels _labels;
  Edges _edges;
  Metric _metric;
  Radii _radii;

  template <class ExecutionSpace>
  FindComponentNearestNeighbors(ExecutionSpace const &space, BVH const &bvh,
                                Labels const &labels, Edges const &edges,
                                Metric const &metric, Radii const &radii)
      : _bvh(bvh)
      , _labels(labels)
      , _edges(edges)
      , _metric{metric}
      , _radii(radii)
  {
    auto const n = bvh.size();
    ARBORX_ASSERT(labels.extent(0) == 2 * n - 1);
    ARBORX_ASSERT(edges.extent(0) == n);
    ARBORX_ASSERT(radii.extent(0) == n);

    Kokkos::parallel_for(
        "ArborX::MST::find_component_nearest_neighbors",
        Kokkos::RangePolicy<ExecutionSpace>(space, n - 1, 2 * n - 1), *this);
  }

  KOKKOS_FUNCTION void operator()(int i) const
  {
    constexpr int undetermined = -1;
    constexpr auto inf = KokkosExt::ArithmeticTraits::infinity<float>::value;

    auto const distance = [bounding_volume_i =
                               HappyTreeFriends::getBoundingVolume(_bvh, i),
                           &bvh = _bvh](int j) {
      using Details::distance;
      return distance(bounding_volume_i,
                      HappyTreeFriends::getBoundingVolume(bvh, j));
    };

    auto const component = _labels(i);
    auto const predicate = [label_i = component, &labels = _labels](int j) {
      return label_i != labels(j);
    };
    auto const leaf_permutation_i =
        HappyTreeFriends::getLeafPermutationIndex(_bvh, i);

    WeightedEdge current_best{i, undetermined, inf};
    static_assert(WeightedEdge{undetermined, undetermined, inf} <
                      WeightedEdge{0, undetermined, inf},
                  "");

    auto const n = _bvh.size();
    auto &radius = _radii(component - n + 1);

    constexpr int SENTINEL = -1;
    int stack[64];
    auto *stack_ptr = stack;
    *stack_ptr++ = SENTINEL;
#if !defined(__CUDA_ARCH__) && !defined(__HIP_DEVICE_COMPILE__)
    float stack_distance[64];
    auto *stack_distance_ptr = stack_distance;
    *stack_distance_ptr++ = 0;
#endif

    int node = HappyTreeFriends::getRoot(_bvh);
    float distance_node = 0;

    // Important! The truncation radius is computed using the provided metric,
    // rather than just assigning the Euclidean distance. This only works for
    // metrics that return a value greater or equal to Euclidean distance
    // (e.g., mutual reachability metric). Metrics that do not satisfy this
    // criteria may return wrong results.
    do
    {
      bool traverse_left = false;
      bool traverse_right = false;

      int left_child;
      int right_child;
      float distance_left = inf;
      float distance_right = inf;

      // Note it is <= instead of < when comparing with radius here and below.
      // The reason is that in Boruvka it matters which of the equidistant
      // points we take so that they don't create a cycle among component
      // connectivity. This requires us to uniquely resolve equidistant
      // neighbors, so we cannot skip any of them.
      if (distance_node <= radius)
      {
        // Insert children into the stack and make sure that the closest one
        // ends on top.
        left_child = HappyTreeFriends::getLeftChild(_bvh, node);
        right_child = HappyTreeFriends::getRightChild(_bvh, node);
        distance_left = distance(left_child);
        distance_right = distance(right_child);

        if (predicate(left_child) && distance_left <= radius)
        {
          if (HappyTreeFriends::isLeaf(_bvh, left_child))
          {
            float const candidate_dist = _metric(
                leaf_permutation_i,
                HappyTreeFriends::getLeafPermutationIndex(_bvh, left_child),
                distance_left);
            WeightedEdge const candidate_edge{i, left_child, candidate_dist};
            if (candidate_edge < current_best)
            {
              current_best = candidate_edge;
              Kokkos::atomic_min(&radius, candidate_dist);
            }
          }
          else
          {
            traverse_left = true;
          }
        }

        // Note: radius may have been already updated here from the left child
        if (predicate(right_child) && distance_right <= radius)
        {
          if (HappyTreeFriends::isLeaf(_bvh, right_child))
          {
            float const candidate_dist = _metric(
                leaf_permutation_i,
                HappyTreeFriends::getLeafPermutationIndex(_bvh, right_child),
                distance_right);
            WeightedEdge const candidate_edge{i, right_child, candidate_dist};
            if (candidate_edge < current_best)
            {
              current_best = candidate_edge;
              Kokkos::atomic_min(&radius, candidate_dist);
            }
          }
          else
          {
            traverse_right = true;
          }
        }
      }

      if (!traverse_left && !traverse_right)
      {
        node = *--stack_ptr;
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
        if (node != SENTINEL)
        {
          // This is a theoretically unnecessary duplication of distance
          // calculation for stack nodes. However, for Cuda it's better than
          // putting the distances in stack.
          distance_node = distance(node);
        }
#else
        distance_node = *--stack_distance_ptr;
#endif
      }
      else
      {
        node = (traverse_left &&
                (distance_left <= distance_right || !traverse_right))
                   ? left_child
                   : right_child;
        distance_node = (node == left_child ? distance_left : distance_right);
        if (traverse_left && traverse_right)
        {
          *stack_ptr++ = (node == left_child ? right_child : left_child);
#if !defined(__CUDA_ARCH__) && !defined(__HIP_DEVICE_COMPILE__)
          *stack_distance_ptr++ =
              (node == left_child ? distance_right : distance_left);
#endif
        }
      }
    } while (node != SENTINEL);

    // This check is only here to reduce hammering the atomics for large
    // components. Otherwise, for a large number of points and a small number of
    // components it becomes extremely expensive.
    auto &component_edge = _edges(component - n + 1);
    if (current_best.weight < inf &&
        current_best.weight <= component_edge.weight)
    {
      Kokkos::atomic_min(&component_edge, current_best);
    }
  }
};

// For every component C, find the shortest edge (v, w) such that v is in C
// and w is not in C. The found edge is stored in component_out_edges(C).
template <class ExecutionSpace, class BVH, class Labels, class Edges,
          class Metric, class Radii>
void findComponentNearestNeighbors(ExecutionSpace const &space, BVH const &bvh,
                                   Labels const &labels, Edges const &edges,
                                   Metric const &metric, Radii const &radii)
{
  FindComponentNearestNeighbors<BVH, Labels, Edges, Metric, Radii>(
      space, bvh, labels, edges, metric, radii);
}

template <class Labels, class OutEdges, class Edges, class EdgesCount>
struct UpdateComponentsAndEdges
{
  Labels _labels;
  OutEdges _out_edges;
  Edges _edges;
  EdgesCount _num_edges;

  template <class ExecutionSpace>
  UpdateComponentsAndEdges(ExecutionSpace const &space, Labels const &labels,
                           OutEdges const &out_edges, Edges const &edges,
                           EdgesCount const &count)
      : _labels(labels)
      , _out_edges(out_edges)
      , _edges(edges)
      , _num_edges(count)
  {
    auto const n = out_edges.extent(0);
    ARBORX_ASSERT(labels.extent(0) == 2 * n - 1);
    ARBORX_ASSERT(edges.extent(0) == n - 1);

    Kokkos::parallel_for(
        "ArborX::MST::update_components_and_edges",
        Kokkos::RangePolicy<ExecutionSpace>(space, n - 1, 2 * n - 1), *this);
  }

  KOKKOS_FUNCTION auto computeNextComponent(int component) const
  {
    auto const n = _out_edges.extent(0);

    int next_component = _labels(_out_edges(component - n + 1).target);
    int next_next_component =
        _labels(_out_edges(next_component - n + 1).target);

    if (next_next_component != component)
    {
      // The component's edge is unidirectional
      return next_component;
    }
    // The component's edge is bidirectional, uniquely resolve the bidirectional
    // edge
    return KokkosExt::min(component, next_component);
  }

  KOKKOS_FUNCTION auto computeFinalComponent(int component) const
  {
    int prev_component = component;
    int next_component;
    while ((next_component = computeNextComponent(prev_component)) !=
           prev_component)
      prev_component = next_component;

    return next_component;
  }

  KOKKOS_FUNCTION void operator()(int i) const
  {
    auto const component = _labels(i);
    auto const final_component = computeFinalComponent(component);
    _labels(i) = final_component;
    if (i != component)
    {
      return;
    }
    auto const n = _out_edges.extent(0);
    if (i != final_component)
    {
      auto const edge = _out_edges(i - n + 1);
      // append new edge at the "end" of the array (akin to
      // std::vector::push_back)
      auto const back =
          Kokkos::atomic_fetch_inc(&_num_edges()); // atomic post-increment
      _edges(back) = edge;
    }
  }
};

// For every component C and a found shortest edge `(u, w)`, merge C with
// the component that w belongs to by updating the labels, and add the edge to
// the list of MST edges.
template <class ExecutionSpace, class Labels, class ComponentOutEdges,
          class Edges, class EdgesCount>
void updateComponentsAndEdges(ExecutionSpace const &space,
                              ComponentOutEdges const &component_out_edges,
                              Labels const &labels, Edges const &edges,
                              EdgesCount const &num_edges)
{
  UpdateComponentsAndEdges<Labels, ComponentOutEdges, Edges, EdgesCount>(
      space, labels, component_out_edges, edges, num_edges);
}

// Reverse node leaf permutation order back to original indices
template <class ExecutionSpace, class BVH, class Edges>
void finalizeEdges(ExecutionSpace const &space, BVH const &bvh,
                   Edges const &edges)
{
  auto const n = bvh.size();
  ARBORX_ASSERT(edges.extent(0) == n - 1);
  Kokkos::parallel_for(
      "ArborX::MST::finalize_edges",
      Kokkos::RangePolicy<ExecutionSpace>(space, 0, n - 1),
      KOKKOS_LAMBDA(int i) {
        edges(i).source =
            HappyTreeFriends::getLeafPermutationIndex(bvh, edges(i).source);
        edges(i).target =
            HappyTreeFriends::getLeafPermutationIndex(bvh, edges(i).target);
      });
}

// Compute upper bound on the shortest edge of each component.
template <class ExecutionSpace, class BVH, class Labels, class Metric,
          class Radii>
void resetSharedRadii(ExecutionSpace const &space, BVH const &bvh,
                      Labels const &labels, Metric const &metric,
                      Radii const &radii)
{
  //  We will search for the shortest outgoing edge of a component. The better
  //  we initialize the upper bound on the distance (i.e., the smaller it is),
  //  the less traversal we will do and the faster it will be.
  //
  // Here, we use the knowledge that it is a self-collision problem. In other
  // words, we only have a single point cloud. We further use the fact that if
  // we sort predicates based on the Morton codes, it will match the order of
  // primitives (or be close enough, as some points with the same Morton codes
  // may be in a different order due to the unstable sort that we use). Thus, if
  // we take an index of a query, we assume that it matches the corresponding
  // primitive. If a label of that primitive is different from a label of its
  // neighbor (which is in fact its Morton neighbor), we compute the distance
  // between the two. The upper bound for a component is set to the minimum
  // distance between all such pairs. Given that the Morton neighbors are
  // typically close to each other, this should provide a reasonably low bound.
  auto const n = bvh.size();
  Kokkos::parallel_for(
      "ArborX::MST::reset_shared_radii",
      Kokkos::RangePolicy<ExecutionSpace>(space, n - 1, 2 * n - 2),
      KOKKOS_LAMBDA(int i) {
        int const j = i + 1;
        auto const label_i = labels(i);
        auto const label_j = labels(j);
        if (label_i != label_j)
        {
          auto const r =
              metric(HappyTreeFriends::getLeafPermutationIndex(bvh, i),
                     HappyTreeFriends::getLeafPermutationIndex(bvh, j),
                     distance(HappyTreeFriends::getBoundingVolume(bvh, i),
                              HappyTreeFriends::getBoundingVolume(bvh, j)));
          Kokkos::atomic_min(&radii(label_i - n + 1), r);
          Kokkos::atomic_min(&radii(label_j - n + 1), r);
        }
      });
}

template <class MemorySpace>
struct MinimumSpanningTree
{
  Kokkos::View<WeightedEdge *, MemorySpace> edges;

  template <class ExecutionSpace, class Primitives>
  MinimumSpanningTree(ExecutionSpace const &space, Primitives const &primitives,
                      int k = 1)
      : edges(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                 "ArborX::MST::edges"),
              AccessTraits<Primitives, PrimitivesTag>::size(primitives) - 1)
  {
    Kokkos::Profiling::pushRegion("ArborX::MST::MST");

    BVH<MemorySpace> bvh(space, primitives);
    auto const n = bvh.size();

    if (k > 1)
    {
      Kokkos::Profiling::pushRegion("ArborX::MST::compute_core_distances");
      Kokkos::View<float *, MemorySpace> core_distances(
          Kokkos::view_alloc(Kokkos::WithoutInitializing,
                             "ArborX::MST::core_distances"),
          n);
      bvh.query(space, NearestK<Primitives>{primitives, k},
                MaxDistance<Primitives, decltype(core_distances)>{
                    primitives, core_distances});
      Kokkos::Profiling::popRegion();

      MutualReachability<decltype(core_distances)> mutual_reachability{
          core_distances};
      doBoruvka(space, bvh, mutual_reachability);
    }
    else
    {
      doBoruvka(space, bvh, Euclidean{});
    }

    finalizeEdges(space, bvh, edges);

    Kokkos::Profiling::popRegion();
  }

private:
  template <class ExecutionSpace, class BVH, class Metric>
  void doBoruvka(ExecutionSpace const &space, BVH const &bvh,
                 Metric const &metric)
  {
    auto const n = bvh.size();
    Kokkos::View<int *, MemorySpace> parents(
        Kokkos::view_alloc(Kokkos::WithoutInitializing, "ArborX::MST::parents"),
        2 * n - 1);
    findParents(space, bvh, parents);

    Kokkos::Profiling::pushRegion("ArborX::MST::initialize_node_labels");
    Kokkos::View<int *, MemorySpace> labels(
        Kokkos::view_alloc(Kokkos::WithoutInitializing, "ArborX::MST::labels"),
        2 * n - 1);
    iota(space, Kokkos::subview(labels, std::make_pair(n - 1, 2 * n - 1)),
         n - 1);
    Kokkos::Profiling::popRegion();

    Kokkos::View<WeightedEdge *, MemorySpace> component_out_edges(
        Kokkos::view_alloc(Kokkos::WithoutInitializing,
                           "ArborX::MST::component_out_edges"),
        n);

    Kokkos::View<float *, MemorySpace> radii(
        Kokkos::view_alloc(Kokkos::WithoutInitializing, "ArborX::MST::radii"),
        n);

    Kokkos::Profiling::pushRegion("ArborX::MST::Boruvka_loop");
    Kokkos::View<int, MemorySpace> num_edges(Kokkos::view_alloc(
        Kokkos::WithoutInitializing, "ArborX::MST::num_edges"));
    Kokkos::deep_copy(space, num_edges, 0);

    // Boruvka iterations
    int iterations = 0;
    int num_components = n;
    do
    {
      Kokkos::Profiling::pushRegion("ArborX::Boruvka_" +
                                    std::to_string(++iterations) + "_" +
                                    std::to_string(num_components));
      // Propagate leaf node labels to internal nodes
      reduceLabels(space, parents, labels);
      constexpr auto inf = KokkosExt::ArithmeticTraits::infinity<float>::value;
      constexpr WeightedEdge uninitialized_edge{-1, -1, inf};
      Kokkos::deep_copy(space, component_out_edges, uninitialized_edge);
      Kokkos::deep_copy(space, radii, inf);
      resetSharedRadii(space, bvh, labels, metric, radii);
      findComponentNearestNeighbors(space, bvh, labels, component_out_edges,
                                    metric, radii);
      // NOTE could perform the label tree reduction as part of the update
      updateComponentsAndEdges(space, component_out_edges, labels, edges,
                               num_edges);
      num_components =
          static_cast<int>(n) -
          Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, num_edges)();
      Kokkos::Profiling::popRegion();
    } while (num_components > 1);
    Kokkos::Profiling::popRegion();
  }
};

} // namespace Details
} // namespace ArborX

#endif
#endif