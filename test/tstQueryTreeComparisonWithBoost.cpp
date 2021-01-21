/****************************************************************************
 * Copyright (c) 2012-2021 by the ArborX authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#include "ArborX_BoostRTreeHelpers.hpp"
#include <ArborX_LinearBVH.hpp>

#include <boost/test/unit_test.hpp>

#include <functional>
#include <iostream>
#include <random>

#include "Search_UnitTestHelpers.hpp"
// clang-format off
#include "ArborXTest_TreeTypeTraits.hpp"
#include <Kokkos_CopyViews.hpp>
// clang-format on

BOOST_AUTO_TEST_SUITE(ComparisonWithBoost)

namespace tt = boost::test_tools;

inline Kokkos::View<ArborX::Point *, Kokkos::HostSpace>
make_stuctured_cloud(double Lx, double Ly, double Lz, int nx, int ny, int nz)
{
  std::function<int(int, int, int)> ind = [nx, ny](int i, int j, int k) {
    return i + j * nx + k * (nx * ny);
  };
  Kokkos::View<ArborX::Point *, Kokkos::HostSpace> cloud(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "structured_cloud"),
      nx * ny * nz);
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j)
      for (int k = 0; k < nz; ++k)
      {
        cloud[ind(i, j, k)] = {
            {i * Lx / (nx - 1), j * Ly / (ny - 1), k * Lz / (nz - 1)}};
      }
  return cloud;
}

inline Kokkos::View<ArborX::Point *, Kokkos::HostSpace>
make_random_cloud(double Lx, double Ly, double Lz, int n)
{
  Kokkos::View<ArborX::Point *, Kokkos::HostSpace> cloud(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "random_cloud"), n);
  std::default_random_engine generator;
  std::uniform_real_distribution<double> distribution_x(0.0, Lx);
  std::uniform_real_distribution<double> distribution_y(0.0, Ly);
  std::uniform_real_distribution<double> distribution_z(0.0, Lz);
  for (int i = 0; i < n; ++i)
  {
    double x = distribution_x(generator);
    double y = distribution_y(generator);
    double z = distribution_z(generator);
    cloud[i] = {{x, y, z}};
  }
  return cloud;
}

BOOST_AUTO_TEST_CASE_TEMPLATE(boost_rtree, TreeTypeTraits, TreeTypeTraitsList)
{
  using Tree = typename TreeTypeTraits::type;
  using ExecutionSpace = typename TreeTypeTraits::execution_space;
  using DeviceType = typename TreeTypeTraits::device_type;

  // construct a cloud of points (nodes of a structured grid)
  double Lx = 10.0;
  double Ly = 10.0;
  double Lz = 10.0;
  int nx = 11;
  int ny = 11;
  int nz = 11;
  auto cloud = make_stuctured_cloud(Lx, Ly, Lz, nx, ny, nz);
  int n = cloud.size();

  Kokkos::View<ArborX::Box *, DeviceType> bounding_boxes("bounding_boxes", n);
  auto bounding_boxes_host = Kokkos::create_mirror_view(bounding_boxes);
  // build bounding volume hierarchy
  for (int i = 0; i < n; ++i)
  {
    auto const &point = cloud[i];
    bounding_boxes_host[i] = ArborX::Box{point, point};
  }

  Kokkos::deep_copy(bounding_boxes, bounding_boxes_host);

  // random points for radius search and kNN queries
  // compare our solution against Boost R-tree
  int const n_points = 100;
  using MemorySpace = typename Tree::memory_space;
  auto points = Kokkos::create_mirror_view_and_copy(
      MemorySpace{}, make_random_cloud(Lx, Ly, Lz, n_points));

  Kokkos::View<double *, ExecutionSpace> radii("radii", n_points);
  auto radii_host = Kokkos::create_mirror_view(radii);
  Kokkos::View<int *, ExecutionSpace> k("distribution_k", n_points);
  auto k_host = Kokkos::create_mirror_view(k);
  // use random radius for the search and random number k of for the kNN
  // search
  std::default_random_engine generator;
  std::uniform_real_distribution<double> distribution_radius(
      0.0, std::sqrt(Lx * Lx + Ly * Ly + Lz * Lz));
  std::uniform_int_distribution<int> distribution_k(
      1, std::floor(sqrt(nx * nx + ny * ny + nz * nz)));
  for (unsigned int i = 0; i < n_points; ++i)
  {
    radii_host[i] = distribution_radius(generator);
    k_host[i] = distribution_k(generator);
  }

  Kokkos::deep_copy(radii, radii_host);
  Kokkos::deep_copy(k, k_host);

  Kokkos::View<ArborX::Nearest<ArborX::Point> *, DeviceType> nearest_queries(
      "nearest_queries", n_points);
  Kokkos::parallel_for("register_nearest_queries",
                       Kokkos::RangePolicy<ExecutionSpace>(0, n_points),
                       KOKKOS_LAMBDA(int i) {
                         nearest_queries(i) = ArborX::nearest(points(i), k(i));
                       });
  auto nearest_queries_host = Kokkos::create_mirror_view(nearest_queries);
  Kokkos::deep_copy(nearest_queries_host, nearest_queries);

  Kokkos::View<decltype(ArborX::intersects(ArborX::Sphere{})) *, DeviceType>
      within_queries("within_queries", n_points);
  Kokkos::parallel_for(
      "register_within_queries",
      Kokkos::RangePolicy<ExecutionSpace>(0, n_points), KOKKOS_LAMBDA(int i) {
        within_queries(i) =
            ArborX::intersects(ArborX::Sphere{points(i), radii(i)});
      });
  auto within_queries_host = Kokkos::create_mirror_view(within_queries);
  Kokkos::deep_copy(within_queries_host, within_queries);

  Tree tree(ExecutionSpace{}, bounding_boxes);

  BoostExt::RTree<ArborX::Box> rtree(ExecutionSpace{}, bounding_boxes_host);

  // FIXME check currently sporadically fails when using the HIP backend
  ARBORX_TEST_QUERY_TREE(ExecutionSpace{}, tree, nearest_queries,
                         query(ExecutionSpace{}, rtree, nearest_queries_host));

  // FIXME ditto
  ARBORX_TEST_QUERY_TREE(ExecutionSpace{}, tree, within_queries,
                         query(ExecutionSpace{}, rtree, within_queries_host));
}

BOOST_AUTO_TEST_SUITE_END()
