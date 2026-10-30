# seekdb x AMD hipVS: submission checkpoint

This branch is the frozen V5 contest baseline (634888d9b) plus this note only.

- No source, CMake, or build-script change: the build products are byte-identical to
  the frozen baseline, so the compile stage cannot fail because of us.
- It exists so the evaluation can confirm the repository, the branch, the commit
  lineage (descendant of 634888d9b) and the GPU path before the optimisation work
  lands on contest/v5-cagra-build.

The optimisation branch keeps the same baseline and only changes
	ools/hipvs_bridge/ plus 	ools/bench/, with the CAGRA build parameters left at
their V5 values unless explicitly overridden.