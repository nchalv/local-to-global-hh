# Measurement and Accounting

The evaluator reports worker memory per logical worker, not a sum over workers.
It includes resident algorithm state and resident key identity state.

Upstream communication includes serialized worker telemetry and on-demand raw
key resolution. Downstream communication includes adaptive capacity updates
and Hybrid head dissemination.

Coordinator peak is the maximum concurrently resident reduction, controller,
and key-resolution state. Retained reusable reducer buffers and concurrent
parallel shards are charged at their high-water capacity.

Serial and parallel reducers preserve the same estimates, certificates, and
controller decisions. Parallel reduction trades a larger coordinator working
set for lower reduction latency.

The evaluator runs independent logical workers and a logical coordinator in one
process. The topology is therefore simulated, while the reported state,
communication, and reduction accounting follows the same method contract for
all compared methods.
