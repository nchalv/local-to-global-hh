import math
import unittest

from data.partitioning import assign_partitions


class FilterAwareVisibilityPartitioningTest(unittest.TestCase):
    def setUp(self):
        self.freq = {
            "strong_hh": 8_000,
            "marginal_hh": 5_000,
            **{f"background_{i}": max(1, 2_900 - 8 * i) for i in range(240)},
        }

    def generate(self):
        return assign_partitions(
            self.freq,
            20,
            policy="filter_aware_visibility_adversary",
            seed=17,
            window_id=3,
            top_n=100,
            fva_local_threshold_scale=1.0,
            fva_competition_width_ratio=1.0,
            fva_max_partition_load_ratio=4.0,
            fva_large_partition_count=1,
            fva_hh_overflow_reporters=1,
        )

    def local_reported_mass(self, partitioned, key):
        reported = 0
        for local_counts in partitioned.values():
            threshold = max(
                1,
                int(math.ceil(sum(local_counts.values()) / 100.0)),
            )
            visible = [
                (count, str(candidate), candidate)
                for candidate, count in local_counts.items()
                if count >= threshold
            ]
            visible.sort(key=lambda item: (-item[0], item[1]))
            if key in {candidate for _, _, candidate in visible[:100]}:
                reported += local_counts[key]
        return reported

    def test_preserves_counts_and_is_deterministic(self):
        first = self.generate()
        self.assertEqual(first, self.generate())

        rebuilt = {}
        for local_counts in first.values():
            for key, count in local_counts.items():
                rebuilt[key] = rebuilt.get(key, 0) + count
        self.assertEqual(rebuilt, self.freq)

    def test_respects_load_bound_and_suppresses_marginal_hh_mass(self):
        partitioned = self.generate()
        loads = [sum(local_counts.values()) for local_counts in partitioned.values()]
        mean_load = sum(loads) / len(loads)
        self.assertTrue(all(load > 0 for load in loads))
        self.assertLessEqual(max(loads), 4.0 * mean_load + 1.0)

        self.assertLess(
            self.local_reported_mass(partitioned, "marginal_hh"),
            self.freq["marginal_hh"],
        )


if __name__ == "__main__":
    unittest.main()
