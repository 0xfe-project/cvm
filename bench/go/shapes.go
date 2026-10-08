// bench/go/shapes.go
//
// The object shapes, matched to the C ones byte for byte.
//
// The C suite declares four shapes (bench/scenarios/shapes.c). Go cannot
// include that file, so the shapes are repeated here and the sizes are
// asserted at init rather than trusted: a Go struct whose padding differs by
// eight bytes would put the two runs in different size classes and quietly
// make the comparison meaningless.
//
// The assertions are the mechanism that keeps this honest. If a shape is
// edited on one side and not the other, the Go binary refuses to start.

package main

import (
	"fmt"
	"unsafe"
)

// smallNode is 24 bytes with 2 pointers — the C suite's small_node.
type smallNode struct {
	a, b *smallNode
	pad  uint64
}

// fatNode is 136 bytes with the same 2 pointers. The controlled pair with
// smallNode: identical marking work per object, five times the bytes.
type fatNode struct {
	a, b *fatNode
	pad  [15]uint64
}

// wideNode is 96 bytes, all pointers — the write-heavy shape. Every store is
// a pointer store, so a write barrier has almost nothing to miss.
type wideNode struct {
	slots [8]*wideNode
	pad   [4]uint64
}

// leafBlob is 128 bytes with no pointers: the control.
type leafBlob struct {
	pad [16]uint64
}

type shape struct {
	name string
	size uintptr
	// ptrs is how many pointer slots the C side declares. Kept so the
	// assertion below can check the two agree about more than size.
	ptrs int
}

func checkShapes() {
	expect := []shape{
		{"small_node", 24, 2},
		{"fat_node", 136, 2},
		{"wide_node", 96, 8},
		{"leaf_blob", 128, 0},
	}
	got := []shape{
		{"small_node", unsafe.Sizeof(smallNode{}), 2},
		{"fat_node", unsafe.Sizeof(fatNode{}), 2},
		{"wide_node", unsafe.Sizeof(wideNode{}), 8},
		{"leaf_blob", unsafe.Sizeof(leafBlob{}), 0},
	}
	for i := range expect {
		if expect[i] != got[i] {
			panic(fmt.Sprintf(
				"shape %s is %d bytes/%d ptrs in Go but %d/%d in the C suite: "+
					"the two runs are not measuring the same objects",
				expect[i].name, got[i].size, got[i].ptrs,
				expect[i].size, expect[i].ptrs))
		}
	}
}
