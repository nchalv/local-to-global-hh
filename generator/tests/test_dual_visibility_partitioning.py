import math
import unittest

from data.partitioning import assign_partitions


class DualVisibilityPartitioningTest(unittest.TestCase):
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
            policy="dual_visibility_adversary",
            seed=17,
            window_id=3,
            top_n=100,
            dva_local_threshold_scale=0.7,
            dva_competition_width_ratio=1.0,
            dva_max_partition_load_ratio=4.0,
            dva_large_partition_count=1,
            dva_hh_overflow_reporters=1,
        )

    def test_preserves_every_global_count(self):
        partitioned = self.generate()
        rebuilt = {}
        for local_counts in partitioned.values():
            for key, count in local_counts.items():
                rebuilt[key] = rebuilt.get(key, 0) + count
        self.assertEqual(rebuilt, self.freq)

    def test_respects_partition_load_bound_and_is_deterministic(self):
        first = self.generate()
        second = self.generate()
        self.assertEqual(first, second)

        loads = [sum(local_counts.values()) for local_counts in first.values()]
        mean_load = sum(loads) / len(loads)
        self.assertTrue(all(load > 0 for load in loads))
        self.assertLessEqual(max(loads), 4.0 * mean_load + 1.0)

    def test_suppresses_marginal_and_strong_hh_reported_mass(self):
        partitioned = self.generate()
        total = sum(self.freq.values())
        global_threshold = total / 100.0
        nonheavy = {key for key, count in self.freq.items() if count <= global_threshold}

        def visible_mass(key):
            visible = 0
            for local_counts in partitioned.values():
                local = local_counts.get(key, 0)
                if local <= 0:
                    continue
                local_threshold = max(
                    1,
                    int(math.ceil(0.7 * sum(local_counts.values()) / 100.0)),
                )
                competitors = sorted(
                    (count for candidate, count in local_counts.items() if candidate in nonheavy),
                    reverse=True,
                )
                rank_floor = competitors[99] if len(competitors) >= 100 else 0
                if local >= local_threshold or rank_floor <= 0 or local >= rank_floor:
                    visible += local
            return visible

        marginal_visible = visible_mass("marginal_hh")
        strong_visible = visible_mass("strong_hh")
        self.assertLess(marginal_visible, global_threshold)
        self.assertLess(strong_visible, self.freq["strong_hh"])


if __name__ == "__main__":
    unittest.main()
