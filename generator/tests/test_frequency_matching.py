import itertools
import unittest

import numpy as np

from data.generate_data import _minimum_absolute_matching


class MinimumAbsoluteMatchingTests(unittest.TestCase):
    def test_matches_brute_force_optimum(self):
        cases = [
            ([1, 4, 9], [2, 5, 8]),
            ([0, 0, 7], [0, 3, 3]),
            ([10, 1, 6, 2], [8, 3, 7, 0]),
        ]

        for left, right in cases:
            with self.subTest(left=left, right=right):
                a = np.asarray(left)
                b = np.asarray(right)
                rows, cols = _minimum_absolute_matching(a, b)
                actual = sum(abs(int(a[i]) - int(b[j])) for i, j in zip(rows, cols))
                expected = min(
                    sum(abs(int(a[i]) - int(b[j])) for i, j in enumerate(perm))
                    for perm in itertools.permutations(range(len(b)))
                )
                self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
