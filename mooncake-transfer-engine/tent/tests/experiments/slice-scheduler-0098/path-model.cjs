const assert = require('assert').strict;
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const commit = 'db6f14bd019d7fe08b2497e88046b72384af287f';

function guardedTwoPath(paths, sliceCount) {
  assert.equal(paths.length, 2);
  const balanced = uniform(paths, sliceCount);
  if (sliceCount < 2 || paths.some(candidate => !Number.isFinite(candidate.rate) ||
      candidate.rate <= 0 || candidate.samples < 8 || candidate.ageMs > 50 ||
      candidate.coefficientOfVariation > 0.25)) {
    return { allocation: balanced, reason: 'insufficient_confidence' };
  }
  let best = balanced;
  for (let first = 1; first < sliceCount; first++) {
    const allocation = [first, sliceCount - first];
    if (Math.max(...allocation) > 4 * Math.min(...allocation)) continue;
    if (finishTime(paths, allocation) < finishTime(paths, best)) best = allocation;
  }
  if (finishTime(paths, best) > 0.95 * finishTime(paths, balanced)) {
    return { allocation: balanced, reason: 'predicted_gain_below_margin' };
  }
  return { allocation: best, reason: 'bounded_adaptation' };
}

function sharedNicTime(paths, allocation, localRates, remoteRates) {
  const localBytes = localRates.map(() => 0);
  const remoteBytes = remoteRates.map(() => 0);
  paths.forEach((candidate, index) => {
    localBytes[candidate.local] += allocation[index];
    remoteBytes[candidate.remote] += allocation[index];
  });
  return Math.max(...localBytes.map((bytes, index) => bytes / localRates[index]),
    ...remoteBytes.map((bytes, index) => bytes / remoteRates[index]));
}

function extractSlicePlan(sourceRoot, outputDirectory) {
  const sourcePath = path.join(sourceRoot,
    'mooncake-transfer-engine/tent/include/tent/transport/rdma/slice.h');
  const source = fs.readFileSync(sourcePath, 'utf8');
  const start = source.indexOf('struct RdmaSlicePlan {');
  const end = source.indexOf('\n// Forward declarations', start);
  assert.ok(start >= 0 && end > start);
  const snippet = source.slice(start, end);
  assert.ok(snippet.includes('inline RdmaSlicePlan planRdmaSlices('));
  fs.mkdirSync(outputDirectory, { recursive: true });
  fs.writeFileSync(path.join(outputDirectory, 'slice-plan.generated.h'),
    '#pragma once\n#include <cstdint>\nnamespace mooncake { namespace tent {\n' +
    snippet + '\n}}\n');
  const digest = text => crypto.createHash('sha256').update(text).digest('hex');
  const metadata = { commit, sourcePath, sourceSha256: digest(source), snippetSha256: digest(snippet),
    scope: 'verbatim extracted struct and function, not full slice.h integration' };
  fs.writeFileSync(path.join(outputDirectory, 'slice-source.json'), JSON.stringify(metadata, null, 2) + '\n');
  console.log(JSON.stringify(metadata, null, 2));
}

function inverseScore(paths, sliceCount, sliceBytes = 1) {
  const weights = paths.map(path => path.rate / (path.queue + sliceBytes));
  const totalWeight = weights.reduce((sum, weight) => sum + weight, 0);
  const allocation = weights.map(weight => Math.floor(weight / totalWeight * sliceCount));
  const best = weights.indexOf(Math.max(...weights));
  allocation[best] += sliceCount - allocation.reduce((sum, count) => sum + count, 0);
  return allocation;
}

function uniform(paths, sliceCount) {
  return paths.map((path, index) => Math.floor(sliceCount / paths.length) +
    (index < sliceCount % paths.length ? 1 : 0));
}

function finishTime(paths, allocation, sliceBytes = 1) {
  return Math.max(0, ...paths.map((path, index) => allocation[index] === 0 ? 0 :
    path.startup + (path.queue + allocation[index] * sliceBytes) / path.rate));
}

function earliestFinish(paths, sliceCount, sliceBytes = 1) {
  const allocation = paths.map(() => 0);
  for (let slice = 0; slice < sliceCount; slice++) {
    let best = 0;
    for (let index = 1; index < paths.length; index++) {
      const next = pathIndex => paths[pathIndex].startup +
        (paths[pathIndex].queue + (allocation[pathIndex] + 1) * sliceBytes) /
        paths[pathIndex].rate;
      if (next(index) < next(best)) best = index;
    }
    allocation[best]++;
  }
  return allocation;
}

function exhaustiveTwoPath(paths, sliceCount) {
  let optimum = Infinity;
  for (let first = 0; first <= sliceCount; first++) {
    optimum = Math.min(optimum, finishTime(paths, [first, sliceCount - first]));
  }
  return optimum;
}

function run() {
  let oracleChecks = 0;
  for (const firstRate of [1, 2, 5]) {
    for (const secondRate of [1, 2, 5]) {
      for (const firstQueue of [0, 8, 32]) {
        for (const secondQueue of [0, 8, 32]) {
          for (const startup of [0, 3]) {
            for (const sliceCount of [1, 2, 7, 16, 64]) {
              const paths = [
                { rate: firstRate, queue: firstQueue, startup },
                { rate: secondRate, queue: secondQueue, startup: 0 },
              ];
              const allocation = earliestFinish(paths, sliceCount);
              assert.equal(allocation.reduce((sum, count) => sum + count, 0), sliceCount);
              assert.ok(Math.abs(finishTime(paths, allocation) -
                exhaustiveTwoPath(paths, sliceCount)) < 1e-9);
              oracleChecks++;
            }
          }
        }
      }
    }
  }

  const scenarios = [
    { name: 'symmetric', paths: [
      { rate: 1, queue: 0, startup: 0 }, { rate: 1, queue: 0, startup: 0 },
    ] },
    { name: 'bandwidth_2_to_1', paths: [
      { rate: 2, queue: 0, startup: 0 }, { rate: 1, queue: 0, startup: 0 },
    ] },
    { name: 'one_path_queued', paths: [
      { rate: 1, queue: 0, startup: 0 }, { rate: 1, queue: 32, startup: 0 },
    ] },
  ];
  const results = scenarios.map(scenario => {
    const sliceCount = 64;
    const current = inverseScore(scenario.paths, sliceCount);
    const balanced = uniform(scenario.paths, sliceCount);
    const candidate = earliestFinish(scenario.paths, sliceCount);
    return {
      name: scenario.name,
      paths: scenario.paths,
      sliceCount,
      current: { allocation: current, completion: finishTime(scenario.paths, current) },
      uniform: { allocation: balanced, completion: finishTime(scenario.paths, balanced) },
      candidate: { allocation: candidate, completion: finishTime(scenario.paths, candidate) },
    };
  });
  assert.deepEqual(results[0].candidate.allocation, [32, 32]);
  assert.deepEqual(results[2].candidate.allocation, [48, 16]);
  assert.ok(results[2].candidate.completion < results[2].current.completion);

  const confident = rate => ({ rate, queue: 0, startup: 0, samples: 16,
    ageMs: 1, coefficientOfVariation: 0.01 });
  const estimates = [confident(1), confident(5)];
  const guarded = guardedTwoPath(estimates, 64);
  assert.deepEqual(guarded.allocation, [13, 51]);
  const invertedReality = [confident(5), confident(1)];
  const staleCounterexample = {
    historicalEstimates: estimates,
    actualPaths: invertedReality,
    guarded,
    guardedActualCompletion: finishTime(invertedReality, guarded.allocation),
    uniformActualCompletion: finishTime(invertedReality, [32, 32]),
  };
  assert.ok(staleCounterexample.guardedActualCompletion > staleCounterexample.uniformActualCompletion);
  let guardChecks = 0;
  for (const changed of [{ samples: 0 }, { samples: 7 }, { ageMs: 51 },
    { coefficientOfVariation: 0.3 }, { rate: 0 }, { rate: NaN }]) {
    const degraded = estimates.map(candidate => ({ ...candidate }));
    Object.assign(degraded[0], changed);
    assert.deepEqual(guardedTwoPath(degraded, 64).allocation, [32, 32]);
    guardChecks++;
  }
  assert.equal(guardedTwoPath([confident(1), confident(1)], 64).reason,
    'predicted_gain_below_margin');
  guardChecks++;

  const sharedPaths = [
    { ...confident(1), local: 0, remote: 0 },
    { ...confident(1), local: 1, remote: 0 },
    { ...confident(1), local: 1, remote: 1 },
  ];
  const independentAllocation = earliestFinish(sharedPaths, 64);
  let sharedOptimum = Infinity;
  let sharedOptimalAllocation;
  for (let first = 0; first <= 64; first++) {
    for (let second = 0; second <= 64 - first; second++) {
      const allocation = [first, second, 64 - first - second];
      const completion = sharedNicTime(sharedPaths, allocation, [1, 1], [1, 1]);
      if (completion < sharedOptimum) {
        sharedOptimum = completion;
        sharedOptimalAllocation = allocation;
      }
    }
  }
  assert.equal(sharedOptimum, 32);
  const sharedNicCounterexample = {
    paths: sharedPaths,
    independentAllocation,
    independentPredictedCompletion: finishTime(sharedPaths, independentAllocation),
    sharedResourceCompletion: sharedNicTime(sharedPaths, independentAllocation, [1, 1], [1, 1]),
    sharedOptimalAllocation,
    sharedOptimum,
    enumeratedAllocations: 2145,
  };
  assert.ok(sharedNicCounterexample.sharedResourceCompletion > sharedOptimum);
  const startupPaths = [confident(1), { ...confident(1), startup: 100 }];
  const explorationCounterexample = {
    paths: startupPaths,
    boundedAllocation: guardedTwoPath(startupPaths, 64).allocation,
    withoutForcedExploration: earliestFinish(startupPaths, 64),
  };
  assert.deepEqual(explorationCounterexample.withoutForcedExploration, [64, 0]);
  assert.ok(finishTime(startupPaths, explorationCounterexample.boundedAllocation) >
    finishTime(startupPaths, explorationCounterexample.withoutForcedExploration));

  const report = {
    commit,
    generatedAt: new Date().toISOString(),
    scriptSha256: crypto.createHash('sha256').update(fs.readFileSync(__filename)).digest('hex'),
    kind: 'independent_path_model_not_rdma_benchmark',
    units: 'normalized_bytes_and_time',
    assumptions: [
      'constant independent path rates',
      'known fixed queues and startup delays',
      'equal slices, no shared NIC or memory bottlenecks',
      'inverse-score baseline omits NUMA, epsilon, jitter and probing',
    ],
    oracleChecks,
    guardChecks,
    results,
    staleCounterexample,
    sharedNicCounterexample,
    explorationCounterexample,
  };
  return report;
}

if (require.main === module) {
  if (process.argv[2] === '--extract-slice') {
    extractSlicePlan(process.argv[3], process.argv[4]);
  } else {
    const report = run();
    if (process.argv[2]) {
      fs.mkdirSync(path.dirname(process.argv[2]), { recursive: true });
      fs.writeFileSync(process.argv[2], JSON.stringify(report, null, 2) + '\n');
    }
    console.log(JSON.stringify(report, null, 2));
  }
}

module.exports = { inverseScore, uniform, finishTime, earliestFinish, guardedTwoPath, run };
