/****************************************************************************
 * Copyright (c) 2012-2020 by the ArborX authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef ARBORX_DBSCAN_HPP
#define ARBORX_DBSCAN_HPP

#include <ArborX_DetailsDBSCANCallback.hpp>
#include <ArborX_DetailsSortUtils.hpp>
#include <ArborX_DetailsUtils.hpp>
#include <ArborX_LinearBVH.hpp>

#include <chrono>
#include <set>
#include <stack>

namespace ArborX
{

template <typename View>
struct Wrapped
{
  View _M_view;
  double _r;
};

template <typename View>
auto wrap(View v, double r)
{
  return Wrapped<View>{v, r};
}

namespace Traits
{
template <typename View>
struct Access<Wrapped<View>, PredicatesTag>
{
  using memory_space = typename View::memory_space;
  static size_t size(Wrapped<View> const &w) { return w._M_view.extent(0); }
  static KOKKOS_FUNCTION auto get(Wrapped<View> const &w, size_t i)
  {
    return attach(intersects(Sphere{w._M_view(i), w._r}), (int)i);
  }
};
} // namespace Traits

namespace DBSCAN
{

template <typename ExecutionSpace, typename IndicesView, typename OffsetView,
          typename CCSView>
bool verifyCC(ExecutionSpace exec_space, IndicesView indices, OffsetView offset,
              CCSView ccs)
{
  int num_nodes = ccs.size();
  ARBORX_ASSERT((int)offset.size() == num_nodes + 1);
  ARBORX_ASSERT(ArborX::lastElement(offset) == (int)indices.size());

  // Check that connected vertices have the same cc index
  int num_incorrect = 0;
  Kokkos::parallel_reduce(
      "ArborX::DBSCAN::verify_connected_indices",
      Kokkos::RangePolicy<ExecutionSpace>(exec_space, 0, num_nodes),
      KOKKOS_LAMBDA(int i, int &update) {
        for (int j = offset(i); j < offset(i + 1); ++j)
        {
          if (ccs(i) != ccs(indices(j)))
          {
            // Would like to do fprintf(stderr, ...), but fprintf is __host__
            // function in CUDA
            printf("Non-matching cc indices: %d [%d] -> %d [%d]\n", i, ccs(i),
                   indices(j), ccs(indices(j)));
            update++;
          }
        }
      },
      num_incorrect);
  if (num_incorrect)
    return false;

  // Check that non-connected vertices have different cc indices
  auto ccs_host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, ccs);
  auto offset_host =
      Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, offset);
  auto indices_host =
      Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, indices);

  std::set<int> unique_cc_indices;
  for (int i = 0; i < num_nodes; i++)
    unique_cc_indices.insert(ccs_host(i));
  auto num_unique_cc_indices = unique_cc_indices.size();

  unsigned int num_ccs = 0;
  std::set<int> cc_sets;
  for (int i = 0; i < num_nodes; i++)
  {
    if (ccs_host(i) >= 0)
    {
      auto id = ccs_host(i);
      cc_sets.insert(id);
      num_ccs++;

      // DFS search
      std::stack<int> stack;
      stack.push(i);
      while (!stack.empty())
      {
        auto k = stack.top();
        stack.pop();
        if (ccs_host(k) >= 0)
        {
          ARBORX_ASSERT(ccs_host(k) == id);
          ccs_host(k) = -1;
          for (int j = offset_host(k); j < offset_host(k + 1); j++)
            stack.push(indices_host(j));
        }
      }
    }
  }
  if (cc_sets.size() != num_unique_cc_indices)
  {
    // FIXME: Not sure how we can get here, but it was in the original verify
    // check in ECL
    std::cerr << "Number of components does not match" << std::endl;
    return false;
  }
  if (num_ccs != num_unique_cc_indices)
  {
    std::cerr << "Component IDs are not unique" << std::endl;
    return false;
  }

  return true;
}

template <typename MemorySpace>
struct NumNeighCallback
{
  Kokkos::View<int *, MemorySpace> num_neigh_;

  template <typename Query, typename Insert>
  KOKKOS_FUNCTION void operator()(Query const &query, int j,
                                  Insert const &) const
  {
    auto i = getData(query);
    Kokkos::atomic_fetch_add(&num_neigh_(i), 1);
  }
};

template <typename ExecutionSpace, typename Primitives,
          typename ClusterIndicesView, typename ClusterOffsetView>
void dbscan(ExecutionSpace exec_space, Primitives const &primitives,
            ClusterIndicesView &cluster_indices,
            ClusterOffsetView &cluster_offsets, float eps,
            int core_min_size = 1, int cluster_min_size = 2,
            bool verbose = false, bool verify = false)
{
  static_assert(Kokkos::is_view<ClusterIndicesView>{}, "");
  static_assert(Kokkos::is_view<ClusterOffsetView>{}, "");
  static_assert(std::is_same<typename ClusterIndicesView::value_type, int>{},
                "");
  static_assert(std::is_same<typename ClusterOffsetView::value_type, int>{},
                "");

  using MemorySpace = typename Primitives::memory_space;
  static_assert(
      std::is_same<typename ClusterIndicesView::memory_space, MemorySpace>{},
      "");
  static_assert(
      std::is_same<typename ClusterOffsetView::memory_space, MemorySpace>{},
      "");

  Kokkos::Profiling::pushRegion("ArborX::DBSCAN");

  using clock = std::chrono::high_resolution_clock;

  clock::time_point start_total;
  clock::time_point start;
  std::chrono::duration<double> elapsed_construction;
  std::chrono::duration<double> elapsed_query;
  std::chrono::duration<double> elapsed_cluster;
  std::chrono::duration<double> elapsed_total;
  std::chrono::duration<double> elapsed_verify = clock::duration::zero();

  start_total = clock::now();

  auto const predicates = wrap(primitives, eps);

  int const n = primitives.extent_int(0);

  // TODO: remove these once type 1 interface is available
  // NOTE: indices and offfset are not going to be used as
  // insert() will not be called
  Kokkos::View<int *, MemorySpace> indices("Testing::indices", 0);
  Kokkos::View<int *, MemorySpace> offset("Testing::offset", 0);

  // build the tree
  start = clock::now();
  Kokkos::Profiling::pushRegion("ArborX::DBSCAN::tree_construction");
  ArborX::BVH<MemorySpace> bvh(exec_space, primitives);
  Kokkos::Profiling::popRegion();
  elapsed_construction = clock::now() - start;

  start = clock::now();
  Kokkos::Profiling::pushRegion("ArborX::DBSCAN::clusters");

  Kokkos::View<int *, MemorySpace> stat(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "ArborX::DBSCAN::stat"),
      n);
  ArborX::iota(exec_space, stat);
  Kokkos::View<int *, MemorySpace> num_neigh(
      Kokkos::ViewAllocateWithoutInitializing("ArborX::DBSCAN::num_neighbors"),
      n);
  if (core_min_size == 1)
  {
    // perform the queries and build clusters through callback
    Kokkos::Profiling::pushRegion("ArborX::DBSCAN::clusters::query");
    bvh.query(
        exec_space, predicates,
        Details::DBSCANCallback<MemorySpace, Details::CCSTag>{stat, num_neigh},
        indices, offset);
    Kokkos::Profiling::popRegion();
  }
  else
  {
    // Compute number of neighbors
    Kokkos::Profiling::pushRegion("ArborX::DBSCAN::clusters::num_neigh");
    Kokkos::deep_copy(num_neigh, 0);
    // FIXME: wrap predicates to attach their index
    bvh.query(exec_space, predicates, NumNeighCallback<MemorySpace>{num_neigh},
              indices, offset);
    Kokkos::Profiling::popRegion();

    Kokkos::Profiling::pushRegion("ArborX::DBSCAN::clusters::query");
    bvh.query(exec_space, predicates,
              Details::DBSCANCallback<MemorySpace, Details::DBSCANTag>{
                  stat, num_neigh},
              indices, offset);
    Kokkos::Profiling::popRegion();
  }

  // Per [1]:
  //
  // ```
  // The finalization kernel will, ultimately, make all parents
  // point directly to the representative.
  // ```
  Kokkos::parallel_for("ArborX::DBSCAN::flatten_stat",
                       Kokkos::RangePolicy<ExecutionSpace>(exec_space, 0, n),
                       KOKKOS_LAMBDA(int const i) {
                         // ##### ECL license (see LICENSE.ECL) #####
                         int next;
                         int vstat = stat(i);
                         int const old = vstat;
                         while (vstat > (next = stat(vstat)))
                         {
                           vstat = next;
                         }
                         if (vstat != old)
                           stat(i) = vstat;
                       });
  Kokkos::Profiling::popRegion();
  elapsed_query = clock::now() - start;

  // Use new name to clearly demonstrate the meaning of this view from now on
  auto clusters = stat;

  elapsed_total += clock::now() - start_total;
  if (verify)
  {
    // FIXME: needs fixing for full DBSCAN
    start = clock::now();
    Kokkos::Profiling::pushRegion("ArborX::DBSCAN::verify");

    bvh.query(exec_space, predicates, indices, offset);
    auto passed = verifyCC(exec_space, indices, offset, clusters);
    printf("Verification %s\n", (passed ? "passed" : "failed"));

    Kokkos::Profiling::popRegion();
    elapsed_verify = clock::now() - start;
  }
  start_total = clock::now();

  // find clusters
  start = clock::now();
  Kokkos::Profiling::pushRegion("ArborX::DBSCAN::sort_and_filter_clusters");

  // sort clusters and compute permutation
  auto permute = Details::sortObjects(exec_space, clusters);

  reallocWithoutInitializing(cluster_offsets, n + 1);
  Kokkos::View<int *, MemorySpace> cluster_starts(
      Kokkos::ViewAllocateWithoutInitializing("ArborX::DBSCAN::cluster_starts"),
      n);
  int num_clusters = 0;
  // In the following scan, we locate the starting position (stored in
  // cluster_starts) and size (stored in cluster_offsets) of each valid halo
  // (i.e., connected component of size >= cluster_min_size). For every index i,
  // we check whether its CC index is different from the previous one (this
  // indicates a start of connected component) and whether the CC index of i +
  // cluster_min_size is the same (this indicates that this CC is at least of
  // cluster_min_size size). If those are true, we do a linear search from i +
  // cluster_min_size till next CC index change to find the CC size.
  Kokkos::parallel_scan(
      "ArborX::DBSCAN::compute_cluster_starts_and_sizes",
      Kokkos::RangePolicy<ExecutionSpace>(exec_space, 0, n),
      KOKKOS_LAMBDA(int i, int &update, bool final_pass) {
        bool const is_cluster_first_index =
            (i == 0 || clusters(i) != clusters(i - 1));
        bool const is_cluster_large_enough =
            ((i + cluster_min_size - 1 < n) &&
             (clusters(i + cluster_min_size - 1) == clusters(i)));
        if (is_cluster_first_index && is_cluster_large_enough)
        {
          if (final_pass)
          {
            cluster_starts(update) = i;
            int end = i + cluster_min_size - 1;
            while (++end < n && clusters(end) == clusters(i))
              ; // do nothing
            cluster_offsets(update) = end - i;
          }
          ++update;
        }
      },
      num_clusters);
  Kokkos::resize(cluster_offsets, num_clusters + 1);
  exclusivePrefixSum(exec_space, cluster_offsets);

  // Construct cluster indices
  reallocWithoutInitializing(cluster_indices, lastElement(cluster_offsets));
  Kokkos::parallel_for(
      "ArborX::DBSCAN::populate_clusters",
      Kokkos::RangePolicy<ExecutionSpace>(exec_space, 0, num_clusters),
      KOKKOS_LAMBDA(int i) {
        for (int k = cluster_offsets(i); k < cluster_offsets(i + 1); ++k)
        {
          cluster_indices(k) =
              permute(cluster_starts(i) + (k - cluster_offsets(i)));
        }
      });
  Kokkos::Profiling::popRegion();
  elapsed_cluster = clock::now() - start;

  elapsed_total += clock::now() - start_total;

  if (verbose)
  {
    printf("total time          : %10.3f\n", elapsed_total.count());
    printf("-> construction     : %10.3f\n", elapsed_construction.count());
    printf("-> query+cluster    : %10.3f\n", elapsed_query.count());
    printf("-> postprocess      : %10.3f\n", elapsed_cluster.count());
    if (verify)
      printf("verify              : %10.3f\n", elapsed_verify.count());
  }

  Kokkos::Profiling::popRegion();
}

} // namespace DBSCAN
} // namespace ArborX

#endif
