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

#include <ArborX_AccessTraits.hpp>
#include <ArborX_AttachIndices.hpp>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(AttachIndices)

BOOST_AUTO_TEST_CASE(attach_indices)
{
  using ArborX::Details::AccessValues;
  using ArborX::Experimental::attach_indices;

  Kokkos::View<ArborX::Point *, Kokkos::HostSpace> p("Testing::p", 10);
  auto p_with_indices = attach_indices(p);
  AccessValues<decltype(p_with_indices), ArborX::PrimitivesTag> p_values{
      p_with_indices};
  BOOST_TEST(p_values(0).index == 0);
  BOOST_TEST(p_values(9).index == 9);

  using IntersectsPredicate = decltype(ArborX::intersects(ArborX::Point{}));
  Kokkos::View<IntersectsPredicate *, Kokkos::HostSpace> q("Testing::q", 10);
  auto q_with_indices = attach_indices<long>(q);
  AccessValues<decltype(q_with_indices), ArborX::PredicatesTag> q_values{
      q_with_indices};
  BOOST_TEST(ArborX::getData(q_values(0)) == 0);
  BOOST_TEST(ArborX::getData(q_values(9)) == 9);
}

BOOST_AUTO_TEST_SUITE_END()
