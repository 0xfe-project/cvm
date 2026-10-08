// bench/go/scenarios.go
//
// The seven scenarios, in Go.
//
// These are rewrites, not shared code: there is no way to link the C
// scenarios into a Go binary, so each one is repeated here and the
// differences are written down where they occur. The rule from BENCH.md §2.4
// is that the *work* must be the same, not that the source is.
//
// The single biggest hazard is escape analysis. Go may allocate an object on
// the stack if it can prove the object never escapes, and a scenario that
// quietly stopped allocating would measure an empty loop — fast, believable
// and meaningless. So every allocation here is stored into a package-level
// sink before being dropped. That is not decoration: it is what makes the
// allocation happen, and it costs the same one store per object in every
// scenario.

package main

// ------------------------------------------------------------------
// allocation
//
// Every allocation goes through one of these, so that the count in the CSV
// is a real count of allocations and so that fixed-cadence mode has one
// place to hook. The C side routes its allocations through bench_alloc for
// the same two reasons; a Go scenario that allocated directly would report
// zero and, by BENCH.md §2.2, invalidate its own row.
// ------------------------------------------------------------------

func countAlloc() {
	allocCount++
	if mode == "fixed" && cadence > 0 && allocCount%int64(cadence) == 0 {
		forceCollect()
	}
}

func newSmall() *smallNode {
	countAlloc()
	return &smallNode{}
}

func newFat() *fatNode {
	countAlloc()
	return &fatNode{}
}

func newWide() *wideNode {
	countAlloc()
	return &wideNode{}
}

func newBlob() *leafBlob {
	countAlloc()
	return &leafBlob{}
}

func newBytes(n int) []byte {
	countAlloc()
	return make([]byte, n)
}

func newFloats(n int) []float64 {
	countAlloc()
	return make([]float64, n)
}

// newChain and newWideChain allocate the pointer array the other scenarios
// hang their objects off. Counted, because it is a heap allocation the C
// version also makes.
func newChain(n int) []*smallNode {
	countAlloc()
	return make([]*smallNode, 0, n)
}

func newWideChain(n int) []*wideNode {
	countAlloc()
	return make([]*wideNode, n)
}

// ------------------------------------------------------------------
// sinks
//
// Package-level, so the compiler cannot prove the values are unobserved.
// One per shape, so a store into a sink is the same kind of store the C
// scenarios do when they put an object in a frame slot.
// ------------------------------------------------------------------

var (
	sinkSmall     *smallNode
	sinkFat       *fatNode
	sinkWide      *wideNode
	sinkBlob      *leafBlob
	sinkBytes     []byte
	sinkChain     []*smallNode
	sinkChainWide []*wideNode
	sinkFloats    []float64
)

// ------------------------------------------------------------------
// 1. churn-unary
// ------------------------------------------------------------------

// churnUnary allocates and drops. Nothing survives, so nothing is ever
// marked, and the only cost measured is the allocator's.
//
// One object in sixty-four is kept alive — the same ratio as the C version —
// so that a collector cannot conclude the heap is entirely garbage.
func churnUnary() {
	const objects = 4000000
	const keepInterval = 64

	for i := 0; i < objects; i++ {
		p := newSmall()
		p.pad = uint64(i)
		if i%keepInterval == 0 {
			sinkSmall = p
		}
	}
}

// ------------------------------------------------------------------
// 2. tree-keep
// ------------------------------------------------------------------

const (
	treeDepth  = 17
	treeRounds = 24
)

// buildTree builds a complete binary tree of the given depth.
//
// The C version holds both children in frame slots while the other is being
// built, because a precise collector cannot see a C local. Go needs no such
// protocol: its compiler knows a live local is a root and reports it in the
// stack map, so a local here is genuinely reachable. That is a real advantage
// of Go's design and not an omission in this rewrite.
func buildTree(depth int) *smallNode {
	if depth == 0 {
		return newSmall()
	}
	left := buildTree(depth - 1)
	right := buildTree(depth - 1)
	node := newSmall()
	node.a = left
	node.b = right
	node.pad = uint64(depth)
	return node
}

// treeKeep builds a tree, drops the previous one, keeps the last. The only
// scenario that exercises marking and sweeping together with a live set worth
// the name.
func treeKeep() {
	for round := 0; round < treeRounds; round++ {
		// The previous tree becomes garbage here, all at once: only
		// sinkSmall reached it, and it is overwritten.
		sinkSmall = buildTree(treeDepth)
	}
}

// ------------------------------------------------------------------
// 3. mixed-sizes
// ------------------------------------------------------------------

const (
	mixedRounds = 400000
	mixedSlots  = 64
	mixedSeed   = 0x9e3779b9
)

// pickSize draws a size across the boundaries cvm's allocator has: the
// 256-byte inline limit and the 8 KiB large-object threshold. Landing on the
// boundaries rather than around them is deliberate — that is where an
// off-by-one in a size class shows up.
func pickSize(state *uint32) int {
	*state = (*state * 1103515245) + 12345
	r := (*state >> 16) & 0xff
	switch {
	case r < 64:
		return 16 + int(r%48)
	case r < 128:
		return 240 + int(r%32)
	case r < 192:
		return 512 + int(r%1536)
	default:
		return 7000 + int(r%2500)
	}
}

// mixedSizes walks every allocation path and is the fragmentation scenario:
// sizes chosen so allocations land awkwardly on line boundaries, lifetimes
// chosen so holes accumulate without ever becoming a whole free block.
//
// A byte slice rather than a typed struct, because the C version allocates
// raw bytes here. make([]byte, n) is one heap allocation of n bytes — the
// slice header stays on the stack — so the two are the same request.
func mixedSizes() {
	slots := make([]byte, mixedSlots)
	state := uint32(mixedSeed)

	for round := 0; round < mixedRounds; round++ {
		slot := (state >> 8) % mixedSlots
		state = (state * 1103515245) + 12345

		if state&1 != 0 {
			slots[slot] = 0
		} else {
			buf := newBytes(pickSize(&state))
			buf[0] = byte(round)
			sinkBytes = buf
		}
	}
	_ = slots
}

// ------------------------------------------------------------------
// 4. flat-fp
// ------------------------------------------------------------------

const (
	bodies = 512
	steps  = 40
)

// flatFP is an n-body kernel over flat arrays. Almost no pointers, so a
// tracing collector should do nearly nothing: this is the control that makes
// a slow result elsewhere attributable to marking rather than to everything.
//
// Four slices rather than one, matching the C version's four allocations.
func flatFP() {
	x := newFloats(bodies)
	y := newFloats(bodies)
	vx := newFloats(bodies)
	vy := newFloats(bodies)
	sinkFloats = x

	for i := 0; i < bodies; i++ {
		x[i] = float64(i%32) - 16.0
		y[i] = float64(i/32) - 8.0
	}

	for step := 0; step < steps; step++ {
		for i := 0; i < bodies; i++ {
			for j := i + 1; j < bodies; j++ {
				dx := x[j] - x[i]
				dy := y[j] - y[i]
				d2 := dx*dx + dy*dy + 1e-9
				inv := 1.0 / (d2 * d2 * d2)
				vx[i] += dx * inv
				vy[i] += dy * inv
				vx[j] -= dx * inv
				vy[j] -= dy * inv
			}
		}
		for i := 0; i < bodies; i++ {
			x[i] += vx[i] * 0.01
			y[i] += vy[i] * 0.01
		}
	}
}

// ------------------------------------------------------------------
// 5. read-heavy
// ------------------------------------------------------------------

const (
	readNodes      = 200000
	readTraversals = 12
)

// readHeavy builds a chain once and traverses it many times with about one
// write per hundred reads. Mark throughput and cache behaviour, with mutation
// cost nearly removed.
//
// The chain is a slice of pointers rather than a next-field, because a Go
// pointer field would be a second store per node during the build and the C
// version uses two. The slice keeps the same shape: one pointer per node.
func readHeavy() {
	chain := newChain(readNodes)
	for i := 0; i < readNodes; i++ {
		n := newSmall()
		n.pad = uint64(i)
		chain = append(chain, n)
	}
	sinkChain = chain

	var sum uint64
	for t := 0; t < readTraversals; t++ {
		// A collection per traversal, as the C version asks for: without
		// one, this measures a traversal with no marking in it.
		forceCollect()
		for i, n := range chain {
			sum += n.pad
			if i%100 == 0 {
				n.pad = uint64(i)
			}
		}
	}
	sinkSmall = chain[len(chain)-1]
	_ = sum
}

// ------------------------------------------------------------------
// 6. write-heavy
// ------------------------------------------------------------------

const (
	writeNodes = 20000
	writeRounds = 400
)

// writeHeavy mutates a dense ring of pointer-heavy objects with few
// traversals. This is the scenario where cvm's free stores meet Go's write
// barrier, and BENCH.md §4 says out loud that a cvm win here is Go paying for
// concurrency rather than Go being slow.
func writeHeavy() {
	nodes := newWideChain(writeNodes)
	for i := range nodes {
		nodes[i] = newWide()
	}
	sinkChainWide = nodes

	for i := range nodes {
		next := nodes[(i+1)%writeNodes]
		for s := range nodes[i].slots {
			nodes[i].slots[s] = next
		}
	}

	n := nodes[0]
	for round := 0; round < writeRounds; round++ {
		next := n.slots[0]
		for s := range n.slots {
			n.slots[s] = next
		}
		n.pad[0] = uint64(round)
		n = next
	}
	sinkWide = n
}

// ------------------------------------------------------------------
// 7. growth-1.5
// ------------------------------------------------------------------

const (
	growthRounds   = 8
	growthBase     = 4000
	growthNum      = 149
	growthDen      = 100
)

// growth allocates a live set that grows by about 1.49x per round and drops
// nothing. The scenario for heap policy rather than mechanism: every backend
// has to decide how much to grow by, and this is where a twenty-year-old
// default has the advantage.
//
// The ratio matters. cvm grows its heap by one factor below the 128 MiB
// threshold and a smaller one above it, so a live set growing at 1.49x
// crosses the threshold partway through and the policy changes underneath it.
func growth() {
	blocks := newChain(growthBase * growthRounds)
	target := growthBase

	for round := 0; round < growthRounds; round++ {
		for i := 0; i < target; i++ {
			n := newSmall()
			n.pad = uint64(i)
			blocks = append(blocks, n)
		}
		// Kept: nothing is dropped, which is the whole point of this
		// scenario.
		sinkChain = blocks

		target = target * growthNum / growthDen
		if target == 0 {
			target = 1
		}
	}
}
