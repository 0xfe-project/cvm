// bench/go/main.go
//
// The Go side of the benchmark: the same three modes, the same metrics, the
// same CSV columns as the C harness, because a comparison where one side
// reports different things is not a comparison.
//
// What differs, and why:
//
//   - One binary with --scenario, not one binary per scenario. The C side
//     builds a binary per (scenario, backend) because it links a different
//     collector into each; Go has one runtime and seven workloads, so seven
//     near-identical binaries would be seven copies of one program.
//
//   - No optimization knob. Go has no equivalent of -O1; it uses its own
//     default. BENCH.md §2.1 states this rather than pretending the two sides
//     are aligned.
//
//   - Alloc-only mode disables the collector rather than enlarging the heap.
//     debug.SetGCPercent(-1) is the exact statement "do not collect", which
//     is stronger and more honest than a heap big enough that collection
//     probably does not happen. Whether it happened is in the gc column.
//
//   - The pause is Go's own stop-the-world pause, from the PauseNs ring
//     buffer. Go's collector is concurrent, so this is the stop-the-world
//     part only — the same interval cvm reports and the same one bdwgc
//     reports from its PRE_STOP_WORLD event. The concurrent marking and
//     sweeping are not in it, for any of the three.

package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"sort"
	"time"
)

type scenario struct {
	name    string
	modes   string // space-separated, matching the C side's --modes output
	cadence int
	run     func()
}

var scenarios = []scenario{
	{"churn-unary", "alloc fixed", 100000, churnUnary},
	{"tree-keep", "alloc fixed auto", 500000, treeKeep},
	{"mixed-sizes", "alloc fixed auto", 20000, mixedSizes},
	{"flat-fp", "alloc auto", 1, flatFP},
	{"read-heavy", "auto", 100000, readHeavy},
	{"write-heavy", "auto", 50000, writeHeavy},
	{"growth-1.5", "auto", 20000, growth},
}

// ------------------------------------------------------------------
// the allocation hook
//
// Every scenario allocates through here, so that fixed-cadence mode has one
// implementation and no scenario can be more or less eager to collect than
// another. The C harness makes the same choice for the same reason.
// ------------------------------------------------------------------

var (
	allocCount   int64
	cadence      int
	mode         string
	forceCollect func()
)

func counting() {
	allocCount++
	if mode == "fixed" && cadence > 0 && allocCount%int64(cadence) == 0 {
		forceCollect()
	}
}

// forceCollect is replaced at init time; this is the real one.
func doForceCollect() {
	runtime.GC()
}

// ------------------------------------------------------------------
// metrics
// ------------------------------------------------------------------

type stats struct {
	allocations  uint64
	collections  uint64
	pauseMaxNs   uint64
	liveObjects  uint64
	heapBytes    uint64
}

func readStats() stats {
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)

	// The pause ring buffer holds the most recent 256 pauses, newest
	// first. Taking the maximum over it gives the longest single
	// stop-the-world pause of the run, which is what the other backends
	// report. A run with more than 256 collections reports the maximum
	// over the last 256 — recorded here because it is a real limit of the
	// source rather than of the measurement.
	var maxPause uint64
	n := ms.NumGC
	ring := uint32(len(ms.PauseNs))
	for i := uint32(0); i < n && i < ring; i++ {
		idx := (n - 1 - i) % ring
		if ms.PauseNs[idx] > maxPause {
			maxPause = ms.PauseNs[idx]
		}
	}

	return stats{
		allocations: uint64(allocCount),
		collections: uint64(ms.NumGC),
		pauseMaxNs:  maxPause,
		liveObjects: ms.HeapObjects,
		heapBytes:   ms.HeapSys,
	}
}

// ------------------------------------------------------------------
// order statistics
// ------------------------------------------------------------------

func median(v []uint64) uint64 {
	s := append([]uint64(nil), v...)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	return s[len(s)/2]
}

func minOf(v []uint64) uint64 {
	s := append([]uint64(nil), v...)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	return s[0]
}

func maxOf(v []uint64) uint64 {
	s := append([]uint64(nil), v...)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	return s[len(s)-1]
}

// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------

func main() {
	name := flag.String("scenario", "", "which scenario to run")
	modeFlag := flag.String("mode", "auto", "alloc | fixed | auto")
	reps := flag.Int("reps", 7, "measured repetitions")
	cadenceFlag := flag.Int("cadence", 0, "allocations between forced collections")
	outPath := flag.String("out", "", "CSV file to append a row to")
	modesProbe := flag.Bool("modes", false, "print the modes and exit")
	noWarmup := flag.Bool("no-warmup", false, "skip the discarded first run")
	flag.Parse()

	checkShapes()

	if *modesProbe {
		// With a scenario, print just that one's modes — the runner asks
		// per scenario, the same way it does for the C binaries. Without
		// one, print them all, which is what a human wants.
		if *name != "" {
			for _, sc := range scenarios {
				if sc.name == *name {
					fmt.Println(sc.modes)
					return
				}
			}
			fmt.Fprintf(os.Stderr, "unknown scenario: %s\n", *name)
			os.Exit(2)
		}
		for _, sc := range scenarios {
			fmt.Printf("%s %s\n", sc.name, sc.modes)
		}
		return
	}

	var sc *scenario
	for i := range scenarios {
		if scenarios[i].name == *name {
			sc = &scenarios[i]
			break
		}
	}
	if sc == nil {
		fmt.Fprintf(os.Stderr, "unknown scenario: %s\n", *name)
		os.Exit(2)
	}

	mode = *modeFlag
	cadence = *cadenceFlag
	if cadence == 0 {
		cadence = sc.cadence
	}
	forceCollect = doForceCollect

	switch mode {
	case "alloc":
		// No collection at all. See the note at the top of this file for
		// why this is preferred to a large heap.
		debug.SetGCPercent(-1)
	case "fixed", "auto":
		debug.SetGCPercent(100)
	default:
		fmt.Fprintf(os.Stderr, "unknown mode: %s\n", mode)
		os.Exit(2)
	}

	if !*noWarmup {
		allocCount = 0
		sc.run()
	}

	wall := make([]uint64, 0, *reps)
	allocs := make([]uint64, 0, *reps)
	colls := make([]uint64, 0, *reps)
	pauses := make([]uint64, 0, *reps)

	for r := 0; r < *reps; r++ {
		before := readStats()

		allocCount = 0
		start := time.Now()
		sc.run()
		took := uint64(time.Since(start).Nanoseconds())

		after := readStats()
		wall = append(wall, took)
		allocs = append(allocs, after.allocations)
		colls = append(colls, after.collections-before.collections)
		pauses = append(pauses, after.pauseMaxNs)
	}

	final := readStats()

	wmed := median(wall)
	fmt.Printf("%-14s %-8s %-8s  wall %8.2f ms [%.2f..%.2f]  alloc %10d  gc %6d  "+
		"pause_max %8.3f ms  live %9d  heap %7.1f MiB\n",
		sc.name, "go", mode, float64(wmed)/1e6,
		float64(minOf(wall))/1e6, float64(maxOf(wall))/1e6,
		median(allocs), median(colls), float64(maxOf(pauses))/1e6,
		final.liveObjects, float64(final.heapBytes)/(1024*1024))

	if *outPath != "" {
		f, err := os.OpenFile(*outPath, os.O_APPEND|os.O_WRONLY, 0o644)
		if err != nil {
			fmt.Fprintf(os.Stderr, "cannot append to %s: %v\n", *outPath, err)
			os.Exit(1)
		}
		fmt.Fprintf(f, "%s,go,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
			sc.name, mode, *reps,
			minOf(wall), wmed, maxOf(wall),
			median(allocs), minOf(allocs), median(colls),
			median(pauses), maxOf(pauses),
			final.liveObjects, final.heapBytes)
		f.Close()
	}
}
