import numba
from numba import njit
from math import nan, inf
from numpy.testing import assert_equal # for nans comparison

from numba.tests.support import TestCase
import unittest

import itertools

_test_values = [-3,-2,-1,0,1,2,3,-2.5,-1.0,-0.5 -0.0, 0.0, 0.5, 1.0, 2.5, -inf, inf] # TODO: nans
class TestMlirBasic(TestCase):

    def test_ret(self):
        def py_func(a):
            return a

        jit_func = njit(py_func)
        for val in _test_values:
            assert_equal(py_func(val), jit_func(val))

    def test_ops(self):
        py_funcs = [
            lambda a, b: a + b,
            lambda a, b: a - b,
            lambda a, b: a * b,
            # TODO: div
            ]

        for py_func in py_funcs:
            jit_func = njit(py_func)
            for a, b in itertools.product(_test_values, _test_values):
                assert_equal(py_func(a, b), jit_func(a, b))

    def test_cmp_ops(self):
        py_funcs = [
            lambda a, b: a if a > b else b,
            lambda a, b: a if a < b else b,
            lambda a, b: a if a >= b else b,
            lambda a, b: a if a <= b else b,
            lambda a, b: a if a == b else b,
            lambda a, b: a if a != b else b,
            ]

        for py_func in py_funcs:
            jit_func = njit(py_func)
            for a, b in itertools.product(_test_values, _test_values):
                assert_equal(py_func(a, b), jit_func(a, b))

    def test_const_ops(self):
        py_funcs = [
            lambda a: a + 42,
            lambda a: 43 + a,
            ]

        for py_func in py_funcs:
            jit_func = njit(py_func)
            for val in _test_values:
                assert_equal(py_func(val), jit_func(val))

    def test_var(self):
        def py_func(a):
            c = 1
            c = c + a
            return c

        jit_func = njit(py_func)
        for val in _test_values:
            assert_equal(py_func(val), jit_func(val))

    def test_jump(self):
        def py_func(a, b):
            c = 3
            if a > 5:
                c = c + a
            c = c + b
            return c

        jit_func = njit(py_func)
        for a, b in itertools.product(_test_values, _test_values):
            assert_equal(py_func(a, b), jit_func(a, b))


if __name__ == '__main__':
    unittest.main()
