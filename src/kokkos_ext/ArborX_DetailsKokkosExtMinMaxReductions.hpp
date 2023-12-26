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

#ifndef ARBORX_DETAILS_KOKKOS_EXT_MIN_MAX_REDUCTIONS_HPP
#define ARBORX_DETAILS_KOKKOS_EXT_MIN_MAX_REDUCTIONS_HPP

#include <ArborX_DetailsKokkosExtAccessibilityTraits.hpp>
#include <ArborX_Exception.hpp>

#include <Kokkos_Core.hpp>

namespace ArborX::Details::KokkosExt
{

template <typename ExecutionSpace, typename ViewType>
std::pair<typename ViewType::non_const_value_type,
          typename ViewType::non_const_value_type>
minmax_reduce(ExecutionSpace const &space, ViewType const &v)
{
  static_assert(Kokkos::is_execution_space<ExecutionSpace>::value);
  static_assert(Kokkos::is_view<ViewType>::value);
  static_assert(ViewType::rank == 1, "minmax_reduce requires a View of rank 1");

  auto const n = v.extent(0);
  ARBORX_ASSERT(n > 0);

  using ValueType = typename ViewType::non_const_value_type;

  ValueType min_val;
  ValueType max_val;
  Kokkos::parallel_reduce(
      "ArborX::Algorithms::minmax",
      Kokkos::RangePolicy<ExecutionSpace>(space, 0, n),
      KOKKOS_LAMBDA(int i, ValueType &local_min, ValueType &local_max) {
        auto const &val = v(i);
        if (val < local_min)
        {
          local_min = val;
        }
        if (local_max < val)
        {
          local_max = val;
        }
      },
      Kokkos::Min<ValueType>(min_val), Kokkos::Max<ValueType>(max_val));
  return std::make_pair(min_val, max_val);
}

template <typename ExecutionSpace, typename ViewType>
typename ViewType::non_const_value_type min_reduce(ExecutionSpace const &space,
                                                   ViewType const &v)
{
  static_assert(Kokkos::is_execution_space<ExecutionSpace>::value);
  static_assert(Kokkos::is_view<ViewType>::value);
  static_assert(ViewType::rank == 1, "min_reduce requires a View of rank 1");

  auto const n = v.extent(0);
  ARBORX_ASSERT(n > 0);

  using ValueType = typename ViewType::non_const_value_type;

  ValueType result;
  Kokkos::parallel_reduce(
      "ArborX::Algorithms::min",
      Kokkos::RangePolicy<ExecutionSpace>(space, 0, n),
      KOKKOS_LAMBDA(int i, ValueType &update) {
        if (v(i) < update)
          update = v(i);
      },
      Kokkos::Min<ValueType>(result));
  return result;
}

template <typename ExecutionSpace, typename ViewType>
typename ViewType::non_const_value_type max_reduce(ExecutionSpace const &space,
                                                   ViewType const &v)
{
  static_assert(Kokkos::is_execution_space<ExecutionSpace>::value);
  static_assert(Kokkos::is_view<ViewType>::value);
  static_assert(ViewType::rank == 1, "max_reduce requires a View of rank 1");

  auto const n = v.extent(0);
  ARBORX_ASSERT(n > 0);

  using ValueType = typename ViewType::non_const_value_type;

  ValueType result;
  Kokkos::parallel_reduce(
      "ArborX::Algorithms::max",
      Kokkos::RangePolicy<ExecutionSpace>(space, 0, n),
      KOKKOS_LAMBDA(int i, ValueType &update) {
        if (v(i) > update)
          update = v(i);
      },
      Kokkos::Max<ValueType>(result));
  return result;
}

} // namespace ArborX::Details::KokkosExt

#endif
