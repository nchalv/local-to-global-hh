import unittest

from data.generate_data import smooth_key_transitions


class TopNChurnTests(unittest.TestCase):
    def test_targets_rank_turnover_without_changing_counts(self):
        counts = {
            "a": 100,
            "b": 90,
            "c": 80,
            "d": 70,
            "e": 60,
            "f": 50,
        }
        windows = [("test", dict(counts), 10) for _ in range(3)]

        result = smooth_key_transitions(
            windows,
            hot_key_drift_pct=0.0,
            top_n_churn_pct=100.0 / 3.0,
            top_n_churn_n=3,
            seed=7,
            max_rel_freq_delta_pct=100.0,
            new_key_pct=0.0,
            newborn_hot_birth_prob=0.0,
            max_hot_newborn_frac=0.0,
        )

        expected_counts = sorted(counts.values())
        heads = []
        for _, freq_map, _ in result:
            self.assertEqual(sorted(freq_map.values()), expected_counts)
            self.assertEqual(sum(freq_map.values()), sum(counts.values()))
            ranked = sorted(freq_map, key=lambda key: (-freq_map[key], key))
            heads.append(set(ranked[:3]))

        for previous, current in zip(heads, heads[1:]):
            self.assertEqual(len(current - previous), 1)


if __name__ == "__main__":
    unittest.main()
