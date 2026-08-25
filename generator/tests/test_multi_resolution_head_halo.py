import unittest

from data.distributions import MultiResolutionHeadHaloDistributionGenerator


class MultiResolutionHeadHaloTest(unittest.TestCase):
    def test_preserves_mass_and_pressure_at_every_resolution(self):
        generator = MultiResolutionHeadHaloDistributionGenerator()
        windows = [generator.generate(60_000, 15_000) for _ in range(56)]

        for counts in windows:
            self.assertEqual(sum(counts.values()), 60_000)
            self.assertGreaterEqual(len(counts), 1_600)
            mean_local_distinct = sum(min(value, 100) for value in counts.values()) / 100.0
            self.assertGreaterEqual(mean_local_distinct, 450.0)
            for n_value in (100, 200, 400):
                threshold = 60_000 / n_value
                self.assertTrue(any(value > threshold for value in counts.values()))
                self.assertTrue(
                    any(0.85 * threshold <= value <= threshold for value in counts.values())
                )

    def test_oscillating_cohorts_change_the_distribution(self):
        generator = MultiResolutionHeadHaloDistributionGenerator()
        first = generator.generate(60_000, 15_000)
        later = None
        for _ in range(6):
            later = generator.generate(60_000, 15_000)
        self.assertNotEqual(first, later)


if __name__ == "__main__":
    unittest.main()
